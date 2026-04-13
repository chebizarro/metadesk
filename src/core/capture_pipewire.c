/*
 * metadesk — capture_pipewire.c
 * Linux capture backend: PipeWire via ScreenCast portal.
 *
 * Flow:
 *   1. pw_init() — initialize PipeWire client library
 *   2. Portal negotiation (D-Bus) — deferred to portal_open()
 *   3. pw_stream_new() — create a PipeWire stream
 *   4. pw_stream_connect() — connect using fd+node from portal
 *   5. on_process() callback — invoked per frame by PipeWire
 *   6. Map buffer (SHM or DMA-BUF), store in ring for get_frame()
 *
 * PipeWire runs its own event loop (pw_main_loop) on a dedicated
 * thread. Frames are handed to the consumer via get_frame /
 * release_frame synchronised with a mutex + condition variable.
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

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
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
    uint32_t                stride;
    uint32_t                spa_format;

    /* Frame delivery (pw thread → consumer) */
    pthread_mutex_t         frame_lock;
    pthread_cond_t          frame_cond;
    MdFrame                 pending_frame;
    bool                    frame_ready;
    struct pw_buffer       *held_pw_buf; /* currently held by consumer */

    /* DMA-BUF preference */
    bool                    prefer_dmabuf;

    /* Sequence counter */
    atomic_uint_least32_t   seq;

    /* Back-pointer to ctx for callbacks */
    MdCaptureCtx           *ctx;
} PipewireState;

/* ── PipeWire stream events ──────────────────────────────────── */

static void on_param_changed(void *userdata, uint32_t id,
                             const struct spa_pod *param) {
    PipewireState *pw = userdata;
    MdCaptureCtx  *ctx = pw->ctx;

    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;

    ctx->width     = info.size.width;
    ctx->height    = info.size.height;
    pw->spa_format = info.format;

    /* Calculate stride from format and width */
    uint32_t bpp = 4; /* most formats are 4 bytes/pixel (BGRx, RGBx, etc.) */
    pw->stride = ctx->width * bpp;

    /* Tell PipeWire what buffer types we support.
     * Prefer DMA-BUF if configured, always support SHM as fallback. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    uint32_t data_type = (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
    if (pw->prefer_dmabuf)
        data_type |= (1 << SPA_DATA_DmaBuf);

    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(data_type));

    pw_stream_update_params(pw->stream, params, 1);
}

/* Map SPA video format to our capture pixel format. */
static MdCapturePixFmt spa_to_capture_fmt(uint32_t spa_fmt) {
    /* SPA_VIDEO_FORMAT values are enums; map the common ones */
    switch (spa_fmt) {
    case SPA_VIDEO_FORMAT_RGBA:  return MD_PIX_CAPTURE_RGBA;
    case SPA_VIDEO_FORMAT_BGRA:  return MD_PIX_CAPTURE_BGRA;
    case SPA_VIDEO_FORMAT_RGBx:  return MD_PIX_CAPTURE_RGBX;
    default:                     return MD_PIX_CAPTURE_BGRX;
    }
}

static void on_process(void *userdata) {
    PipewireState *pw = userdata;
    MdCaptureCtx  *ctx = pw->ctx;

    struct pw_buffer *pw_buf = pw_stream_dequeue_buffer(pw->stream);
    if (!pw_buf) return;

    struct spa_buffer *spa_buf = pw_buf->buffer;
    if (!spa_buf || spa_buf->n_datas == 0)
        goto requeue;

    struct spa_data *d = &spa_buf->datas[0];
    if (!d->chunk || d->chunk->size == 0)
        goto requeue;

    /* Build MdFrame */
    MdFrame frame = {
        .width        = ctx->width,
        .height       = ctx->height,
        .stride       = pw->stride,
        .format       = spa_to_capture_fmt(pw->spa_format),
        .seq          = atomic_fetch_add_explicit(&pw->seq, 1, memory_order_relaxed),
        .timestamp_ns = pw_buf->time,
    };

    if (d->type == SPA_DATA_DmaBuf) {
        frame.buf_type   = MD_BUF_DMABUF;
        frame.dmabuf_fd  = d->fd;
        frame.data       = NULL;
        frame.gpu_handle = NULL;
        frame.data_size  = d->maxsize;
    } else {
        frame.buf_type   = MD_BUF_CPU;
        frame.dmabuf_fd  = -1;
        frame.data       = d->data;
        frame.gpu_handle = NULL;
        frame.data_size  = d->chunk->size;
    }

    /* Hand frame to consumer via lock + cond */
    pthread_mutex_lock(&pw->frame_lock);
    pw->pending_frame = frame;
    pw->held_pw_buf   = pw_buf;
    pw->frame_ready   = true;
    pthread_cond_signal(&pw->frame_cond);
    pthread_mutex_unlock(&pw->frame_lock);
    return; /* don't requeue — consumer calls release_frame */

requeue:
    pw_stream_queue_buffer(pw->stream, pw_buf);
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
    PipewireState *pw = userdata;
    MdCaptureCtx  *ctx = pw->ctx;
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
        /* Wake any blocked get_frame */
        pthread_mutex_lock(&pw->frame_lock);
        pthread_cond_signal(&pw->frame_cond);
        pthread_mutex_unlock(&pw->frame_lock);
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
    PipewireState *pw = arg;
    pw_main_loop_run(pw->loop);
    return NULL;
}

/* ── Backend vtable implementation ───────────────────────────── */

static int pw_init(MdCaptureCtx *ctx, const MdCaptureConfig *cfg) {
    /* Initialize PipeWire (idempotent, safe to call multiple times) */
    pw_init(NULL, NULL);

    PipewireState *pw = calloc(1, sizeof(PipewireState));
    if (!pw) return -1;

    pw->ctx = ctx;
    pw->prefer_dmabuf = false; /* SHM default; backends may expose knob later */
    pthread_mutex_init(&pw->frame_lock, NULL);
    pthread_cond_init(&pw->frame_cond, NULL);

    pw->loop = pw_main_loop_new(NULL);
    if (!pw->loop) goto fail;

    pw->pw_ctx = pw_context_new(pw_main_loop_get_loop(pw->loop), NULL, 0);
    if (!pw->pw_ctx) goto fail;

    ctx->backend_data = pw;
    return 0;

fail:
    if (pw->pw_ctx) pw_context_destroy(pw->pw_ctx);
    if (pw->loop) pw_main_loop_destroy(pw->loop);
    pthread_mutex_destroy(&pw->frame_lock);
    pthread_cond_destroy(&pw->frame_cond);
    free(pw);
    return -1;
}

static int pw_start(MdCaptureCtx *ctx) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return -1;

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
     *   5. Use pw_context_connect_fd(pw->pw_ctx, pipewire_fd, ...)
     */
    pw->core = pw_context_connect(pw->pw_ctx, NULL, 0);
    if (!pw->core) return -1;

    /* Create capture stream */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,    "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE,    "Screen",
        NULL);

    pw->stream = pw_stream_new(pw->core, "metadesk-capture", props);
    if (!pw->stream) return -1;

    /* Listen for stream events */
    pw_stream_add_listener(pw->stream, &pw->stream_listener,
                           &stream_events, pw);

    /* Build format negotiation: request raw video, any format/size */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    uint32_t target_fps = ctx->config.target_fps ? ctx->config.target_fps : 60;

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
            &SPA_FRACTION(target_fps, 1),
            &SPA_FRACTION(0, 1),
            &SPA_FRACTION(120, 1)));

    /* Connect stream — PW_ID_ANY for now; will use portal node_id later */
    int ret = pw_stream_connect(pw->stream,
        PW_DIRECTION_INPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1);
    if (ret < 0) return -1;

    /* Start PipeWire loop on its own thread */
    if (pthread_create(&pw->thread, NULL, pw_thread_func, pw) != 0)
        return -1;
    pw->thread_started = true;

    return 0;
}

