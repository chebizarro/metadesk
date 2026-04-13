/*
 * metadesk — capture.c
 * PipeWire screen capture via ScreenCast portal.
 *
 * Flow:
 *   1. pw_init() — initialize PipeWire client library
 *   2. Portal negotiation (D-Bus) — deferred to portal_open()
 *   3. pw_stream_new() — create a PipeWire stream
 *   4. pw_stream_connect() — connect using fd+node from portal
 *   5. on_process() callback — invoked per frame by PipeWire
 *   6. Map buffer (SHM or DMA-BUF), invoke user callback
 *
 * PipeWire runs its own event loop (pw_main_loop) on a dedicated
 * thread. All PipeWire calls happen on that thread; the user's
 * frame callback is also invoked from it.
 *
 * Milestone 1.1 target: single frame to disk, DMA-BUF path
 * confirmed on T7610/P40.
 */
#include "capture.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <spa/buffer/buffer.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

struct MdCapture {
    MdCaptureConfig     config;
    MdFrameCallback     callback;
    void               *userdata;

    /* PipeWire state */
    struct pw_main_loop    *loop;
    struct pw_context      *pw_ctx;
    struct pw_core         *core;
    struct pw_stream       *stream;
    struct spa_hook         stream_listener;

    /* Thread running pw_main_loop */
    pthread_t               thread;
    bool                    thread_started;

    /* Negotiated format */
    uint32_t                width;
    uint32_t                height;
    uint32_t                stride;
    uint32_t                format; /* SPA video format */

    /* State */
    volatile bool           active;
    atomic_uint_least32_t   seq;
};

/* ── PipeWire stream events ──────────────────────────────────── */

static void on_param_changed(void *userdata, uint32_t id,
                             const struct spa_pod *param) {
    MdCapture *ctx = userdata;
    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;

    ctx->width  = info.size.width;
    ctx->height = info.size.height;
    ctx->format = info.format;

    /* Calculate stride from format and width */
    uint32_t bpp = 4; /* most formats are 4 bytes/pixel (BGRx, RGBx, etc.) */
    ctx->stride = ctx->width * bpp;

    /* Tell PipeWire what buffer types we support.
     * Prefer DMA-BUF if configured, always support SHM as fallback. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    uint32_t data_type = (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
    if (ctx->config.prefer_dmabuf)
        data_type |= (1 << SPA_DATA_DmaBuf);

    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(data_type));

    pw_stream_update_params(ctx->stream, params, 1);
}

static void on_process(void *userdata) {
    MdCapture *ctx = userdata;
    struct pw_buffer *pw_buf;

    pw_buf = pw_stream_dequeue_buffer(ctx->stream);
    if (!pw_buf)
        return;

    struct spa_buffer *spa_buf = pw_buf->buffer;
    if (!spa_buf || spa_buf->n_datas == 0)
        goto done;

    struct spa_data *d = &spa_buf->datas[0];
    if (!d->chunk || d->chunk->size == 0)
        goto done;

    MdFrame frame = {
        .width        = ctx->width,
        .height       = ctx->height,
        .stride       = ctx->stride,
        .format       = ctx->format,
        .seq          = atomic_fetch_add_explicit(&ctx->seq, 1, memory_order_relaxed),
        .timestamp_ns = pw_buf->time,
    };

    if (d->type == SPA_DATA_DmaBuf) {
        frame.buf_type  = MD_BUF_DMABUF;
        frame.dmabuf_fd = d->fd;
        frame.data      = NULL;
        frame.data_size = d->maxsize;
    } else {
        /* SHM: MemFd or MemPtr */
        frame.buf_type  = MD_BUF_SHM;
        frame.dmabuf_fd = -1;
        frame.data      = d->data;
        frame.data_size = d->chunk->size;
    }

    if (ctx->callback)
        ctx->callback(&frame, ctx->userdata);

done:
    pw_stream_queue_buffer(ctx->stream, pw_buf);
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
    MdCapture *ctx = userdata;
    (void)old;
    (void)error;

    switch (state) {
    case PW_STREAM_STATE_STREAMING:
        ctx->active = true;
        break;
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_ERROR:
    case PW_STREAM_STATE_UNCONNECTED:
        ctx->active = false;
        break;
    default:
        break;
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process       = on_process,
    .state_changed = on_state_changed,
};

/* ── PipeWire thread ─────────────────────────────────────────── */

