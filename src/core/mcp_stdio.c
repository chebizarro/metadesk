/*
 * metadesk — mcp_stdio.c
 * stdio transport for MCP server.
 *
 * Reads newline-delimited JSON-RPC messages from in_fd, dispatches
 * to the MCP server, writes responses to out_fd.
 */
#include "mcp_stdio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

/* ── Initial line buffer size ────────────────────────────────── */

#define INITIAL_BUF_SIZE 4096

/* ── stdio context ───────────────────────────────────────────── */

struct MdMcpStdio {
    MdMcpServer    *server;
    int             in_fd;
    int             out_fd;
    volatile bool   shutdown;
    pthread_mutex_t write_mu;  /* serialize writes to out_fd */
};

/* ── Write callback (called by MCP server to send responses) ── */

static int stdio_write(const char *json, size_t len, void *userdata)
{
    MdMcpStdio *ctx = (MdMcpStdio *)userdata;
    if (!ctx || ctx->shutdown) return -1;

    pthread_mutex_lock(&ctx->write_mu);

    /* Write JSON + newline atomically */
    ssize_t written = 0;
    size_t total = 0;
    while (total < len) {
        written = write(ctx->out_fd, json + total, len - total);
        if (written <= 0) {
            pthread_mutex_unlock(&ctx->write_mu);
            return -1;
        }
        total += (size_t)written;
    }

    /* Write trailing newline */
    char nl = '\n';
    if (write(ctx->out_fd, &nl, 1) != 1) {
        pthread_mutex_unlock(&ctx->write_mu);
        return -1;
    }

    pthread_mutex_unlock(&ctx->write_mu);
    return 0;
}

/* ── Lifecycle ───────────────────────────────────────────────── */

MdMcpStdio *md_mcp_stdio_create(MdMcpServer *server, int in_fd, int out_fd)
{
    if (!server) return NULL;

    MdMcpStdio *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->server = server;
    ctx->in_fd = in_fd;
    ctx->out_fd = out_fd;
    ctx->shutdown = false;
    pthread_mutex_init(&ctx->write_mu, NULL);

    return ctx;
}

MdMcpWriteFn md_mcp_stdio_get_write_fn(MdMcpStdio *stdio_ctx)
{
    (void)stdio_ctx;
    return stdio_write;
}

void *md_mcp_stdio_get_write_userdata(MdMcpStdio *stdio_ctx)
{
    return stdio_ctx;
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

    while (!ctx->shutdown) {
        /* Read a chunk */
        if (buf_len + 1 >= buf_cap) {
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
                md_mcp_server_handle_message(ctx->server, start, line_len);
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
    if (ctx) ctx->shutdown = true;
}

void md_mcp_stdio_destroy(MdMcpStdio *ctx)
{
    if (!ctx) return;
    pthread_mutex_destroy(&ctx->write_mu);
    free(ctx);
}
