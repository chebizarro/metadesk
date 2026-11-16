/*
 * metadesk — mcp_stdio.c
 * stdio transport for MCP server.
 *
 * Reads newline-delimited JSON-RPC messages from in_fd, dispatches
 * to the MCP server, writes responses to out_fd.
 */
#include "mcp_stdio.h"
#define MD_LOG_TAG "mcp-stdio"
#include "log.h"
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

/* ── Initial line buffer size ────────────────────────────────── */

#define INITIAL_BUF_SIZE 4096
#define MD_MCP_STDIO_WRITE_TIMEOUT_MS 2000

/* ── stdio context ───────────────────────────────────────────── */

struct MdMcpStdio {
    MdMcpServer    *server;
    int             in_fd;
    int             out_fd;
    int             out_flags;
    bool            write_error_logged;
    bool            output_failed; /* partial frame cannot be safely resumed */
    atomic_bool     shutdown;
    pthread_mutex_t write_mu;  /* serialize writes to out_fd */
};

/* ── Write callback (called by MCP server to send responses) ── */

int md_mcp_stdio_write(const char *json, size_t len, void *userdata)
{
    MdMcpStdio *ctx = (MdMcpStdio *)userdata;
    if (!ctx || atomic_load(&ctx->shutdown)) return -1;

    pthread_mutex_lock(&ctx->write_mu);

    if (ctx->output_failed || atomic_load(&ctx->shutdown)) {
        pthread_mutex_unlock(&ctx->write_mu);
        return -1;
    }

    /* Responses and notifications share a single budget, including newline.
     * O_NONBLOCK is essential: POLLOUT alone does not bound a large write. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000
                     + MD_MCP_STDIO_WRITE_TIMEOUT_MS;
    size_t total = 0;
    while (total <= len) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t remaining = deadline - ((int64_t)ts.tv_sec * 1000
                                        + ts.tv_nsec / 1000000);
        if (remaining <= 0) break;
        struct pollfd pfd = { .fd = ctx->out_fd, .events = POLLOUT };
        int rc = poll(&pfd, 1, (int)remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) break;
        const char *data = total < len ? json + total : "\n";
        size_t count = total < len ? len - total : 1;
        ssize_t written = write(ctx->out_fd, data, count);
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (written <= 0) break;
        total += (size_t)written;
    }

    int rc = total > len ? 0 : -1;
    if (rc < 0) {
        /* Do not append another JSON message to a truncated frame. Input
         * dispatch remains live, but this output channel is no longer usable. */
        if (total > 0) ctx->output_failed = true;
        if (!ctx->write_error_logged) {
            MD_LOG_W("dropping stdio message after failed or timed-out write");
            ctx->write_error_logged = true;
        }
    }
    pthread_mutex_unlock(&ctx->write_mu);
    return rc;
}

/* ── Lifecycle ───────────────────────────────────────────────── */

MdMcpStdio *md_mcp_stdio_create(MdMcpServer *server, int in_fd, int out_fd)
{
    if (!server) return NULL;

    MdMcpStdio *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->out_flags = fcntl(out_fd, F_GETFL);
    if (ctx->out_flags < 0 ||
        fcntl(out_fd, F_SETFL, ctx->out_flags | O_NONBLOCK) < 0) {
        free(ctx);
        return NULL;
    }

    ctx->server = server;
    ctx->in_fd = in_fd;
    ctx->out_fd = out_fd;
    atomic_init(&ctx->shutdown, false);
    pthread_mutex_init(&ctx->write_mu, NULL);

    return ctx;
}

/* ── Run loop ────────────────────────────────────────────────── */

int md_mcp_stdio_run(MdMcpStdio *ctx)
{
    if (!ctx) return -1;

    /* Line buffer for reading newline-delimited JSON */
    size_t buf_cap = INITIAL_BUF_SIZE;
    char *buf = malloc(buf_cap);
    if (!buf) return -1;
    size_t buf_len = 0;

    while (!atomic_load(&ctx->shutdown)) {
        /* Read a chunk */
        if (buf_len + 1 >= buf_cap) {
            if (buf_cap >= MD_MCP_MAX_MESSAGE_SIZE) {
                /* A peer that never sends a newline must not grow the
                 * buffer until realloc fails — drop the connection. */
                free(buf);
                return -1;
            }
            buf_cap *= 2;
            char *new_buf = realloc(buf, buf_cap);
            if (!new_buf) { free(buf); return -1; }
            buf = new_buf;
        }

        ssize_t n = read(ctx->in_fd, buf + buf_len, buf_cap - buf_len - 1);
        if (n <= 0) {
            /* EOF or error */
            if (n == 0 || (n < 0 && errno != EINTR)) {
                break;
            }
            continue;  /* EINTR — retry */
        }
        buf_len += (size_t)n;
        buf[buf_len] = '\0';

        /* Process complete lines */
        char *start = buf;
        char *newline;
        while ((newline = strchr(start, '\n')) != NULL) {
            *newline = '\0';
            size_t line_len = (size_t)(newline - start);

            /* Skip empty lines */
            if (line_len > 0) {
                md_mcp_server_handle_message_with_sink(ctx->server,
                                                       start, line_len,
                                                       md_mcp_stdio_write, ctx);
            }

            start = newline + 1;
        }

        /* Shift remaining partial line to front of buffer */
        size_t remaining = buf_len - (size_t)(start - buf);
        if (remaining > 0 && start != buf) {
            memmove(buf, start, remaining);
        }
        buf_len = remaining;
    }

    free(buf);
    return 0;
}

void md_mcp_stdio_shutdown(MdMcpStdio *ctx)
{
    if (ctx) atomic_store(&ctx->shutdown, true);
}

void md_mcp_stdio_destroy(MdMcpStdio *ctx)
{
    if (!ctx) return;
    fcntl(ctx->out_fd, F_SETFL, ctx->out_flags);
    pthread_mutex_destroy(&ctx->write_mu);
    free(ctx);
}
