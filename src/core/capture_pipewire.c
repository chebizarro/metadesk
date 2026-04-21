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

#include <dbus/dbus.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

/* ══════════════════════════════════════════════════════════════
 * Portal ScreenCast D-Bus flow (xdg-desktop-portal)
 *
 * On Wayland compositors, PipeWire screen capture requires explicit
 * user consent via the xdg-desktop-portal ScreenCast interface.
 * The protocol is:
 *   1. CreateSession()    → session handle
 *   2. SelectSources()    → configure capture source type
 *   3. Start()            → user approves in portal dialog
 *   4. OpenPipeWireRemote → PipeWire fd for pw_context_connect_fd()
 *
 * Each method returns a Request object path; a Response signal is
 * emitted asynchronously when the operation completes.  We use the
 * handle_token approach to predict the Request path and subscribe
 * to the signal before calling the method (avoids race condition).
 * ══════════════════════════════════════════════════════════════ */

#define PORTAL_BUS_NAME   "org.freedesktop.portal.Desktop"
#define PORTAL_OBJ_PATH   "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST  "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST     "org.freedesktop.portal.Request"

/* Portal source types (bitmask) */
#define PORTAL_SOURCE_MONITOR  1u
#define PORTAL_SOURCE_WINDOW   2u

/* Portal cursor modes (bitmask) */
#define PORTAL_CURSOR_HIDDEN   1u
#define PORTAL_CURSOR_EMBEDDED 2u
#define PORTAL_CURSOR_METADATA 4u

typedef struct {
    DBusConnection *bus;
    char           *session_handle;
    int             pw_fd;       /* PipeWire fd from portal       */
    uint32_t        node_id;     /* stream node ID from portal    */
    int             token_seq;   /* counter for unique tokens     */
    char            sender[128]; /* munged bus unique name         */
} PortalScreencast;

/* Munge a D-Bus unique name (":1.234" → "1_234") for request paths */
static void munge_sender(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = (name[0] == ':') ? 1 : 0; name[i] && j < out_sz - 1; i++)
        out[j++] = (name[i] == '.') ? '_' : name[i];
    out[j] = '\0';
}

/* ── D-Bus a{sv} dict helpers ────────────────────────────────── */

static void dict_append_sv_string(DBusMessageIter *dict,
                                  const char *key, const char *val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

static void dict_append_sv_uint32(DBusMessageIter *dict,
                                  const char *key, uint32_t val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

static void dict_append_sv_bool(DBusMessageIter *dict,
                                const char *key, dbus_bool_t val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

/* Look up a string value in an a{sv} dict iter positioned at the array */
static const char *dict_lookup_string(DBusMessageIter *dict_iter,
                                      const char *key) {
    DBusMessageIter arr;
    dbus_message_iter_recurse(dict_iter, &arr);

    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&arr, &entry);

        const char *k;
        dbus_message_iter_get_basic(&entry, &k);

        if (strcmp(k, key) == 0) {
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                const char *v;
                dbus_message_iter_get_basic(&variant, &v);
                return v;
            }
            return NULL;
        }
        dbus_message_iter_next(&arr);
    }
    return NULL;
}

/* ── Portal call + response pattern ──────────────────────────── */

/*
 * Make a unique handle_token string and compute the expected request
 * object path.  The caller must free the returned token.
 */
static char *portal_make_token(PortalScreencast *p, char *path_out, size_t path_sz) {
    char token[64];
    snprintf(token, sizeof(token), "metadesk%d", p->token_seq++);

    snprintf(path_out, path_sz,
             "/org/freedesktop/portal/desktop/request/%s/%s",
             p->sender, token);

    return strdup(token);
}

/*
 * Wait for a Response signal on @request_path.
 * Returns the response code (0 = success) and stores the signal
 * message in *out_msg (caller must unref).  Returns UINT32_MAX on
 * timeout or error.
 */
