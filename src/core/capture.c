/*
 * metadesk — capture.c
 * Platform-agnostic capture convenience API.
 *
 * Delegates to the backend vtable returned by md_capture_backend_create().
 * The actual platform implementation lives in capture_pipewire.c,
 * capture_screencapturekit.m, or capture_dxgi.cpp.
 */
#include "capture.h"

#include <stdlib.h>
#include <string.h>

/* ── Public convenience API ──────────────────────────────────── */

MdCaptureCtx *md_capture_create(const MdCaptureConfig *cfg) {
    const MdCaptureBackend *vtable = md_capture_backend_create();
    if (!vtable) return NULL;

    MdCaptureCtx *ctx = calloc(1, sizeof(MdCaptureCtx));
    if (!ctx) return NULL;

    ctx->vtable = vtable;

    /* Apply config or defaults */
    if (cfg) {
        ctx->config = *cfg;
    } else {
        ctx->config.target_fps  = 60;
        ctx->config.show_cursor = true;
    }

    if (ctx->vtable->init(ctx, &ctx->config) != 0) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

int md_capture_start(MdCaptureCtx *ctx) {
    if (!ctx || !ctx->vtable || !ctx->vtable->start) return -1;
    return ctx->vtable->start(ctx);
}

int md_capture_get_frame(MdCaptureCtx *ctx, MdFrame *out) {
    if (!ctx || !ctx->vtable || !ctx->vtable->get_frame) return -1;
    return ctx->vtable->get_frame(ctx, out);
}

void md_capture_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    if (!ctx || !ctx->vtable || !ctx->vtable->release_frame) return;
    ctx->vtable->release_frame(ctx, frame);
}

bool md_capture_is_active(const MdCaptureCtx *ctx) {
    return ctx ? ctx->active : false;
}

int md_capture_get_size(const MdCaptureCtx *ctx, uint32_t *width, uint32_t *height) {
    if (!ctx || !width || !height) return -1;
    if (ctx->width == 0 || ctx->height == 0) return -1;
    *width  = ctx->width;
    *height = ctx->height;
    return 0;
}

void md_capture_stop(MdCaptureCtx *ctx) {
    if (!ctx || !ctx->vtable || !ctx->vtable->stop) return;
    ctx->vtable->stop(ctx);
}

void md_capture_destroy(MdCaptureCtx *ctx) {
    if (!ctx) return;
    md_capture_stop(ctx);
    if (ctx->vtable && ctx->vtable->destroy)
        ctx->vtable->destroy(ctx);
    free(ctx);
}