static void *pw_thread_func(void *arg) {
    MdCapture *ctx = arg;
    pw_main_loop_run(ctx->loop);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

MdCapture *md_capture_create(const MdCaptureConfig *cfg) {
    /* Initialize PipeWire (idempotent, safe to call multiple times) */
    pw_init(NULL, NULL);

    MdCapture *ctx = calloc(1, sizeof(MdCapture));
    if (!ctx) return NULL;

    if (cfg)
        ctx->config = *cfg;
    else {
        ctx->config.target_fps   = 60;
        ctx->config.prefer_dmabuf = true;
        ctx->config.show_cursor  = true;
    }

    ctx->loop = pw_main_loop_new(NULL);
    if (!ctx->loop) {
        free(ctx);
        return NULL;
    }

    ctx->pw_ctx = pw_context_new(pw_main_loop_get_loop(ctx->loop), NULL, 0);
    if (!ctx->pw_ctx) {
        pw_main_loop_destroy(ctx->loop);
        free(ctx);
        return NULL;
    }

    return ctx;
}

int md_capture_start(MdCapture *ctx, MdFrameCallback cb, void *userdata) {
    if (!ctx || !cb) return -1;

    ctx->callback = cb;
    ctx->userdata = userdata;

    /* Connect to PipeWire daemon.
     *
     * In Phase 1, we connect directly to the PipeWire daemon.
     * The ScreenCast portal flow (D-Bus → CreateSession → SelectSources → Start)
     * would give us a PipeWire fd and node_id. For now, we accept a node_id
     * of PW_ID_ANY to capture any available screen source.
     *
     * TODO: Implement full xdg-desktop-portal ScreenCast flow:
     *   1. D-Bus call: org.freedesktop.portal.ScreenCast.CreateSession()
     *   2. D-Bus call: SelectSources(session, {types: MONITOR, cursor_mode: ...})
     *   3. D-Bus call: Start(session)  — user approves in portal dialog
     *   4. Portal returns pipewire_fd and streams[{node_id, ...}]
     *   5. Use pw_context_connect_fd(ctx->pw_ctx, pipewire_fd, ...)
     */
    ctx->core = pw_context_connect(ctx->pw_ctx, NULL, 0);
    if (!ctx->core) return -1;

    /* Create capture stream */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,    "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,    "Screen",
        NULL);

    ctx->stream = pw_stream_new(ctx->core, "metadesk-capture", props);
    if (!ctx->stream) return -1;

    /* Listen for stream events */
    pw_stream_add_listener(ctx->stream, &ctx->stream_listener,
                           &stream_events, ctx);

    /* Build format negotiation: request raw video, any format/size */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size,   SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1920, 1080),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(4096, 4096)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(ctx->config.target_fps ? ctx->config.target_fps : 60, 1),
            &SPA_FRACTION(0, 1),
            &SPA_FRACTION(120, 1)));

    /* Connect stream — PW_ID_ANY for now; will use portal node_id later */
    int ret = pw_stream_connect(ctx->stream,
        PW_DIRECTION_INPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1);
    if (ret < 0) return -1;

    /* Start PipeWire loop on its own thread */
    if (pthread_create(&ctx->thread, NULL, pw_thread_func, ctx) != 0)
        return -1;
    ctx->thread_started = true;

    return 0;
}

void md_capture_stop(MdCapture *ctx) {
    if (!ctx) return;

    if (ctx->loop)
        pw_main_loop_quit(ctx->loop);

    if (ctx->thread_started) {
        pthread_join(ctx->thread, NULL);
        ctx->thread_started = false;
    }

    ctx->active = false;
}

bool md_capture_is_active(const MdCapture *ctx) {
    return ctx ? ctx->active : false;
}

int md_capture_get_size(const MdCapture *ctx, uint32_t *width, uint32_t *height) {
    if (!ctx || !width || !height) return -1;
    if (ctx->width == 0 || ctx->height == 0) return -1;
    *width  = ctx->width;
    *height = ctx->height;
    return 0;
}

void md_capture_destroy(MdCapture *ctx) {
    if (!ctx) return;

    md_capture_stop(ctx);

    if (ctx->stream) {
        pw_stream_disconnect(ctx->stream);
        pw_stream_destroy(ctx->stream);
    }
    if (ctx->core)
        pw_core_disconnect(ctx->core);
    if (ctx->pw_ctx)
        pw_context_destroy(ctx->pw_ctx);
    if (ctx->loop)
        pw_main_loop_destroy(ctx->loop);

    free(ctx);
}