static int pw_get_frame(MdCaptureCtx *ctx, MdFrame *out) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return -1;

    pthread_mutex_lock(&pw->frame_lock);

    /* Wait for a frame from the PipeWire thread */
    while (!pw->frame_ready && ctx->active) {
        /* Use a timed wait to periodically check active flag */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L; /* 100ms timeout */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec += 1;
        }
        pthread_cond_timedwait(&pw->frame_cond, &pw->frame_lock, &ts);
    }

    if (!pw->frame_ready) {
        pthread_mutex_unlock(&pw->frame_lock);
        return -1; /* capture stopped */
    }

    *out = pw->pending_frame;
    pw->frame_ready = false;
    pthread_mutex_unlock(&pw->frame_lock);
    return 0;
}

static void pw_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    PipewireState *pw = ctx->backend_data;
    (void)frame;
    if (!pw) return;

    pthread_mutex_lock(&pw->frame_lock);
    if (pw->held_pw_buf) {
        pw_stream_queue_buffer(pw->stream, pw->held_pw_buf);
        pw->held_pw_buf = NULL;
    }
    pthread_mutex_unlock(&pw->frame_lock);
}

static void pw_stop(MdCaptureCtx *ctx) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return;

    if (pw->loop)
        pw_main_loop_quit(pw->loop);

    if (pw->thread_started) {
        pthread_join(pw->thread, NULL);
        pw->thread_started = false;
    }

    ctx->active = false;
}

static void pw_destroy(MdCaptureCtx *ctx) {
    PipewireState *pw = ctx->backend_data;
    if (!pw) return;

    if (pw->stream) {
        pw_stream_disconnect(pw->stream);
        pw_stream_destroy(pw->stream);
    }
    if (pw->core)
        pw_core_disconnect(pw->core);
    if (pw->pw_ctx)
        pw_context_destroy(pw->pw_ctx);
    if (pw->loop)
        pw_main_loop_destroy(pw->loop);

    pthread_mutex_destroy(&pw->frame_lock);
    pthread_cond_destroy(&pw->frame_cond);

    free(pw);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdCaptureBackend pipewire_backend = {
    .init          = pw_init,
    .start         = pw_start,
    .get_frame     = pw_get_frame,
    .release_frame = pw_release_frame,
    .stop          = pw_stop,
    .destroy       = pw_destroy,
};

const MdCaptureBackend *md_capture_backend_create(void) {
    return &pipewire_backend;
}
