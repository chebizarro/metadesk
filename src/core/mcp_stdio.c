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
#include "io_util.h"
#include <sys/stat.h>
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
    bool            out_is_socket;
    int             write_timeout_ms;
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

    /* Responses and notifications share a budget, including the newline. */
    int64_t deadline = md_now_ms() + ctx->write_timeout_ms;
    size_t total = 0;
    int rc = md_fd_write_deadline(ctx->out_fd, json, len, deadline,
                                  ctx->out_is_socket, &total);
    if (rc == 0)
        rc = md_fd_write_deadline(ctx->out_fd, "\n", 1, deadline,
                                  ctx->out_is_socket, &total);
    if (rc < 0) {
        /* Do not append another JSON message to a truncated frame. Input
         * dispatch remains live, but this output channel is no longer usable. */
        if (total > 0
#ifdef _WIN32
            || errno == ETIMEDOUT /* CancelSynchronousIo may have committed unknown bytes. */
#endif
        ) ctx->output_failed = true;
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
    return md_mcp_stdio_create_ex(server, in_fd, out_fd, MD_MCP_STDIO_WRITE_TIMEOUT_MS);
}

MdMcpStdio *md_mcp_stdio_create_ex(MdMcpServer *server, int in_fd, int out_fd,
                                  int write_timeout_ms)
{
    struct stat st;
    if (!server || fstat(out_fd, &st) < 0) return NULL;

    MdMcpStdio *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

#ifdef _WIN32
    /* The public stdio API takes CRT descriptors, never Winsock handles. */
    ctx->out_is_socket = false;
#else
    ctx->out_is_socket = S_ISSOCK(st.st_mode);
#endif
    ctx->write_timeout_ms = write_timeout_ms > 0
        ? write_timeout_ms : MD_MCP_STDIO_WRITE_TIMEOUT_MS;

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

        /* Blocking stdin must not strand a shutdown request. Do not change
         * descriptor flags: dup'd stdin may share its open-file description
         * with the launcher. This transport is the sole reader. */
        ssize_t n = md_fd_read_timeout(ctx->in_fd, buf + buf_len,
                                       buf_cap - buf_len - 1, 100);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Nonblocking launchers can race readiness; retry the poll. */
            continue;
        }
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
    pthread_mutex_destroy(&ctx->write_mu);
    free(ctx);
}