static uint32_t portal_wait_response(PortalScreencast *p,
                                     const char *request_path,
                                     int timeout_ms,
                                     DBusMessage **out_msg) {
    char rule[1024];
    snprintf(rule, sizeof(rule),
             "type='signal',sender='" PORTAL_BUS_NAME "',"
             "interface='" PORTAL_REQUEST "',"
             "member='Response',path='%s'", request_path);

    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(p->bus, rule, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "portal: add_match failed: %s\n", err.message);
        dbus_error_free(&err);
        return UINT32_MAX;
    }

    uint32_t result = UINT32_MAX;
    int polls = timeout_ms / 100;
    if (polls < 1) polls = 1;

    for (int i = 0; i < polls; i++) {
        dbus_connection_read_write(p->bus, 100);
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(p->bus)) != NULL) {
            if (dbus_message_is_signal(msg, PORTAL_REQUEST, "Response")
                && strcmp(dbus_message_get_path(msg), request_path) == 0) {
                /* Parse response code (first arg) */
                DBusMessageIter args;
                dbus_message_iter_init(msg, &args);
                dbus_message_iter_get_basic(&args, &result);

                if (out_msg)
                    *out_msg = msg;  /* caller takes ownership */
                else
                    dbus_message_unref(msg);

                goto done;
            }
            dbus_message_unref(msg);
        }
    }
    fprintf(stderr, "portal: timeout waiting for Response on %s\n",
            request_path);

done:
    dbus_bus_remove_match(p->bus, rule, NULL);
    return result;
}

/*
 * portal_screencast_open:
 * Execute the full ScreenCast portal flow and return a PipeWire fd
 * and stream node_id.  Returns 0 on success, -1 on failure.
 *
 * On success, portal->pw_fd and portal->node_id are populated.
 * The caller must eventually call portal_screencast_close().
 */
static int portal_screencast_open(PortalScreencast *portal) {
    memset(portal, 0, sizeof(*portal));
    portal->pw_fd = -1;

    /* Connect to session bus */
    DBusError err;
    dbus_error_init(&err);
    portal->bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!portal->bus) {
        fprintf(stderr, "portal: cannot connect to session bus: %s\n",
                err.message);
        dbus_error_free(&err);
        return -1;
    }

    /* Munge our unique name for request object paths */
    const char *unique = dbus_bus_get_unique_name(portal->bus);
    if (!unique) goto fail;
    munge_sender(unique, portal->sender, sizeof(portal->sender));

    /* ── Step 1: CreateSession ──────────────────────────────── */
    {
        char req_path[512];
        char *token = portal_make_token(portal, req_path, sizeof(req_path));

        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "CreateSession");
        if (!msg) { free(token); goto fail; }

        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_sv_string(&dict, "handle_token", token);
        dict_append_sv_string(&dict, "session_handle_token", "metadesk_session");
        dbus_message_iter_close_container(&args, &dict);

        /* Fire and forget — we wait for the signal */
        dbus_connection_send(portal->bus, msg, NULL);
        dbus_connection_flush(portal->bus);
        dbus_message_unref(msg);
        free(token);

        DBusMessage *resp = NULL;
        uint32_t rc = portal_wait_response(portal, req_path, 5000, &resp);
        if (rc != 0 || !resp) {
            fprintf(stderr, "portal: CreateSession failed (rc=%u)\n", rc);
            if (resp) dbus_message_unref(resp);
            goto fail;
        }

        /* Extract session_handle from results dict */
        DBusMessageIter resp_args;
        dbus_message_iter_init(resp, &resp_args);
        dbus_message_iter_next(&resp_args); /* skip response code */

        const char *sh = dict_lookup_string(&resp_args, "session_handle");
        if (!sh) {
            fprintf(stderr, "portal: no session_handle in CreateSession response\n");
            dbus_message_unref(resp);
            goto fail;
        }
        portal->session_handle = strdup(sh);
        dbus_message_unref(resp);

        fprintf(stderr, "portal: session created: %s\n", portal->session_handle);
    }

    /* ── Step 2: SelectSources ─────────────────────────────── */
    {
        char req_path[512];
        char *token = portal_make_token(portal, req_path, sizeof(req_path));

        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "SelectSources");
        if (!msg) { free(token); goto fail; }

        const char *sh = portal->session_handle;
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_sv_string(&dict, "handle_token", token);
        dict_append_sv_uint32(&dict, "types", PORTAL_SOURCE_MONITOR);
        dict_append_sv_uint32(&dict, "cursor_mode", PORTAL_CURSOR_EMBEDDED);
        dict_append_sv_bool(&dict, "persist_mode", FALSE);
        dbus_message_iter_close_container(&args, &dict);

        dbus_connection_send(portal->bus, msg, NULL);
        dbus_connection_flush(portal->bus);
        dbus_message_unref(msg);
        free(token);

        uint32_t rc = portal_wait_response(portal, req_path, 5000, NULL);
        if (rc != 0) {
            fprintf(stderr, "portal: SelectSources failed (rc=%u)\n", rc);
            goto fail;
        }
        fprintf(stderr, "portal: sources selected (monitor, cursor embedded)\n");
    }

    /* ── Step 3: Start (shows user consent dialog) ────────── */
    {
        char req_path[512];
        char *token = portal_make_token(portal, req_path, sizeof(req_path));

        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "Start");
        if (!msg) { free(token); goto fail; }

        const char *sh = portal->session_handle;
        const char *parent = ""; /* no parent window */
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dict_append_sv_string(&dict, "handle_token", token);
        dbus_message_iter_close_container(&args, &dict);

        dbus_connection_send(portal->bus, msg, NULL);
        dbus_connection_flush(portal->bus);
        dbus_message_unref(msg);
        free(token);

        /* 60 s timeout — user needs time to interact with the dialog */
        DBusMessage *resp = NULL;
        uint32_t rc = portal_wait_response(portal, req_path, 60000, &resp);
        if (rc != 0 || !resp) {
            fprintf(stderr, "portal: Start failed or user cancelled (rc=%u)\n", rc);
            if (resp) dbus_message_unref(resp);
            goto fail;
        }

        /* Extract streams array from results.
         * Response is: (u response, a{sv} results)
         * results["streams"] is a(ua{sv}) — array of (node_id, props) */
        DBusMessageIter resp_args;
        dbus_message_iter_init(resp, &resp_args);
        dbus_message_iter_next(&resp_args); /* skip response code */

        /* Iterate the results dict looking for "streams" */
        DBusMessageIter results_arr;
        dbus_message_iter_recurse(&resp_args, &results_arr);

        bool found_streams = false;
        while (dbus_message_iter_get_arg_type(&results_arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry, variant;
            dbus_message_iter_recurse(&results_arr, &entry);

            const char *key;
            dbus_message_iter_get_basic(&entry, &key);

            if (strcmp(key, "streams") == 0) {
                dbus_message_iter_next(&entry);
                dbus_message_iter_recurse(&entry, &variant);

                /* variant contains a(ua{sv}) */
                DBusMessageIter streams_arr, stream_struct;
                dbus_message_iter_recurse(&variant, &streams_arr);

                if (dbus_message_iter_get_arg_type(&streams_arr) == DBUS_TYPE_STRUCT) {
                    dbus_message_iter_recurse(&streams_arr, &stream_struct);
                    dbus_message_iter_get_basic(&stream_struct, &portal->node_id);
                    found_streams = true;
                    fprintf(stderr, "portal: stream node_id=%u\n", portal->node_id);
                }
                break;
            }
            dbus_message_iter_next(&results_arr);
        }
        dbus_message_unref(resp);

        if (!found_streams) {
            fprintf(stderr, "portal: no streams in Start response\n");
            goto fail;
        }
    }

    /* ── Step 4: OpenPipeWireRemote (get fd) ──────────────── */
    {
        DBusMessage *msg = dbus_message_new_method_call(
            PORTAL_BUS_NAME, PORTAL_OBJ_PATH,
            PORTAL_SCREENCAST, "OpenPipeWireRemote");
        if (!msg) goto fail;

        const char *sh = portal->session_handle;
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(msg, &args);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sh);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage *reply = dbus_connection_send_with_reply_and_block(
            portal->bus, msg, 5000, &err);
        dbus_message_unref(msg);

        if (!reply || dbus_error_is_set(&err)) {
            fprintf(stderr, "portal: OpenPipeWireRemote failed: %s\n",
                    err.message);
            dbus_error_free(&err);
            goto fail;
        }

        /* Reply contains a Unix FD */
        int fd = -1;
        if (!dbus_message_get_args(reply, &err,
                                  DBUS_TYPE_UNIX_FD, &fd,
                                  DBUS_TYPE_INVALID)) {
            fprintf(stderr, "portal: cannot extract fd: %s\n", err.message);
            dbus_error_free(&err);
            dbus_message_unref(reply);
            goto fail;
        }
        dbus_message_unref(reply);

        portal->pw_fd = fd;
        fprintf(stderr, "portal: PipeWire fd=%d, node_id=%u\n",
                portal->pw_fd, portal->node_id);
    }

    return 0;

fail:
    if (portal->session_handle) { free(portal->session_handle); portal->session_handle = NULL; }
    if (portal->bus) { dbus_connection_unref(portal->bus); portal->bus = NULL; }
    return -1;
}

static void portal_screencast_close(PortalScreencast *portal) {
    if (portal->pw_fd >= 0) {
        close(portal->pw_fd);
        portal->pw_fd = -1;
    }
    free(portal->session_handle);
    portal->session_handle = NULL;
    if (portal->bus) {
        dbus_connection_unref(portal->bus);
        portal->bus = NULL;
    }
}

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

    /* Portal state (cleaned up on destroy) */
    PortalScreencast        portal;
    bool                    portal_active;
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

static int pw_backend_init(MdCaptureCtx *ctx, const MdCaptureConfig *cfg) {
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

    /* Connect to PipeWire via xdg-desktop-portal ScreenCast flow.
     * This shows a user consent dialog and returns a PipeWire fd + node_id.
     * Falls back to direct PW_ID_ANY connection if portal is unavailable
     * (e.g. running under X11 without a portal, or in a test harness). */
    uint32_t stream_node_id = PW_ID_ANY;

    if (portal_screencast_open(&pw->portal) == 0) {
        /* Portal succeeded — connect using the portal's PipeWire fd */
        pw->core = pw_context_connect_fd(pw->pw_ctx,
                                         fcntl(pw->portal.pw_fd, F_DUPFD_CLOEXEC, 3),
                                         NULL, 0);
        stream_node_id = pw->portal.node_id;
        pw->portal_active = true;
        fprintf(stderr, "capture: connected via portal (node=%u)\n", stream_node_id);
    } else {
        /* Fallback: direct connection (works on X11 / test environments) */
        fprintf(stderr, "capture: portal unavailable, falling back to direct connect\n");
        pw->core = pw_context_connect(pw->pw_ctx, NULL, 0);
    }
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

    /* Connect stream — use portal node_id when available, PW_ID_ANY as fallback */
    int ret = pw_stream_connect(pw->stream,
        PW_DIRECTION_INPUT, stream_node_id,
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

    /* Release portal resources (D-Bus session, fd) */
    if (pw->portal_active)
        portal_screencast_close(&pw->portal);

    pthread_mutex_destroy(&pw->frame_lock);
    pthread_cond_destroy(&pw->frame_cond);

    free(pw);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdCaptureBackend pipewire_backend = {
    .init          = pw_backend_init,
    .start         = pw_start,
    .get_frame     = pw_get_frame,
    .release_frame = pw_release_frame,
    .stop          = pw_stop,
    .destroy       = pw_destroy,
};

const MdCaptureBackend *md_capture_backend_create(void) {
    return &pipewire_backend;
}
