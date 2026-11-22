/*
 * metadesk-host — main entry point.
 * Capture → encode → stream pipeline over TCP.
 *
 * Phase 1 flow:
 *   1. Start TCP server on port 7700
 *   2. Wait for client connection
 *   3. Initialize PipeWire capture and NVENC encoder
 *   4. For each captured frame:
 *      a. Encode to H.264 via FFmpeg
 *      b. Send encoded packet over TCP with MdPacketHeader framing
 *   5. Handle ping/pong keepalive
 *   6. Clean shutdown on SIGINT/SIGTERM
 */
#include "capture.h"
#include "encode.h"
#include "a11y.h"
#include "input.h"
#include "input_event.h"
#include "session.h"
#include "packet.h"
#include "stream.h"
#include "agent.h"
#include "fips_addr.h"
#include "fips_control.h"
#include "nostr.h"
#include "secrets.h"
#include "signer.h"
#include "mcp_bridge.h"
#include "mcp_http.h"
#include "bitrate_ctrl.h"
#include "session_log.h"
#include "host_mcp.h"
#include "capture_sync.h"
#include "md_time.h"
#include "host_access.h"
#include "management_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <go.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include "lifecycle.h"
static atomic_bool g_running = true;
static bool host_running(void) {
    return atomic_load(&g_running) && !md_host_lifecycle_stopping();
}

/* ── State shared between capture callback and main loop ─────── */

/* Full caches fail closed; no unexpired replay fence is evicted. */
#define HOST_REQUEST_CACHE_SIZE 256
typedef struct {
    char id[MD_SESSION_REQUEST_ID_LEN + 1];
    int64_t expires_at;
} HostRequestSeen;

typedef struct {
    char          tls_cert_sha256[65]; /* listener cert, bound into each acceptance DM */
    MdStream     *client;
    pthread_mutex_t client_mu;  /* protects client pointer lifetime */
    MdAgent      *agent;
    MdInput      *input;        /* raw human input injection (MD_PKT_INPUT) */
    MdNostr      *nostr;
    MdSession    *session;
    MdHostAccess *access;
    bool attended;
    uint32_t supported_caps;
    uint32_t approved_caps;
    bool human_approved;
    uint32_t      frame_seq;
    uint32_t      pkt_seq;
    int64_t       total_encode_us;
    uint32_t      frames_encoded;
    uint32_t      frames_sent;
    /* Encoder + input bounds follow the capture; the capture thread owns
     * the encoder, other threads go through its request calls. */
    HostCaptureSync sync;
    MdClipboard *clipboard; /* stream thread only, destroyed on disconnect */
    uint8_t clipboard_policy; /* local directions; grant still required */
    MdCaptureOutputs outputs;         /* client_mu */
    char desired_output[32], startup_output[32];
    bool output_pending;
    uint32_t control_epoch;
    uint64_t output_revision;
    pthread_cond_t capture_control_cv;
    MdQuality quality_policy, quality; /* client_mu; policy immutable */
    uint32_t applied_quality_epoch; /* capture thread only */
    uint64_t quality_revision, applied_quality_revision;
    uint32_t native_width, native_height; /* client_mu, current capture size */
    MdQuality applied_quality;         /* capture thread only */
    bool quality_error;               /* client_mu; reject this admission, keep host alive */
    bool quality_pending;             /* client_mu; admission waits for codec */
    bool quality_state_pending;       /* client_mu */
    uint8_t *quality_pixels;           /* capture thread only */
    size_t quality_capacity;
    uint64_t last_quality_frame_ns;


    /* Session log for agent monitoring (M2.4) */
    MdSessionLog *session_log;

    /* Nostr session negotiation */
    char          expected_client_pk[128]; /* optional required client pubkey hex */
    char          pending_client_pk[128]; /* pubkey of requesting client */
    MdSessionRequest pending_req;         /* parsed session request      */
    int           session_requested;      /* guarded by g_host_request_mu */
    HostRequestSeen seen_requests[HOST_REQUEST_CACHE_SIZE];
    bool          capture_required;
    bool          capture_ready;
    atomic_bool   capture_failed; /* capture worker -> admission/stream loop */
    atomic_bool   capture_stopping;
    atomic_bool   initial_frame_requested;
    MdFrame       cached_frame;   /* owned CPU snapshot; capture-thread only */
    size_t        cached_capacity;
    GoChannel      *session_req_ch;       /* signaled on session request */
} HostCtx;

/* ── Encode callback: send encoded packet to client ──────────── */

static uint64_t host_frame_tag(uint32_t epoch,uint64_t geometry) {
    return ((uint64_t)epoch<<32) | (uint32_t)geometry;
}

static void on_encoded(const MdEncodedPacket *pkt, void *userdata) {
    HostCtx *ctx = userdata;

    pthread_mutex_lock(&ctx->client_mu);
    MdStream *client = ctx->client;
    if (!md_stream_has_capability(client, MD_CAP_VIDEO) ||
        (uint32_t)(pkt->tag>>32)!=ctx->control_epoch ||
        (ctx->access && !md_host_access_valid(ctx->access, ctx->pending_req.request_id))) {
        pthread_mutex_unlock(&ctx->client_mu);
        return;
    }

    /* The upper tag word fences a frame to the session that captured it;
     * the lower word still attributes geometry by capture frame (md-f7uc).
     * Attributed to the geometry its frame was captured under, which an
     * asynchronous encoder may have left behind by now (md-f7uc). */
    host_capture_sync_packet_sent(&ctx->sync, (uint32_t)pkt->tag, ctx->pkt_seq);
    int ret = md_stream_send(client, MD_PKT_VIDEO_FRAME,
                             ctx->pkt_seq++,
                             pkt->data, (uint32_t)pkt->size);
    if (ret == 0)
        ctx->frames_sent++;
    pthread_mutex_unlock(&ctx->client_mu);
}

/* ── Nostr callbacks for session negotiation ──────────────── */

static void host_owner_revoked(void *data) {
    HostCtx *ctx = data;
    g_running = 0;
    if (ctx->session_req_ch) go_channel_close(ctx->session_req_ch);
}


static pthread_mutex_t g_host_request_mu = PTHREAD_MUTEX_INITIALIZER;

/* Callback and host thread transfer one immutable request at a time. */
static bool host_has_request(HostCtx *ctx) {
    pthread_mutex_lock(&g_host_request_mu);
    if (!ctx->session_requested && ctx->access) {
        MdHostRequest q;
        if (md_host_access_take(ctx->access, (int64_t)time(NULL), &q)) {
            ctx->pending_req = q.request;
            strcpy(ctx->pending_client_pk, q.peer);
            ctx->approved_caps = q.granted;
            ctx->human_approved = q.human;
            ctx->session_requested = 1;
        }
    }
    bool ready = ctx->session_requested != 0;
    pthread_mutex_unlock(&g_host_request_mu);
    return ready;
}

static bool host_wait_request(HostCtx *ctx) {
    void *signal = NULL;
    GoSelectCase request_case = {
        .op = GO_SELECT_RECEIVE, .chan = ctx->session_req_ch, .recv_buf = &signal,
    };
    while (host_running() && !atomic_load(&ctx->capture_failed) && !host_has_request(ctx)) {
        /* Delivery wakes this wait. The bound only observes process shutdown;
         * it never expires admission or polls/reopens a relay subscription. */
        go_select_timeout(&request_case, 1, 1000);
    }
    return host_running() && !atomic_load(&ctx->capture_failed) && host_has_request(ctx);
}

static void host_rearm_request(HostCtx *ctx) {
    pthread_mutex_lock(&g_host_request_mu);
    if (ctx->access) md_host_access_release(ctx->access);
    ctx->approved_caps = 0;
    ctx->human_approved = false;
    memset(&ctx->pending_req, 0, sizeof(ctx->pending_req));
    ctx->pending_client_pk[0] = '\0';
    md_session_reset(ctx->session);
    ctx->session_requested = 0;
    pthread_mutex_unlock(&g_host_request_mu);
}

/* Caller holds g_host_request_mu. Remember every admitted ID, including
 * refused/expired negotiations, for the entire authenticated freshness window. */
static bool host_remember_request(HostCtx *ctx, const MdSessionRequest *req,
                                   int64_t now) {
    HostRequestSeen *slot = NULL;
    for (size_t i = 0; i < HOST_REQUEST_CACHE_SIZE; i++) {
        HostRequestSeen *seen = &ctx->seen_requests[i];
        if (seen->id[0] && now <= seen->expires_at) {
            if (!strcmp(seen->id, req->request_id)) return false;
        } else if (!slot) {
            slot = seen;
        }
    }
    if (!slot) return false;
    memcpy(slot->id, req->request_id, sizeof(slot->id));
    slot->expires_at = req->expires_at + MD_SESSION_CLOCK_SKEW_S;
    return true;
}

/* Caller holds g_host_request_mu. */
static void host_handle_dm(const char *sender_pubkey_hex, const char *content,
                       const MdNostrDmInfo *info, void *userdata) {
    HostCtx *ctx = userdata;
    if (!ctx || !content || !info) return;
    char cancelled[MD_SESSION_REQUEST_ID_LEN + 1];
    if (ctx->access && info->live && md_session_cancel_from_json(content, cancelled) == 0) {
        md_host_access_cancel(ctx->access, sender_pubkey_hex, cancelled);
        go_channel_try_send(ctx->session_req_ch, (void *)(uintptr_t)1);
        return;
    }
    if (ctx->session_requested && !ctx->access) return;

    /* Try to parse as session_request */
    MdSessionRequest req;
    if (md_session_request_from_json(content, &req) == 0) {
        if (ctx->expected_client_pk[0] != '\0' &&
            strcmp(sender_pubkey_hex, ctx->expected_client_pk) != 0) {
            fprintf(stderr, "host: REJECTED session request from unexpected FIPS client %.*s...\n",
                    8, sender_pubkey_hex);
            return;
        }

        /* md-6ns: a request stored before this host subscribed was addressed
         * to an earlier host instance (NIP-59 timestamps defeat `since`).
         * Ignore history and stale/uncorrelated requests without latching,
         * so the client's live request can still be answered. */
        if (!info->live) {
            fprintf(stderr, "host: ignoring session request from relay history "
                    "(%.*s..., request_id=%.8s)\n", 8, sender_pubkey_hex,
                    req.request_id[0] ? req.request_id : "none");
            return;
        }
        MdSessionRequestFreshness fresh = md_session_request_freshness(
            &req, (int64_t)time(NULL), info->rumor_created_at);
        if (fresh != MD_SESSION_REQUEST_FRESH) {
            fprintf(stderr, "host: ignoring %s session request from %.*s...\n",
                    md_session_request_freshness_name(fresh), 8, sender_pubkey_hex);
            return;
        }

        if (!host_remember_request(ctx, &req, (int64_t)time(NULL))) {
            fprintf(stderr, "host: ignoring replayed session request or full replay cache\n");
            return;
        }
        uint32_t grant = md_nostr_granted_capabilities(ctx->nostr, sender_pubkey_hex, req.capabilities);
        if (ctx->access) {
            grant &= ctx->supported_caps;
            if (!grant || md_host_access_auto(ctx->access, sender_pubkey_hex, &req,
                                               grant, (int64_t)time(NULL))) {
                if (ctx->attended && !md_host_access_enqueue(ctx->access, sender_pubkey_hex,
                                                            &req, (int64_t)time(NULL))) {
                    fprintf(stderr, "host: pending attended request %.32s from %.64s\n", req.request_id, sender_pubkey_hex);
                } else {
                    fprintf(stderr, "host: refused session request %.32s (not authorized or busy)\n", req.request_id);
                    MdSessionReject reject = {0};
                    strcpy(reject.request_id, req.request_id);
                    strcpy(reject.reason, "request_refused");
                    char *json = md_session_reject_to_json(&reject);
                    if (json && md_nostr_send_session_accept(ctx->nostr, sender_pubkey_hex, json))
                        fprintf(stderr, "host: failed to publish admission refusal\n");
                    free(json);
                }
                return;
            }
            ctx->approved_caps = grant;
        } else if (!grant) return;

        strncpy(ctx->pending_client_pk, sender_pubkey_hex,
                sizeof(ctx->pending_client_pk) - 1);
        ctx->pending_req = req;
        ctx->session_requested = 1;
        go_channel_try_send(ctx->session_req_ch, (void *)(uintptr_t)1);
        fprintf(stderr, "host: received session request from %.*s...\n",
                8, sender_pubkey_hex);

        /* Log the request event */
        if (ctx->session_log)
            md_session_log_event(ctx->session_log, MD_SESSION_LOG_REQUEST,
                                 NULL, sender_pubkey_hex,
                                 "session request via NIP-17 DM");
    }
}

static void host_on_dm(const char *sender_pubkey_hex, const char *content,
                       const MdNostrDmInfo *info, void *userdata) {
    pthread_mutex_lock(&g_host_request_mu);
    host_handle_dm(sender_pubkey_hex, content, info, userdata);
    pthread_mutex_unlock(&g_host_request_mu);
}

/* ── Map capture pixel format to encoder pixel format ──────────── */

/* Exhaustive: an RGB-ordered frame labelled BGRX would swap red and blue. */
static MdPixFmt capture_fmt_to_enc(MdCapturePixFmt cfmt) {
    switch (cfmt) {
    case MD_PIX_CAPTURE_BGRX: return MD_PIX_FMT_BGRX;
    case MD_PIX_CAPTURE_BGRA: return MD_PIX_FMT_BGRA;
    case MD_PIX_CAPTURE_RGBX: return MD_PIX_FMT_RGBX;
    case MD_PIX_CAPTURE_RGBA: return MD_PIX_FMT_RGBA;
    case MD_PIX_CAPTURE_NV12: return MD_PIX_FMT_NV12;
    }
    return MD_PIX_FMT_BGRX; /* unreachable: backends emit only the above */
}

/* Failure-only first-frame bound, matching the PipeWire startup deadline.
 * A negotiated size alone is not evidence that capture produces frames. */
#define HOST_CAPTURE_FIRST_FRAME_WAIT_MS 5000
/* Upper bound on one idle get_frame wait, so has_client and g_running are
 * re-sampled while a damage-driven stream is quiet. */
#define HOST_CAPTURE_FRAME_WAIT_MS 100
/* Pacing after MD_CAPTURE_ERROR (returned without waiting) while the
 * watchdog decides whether to restart. */
#define HOST_CAPTURE_ERROR_BACKOFF_US 10000
/* Restart only broken capture, not healthy damage-driven idle streams.
 * Authorization removal is separately opt-in and forbidden unattended. */
#define HOST_CAPTURE_STALL_MS      4000
#define HOST_CAPTURE_MAX_RESTARTS  2

static bool host_dimensions_are_valid(uint32_t width, uint32_t height) {
    return width >= MD_INPUT_MIN_SCREEN_DIMENSION &&
           height >= MD_INPUT_MIN_SCREEN_DIMENSION;
}

static void host_apply_fallback_dimensions(uint32_t *width, uint32_t *height,
                                           const char *reason) {
    *width = MD_INPUT_FALLBACK_SCREEN_WIDTH;
    *height = MD_INPUT_FALLBACK_SCREEN_HEIGHT;
    fprintf(stderr,
            "host: WARNING: using fallback display dimensions %ux%u (%s)\n",
            *width, *height,
            reason ? reason : "actual capture/display size unavailable");
}

/* Keep one owned snapshot, not a borrowed portal buffer: a static desktop
 * need not produce damage just because another client has bound. */
static bool host_cache_frame(HostCtx *ctx, const MdFrame *frame) {
    if (frame->buf_type != MD_BUF_CPU || !frame->data || !frame->data_size)
        return false;
    uint8_t *data = ctx->cached_frame.data;
    if (frame->data_size > ctx->cached_capacity) {
        data = realloc(data, frame->data_size);
        if (!data) return false;
        ctx->cached_capacity = frame->data_size;
    }
    memcpy(data, frame->data, frame->data_size);
    ctx->cached_frame = *frame;
    ctx->cached_frame.data = data;
    ctx->cached_frame.dmabuf_fd = -1;
    ctx->cached_frame.gpu_handle = NULL;
    return true;
}

static int host_start_capture(MdCaptureCtx *capture,
                               uint32_t *width, uint32_t *height, HostCtx *ctx) {
    if (!capture || !width || !height || !ctx) return MD_CAPTURE_ERROR;
    int start_status = md_capture_start(capture);
    if (start_status != 0) return start_status;
    MdFrame frame;
    if (md_capture_get_frame(capture, &frame,
                             HOST_CAPTURE_FIRST_FRAME_WAIT_MS) != 0)
        return -1;
    bool valid = host_dimensions_are_valid(frame.width, frame.height) &&
                 host_cache_frame(ctx, &frame);
    if (valid) {
        *width = frame.width;
        *height = frame.height;
    }
    md_capture_release_frame(capture, &frame);
    return valid ? 0 : -1;
}

/* ── Capture thread: pull frames and encode ──────────────────── */

typedef struct {
    HostCtx      *host;
    MdCaptureCtx *capture;
    char *owned_output;
} CaptureThread;

static void host_quality_failed(HostCtx *ctx,const MdQuality *q,uint64_t revision,uint32_t epoch) {
    pthread_mutex_lock(&ctx->client_mu);
    bool current=ctx->control_epoch==epoch && ctx->quality_revision==revision;
    if (current && ctx->applied_quality.fps &&
        ((ctx->applied_quality_epoch==epoch && memcmp(q,&ctx->applied_quality,sizeof(*q))) || ctx->quality_pending)) {
        bool admission=ctx->quality_pending;
        ctx->quality_error=admission;
        ctx->quality=ctx->applied_quality; ctx->quality_revision++;
        ctx->applied_quality_revision=ctx->quality_revision;
        ctx->applied_quality_epoch=epoch;
        ctx->quality_state_pending=false;
        if (ctx->client && !admission) {
            char *json=md_quality_state(&ctx->applied_quality,"quality unsupported by capture/encoder; previous settings retained");
            if (json) { if (md_stream_send(ctx->client,MD_PKT_SESSION_STATE,ctx->pkt_seq++,
                (uint8_t*)json,(uint32_t)strlen(json))) md_stream_close(ctx->client); free(json); }
        }
        atomic_store(&ctx->initial_frame_requested,true);
    } else if (current) {
        fprintf(stderr,"host: capture/encoder cannot honor negotiated quality; refusing video session\n");
        atomic_store(&ctx->capture_failed,true);
    }
    if (current && ctx->quality_pending) { ctx->quality_pending=false; pthread_cond_broadcast(&ctx->capture_control_cv); }
    pthread_mutex_unlock(&ctx->client_mu);
}
static void host_encode_frame(HostCtx *ctx, MdCaptureCtx *cap,
                               const MdFrame *frame, bool has_client, bool force) {
    pthread_mutex_lock(&ctx->client_mu);
    ctx->native_width=frame->width; ctx->native_height=frame->height;
    uint32_t epoch=ctx->control_epoch;
    uint64_t revision=ctx->quality_revision;
    MdQuality q = md_quality_clamp(&ctx->quality, &ctx->quality_policy);
    pthread_mutex_unlock(&ctx->client_mu);
    MdFrame scaled;
    if (md_quality_scale(&q,frame,&scaled,&ctx->quality_pixels,&ctx->quality_capacity)) {
        host_quality_failed(ctx,&q,revision,epoch); return;
    }
    if (epoch!=ctx->applied_quality_epoch || revision!=ctx->applied_quality_revision || memcmp(&q, &ctx->applied_quality, sizeof(q))) {
        if (md_encoder_set_quality(ctx->sync.encoder,scaled.width&~1u,scaled.height&~1u,q.fps,q.bitrate)) {
            host_quality_failed(ctx,&q,revision,epoch); return;
        }
        atomic_store(&ctx->sync.bitrate_req, 0);
        ctx->applied_quality = q;
        ctx->applied_quality_revision=revision;
        if (epoch!=ctx->applied_quality_epoch) ctx->last_quality_frame_ns=0;
        ctx->applied_quality_epoch=epoch;
        pthread_mutex_lock(&ctx->client_mu);
        if (ctx->control_epoch==epoch && ctx->quality_revision==revision && ctx->quality_pending) {
            ctx->quality_pending=false; pthread_cond_broadcast(&ctx->capture_control_cv);
        }
        pthread_mutex_unlock(&ctx->client_mu);
    }
    uint64_t start_ns = md_monotonic_ns();
    if (!md_quality_frame_due(q.fps,ctx->last_quality_frame_ns,start_ns)) {
        /* A live control cannot bypass the host/session FPS ceiling. Keep
         * an initial/changed-frame request pending until the media cadence. */
        if (force) atomic_store(&ctx->initial_frame_requested,true);
        return;
    }
    if (!host_capture_sync_frame(&ctx->sync, cap, &scaled)) {
        fprintf(stderr,"host: captured geometry cannot be encoded/mapped safely\n");
        atomic_store(&ctx->capture_failed,true); return;
    }
    frame = &scaled;
    if (!has_client) return;
    ctx->last_quality_frame_ns = start_ns;
    pthread_mutex_lock(&ctx->client_mu);
    if (ctx->client && ctx->quality_state_pending && ctx->quality_revision==revision && ctx->control_epoch==epoch) {
        char *json = md_quality_control(&q);
        if (json) {
            if (md_stream_send(ctx->client, MD_PKT_SESSION_STATE, ctx->pkt_seq++,
                               (uint8_t *)json, (uint32_t)strlen(json))) md_stream_close(ctx->client);
            free(json);
        }
        ctx->quality_state_pending = false;
        fprintf(stderr, "host: quality applied cap=%ux%u bitrate=%u fps=%u frame=%ux%u\n",
            q.max_width, q.max_height, q.bitrate, q.fps, frame->width, frame->height);
    }
    pthread_mutex_unlock(&ctx->client_mu);
    int ret = md_encoder_submit_tagged(ctx->sync.encoder, frame->data,
        /* Native sequence gaps are not codec time: FPS follows paced submits.
         * The opaque tag still identifies this captured frame's geometry. */
        frame->stride, capture_fmt_to_enc(frame->format), -1,
        host_frame_tag(epoch,host_capture_sync_frame_tag(&ctx->sync)), on_encoded, ctx);
    if (ret == 0) {
        pthread_mutex_lock(&ctx->client_mu);
        ctx->total_encode_us += (int64_t)((md_monotonic_ns() - start_ns) / 1000u);
        ctx->frames_encoded++;
        pthread_mutex_unlock(&ctx->client_mu);
    }
    ctx->frame_seq++;
}

static void host_request_initial_frame(HostCtx *ctx) {
    host_capture_sync_request_keyframe(&ctx->sync);
    atomic_store(&ctx->initial_frame_requested, true);
}

/* Only the capture owner switches sources. Startup of the replacement is
 * transactional; late completion from a disconnected session cannot retarget it. */
static void host_switch_output(CaptureThread *ct) {
    HostCtx *ctx=ct->host; char id[32]; uint64_t epoch, revision;
    pthread_mutex_lock(&ctx->client_mu);
    if (!ctx->output_pending) { pthread_mutex_unlock(&ctx->client_mu); return; }
    strcpy(id,ctx->desired_output); epoch=ctx->control_epoch; revision=ctx->output_revision;
    pthread_mutex_unlock(&ctx->client_mu);
    char *owned=strdup(id); MdCaptureConfig cfg=ct->capture->config; cfg.output=owned;
    MdCaptureCtx *next=owned ? md_capture_create(&cfg) : NULL;
    HostCtx staging={0}; uint32_t w=0,h=0;
    bool ok=next && !host_start_capture(next,&w,&h,&staging);
    pthread_mutex_lock(&ctx->client_mu);
    bool current=ctx->control_epoch==epoch && ctx->output_revision==revision;
    MdFrame scaled;
    if (ok && current && (md_quality_scale(&ctx->quality,&staging.cached_frame,&scaled,
        &staging.quality_pixels,&staging.quality_capacity) ||
        md_encoder_set_quality(ctx->sync.encoder,scaled.width&~1u,scaled.height&~1u,
            ctx->quality.fps,md_encoder_get_bitrate(ctx->sync.encoder)<ctx->quality.bitrate ?
                md_encoder_get_bitrate(ctx->sync.encoder) : ctx->quality.bitrate))) ok=false;
    if (ok && current) {
        MdCaptureCtx *old=ct->capture; ct->capture=next; next=old;
        char *old_id=ct->owned_output; ct->owned_output=owned; owned=old_id;
        free(ctx->cached_frame.data); ctx->cached_frame=staging.cached_frame;
        ctx->cached_capacity=staging.cached_capacity; staging.cached_frame.data=NULL;
        strcpy(ctx->outputs.selected,id);
        /* New source identity matters even for identical rects and sizes. */
        host_capture_sync_reset_sequence(&ctx->sync);
        atomic_store(&ctx->initial_frame_requested,true);
    }
    if (current) {
        ctx->output_pending=false;
        if (ctx->client) {
            char *json=md_session_outputs_json(&ctx->outputs, ok ? NULL : "output unavailable; previous output retained");
            if (json) { if (md_stream_send(ctx->client,MD_PKT_SESSION_STATE,ctx->pkt_seq++,
                (uint8_t*)json,(uint32_t)strlen(json))) md_stream_close(ctx->client); free(json); }
        }
        pthread_cond_broadcast(&ctx->capture_control_cv);
    }
    pthread_mutex_unlock(&ctx->client_mu);
    free(staging.quality_pixels); free(staging.cached_frame.data); if (next) md_capture_destroy(next); free(owned);
}
static void *capture_thread_func(void *arg) {
    CaptureThread *ct  = arg;
    HostCtx       *ctx = ct->host;
    MdCaptureCtx  *cap = ct->capture;

    /* A restored or damage-driven stream can be paused but healthy. Keep
     * pulling frames; only MD_CAPTURE_ERROR arms the recovery watchdog. */
    MdCaptureWatchdog watchdog = {0};

    while (!atomic_load(&ctx->capture_stopping) &&
           !atomic_load(&ctx->capture_failed)) {
        host_switch_output(ct);
        cap=ct->capture;
        pthread_mutex_lock(&ctx->client_mu);
        bool has_client = (ctx->client != NULL);
        bool negotiating_quality=ctx->quality_pending;
        pthread_mutex_unlock(&ctx->client_mu);
        MdFrame frame;
        int capture_result = md_capture_get_frame(cap, &frame,
                                                  HOST_CAPTURE_FRAME_WAIT_MS);
        MdCaptureRecovery recovery = md_capture_watchdog_step(&watchdog,
            capture_result, has_client || negotiating_quality, md_stream_now_ms(),
            HOST_CAPTURE_STALL_MS, HOST_CAPTURE_MAX_RESTARTS);
        if (capture_result != 0) {
            pthread_mutex_lock(&ctx->client_mu);
            bool quality_pending=ctx->quality_pending;
            pthread_mutex_unlock(&ctx->client_mu);
            if (capture_result==MD_CAPTURE_IDLE && quality_pending && ctx->cached_frame.data_size)
                host_encode_frame(ctx,cap,&ctx->cached_frame,false,false);
            if (capture_result == MD_CAPTURE_IDLE && has_client &&
                ctx->cached_frame.data_size &&
                atomic_exchange(&ctx->initial_frame_requested, false))
                host_encode_frame(ctx, cap, &ctx->cached_frame, true, true);
            if (capture_result == MD_CAPTURE_ERROR)
                ctx->cached_frame.data_size = 0; /* never replay a broken capture */
            if (recovery != MD_CAPTURE_RECOVERY_NONE) {
                md_capture_stop(cap);
                if (recovery == MD_CAPTURE_RECOVERY_EXHAUSTED) {
                    bool forget = md_capture_stall_may_forget(&cap->config);
                    if (forget) md_capture_forget_authorization(cap);
                    fprintf(stderr,
                        "host: capture unrecoverable after %u restarts; "
                        "screencast authorization %s — operator action required\n",
                        watchdog.restarts, forget ? "cleared" : "retained");
                    atomic_store(&ctx->capture_failed, true);
                    break;
                } else {
                    fprintf(stderr,
                        "host: capture broken for %dms with a client "
                        "connected — restarting capture (attempt %u)\n",
                        HOST_CAPTURE_STALL_MS, watchdog.restarts);
                    if (md_capture_start(cap) != 0)
                        fprintf(stderr, "host: capture restart failed\n");
                }
            }
            if (capture_result != MD_CAPTURE_IDLE)
                usleep(HOST_CAPTURE_ERROR_BACKOFF_US);
            continue;
        }

        bool cached = host_cache_frame(ctx, &frame);
        md_capture_release_frame(cap, &frame);
        if (!cached) {
            fprintf(stderr, "host: cannot retain a usable capture frame\n");
            atomic_store(&ctx->capture_failed, true);
            break;
        }
        bool force = has_client && atomic_exchange(&ctx->initial_frame_requested, false);
        host_encode_frame(ctx, cap, &ctx->cached_frame, has_client, force);
    }
    pthread_mutex_lock(&ctx->client_mu);
    ctx->output_pending=false; ctx->quality_pending=false;
    pthread_cond_broadcast(&ctx->capture_control_cv);
    pthread_mutex_unlock(&ctx->client_mu);
    return NULL;
}

/* A refusal is a correlated NIP-17 session response, through the same
 * host-role and relay-OK-checked publisher as acceptance. Never mint a token. */
static int host_reject_request(HostCtx *ctx, const char *reason) {
    MdSessionReject reject = {0};
    memcpy(reject.request_id, ctx->pending_req.request_id, sizeof(reject.request_id));
    snprintf(reject.reason, sizeof(reject.reason), "%s", reason);
    char *json = md_session_reject_to_json(&reject);
    if (!json) return -1;
    int rc = md_nostr_send_session_accept(ctx->nostr, ctx->pending_client_pk, json);
    free(json);
    if (rc != 0) fprintf(stderr, "host: failed to publish session refusal\n");
    md_session_reset(ctx->session);
    return rc;
}

/* Publish only after listener/backend startup, so no valid proof is stranded
 * in the listen backlog while the client's bounded ACK wait expires. */
static int host_publish_session_accept(HostCtx *ctx, MdAccountBinding *owner) {
    if ((ctx->access && (!md_host_access_valid(ctx->access, ctx->pending_req.request_id) ||
        (int64_t)time(NULL) >= ctx->pending_req.expires_at)) ||
        !host_running() || (owner && !md_account_binding_is_current(owner)) ||
        (ctx->capture_required && (!ctx->capture_ready ||
                                   atomic_load(&ctx->capture_failed)))) return -1;
    if (md_session_request_freshness(&ctx->pending_req, (int64_t)time(NULL),
                                     ctx->pending_req.issued_at) != MD_SESSION_REQUEST_FRESH)
        return -1;
    if (!ctx->pending_req.stream_tls || !ctx->tls_cert_sha256[0]) return -1;
    MdSessionAccept acc;
    /* Same clock as md_stream_accept_binding()'s deadline checks. */
    if (!ctx->pending_req.request_id[0] ||
        md_session_prepare_binding(ctx->session, &acc,
                                   (uint64_t)md_now_ms()) != 0)
        return -1;
    /* Correlate the answer with exactly the request it grants (md-6ns). */
    MdCaptureOutputs inventory;
    bool have_inventory=ctx->capture_required && !md_capture_outputs(&inventory);
    uint64_t deadline=md_monotonic_ns()+(uint64_t)HOST_CAPTURE_FIRST_FRAME_WAIT_MS*1000000u;
    pthread_mutex_lock(&ctx->client_mu);
    while ((ctx->output_pending || ctx->quality_pending) && !atomic_load(&ctx->capture_failed) && host_running()) {
        if (md_cond_timedwait_until(&ctx->capture_control_cv,&ctx->client_mu,deadline)) {
            /* Failure watchdog, never successful negotiation by elapsed time.
             * Fence late work; keep a healthy policy baseline for the next peer. */
            ctx->control_epoch++; ctx->output_revision++; ctx->quality_revision++;
            ctx->quality=md_quality_clamp(&(MdQuality){0},&ctx->quality_policy);
            ctx->quality_pending=ctx->output_pending=false; ctx->quality_error=true;
            break;
        }
    }
    if (have_inventory) {
        if (ctx->outputs.selected[0]) strcpy(inventory.selected,ctx->outputs.selected);
        ctx->outputs=inventory;
    }
    acc.outputs=ctx->outputs;
    acc.outputs.switch_supported &= (ctx->session->capabilities & MD_CAP_MONITOR)!=0;
    acc.outputs_present=ctx->capture_required;
    bool selected_ok=(!ctx->pending_req.output[0] || !strcmp(ctx->pending_req.output,ctx->outputs.selected)) &&
        !ctx->quality_error && !atomic_load(&ctx->capture_failed) && (int64_t)time(NULL)<ctx->pending_req.expires_at;
    pthread_mutex_unlock(&ctx->client_mu);
    if (!selected_ok || !host_running() || (owner && !md_account_binding_is_current(owner)) ||
        (ctx->access && !md_host_access_valid(ctx->access,ctx->pending_req.request_id))) {
        OPENSSL_cleanse(acc.stream_token,sizeof(acc.stream_token)); return -1;
    }
    acc.clipboard_directions=ctx->session->clipboard_directions;
    acc.quality = ctx->session->quality;
    acc.quality_present = ctx->session->quality_present;
    memcpy(acc.request_id, ctx->pending_req.request_id, sizeof(acc.request_id));
    memcpy(acc.tls_cert_sha256, ctx->tls_cert_sha256, sizeof(acc.tls_cert_sha256));
    char *json = md_session_accept_to_json(&acc);
    OPENSSL_cleanse(acc.stream_token, sizeof(acc.stream_token));
    if (!json) return -1;
    int ret = md_nostr_send_session_accept(ctx->nostr, ctx->pending_client_pk, json);
    OPENSSL_cleanse(json, strlen(json));
    free(json);
    if (ret || !host_running() ||
        (ctx->access && !md_host_access_valid(ctx->access, ctx->pending_req.request_id)) ||
        (owner && !md_account_binding_is_current(owner)))
        return -1;
    if (ctx->session_log)
        md_session_log_event(ctx->session_log, MD_SESSION_LOG_ACCEPT,
                             ctx->session->session_id, ctx->pending_client_pk,
                             "session accepted");
    return 0;
}

/* Candidate ownership stays on the host thread. It is never published here;
 * capture sees it only after the bind has consumed the reservation and ACKed. */
static int host_bind_candidate(HostCtx *ctx, MdStream *candidate) {
    if (ctx->client || (ctx->access &&
        !md_host_access_valid(ctx->access, ctx->pending_req.request_id))) {
        md_stream_close(candidate);
        return -1;
    }
    int rc = md_stream_accept_binding(candidate, ctx->session);
    if (!rc && ctx->access && !md_host_access_valid(ctx->access, ctx->pending_req.request_id)) {
        md_stream_close(candidate);
        return -1;
    }
    return rc;
}

static MdAgentTreeJob *host_send_initial_tree(HostCtx *ctx) {
    return ctx->agent && md_stream_has_capability(ctx->client, MD_CAP_AGENT)
        ? md_agent_tree_start(ctx->agent) : NULL;
}

/* ── Handle incoming packets from client ─────────────────────── */

static void handle_client_packet(HostCtx *ctx, const MdPacketHeader *hdr,
                                 const uint8_t *payload) {
    /* Only the published, proven stream can dispatch. Rebinding is always a
     * protocol violation, even on the stream that consumed the token. */
    uint32_t needed = 0;
    if (hdr->type == MD_PKT_CLIPBOARD) needed=MD_CAP_CLIPBOARD;
    /* Each control operation checks its own independent capability below. */
    if (hdr->type == MD_PKT_INPUT) needed = MD_CAP_INPUT;
    if (hdr->type == MD_PKT_ACTION) needed = MD_CAP_AGENT | MD_CAP_INPUT;
    if ((ctx->access && !md_host_access_valid(ctx->access, ctx->pending_req.request_id)) ||
        hdr->type == MD_PKT_SESSION_INFO ||
        !md_stream_has_capability(ctx->client, needed)) {
        fprintf(stderr, "host: rejecting stream packet type=0x%02x: capability/proof violation\n",
                hdr->type);
        md_stream_close(ctx->client);
        return;
    }
    switch (hdr->type) {
    case MD_PKT_CLIPBOARD:
        if (md_clipboard_receive(ctx->clipboard,payload,hdr->payload_len)) {
            fprintf(stderr,"host: clipboard update rejected (direction, size, UTF-8 or platform error)\n");
            md_stream_close(ctx->client);
        } else fprintf(stderr,"host: clipboard received (%u bytes)\n",hdr->payload_len);
        break;
    case MD_PKT_SESSION_CONTROL: {
        if (!payload || !hdr->payload_len ||
            hdr->payload_len > MD_SESSION_JSON_MAX || memchr(payload, 0, hdr->payload_len)) {
            md_stream_close(ctx->client); break;
        }
        char json[MD_SESSION_JSON_MAX + 1];
        memcpy(json, payload, hdr->payload_len); json[hdr->payload_len] = 0;
        cJSON *root = md_session_json_strings_safe(json) ? cJSON_ParseWithOpts(json, NULL, true) : NULL;
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (cJSON_IsString(type) && !strcmp(type->valuestring,"monitor")) {
            cJSON *output=cJSON_GetObjectItemCaseSensitive(root,"output");
            bool allowed=md_stream_has_capability(ctx->client,MD_CAP_MONITOR) &&
                cJSON_GetArraySize(root)==2 && cJSON_IsString(output) &&
                strlen(output->valuestring)<sizeof(ctx->desired_output) &&
                md_capture_output_id_valid(output->valuestring) && !strstr(json,"\\u0000");
            bool known=false;
            for (size_t i=0;allowed && i<ctx->outputs.count;i++)
                if (!strcmp(output->valuestring,ctx->outputs.items[i].id)) known=true;
            if (!allowed) md_stream_close(ctx->client);
            else if (!known || !ctx->outputs.switch_supported) {
                char *reply=md_session_outputs_json(&ctx->outputs,
                    "switch unavailable; Linux portal selection requires new local consent");
                if (reply) { if (md_stream_send(ctx->client,MD_PKT_SESSION_STATE,ctx->pkt_seq++,
                    (uint8_t*)reply,(uint32_t)strlen(reply))) md_stream_close(ctx->client); free(reply); }
            } else {
                strcpy(ctx->desired_output,output->valuestring);
                ctx->output_revision++; ctx->output_pending=true;
            }
            cJSON_Delete(root); break;
        }
        MdQuality requested; bool present;
        if (!ctx->session->quality_present || !md_stream_has_capability(ctx->client,MD_CAP_VIDEO) ||
            !cJSON_IsObject(root) || cJSON_GetArraySize(root) != 2 ||
            !cJSON_IsString(type) || strcmp(type->valuestring, "quality") ||
            md_quality_read(root, &requested, &present) || !present) {
            cJSON_Delete(root); md_stream_close(ctx->client); break;
        }
        MdQuality next=md_quality_clamp(&requested,&ctx->quality_policy);
        uint32_t w,h;
        if (ctx->native_width && !md_capture_fit_size(ctx->native_width,ctx->native_height,
            next.max_width,next.max_height,&w,&h) && ((w&~1u)<2 || (h&~1u)<2)) {
            char *reply=md_quality_state(&ctx->quality,"resolution cap too small for current output aspect ratio");
            if (reply) { if (md_stream_send(ctx->client,MD_PKT_SESSION_STATE,ctx->pkt_seq++,
                (uint8_t*)reply,(uint32_t)strlen(reply))) md_stream_close(ctx->client); free(reply); }
            cJSON_Delete(root); break;
        }
        ctx->quality=next;
        ctx->quality_revision++;
        ctx->quality_state_pending = true;
        host_request_initial_frame(ctx);
        cJSON_Delete(root);
        break;
    }
    case MD_PKT_PING:
        /* Reply with pong */
        md_stream_send(ctx->client, MD_PKT_PONG, ctx->pkt_seq++, NULL, 0);
        break;

    case MD_PKT_PONG:
        md_stream_handle_pong(ctx->client, hdr);
        break;

    case MD_PKT_ACTION:
        if (ctx->agent) {
            md_agent_handle_action(ctx->agent, ctx->client, &ctx->pkt_seq,
                                   payload, hdr->payload_len);
        } else {
            fprintf(stderr, "host: received action but no agent handler\n");
        }
        break;

    case MD_PKT_INPUT: {
        /* Raw human input from the video client. */
        if (!ctx->input || !payload || hdr->payload_len == 0)
            break;
        uint8_t kind = 0, button = 0;
        int16_t x = 0, y = 0;
        uint16_t tlen = 0, frame_w = 0, frame_h = 0;
        uint32_t frame_pkt_seq = 0;
        const char *text = NULL;
        if (md_input_event_unpack(payload, hdr->payload_len, &kind, &button,
                                  &x, &y, &text, &tlen,
                                  &frame_w, &frame_h, &frame_pkt_seq) != 0)
            break;
        /* Points are interpreted against the frame the client showed, which
         * lags a resize by about one RTT (md-383x). */
        int px = 0, py = 0;
        if ((kind == MD_INPUT_EV_MOVE || kind == MD_INPUT_EV_BUTTON) &&
            host_capture_sync_map_point(&ctx->sync, frame_w, frame_h,
                                        frame_pkt_seq, x, y, &px, &py) ==
                HOST_POINT_DROPPED)
            break;
        switch (kind) {
        case MD_INPUT_EV_MOVE:
            md_input_mouse_move(ctx->input, px, py);
            break;
        case MD_INPUT_EV_BUTTON: {
            /* Wire buttons are 1=left, 2=middle, 3=right; md_input_click()
             * uses 0=left, 1=right, 2=middle. */
            int mb = (button == 2) ? 2 : (button == 3) ? 1 : 0;
            md_input_click(ctx->input, px, py, mb);
            break;
        }
        case MD_INPUT_EV_WHEEL:
            md_input_scroll(ctx->input, x, y);
            break;
        case MD_INPUT_EV_KEY: {
            const char *keys[MD_INPUT_MAX_COMBO_KEYS];
            int kc = 0;
            if (button & 1) keys[kc++] = "ctrl";
            if (button & 2) keys[kc++] = "shift";
            if (button & 4) keys[kc++] = "alt";
            if (button & 8) keys[kc++] = "super";
            char namez[64];
            size_t nl = tlen < sizeof(namez) - 1 ? tlen : sizeof(namez) - 1;
            if (text) memcpy(namez, text, nl);
            namez[nl] = '\0';
            if (kc < MD_INPUT_MAX_COMBO_KEYS && nl > 0) {
                keys[kc++] = namez;
                md_input_key_combo(ctx->input, keys, kc);
            }
            break;
        }
        case MD_INPUT_EV_TEXT: {
            char txt[MD_INPUT_EVENT_MAX_TEXT + 1];
            size_t tl = tlen < sizeof(txt) - 1 ? tlen : sizeof(txt) - 1;
            if (text) memcpy(txt, text, tl);
            txt[tl] = '\0';
            md_input_type_text(ctx->input, txt);
            break;
        }
        default:
            break;
        }
        break;
    }

    default:
        fprintf(stderr, "host: unknown packet type 0x%02x\n", hdr->type);
        break;
    }
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n\n", argv0);
    fprintf(stderr, "  %s capture-frame OUTPUT.raw [DISPLAY]  Capture one local frame (no remote session)\n", argv0);
    fprintf(stderr, "  %s access --help  Manage saved host-role session access\n", argv0);
    fprintf(stderr, "  %s mcp-proxy --help  stdio MCP client -> NIP-98-signed HTTP MCP\n", argv0);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --port PORT      Listen port (default: %d)\n", MD_STREAM_PORT);
    fprintf(stderr, "  --bind ADDR      Bind address (default: any)\n");
    fprintf(stderr, "  --fps FPS        Target framerate (default: 60)\n");
    fprintf(stderr, "  --bitrate BPS    Encoder bitrate (default: 8000000)\n");
    fprintf(stderr, "  --fetch-openh264 Download verified Cisco binary after displaying its license (standalone)\n");
    fprintf(stderr, "  --openh264-path FILE  Load an explicitly installed Cisco OpenH264 library\n");
    fprintf(stderr, "  --encoder NAME   Require this backend (e.g. openh264, h264_mf); no fallback\n");
    fprintf(stderr, "  --no-nvenc       Skip NVENC; try HW/OS, OpenH264, optional x264\n");
    fprintf(stderr, "  --no-capture     Skip screen capture (test mode)\n");
    fprintf(stderr, "  --capture-unattended  Portal consent budget <=10s; never auto-forget\n");
    fprintf(stderr, "  --capture-consent-timeout-ms MS  Total portal budget (1..180000)\n");
    fprintf(stderr, "  --capture-forget-on-stall  Opt in to token removal after broken capture\n");
    fprintf(stderr, "  --capture-output SEL  Display to capture (default: primary): index; Windows\n"
                    "                   device name (\\\\.\\DISPLAY2); macOS id:CGDirectDisplayID or name\n");
    fprintf(stderr, "  --capture-max-size WxH  macOS, Windows, Linux: scale frames down to fit WxH (aspect kept;\n"
                    "                   0 = unbounded, e.g. 1920x0), e.g. the display's point size\n");
    fprintf(stderr, "  --npub NPUB      Require FIPS client npub for auth\n");
    fprintf(stderr, "  --fips-control PATH  FIPS daemon control socket override\n");
    fprintf(stderr, "  --fips-ready-timeout MS  Peer readiness timeout (default: 10000)\n");
    fprintf(stderr, "\nSigner options (choose one):\n");
    fprintf(stderr, "  --bunker URI     NIP-46 Nostr Connect bunker URI\n");
    fprintf(stderr, "  --dbus-signer    Use NIP-55L D-Bus signer daemon\n");
    fprintf(stderr, "  --socket-signer [PATH]  Use NIP-5F Unix socket signer\n");
    fprintf(stderr, "  --auto-signer    Auto-detect local signer (NIP-5F, NIP-55L)\n");
    fprintf(stderr, "  --relay URL      Relay URL (default: " MD_DEFAULT_RELAY ")\n");
    fprintf(stderr, "\nMCP agent interface:\n");
    fprintf(stderr, "  --mcp            Start MCP server on stdio (JSON-RPC 2.0)\n");
    fprintf(stderr, "  --mcp-token-file PATH  MCP HTTP bearer token file (mode 0600)\n");
    fprintf(stderr, "  --mcp-public-url URL   Accepted exact NIP-98 /mcp URL (repeatable)\n");
    fprintf(stderr, "  --mcp-allow-npub NPUB  MCP HTTP authorized signer (repeatable)\n");
    fprintf(stderr, "  --mcp-allow-file PATH  MCP HTTP npub allowlist (one per line)\n");
    fprintf(stderr, "  --attended           Enable bounded local approval of unlisted clients\n");
    fprintf(stderr, "  --managed            Session-supervised, no consent UI; provision account first\n");
    fprintf(stderr, "  --management-socket PATH  Private same-user management endpoint\n");
    fprintf(stderr, "  --mcp-allow-host-list  Also authorize NIP-98 signers granted 'agent' by the saved\n"
                    "                   host role's live metadesk-allowlist (uses --relay);\n"
                    "                   action tools also need their 'input' grant\n");
    fprintf(stderr, "  --mcp-http [PORT] Start MCP HTTP+SSE server (default: 7710)\n");
    fprintf(stderr, "  --mcp-http-bind ADDR  HTTP listen address (default: ::1)\n");
    fprintf(stderr, "  --clipboard MODE Restrict granted clipboard: off|send|receive|both (default both; grant required)\n");
    fprintf(stderr, "  --no-input       Disable input injection and omit MCP action tools\n");
    fprintf(stderr, "  --input-backend auto|portal|uinput|wayland  Input backend (default: auto)\n");
    fprintf(stderr, "  --input-wayland-display PATH  Absolute private Wayland socket; no ambient display\n");
    fprintf(stderr, "  --mcp-tree-format=json|compact  UI tree resource only (default: compact); action results always JSON\n");
    fprintf(stderr, "  -h, --help       Show this help\n");
}


static void host_print_fips_setup_guidance(const char *npub,
                                           const MdFipsPeerReadiness *ready) {
    fprintf(stderr,
            "ERROR: host: FIPS peer not configured or not discovered by local daemon (%.*s...)\n",
            12, npub ? npub : "");
    if (ready && ready->detail[0])
        fprintf(stderr, "  FIPS detail: %s\n", ready->detail);
    fprintf(stderr,
            "  Configure this client peer in the FIPS daemon (via_nostr/auto_connect/peer discovery)\n"
            "  and verify `fipsctl show peers` reports a connected peer before using metadesk-host --npub.\n");
}

static uint64_t host_fips_metric_u64(cJSON *family, const char *key) {
    cJSON *item = family ? cJSON_GetObjectItemCaseSensitive(family, key) : NULL;
    return cJSON_IsNumber(item) && item->valuedouble >= 0.0
         ? (uint64_t)item->valuedouble : 0u;
}

static void host_print_fips_metrics_summary(const char *socket_path, uint32_t timeout_ms) {
    MdFipsControlResponse metrics;
    md_fips_control_response_init(&metrics);
    MdFipsControlResult r = md_fips_control_request(socket_path, "show_metrics",
                                                    NULL, timeout_ms, &metrics);
    if (r != MD_FIPS_CONTROL_OK) {
        fprintf(stderr, "host: FIPS metrics unavailable: %s%s%s\n",
                md_fips_control_result_string(r),
                metrics.message ? ": " : "",
                metrics.message ? metrics.message : "");
        md_fips_control_response_free(&metrics);
        return;
    }

    cJSON *forwarding = metrics.data ? cJSON_GetObjectItemCaseSensitive(metrics.data, "forwarding") : NULL;
    cJSON *discovery = metrics.data ? cJSON_GetObjectItemCaseSensitive(metrics.data, "discovery") : NULL;
    cJSON *errors = metrics.data ? cJSON_GetObjectItemCaseSensitive(metrics.data, "errors") : NULL;

    printf("host: FIPS metrics forwarding sent=%llu recv=%llu discovery sent=%llu recv=%llu errors=%llu\n",
           (unsigned long long)host_fips_metric_u64(forwarding, "packets_sent"),
           (unsigned long long)host_fips_metric_u64(forwarding, "packets_recv"),
           (unsigned long long)host_fips_metric_u64(discovery, "sent"),
           (unsigned long long)host_fips_metric_u64(discovery, "recv"),
           (unsigned long long)host_fips_metric_u64(errors, "total"));
    md_fips_control_response_free(&metrics);
}

static int host_check_fips_daemon(const char *socket_path, uint32_t timeout_ms) {
    MdFipsControlResponse resp;
    md_fips_control_response_init(&resp);
    MdFipsControlResult r = md_fips_control_request(socket_path, "show_status",
                                                    NULL, timeout_ms, &resp);
    if (r != MD_FIPS_CONTROL_OK) {
        fprintf(stderr, "ERROR: FIPS daemon health check failed: %s%s%s\n",
                md_fips_control_result_string(r),
                resp.message ? ": " : "",
                resp.message ? resp.message : "");
        if (resp.socket_path)
            fprintf(stderr, "  control socket: %s\n", resp.socket_path);
        fprintf(stderr,
                "  Start and configure the FIPS daemon, or pass --fips-control PATH if it uses a custom socket.\n");
        md_fips_control_response_free(&resp);
        return -1;
    }

    /*
     * `state` is an opaque diagnostic string from FIPS' format!("{}", node.state()).
     * Do not branch on it. Future readiness checks should use stable documented
     * fields like tun_state, peer_count, or structured show_peers/show_sessions data.
     */
    cJSON *state = resp.data ? cJSON_GetObjectItemCaseSensitive(resp.data, "state") : NULL;
    cJSON *npub = resp.data ? cJSON_GetObjectItemCaseSensitive(resp.data, "npub") : NULL;
    printf("host: FIPS daemon ready");
    if (resp.socket_path) printf(" (%s)", resp.socket_path);
    if (cJSON_IsString(state) && state->valuestring) printf(" state=%s", state->valuestring);
    if (cJSON_IsString(npub) && npub->valuestring) printf(" npub=%.*s...", 12, npub->valuestring);
    printf("\n");
    md_fips_control_response_free(&resp);

    host_print_fips_metrics_summary(socket_path, timeout_ms);
    return 0;
}

/* ── MCP lifecycle ───────────────────────────────────────────── */

static int host_run_mcp(bool stdio_mode, bool http_mode, uint16_t port,
                        const char *bind_addr, bool no_input,
                        const MdInputConfig *input_options,
                        MdMcpTreeFormat tree_format,
                        const MdMcpBridgeConfig *http_auth,
                        const char *const *acl_relays, int acl_relay_count) {
    int rc = 1;
    MdA11yCtx *a11y = md_a11y_create();
    MdInput *input = NULL;
    MdMcpBridge *bridge = NULL;
    HostMcpAcl *acl = NULL;
    if (!md_a11y_is_connected(a11y)) {
        fprintf(stderr, "host[mcp]: accessibility backend unavailable\n");
        goto cleanup;
    }

    if (!no_input) {
        uint32_t width = 0, height = 0;
        host_apply_fallback_dimensions(&width, &height,
                                       "MCP mode has no capture pipeline");
        MdInputConfig input_cfg = *input_options;
        input_cfg.screen_width = width;
        input_cfg.screen_height = height;
        input = md_input_create(&input_cfg);
        if (!md_input_is_ready(input))
            fprintf(stderr, "host[mcp]: input unavailable; action tools disabled\n");
    }
    MdMcpBridgeConfig cfg = {
        .a11y = a11y,
        .input = input,
        .tree_format = tree_format,
        .settle_ms = 100,
        .stdio_in_fd = stdio_mode ? STDIN_FILENO : -1,
        .stdio_out_fd = stdio_mode ? STDOUT_FILENO : -1,
        .http_bind_addr = bind_addr,
        .http_port = http_mode ? (port ? port : MD_MCP_HTTP_DEFAULT_PORT) : 0,
        .http_token_file = http_auth->http_token_file,
        .http_public_urls = http_auth->http_public_urls,
        .http_public_url_count = http_auth->http_public_url_count,
        .http_allow_npubs = http_auth->http_allow_npubs,
        .http_allow_npub_count = http_auth->http_allow_npub_count,
        .http_allow_file = http_auth->http_allow_file,
    };
    /* Optional live NIP-98 source; outlives the bridge's HTTP handlers. */
    if (acl_relays) {
        acl = host_mcp_acl_start(acl_relays, acl_relay_count);
        if (!acl) goto cleanup;
        cfg.http_nip98_grant = host_mcp_acl_grant;
        cfg.http_nip98_grant_userdata = acl;
        cfg.http_nip98_grant_watch = host_mcp_acl_watch;
    }
    bridge = md_mcp_bridge_create(&cfg);
    if (!bridge) {
        fprintf(stderr, "host[mcp]: failed to create MCP bridge\n");
        goto cleanup;
    }

    fprintf(stderr, "host[mcp]: serving on %s\n", stdio_mode ? "stdio" : "HTTP");
    rc = md_mcp_bridge_run_supervised(bridge) < 0 ? 1 : 0;

cleanup:
    md_mcp_bridge_destroy(bridge);
    host_mcp_acl_stop(acl);
    if (input) md_input_destroy(input);
    if (a11y) md_a11y_destroy(a11y);
    return rc;
}

/* ── Main ────────────────────────────────────────────────────── */

/* Acceptance is the policy boundary, not the earlier membership prefilter.
 * Read the latest winning grant immediately before recording negotiation. */
static int host_accept_request(HostCtx *ctx, MdNostr *nostr,
                               const char *session_id, bool no_input) {
    if (!ctx->pending_req.stream_tls) {
        fprintf(stderr, "ERROR: pinned TLS 1.3 required; upgrade both host and client\n");
        return -1;
    }
    uint32_t granted = ctx->access && ctx->human_approved ? ctx->approved_caps : md_nostr_granted_capabilities(nostr,
        ctx->pending_client_pk, ctx->pending_req.capabilities);
    if (ctx->access && (!md_host_access_valid(ctx->access, ctx->pending_req.request_id) ||
        (int64_t)time(NULL) >= ctx->pending_req.expires_at)) return -1;
    if (ctx->pending_req.output[0]) {
        if (!(granted & MD_CAP_MONITOR)) return -1;
        MdCaptureOutputs outputs;
        if (md_capture_outputs(&outputs) || !outputs.switch_supported) return -1;
        bool known=false;
        for (size_t i=0;i<outputs.count;i++) if (!strcmp(outputs.items[i].id,ctx->pending_req.output)) known=true;
        if (!known) return -1;
    }
    uint8_t clipboard_directions=ctx->pending_req.clipboard_directions & md_clipboard_reverse(ctx->clipboard_policy);
    if (!md_clipboard_backend() || !clipboard_directions) granted &= ~MD_CAP_CLIPBOARD;
    if (!(granted&MD_CAP_CLIPBOARD)) clipboard_directions=0;
    if (no_input) granted &= ~MD_CAP_INPUT;
    if (!granted) return -1;
    if (md_session_request(ctx->session, ctx->pending_client_pk,
            ctx->pending_req.capabilities, ctx->pending_req.tree_format)) return -1;
    int rc = md_session_accept(ctx->session, session_id, granted);
    if (!rc) {
        pthread_mutex_lock(&ctx->client_mu);
        ctx->session->clipboard_directions=clipboard_directions;
        ctx->control_epoch++;
        if (ctx->capture_ready && ctx->pending_req.output[0] &&
            strcmp(ctx->pending_req.output,ctx->outputs.selected)) {
            strcpy(ctx->desired_output,ctx->pending_req.output);
            ctx->output_revision++; ctx->output_pending=true;
        }
        ctx->quality_error=false;
        ctx->quality_pending=ctx->capture_ready;
        ctx->quality_revision++;
        ctx->quality = md_quality_clamp(&ctx->pending_req.quality, &ctx->quality_policy);
        ctx->session->quality = ctx->quality;
        ctx->session->quality_present = ctx->pending_req.quality_present;
        ctx->quality_state_pending = ctx->pending_req.quality_present;
        pthread_mutex_unlock(&ctx->client_mu);
    }
    if (!rc && ctx->access) md_host_access_bound(ctx->access, ctx->pending_req.request_id, session_id, granted);
    return rc;
}

static int host_accept_fresh_request(HostCtx *ctx, MdNostr *nostr, bool no_input) {
    unsigned char random[16];
    char session_id[64];
    if (RAND_bytes(random, sizeof(random)) != 1) return -1;
    snprintf(session_id, sizeof(session_id),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             random[0], random[1], random[2], random[3], random[4], random[5],
             random[6], random[7], random[8], random[9], random[10], random[11],
             random[12], random[13], random[14], random[15]);
    if (md_session_request_freshness(&ctx->pending_req, (int64_t)time(NULL),
                                     ctx->pending_req.issued_at) != MD_SESSION_REQUEST_FRESH ||
        host_accept_request(ctx, nostr, session_id, no_input) != 0) {
        fprintf(stderr, "ERROR: stale request or current client policy grants no requested capabilities\n");
        return -1;
    }
    return 0;
}

static int host_replace_agent(HostCtx *ctx, MdAgent **agent, MdAgentConfig *cfg) {
    cfg->tree_format = ctx->session->tree_format;
    MdAgent *next = md_agent_create(cfg);
    if (!next) return -1;
    md_agent_destroy(*agent);
    *agent = next;
    ctx->agent = next;
    return 0;
}

/* A policy change or an expired request rejects just that negotiation. */
static int host_next_session(HostCtx *ctx, MdNostr *nostr, bool no_input,
                             MdAccountBinding *owner, MdAgent **agent,
                             MdAgentConfig *cfg) {
    while (host_wait_request(ctx)) {
        if (owner && !md_account_binding_is_current(owner)) return -1;
        if (host_accept_fresh_request(ctx, nostr, no_input) != 0) {
            host_reject_request(ctx, ctx->pending_req.output[0] ? "output_unavailable" : ctx->pending_req.stream_tls ? "request_refused" : "tls_required");
            host_rearm_request(ctx);
            continue;
        }
        if (host_replace_agent(ctx, agent, cfg) != 0) {
            host_reject_request(ctx, "request_refused");
            return -1;
        }
        if (host_publish_session_accept(ctx, owner) != 0) {
            host_reject_request(ctx, ctx->quality_error ? "quality_unsupported" : "request_refused");
            if (!ctx->access || !host_running() || atomic_load(&ctx->capture_failed) ||
                (owner && !md_account_binding_is_current(owner))) return -1;
            host_rearm_request(ctx);
            continue;
        }
        return 0;
    }
    return -1;
}

static bool host_binding_pending(HostCtx *ctx, uint64_t now_ms) {
    return (!ctx->access || md_host_access_valid(ctx->access, ctx->pending_req.request_id)) &&
        ctx->session->state == MD_SESSION_NEGOTIATING &&
        ctx->session->stream_token[0] && now_ms < ctx->session->stream_deadline_ms;
}

/* Called only after recv reports disconnect (or process shutdown), with no
 * dispatch in flight. Capture must relinquish the pointer before destruction. */
static void host_finish_client(HostCtx *ctx, MdStream *client) {
    pthread_mutex_lock(&ctx->client_mu);
    ctx->client = NULL;
    ctx->control_epoch++;
    if (ctx->output_pending) { ctx->output_pending=false; pthread_cond_broadcast(&ctx->capture_control_cv); }
    pthread_mutex_unlock(&ctx->client_mu);
    md_clipboard_destroy(ctx->clipboard); ctx->clipboard=NULL;
    md_stream_destroy(client);
    host_rearm_request(ctx);
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "win_dpi.h"
#endif

static void host_management_wake(void *data) {
    HostCtx *ctx = data;
    if (ctx->session_req_ch) go_channel_try_send(ctx->session_req_ch, (void *)(uintptr_t)1);
}

static int host_run(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "session") || !strcmp(argv[1], "requests")))
        return md_host_management_cli(argv[1], argc - 2, argv + 2);
    /* Standalone explicit action, before capture, signer, relay or threads. */
    if (argc > 1 && strcmp(argv[1], "--fetch-openh264") == 0) {
        if (argc != 2) {
            fprintf(stderr, "--fetch-openh264 must be used alone\n");
            return 1;
        }
        extern int md_openh264_fetch(void);
        return md_openh264_fetch();
    }


#ifdef _WIN32
    /* md-m5x4: per-monitor DPI aware before any window, UIA, DXGI or input
     * call, so UIA rectangles, capture and SendInput share physical pixels. */
    md_win_dpi_process_init();
#endif
    if (argc > 1 && strcmp(argv[1], "capture-frame") == 0) {
        extern int md_host_capture_cli(int argc, char **argv);
        return md_host_capture_cli(argc - 2, argv + 2);
    }
    /* Isolated metadata CLI: no capture, session, or Nostr startup. */
    if (argc > 1 && strcmp(argv[1], "account") == 0) {
        extern int md_host_account_cli(int argc, char **argv);
        return md_host_account_cli(argc - 2, argv + 2);
    }
    if (argc > 1 && strcmp(argv[1], "access") == 0) {
        extern int md_host_access_cli(int argc, char **argv);
        return md_host_access_cli(argc - 2, argv + 2);
    }
    /* Agent-side shim: signer + HTTP only, no capture/a11y/input/relay. */
    if (argc > 1 && strcmp(argv[1], "mcp-proxy") == 0)
        return md_host_mcp_proxy_cli(argc - 2, argv + 2);
    /* Every subsequently created thread inherits this mask. MCP consumes
     * termination signals with sigwait, never an async-signal-unsafe handler. */
#ifndef _WIN32
    sigset_t term_signals;
    sigemptyset(&term_signals);
    sigaddset(&term_signals, SIGINT);
    sigaddset(&term_signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &term_signals, NULL) != 0) {
        fprintf(stderr, "ERROR: cannot block termination signals\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Default options */
    uint16_t port = MD_STREAM_PORT;
    const char *bind_addr = NULL;
    const char *fips_npub = NULL;  /* expected client npub (FIPS auth) */
    const char *bunker_uri = NULL;
    const char *socket_path = NULL;
    const char *fips_control_socket = NULL;
    bool use_dbus_signer = false;
    bool auto_signer = false;
    bool use_socket_signer = false;
    uint32_t fps = 60;
    uint32_t bitrate = MD_ENCODER_DEFAULT_BITRATE;
    bool use_nvenc = true;
    const char *openh264_path = NULL, *encoder_name = NULL;
    bool managed = false;
    bool do_capture = true;
    /* Capture policy options: keep this block independent of input selection. */
    bool capture_unattended = false;
    bool capture_forget_on_stall = false;
    uint32_t capture_consent_timeout_ms = 0;
    const char *capture_output = NULL;
    uint32_t capture_max_w = 0, capture_max_h = 0;
    bool mcp_stdio = false;
    bool mcp_http = false;
    const char *mcp_token_file = NULL, *mcp_allow_file = NULL;
    const char *mcp_public_urls[32], *mcp_allow_npubs[32];
    size_t mcp_public_url_count = 0, mcp_allow_npub_count = 0;
    bool mcp_allow_host_list = false;
    uint8_t clipboard_policy=MD_CLIPBOARD_BOTH;
    bool no_input = false;
    bool attended = false;
    const char *management_socket = NULL;
    MdInputConfig input_options = {0};
    bool signaling_option = false;
    const char *mcp_http_bind = NULL;
    MdMcpTreeFormat mcp_tree_format = MD_MCP_TREE_FORMAT_DEFAULT;
    uint16_t mcp_http_port = 0;  /* 0 = default (7710) */
    uint32_t fips_ready_timeout_ms = 10000;
    const char *relay_urls[16];
    int relay_count = 0;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--npub") == 0 ||
            strcmp(argv[i], "--bunker") == 0 || strcmp(argv[i], "--dbus-signer") == 0 ||
            strcmp(argv[i], "--socket-signer") == 0 || strcmp(argv[i], "--auto-signer") == 0 ||
            strcmp(argv[i], "--fips-control") == 0 || strcmp(argv[i], "--fips-ready-timeout") == 0)
            signaling_option = true;
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc)
            bind_addr = argv[++i];
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            fps = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc)
            bitrate = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-nvenc") == 0)
            use_nvenc = false;
        else if (strcmp(argv[i], "--openh264-path") == 0 || strcmp(argv[i], "--encoder") == 0) {
            bool path_option = strcmp(argv[i], "--openh264-path") == 0;
            if (++i >= argc || !argv[i][0]) {
                fprintf(stderr, "ERROR: encoder option requires a value\n");
                return 1;
            }
            if (path_option) openh264_path = argv[i];
            else encoder_name = argv[i];
        }
        else if (strcmp(argv[i], "--managed") == 0)
            managed = true;
        else if (strcmp(argv[i], "--attended") == 0)
            attended = true;
        else if (strcmp(argv[i], "--management-socket") == 0 && i + 1 < argc)
            management_socket = argv[++i];
        else if (strcmp(argv[i], "--no-capture") == 0)
            do_capture = false;
        /* Capture policy: portal-required on Linux, never direct fallback. */
        else if (strcmp(argv[i], "--capture-unattended") == 0)
            capture_unattended = true;
        else if (strcmp(argv[i], "--capture-forget-on-stall") == 0)
            capture_forget_on_stall = true;
        else if (strcmp(argv[i], "--capture-consent-timeout-ms") == 0) {
            if (++i >= argc || !argv[i][0]) {
                fprintf(stderr, "ERROR: capture consent timeout requires 1..180000 ms\n");
                return 1;
            }
            char *end = NULL;
            unsigned long ms = strtoul(argv[i], &end, 10);
            if (*end || ms < 1 || ms > 180000) {
                fprintf(stderr, "ERROR: capture consent timeout requires 1..180000 ms\n");
                return 1;
            }
            capture_consent_timeout_ms = (uint32_t)ms;
        }
        else if (strcmp(argv[i], "--capture-output") == 0) {
            if (++i >= argc || !argv[i][0]) {
                fprintf(stderr, "ERROR: --capture-output requires an output index or device name\n");
                return 1;
            }
#if defined(_WIN32) || defined(__APPLE__)
            capture_output = argv[i];
#else
            fprintf(stderr, "ERROR: --capture-output is not supported on Linux: "
                            "PipeWire capture picks the source in the ScreenCast portal dialog\n");
            return 1;
#endif
        }
        else if (strcmp(argv[i], "--capture-max-size") == 0) {
            /* Each side 0 (unbounded) or 64..16384, not both 0. */
            char *end = NULL;
            unsigned long mw = 0, mh = 0;
            bool ok = ++i < argc && argv[i][0] >= '0' && argv[i][0] <= '9';
            if (ok) {
                mw = strtoul(argv[i], &end, 10);
                ok = (*end == 'x' || *end == 'X') && end[1] >= '0' && end[1] <= '9';
            }
            if (ok) {
                mh = strtoul(end + 1, &end, 10);
                ok = !*end && (mw || mh) &&
                     (!mw || (mw >= MD_INPUT_MIN_SCREEN_DIMENSION && mw <= 16384)) &&
                     (!mh || (mh >= MD_INPUT_MIN_SCREEN_DIMENSION && mh <= 16384));
            }
            if (!ok) {
                fprintf(stderr, "ERROR: --capture-max-size requires WxH, each 0 (unbounded) "
                                "or %u..16384, not both 0\n", MD_INPUT_MIN_SCREEN_DIMENSION);
                return 1;
            }
            /* Every backend scales: SCK in hardware, DXGI on the GPU,
             * PipeWire on the CPU. */
            capture_max_w = (uint32_t)mw;
            capture_max_h = (uint32_t)mh;
        }
        else if (strcmp(argv[i], "--npub") == 0 && i + 1 < argc)
            fips_npub = argv[++i];
        else if (strcmp(argv[i], "--fips-control") == 0 && i + 1 < argc)
            fips_control_socket = argv[++i];
        else if (strcmp(argv[i], "--fips-ready-timeout") == 0 && i + 1 < argc)
            fips_ready_timeout_ms = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bunker") == 0 && i + 1 < argc)
            bunker_uri = argv[++i];
        else if (strcmp(argv[i], "--dbus-signer") == 0)
            use_dbus_signer = true;
        else if (strcmp(argv[i], "--socket-signer") == 0) {
            use_socket_signer = true;
            socket_path = (i + 1 < argc && argv[i + 1][0] != '-')
                ? argv[++i] : NULL;
        }
        else if (strcmp(argv[i], "--auto-signer") == 0)
            auto_signer = true;
        else if (strcmp(argv[i], "--relay") == 0 && i + 1 < argc) {
            if (relay_count < 16)
                relay_urls[relay_count++] = argv[++i];
        }
        /* Wayland input options: kept together, independent of capture flags. */
        else if (strcmp(argv[i], "--input-backend") == 0 ||
                 strcmp(argv[i], "--input-wayland-display") == 0) {
            bool backend = strcmp(argv[i], "--input-backend") == 0;
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Missing input option value\n");
                return 1;
            }
            if (backend) input_options.backend = argv[++i];
            else input_options.wayland_display = argv[++i];
        }
        else if (!strcmp(argv[i],"--clipboard") && i+1<argc) {
            int d=md_clipboard_directions_parse(argv[++i]);
            if (d<0) { fprintf(stderr,"ERROR: --clipboard requires off|send|receive|both\n"); return 1; }
            clipboard_policy=(uint8_t)d;
        }
        else if (strcmp(argv[i], "--no-input") == 0)
            no_input = true;
        else if (strcmp(argv[i], "--mcp-http-bind") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "Missing --mcp-http-bind address\n");
                return 1;
            }
            mcp_http_bind = argv[++i];
        }
        else if (strcmp(argv[i], "--mcp") == 0)
            mcp_stdio = true;
        else if (strncmp(argv[i], "--mcp-tree-format=", 18) == 0 ||
                 strcmp(argv[i], "--mcp-tree-format") == 0) {
            const char *format = argv[i][17] == '=' ? argv[i] + 18
                : (i + 1 < argc ? argv[++i] : "");
            if (strcmp(format, "json") == 0)
                mcp_tree_format = MD_MCP_TREE_FORMAT_JSON;
            else if (strcmp(format, "compact") == 0)
                mcp_tree_format = MD_MCP_TREE_FORMAT_COMPACT;
            else {
                fprintf(stderr, "Invalid --mcp-tree-format: expected json or compact\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--mcp-token-file") == 0 && i + 1 < argc)
            mcp_token_file = argv[++i];
        else if (strcmp(argv[i], "--mcp-allow-file") == 0 && i + 1 < argc)
            mcp_allow_file = argv[++i];
        else if (strcmp(argv[i], "--mcp-allow-host-list") == 0)
            mcp_allow_host_list = true;
        else if (strcmp(argv[i], "--mcp-public-url") == 0 && i + 1 < argc) {
            if (mcp_public_url_count == 32) {
                fprintf(stderr, "Too many --mcp-public-url values (max 32)\n");
                return 1;
            }
            mcp_public_urls[mcp_public_url_count++] = argv[++i];
        }
        else if (strcmp(argv[i], "--mcp-allow-npub") == 0 && i + 1 < argc) {
            if (mcp_allow_npub_count == 32) {
                fprintf(stderr, "Too many --mcp-allow-npub values (max 32)\n");
                return 1;
            }
            mcp_allow_npubs[mcp_allow_npub_count++] = argv[++i];
        }
        else if (strcmp(argv[i], "--mcp-http") == 0) {
            mcp_http = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                mcp_http_port = (uint16_t)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if ((input_options.backend && strcmp(input_options.backend, "auto") &&
         strcmp(input_options.backend, "uinput") && strcmp(input_options.backend, "wayland")) ||
        (input_options.backend && !strcmp(input_options.backend, "wayland") &&
         (!input_options.wayland_display || input_options.wayland_display[0] != '/')) ||
        (input_options.wayland_display &&
         (!input_options.backend || strcmp(input_options.backend, "wayland")))) {
        fprintf(stderr, "Invalid input selection: wayland requires an absolute --input-wayland-display path\n");
        return 1;
    }

    if ((mcp_stdio && mcp_http) || (mcp_http_bind && !mcp_http) ||
        ((mcp_stdio || mcp_http) && signaling_option)) {
        fprintf(stderr, "ERROR: MCP requires one transport; --mcp-http-bind requires HTTP; "
                        "MCP cannot use video-session Nostr/FIPS/signer options\n");
        return 1;
    }
    /* In MCP mode a relay serves only the host-allowlist NIP-98 source. */
    if ((mcp_allow_host_list && !mcp_http) ||
        ((mcp_stdio || mcp_http) && relay_count && !mcp_allow_host_list)) {
        fprintf(stderr, "ERROR: --mcp-allow-host-list requires --mcp-http; in MCP mode "
                        "--relay requires --mcp-allow-host-list\n");
        return 1;
    }

    /* MCP does not use video session negotiation, Nostr or its session log.
     * Enter before allocating them, and reserve stdout for JSON-RPC. */
    if (managed && (mcp_stdio || mcp_http)) {
        fprintf(stderr, "ERROR: --managed is a video host mode, not an MCP transport\n");
        return MD_HOST_EXIT_CONFIG;
    }
    input_options.no_interaction = managed;
    if (mcp_stdio || mcp_http) {
        const MdMcpBridgeConfig http_auth = {
            .http_token_file = mcp_token_file,
            .http_public_urls = mcp_public_urls,
            .http_public_url_count = mcp_public_url_count,
            .http_allow_npubs = mcp_allow_npubs,
            .http_allow_npub_count = mcp_allow_npub_count,
            .http_allow_file = mcp_allow_file,
        };
        if (mcp_allow_host_list && relay_count == 0)
            relay_urls[relay_count++] = MD_DEFAULT_RELAY;
        return host_run_mcp(mcp_stdio, mcp_http, mcp_http_port,
                            mcp_http_bind, no_input, &input_options, mcp_tree_format,
                            &http_auth, mcp_allow_host_list ? relay_urls : NULL,
                            relay_count);
    }

    /* Video mode only: progress and readiness lines must reach a pipe
     * (container log, supervisor) when printed. MCP returned above with
     * stdout untouched; its stdio transport owns fd 1 via write(2). */
#ifdef _WIN32
    /* UCRT treats _IOLBF as full buffering, including redirected output. */
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif

    if (md_host_lifecycle_init(managed) != 0) {
        fprintf(stderr, "ERROR: cannot install termination handlers\n");
        return 1;
    }
#ifndef _WIN32
    pthread_sigmask(SIG_UNBLOCK, &term_signals, NULL);
#endif

    md_host_lifecycle_event("starting", 0);
    if (bitrate<MD_ENCODER_MIN_BITRATE || bitrate>MD_ENCODER_MAX_BITRATE ||
        !fps || fps>MD_QUALITY_MAX_FPS || capture_max_w>MD_QUALITY_MAX_DIMENSION || capture_max_h>MD_QUALITY_MAX_DIMENSION) {
        fprintf(stderr,"ERROR: host quality policy exceeds capture/encoder limits (bitrate 100000..100000000, fps 1..240, size <=16384)\n");
        return MD_HOST_EXIT_CONFIG;
    }
    if (managed && (mcp_stdio || mcp_http || bunker_uri || auto_signer ||
        use_dbus_signer || use_socket_signer)) {
        fprintf(stderr, "ERROR: --managed requires a pre-bound host account, not interactive/auto signer or MCP options\n");
        return MD_HOST_EXIT_CONFIG;
    }
    printf("metadesk-host v0.1.0\n");
    printf("  port:      %u\n", port);
    printf("  fps:       %u\n", fps);
    printf("  bitrate:   %u bps\n", bitrate);
    printf("  encoder:   %s\n", encoder_name ? encoder_name
           : use_nvenc ? "NVENC preferred; HW/OS, OpenH264, optional x264 fallback"
                       : "non-NVENC HW/OS, OpenH264, optional x264 fallback");
    printf("  capture:   %s\n", do_capture ? "enabled" : "disabled");
    if (fips_npub)
        printf("  fips:      %.*s...\n", 12, fips_npub);
    printf("\n");

    /* ── Signer initialization ────────────────────────────────── */
    MdSigner *signer = NULL;
    MdAccountStartup account = {0};
    bool explicit_signer = bunker_uri || use_dbus_signer || use_socket_signer || auto_signer;
    if (!explicit_signer) {
        int rc = md_account_startup_open(MD_ACCOUNT_HOST, &account);
        if (rc) {
            fprintf(stderr, "ERROR: host account restore: %s\n", md_accounts_error_string(rc));
            if (rc == MD_SIGNER_ERR_CONNECT || rc == MD_SIGNER_ERR_TIMEOUT ||
                rc == MD_ACCOUNTS_BUSY || rc == MD_CREDENTIAL_UNAVAILABLE) return 1;
            return managed ? MD_HOST_EXIT_CONFIG : 1;
        }
        signer = md_signer_ref(md_account_binding_signer(account.binding));
    } else if (bunker_uri) {
        printf("host: connecting to NIP-46 bunker...\n");
        signer = md_signer_create_nip46(bunker_uri, 30000);
    } else if (use_dbus_signer) {
        printf("host: connecting to NIP-55L D-Bus signer...\n");
        signer = md_signer_create_nip55l();
    } else if (use_socket_signer || auto_signer) {
        if (use_socket_signer) {
            printf("host: connecting to NIP-5F socket signer...\n");
            signer = md_signer_create_nip5f(socket_path);
        }
        if (!signer && auto_signer) {
            printf("host: auto-detecting signer...\n");
            signer = md_signer_auto_detect();
        }
    }
    if (managed && !signer) {
        fprintf(stderr, "ERROR: managed host needs a provisioned, bound host account; run account add/bind-role interactively\n");
        md_account_startup_clear(&account);
        return MD_HOST_EXIT_CONFIG;
    }
    /* If no signer from CLI, log that Nostr features require a signer */
    if (signer) {
        printf("host: signer ready (%s)\n",
               md_signer_type_name(md_signer_get_type(signer)));
    } else if (explicit_signer) {
        fprintf(stderr, "ERROR: requested signer backend not available\n");
        return 1;
    }
    if (fips_npub && !signer) {
        fprintf(stderr, "ERROR: host --npub requires a signer for Nostr session authorization\n");
        fprintf(stderr, "  Bind a host account or use --auto-signer, --socket-signer, --dbus-signer, or --bunker.\n");
        return 1;
    }

    char expected_client_pk[65] = {0};
    if (fips_npub) {
        if (md_fips_npub_to_pubkey_hex(fips_npub, expected_client_pk,
                                       sizeof(expected_client_pk)) != 0) {
            fprintf(stderr, "ERROR: --npub must be a valid bech32 FIPS/Nostr npub identity\n");
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }

        if (!bind_addr)
            printf("host: FIPS mode — accepting connections via fips0 TUN\n");

        printf("host: checking FIPS daemon health...\n");
        if (host_check_fips_daemon(fips_control_socket, 5000) != 0) {
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }
        /* Compare our checked host identity, not the expected peer, with the
         * actual daemon's show_status identity. Missing identity fails closed. */
        MdFipsControlResponse status;
        md_fips_control_response_init(&status);
        MdFipsControlResult result = md_fips_control_request(fips_control_socket,
            "show_status", NULL, 5000, &status);
        cJSON *node = status.data ? cJSON_GetObjectItemCaseSensitive(status.data, "npub") : NULL;
        char node_pk[65] = {0};
        int rc = MD_ACCOUNTS_INVALID;
        if (result == MD_FIPS_CONTROL_OK && cJSON_IsString(node) &&
            !md_fips_npub_to_pubkey_hex(node->valuestring, node_pk, sizeof(node_pk))) {
            if (account.binding) rc = md_accounts_check_fips_host(account.accounts, node_pk);
            else {
                char *host_pk = NULL;
                rc = md_signer_check_connection(signer);
                if (!rc) rc = md_signer_get_pubkey(signer, &host_pk);
                if (!rc && (!host_pk || strcmp(host_pk, node_pk))) rc = MD_ACCOUNTS_FIPS_KEY_MISMATCH;
                free(host_pk);
            }
        }
        md_fips_control_response_free(&status);
        if (rc) {
            fprintf(stderr, "ERROR: FIPS host identity check refused: %s (requires show_status npub matching host signer)\n",
                    md_accounts_error_string(rc));
            md_signer_destroy(signer);
            md_account_startup_clear(&account);
            return 1;
        }
    }

#ifdef _WIN32
    if (attended || management_socket) {
        fprintf(stderr, "ERROR: attended management unsupported on Windows; owner-DACL/client-SID pipe required\n");
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        return 1;
    }
#endif
    int startup_status = 1;
    MdHostAccess access;
    if (md_host_access_init(&access)) {
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        return 1;
    }
    md_host_access_lifecycle(&access, managed, "starting");
    MdManagementIpc *management = NULL;
    /* Initialize session state */
    MdSession session;
    md_session_init(&session);

    /* Session log for agent monitoring (M2.4) */
    MdSessionLogConfig slog_cfg = {
        .signer  = signer,
        .publish = (signer != NULL),  /* publish if signer available */
    };
    MdSessionLog *session_log = md_session_log_create(&slog_cfg);
    if (session_log)
        printf("host: session log ready\n");

    /* Host context (needed before TCP for nostr callbacks) */
    HostCtx ctx = {
        .session     = &session,
        .session_log = session_log,
        .capture_required = do_capture,
        .quality_policy = {capture_max_w, capture_max_h, bitrate, fps},
        .access = &access,
        .attended = attended,
        .clipboard_policy=clipboard_policy,
        .supported_caps = MD_HOST_ACCESS_CAPS & (no_input ? ~MD_CAP_INPUT : MD_HOST_ACCESS_CAPS),
    };
    if (!md_clipboard_backend() || !clipboard_policy) ctx.supported_caps &= ~MD_CAP_CLIPBOARD;
    pthread_mutex_init(&ctx.client_mu, NULL);
    md_cond_init_monotonic(&ctx.capture_control_cv);
    if (expected_client_pk[0])
        strncpy(ctx.expected_client_pk, expected_client_pk, sizeof(ctx.expected_client_pk) - 1);

    /* ── Nostr bridge (if signer available) ───────────────── */
    MdNostr *nostr = NULL;
    if (signer) {
        if (relay_count == 0) {
            relay_urls[0] = MD_DEFAULT_RELAY;
            relay_count = 1;
        }

        MdNostrCallbacks nostr_cbs = { 0 };
        nostr_cbs.on_dm = host_on_dm;
        nostr_cbs.dm_userdata = &ctx;
        nostr_cbs.on_owner_revoked = host_owner_revoked;
        nostr_cbs.owner_userdata = &ctx;
        /* Callbacks may fire during create. */
        ctx.session_req_ch = go_channel_create(1);

        MdNostrConfig nostr_cfg = {
            .signer = signer,
            .owner = account.binding,
            .relay_urls = relay_urls,
            .relay_count = relay_count,
        };

        nostr = md_nostr_create(&nostr_cfg, &nostr_cbs);
        if (nostr) {
            pthread_mutex_lock(&g_host_request_mu);
            ctx.nostr = nostr;
            pthread_mutex_unlock(&g_host_request_mu);

            /* Wire nostr into session log for relay publishing */
            if (session_log)
                md_session_log_set_nostr(session_log, nostr);

#ifndef _WIN32
            char *host_key = NULL;
            if (md_signer_get_pubkey(signer, &host_key) == 0)
                management = (managed ? md_management_start_managed : md_management_start)(&access, nostr, host_key, management_socket,
                    ctx.supported_caps, host_management_wake, &ctx);
            free(host_key);
            if (!management) {
                fprintf(stderr, "host: management endpoint unavailable/unsafe; use --management-socket with a private canonical parent\n");
                if (managed || attended || management_socket) goto signaling_failed;
            }
#endif
            /* Subscribe to allowlist updates */
            md_nostr_refresh_allowlist(nostr);

            /* Only live (post-EOSE) requests are answered; announce readiness
             * at the catch-up -> live boundary, not before it. */
            if (md_nostr_wait_history(nostr, 10000) != 0)
                fprintf(stderr, "host: WARNING: relay history catch-up (EOSE) not "
                        "confirmed; answering live session requests only\n");

            md_host_access_lifecycle(&access, managed, "waiting");
            md_host_lifecycle_event("waiting", 0);
            printf("host: nostr bridge ready, waiting for session requests...\n");

            while (host_wait_request(&ctx) &&
                (!account.binding || md_account_binding_is_current(account.binding))) {
                if (fips_npub) {
                    printf("host: waiting for FIPS peer readiness for %.*s...\n",
                           12, fips_npub);
                    MdFipsPeerReadiness ready;
                    MdFipsPeerReadinessState rstate = md_fips_control_wait_peer_ready(
                        fips_control_socket, fips_npub, fips_ready_timeout_ms,
                        MD_FIPS_CONTROL_DEFAULT_PEER_POLL_MS, &ready);
                    if (rstate != MD_FIPS_PEER_READY) {
                        if (rstate == MD_FIPS_PEER_NOT_FOUND)
                            host_print_fips_setup_guidance(fips_npub, &ready);
                        else
                            fprintf(stderr, "ERROR: FIPS route not ready (%s): %s\n",
                                    md_fips_peer_readiness_string(rstate), ready.detail);
                        goto signaling_failed;
                    }
                    printf("host: FIPS peer ready: %s\n", ready.detail);
                }

                if (host_accept_fresh_request(&ctx, nostr, no_input) == 0) break;
                host_reject_request(&ctx, ctx.pending_req.output[0] ? "output_unavailable" : ctx.pending_req.stream_tls ? "request_refused" : "tls_required");
                host_rearm_request(&ctx);
            }
        } else {
            if (fips_npub || account.binding) {
                fprintf(stderr, "ERROR: nostr bridge creation failed; authorized hosting cannot fall back to direct TCP\n");
                goto signaling_failed;
            }
            fprintf(stderr, "ERROR: nostr bridge creation failed\n");
        }
    }

    if (!host_running() || session.state != MD_SESSION_NEGOTIATING) {
        fprintf(stderr, "ERROR: negotiated stream authorization required; direct TCP disabled\n");
        goto signaling_failed;
    }
    goto signaling_ready;
signaling_failed:
    free(ctx.quality_pixels);
    md_host_access_lifecycle(&access, managed, "stopping");
    free(ctx.cached_frame.data);
    md_nostr_stop(nostr);
    md_management_stop(management);
    md_nostr_destroy(nostr);
    if (ctx.session_req_ch) {
        go_channel_close(ctx.session_req_ch);
        go_channel_unref(ctx.session_req_ch);
    }
    md_session_log_destroy(session_log);
    md_signer_destroy(signer);
    md_account_startup_clear(&account);
    md_host_access_destroy(&access);
    pthread_cond_destroy(&ctx.capture_control_cv);
    pthread_mutex_destroy(&ctx.client_mu);
    return startup_status;
signaling_ready:
    ;
    /* Create TCP server */
    MdStreamTlsConfig stream_tls = { .enabled = true };
    MdStreamServer *srv = md_stream_server_create_tls(bind_addr, port, &stream_tls);
    if (!srv) {
        fprintf(stderr, "ERROR: failed to bind TCP server on port %u\n", port);
        goto signaling_failed;
    }
    if (md_stream_server_tls_fingerprint(srv, ctx.tls_cert_sha256) != 0) {
        md_stream_server_destroy(srv);
        goto signaling_failed;
    }
    md_host_access_lifecycle(&access, managed, "serving");
    printf("host: listening on port %u (pinned TLS 1.3 required)\n", port);

    /* Start capture first so encoder/input dimensions match the negotiated
     * capture size instead of assuming a fixed desktop resolution. */
    MdCaptureCtx *capture = NULL;
    pthread_t cap_thread = 0;
    bool cap_thread_started = false;
    CaptureThread cap_thr_ctx = {0};
    uint32_t display_width = 0;
    uint32_t display_height = 0;

    if (do_capture) {
        strcpy(ctx.startup_output,ctx.pending_req.output);
        MdCaptureConfig cap_cfg = {
            .no_interaction = managed,
            .target_fps  = fps,
            .show_cursor = true,
            .unattended = capture_unattended,
            .forget_authorization_on_stall = capture_forget_on_stall,
            .consent_timeout_ms = capture_consent_timeout_ms,
            .output = ctx.startup_output[0] ? ctx.startup_output : capture_output,
            .max_width = capture_max_w,
            .max_height = capture_max_h,
        };
        capture = md_capture_create(&cap_cfg);
        int capture_status = host_start_capture(capture, &display_width, &display_height, &ctx);
        if (capture_status != 0) {
            fprintf(stderr, "ERROR: capture unavailable: authorize screen sharing and retry; "
                    "refusing video session (use --no-capture only for fixtures)\n");
            if (managed && capture_status == MD_CAPTURE_INTERACTION_REQUIRED) {
                startup_status = MD_HOST_EXIT_CONFIG;
                md_host_access_lifecycle(&access, managed, "needs_interaction");
                md_host_lifecycle_event("needs_interaction", startup_status);
            }
            host_reject_request(&ctx, "capture_unavailable");
            if (capture) md_capture_destroy(capture);
            md_stream_server_destroy(srv);
            goto signaling_failed;
        }
        md_capture_outputs(&ctx.outputs);
        MdCaptureRect selected_rect={0};
        if (ctx.pending_req.output[0]) strcpy(ctx.outputs.selected,ctx.pending_req.output);
        else if (capture_output && !strcmp(capture_output,"all")) strcpy(ctx.outputs.selected,"all");
        else if (!md_capture_get_output_rect(capture,&selected_rect)) {
            for (size_t i=0;i<ctx.outputs.count;i++) if (!memcmp(&selected_rect,&ctx.outputs.items[i].rect,sizeof(selected_rect)))
                strcpy(ctx.outputs.selected,ctx.outputs.items[i].id);
        }
        printf("host: capture ready (%ux%u, first frame received)\n",
               display_width, display_height);
    }

    if (!host_dimensions_are_valid(display_width, display_height)) {
        host_apply_fallback_dimensions(&display_width, &display_height,
                                       do_capture ? "capture unavailable"
                                                  : "capture disabled");
    }

    /* Initialize encoder (persistent across reconnections).
     * NV12/H.264 need even dimensions; a display with an odd edge (e.g. a
     * 1512x909 VM screen) is cropped by one row/column at encode time, while
     * input injection keeps the true display size. */
    MdQuality baseline=do_capture ? md_quality_clamp(&(MdQuality){0},&ctx.quality_policy) : ctx.quality;
    uint32_t quality_w, quality_h;
    md_capture_fit_size(display_width, display_height, baseline.max_width,
        baseline.max_height, &quality_w, &quality_h);
    uint32_t enc_width  = quality_w & ~1u;
    uint32_t enc_height = quality_h & ~1u;
    MdEncoderConfig enc_cfg = {
        .width        = enc_width,
        .height       = enc_height,
        .bitrate      = baseline.bitrate,
        .fps          = baseline.fps,
        .prefer_nvenc = use_nvenc,
        .openh264_path = openh264_path,
        .encoder_name = encoder_name,
    };

    MdEncoder *encoder = md_encoder_create(&enc_cfg);
    if (!encoder) {
        fprintf(stderr, "ERROR: failed to create encoder\n");
        if (capture) md_capture_destroy(capture);
        md_stream_server_destroy(srv);
        host_reject_request(&ctx, "capture_unavailable");
        goto signaling_failed;
    }
    printf("host: encoder ready (%s, %ux%u)\n",
           md_encoder_get_name(encoder),
           enc_width, enc_height);

    /* Initialize input injection (persistent) */
    MdInputConfig input_cfg = input_options;
    input_cfg.screen_width = display_width;
    input_cfg.screen_height = display_height;
    MdCaptureRect output_rect = {0};
    if (md_capture_get_output_rect(capture, &output_rect) == 0) {
        input_cfg.output_x = output_rect.x;
        input_cfg.output_y = output_rect.y;
        input_cfg.output_width = output_rect.width;
        input_cfg.output_height = output_rect.height;
    }
    MdInput *input = no_input ? NULL : md_input_create(&input_cfg);
    if (!input || !md_input_is_ready(input)) {
        if (managed && !no_input) {
            fprintf(stderr, "ERROR: managed input unavailable; use --no-input for a read-only host or authorize an interactive host\n");
            startup_status = MD_HOST_EXIT_CONFIG;
            md_host_access_lifecycle(&access, managed, "needs_interaction");
            md_host_lifecycle_event("needs_interaction", startup_status);
            md_input_destroy(input);
            md_encoder_destroy(encoder);
            if (capture) md_capture_destroy(capture);
            md_stream_server_destroy(srv);
            host_reject_request(&ctx, "input_unavailable");
            goto signaling_failed;
        }
        fprintf(stderr, "WARNING: input injection unavailable\n");
    } else {
        printf("host: input injection ready (%ux%u)\n",
               display_width, display_height);
    }

    /* Initialize accessibility tree walker (persistent) */
    MdA11yCtx *a11y = md_a11y_create();
    if (!md_a11y_is_connected(a11y)) {
        fprintf(stderr, "WARNING: accessibility tree unavailable — agent mode disabled\n");
        md_a11y_destroy(a11y);
        a11y = NULL;
    } else {
        printf("host: accessibility tree connected\n");
    }

    /* Initialize agent action handler (persistent) */
    MdAgentConfig agent_cfg = {
        .a11y        = a11y,
        .input       = input,
        .tree_format = session.tree_format,
        .settle_ms   = MD_AGENT_DEFAULT_SETTLE_MS,
    };
    MdAgent *agent = md_agent_create(&agent_cfg);

    /* Set persistent host context fields */
    ctx.agent   = agent;
    ctx.input   = input;  /* raw human input injection for MD_PKT_INPUT */
    /* Packet dispatch (and the agent's actions) run under client_mu, so the
     * capture thread retargets input under it too. */
    host_capture_sync_init(&ctx.sync, encoder, input, &ctx.client_mu,
                           &ctx.pkt_seq,
                           display_width, display_height, &output_rect);

    ctx.applied_quality=baseline;
    ctx.applied_quality_epoch=ctx.control_epoch;
    ctx.quality_pending=do_capture && memcmp(&baseline,&ctx.quality,sizeof(baseline));
    if (!ctx.quality_pending) ctx.applied_quality_revision=ctx.quality_revision;
    /* Capture thread starts only after encoder/input are ready. */
    if (capture) {
        cap_thr_ctx.host    = &ctx;
        cap_thr_ctx.capture = capture;
        if (pthread_create(&cap_thread, NULL, capture_thread_func,
                           &cap_thr_ctx) == 0) {
            cap_thread_started = true;
        } else {
            fprintf(stderr, "ERROR: failed to start capture thread\n");
        }
    }

    /* No remote client can start its bind/ACK wait before this point. */
    int exit_status = 0;
    ctx.capture_ready = cap_thread_started;
    if (host_publish_session_accept(&ctx, account.binding) != 0) {
        fprintf(stderr, "ERROR: session acceptance failed or host role revoked\n");
        host_reject_request(&ctx, ctx.quality_error ? "quality_unsupported" : ctx.capture_required &&
            (!ctx.capture_ready || atomic_load(&ctx.capture_failed))
            ? "capture_unavailable" : "request_refused");
        if (host_running() && !atomic_load(&ctx.capture_failed) &&
            (!ctx.capture_required || ctx.capture_ready) &&
            (!account.binding || md_account_binding_is_current(account.binding))) {
            host_rearm_request(&ctx);
        } else {
            exit_status = 1;
            g_running = 0; /* Use the normal backend cleanup below. */
        }
    }

    /* ══════════════════════════════════════════════════════════════
     * Client accept + stream loop
     *
     * The host accepts a client, streams until disconnect, then
     * loops back to accept the next connection.  Persistent resources
     * (encoder, capture, a11y, agent) stay alive across reconnections.
     * ══════════════════════════════════════════════════════════════ */
    uint32_t client_num = 0;

    while (host_running() && !atomic_load(&ctx.capture_failed)) {
        /* A consumed/expired grant must never be reused. Refused negotiations
         * return to the same long-lived DM subscription, not process exit. */
        if (!host_has_request(&ctx)) {
            printf("host: ready for next authorized session request\n");
            if (host_next_session(&ctx, nostr, no_input, account.binding,
                                   &agent, &agent_cfg) != 0) {
                if (host_running()) exit_status = 1;
                break;
            }
        }
        /* Wait for client connection */
        printf("host: waiting for client...\n");
        MdStream *client = NULL;
        while (host_running() && !atomic_load(&ctx.capture_failed) && !client &&
               host_binding_pending(&ctx, (uint64_t)md_now_ms())) {
            client = md_stream_server_accept(srv, 1000);
        }
        if (!host_running() || atomic_load(&ctx.capture_failed)) {
            md_stream_destroy(client);
            break;
        }
        if (!client) {
            host_rearm_request(&ctx); /* abandoned binding reservation */
            continue;
        }

        client_num++;
        printf("host: client #%u connected\n", client_num);

        /* Log connection */
        if (session_log)
            md_session_log_event(session_log, MD_SESSION_LOG_CONNECT,
                                 session.session_id,
                                 session.peer_npub[0] ? session.peer_npub : NULL,
                                 "TCP client connected");

        /* A candidate never reaches capture/tree/dispatch. Failed and reused
         * proofs close only that candidate, without replacing the active one. */
        if (host_bind_candidate(&ctx, client) != 0) {
            fprintf(stderr, "host: rejected unproven client #%u\n", client_num);
            md_stream_destroy(client);
            continue;
        }

        /* Reset and publish only after the authoritative bind ACK. Serialize
         * all host sends with capture so headers/payloads cannot interleave. */
        pthread_mutex_lock(&ctx.client_mu);
        ctx.client     = client;
        ctx.clipboard=md_clipboard_create(session.capabilities,md_clipboard_reverse(session.clipboard_directions));
        if ((session.capabilities&MD_CAP_CLIPBOARD) && !ctx.clipboard) {
            fprintf(stderr,"host: %s\n",md_clipboard_unavailable_reason()); md_stream_close(client);
        }
        ctx.pkt_seq    = 0;
        host_capture_sync_reset_sequence(&ctx.sync);
        ctx.frames_sent = 0;
        ctx.total_encode_us = 0;
        ctx.frames_encoded  = 0;

        /* The encoder is persistent across connections, so a new client joins
         * mid-GOP with no decodable entry point. Force an IDR for it. */
        host_request_initial_frame(&ctx);

        pthread_mutex_unlock(&ctx.client_mu);

        /* No stream/client lock is held during the potentially slow OS walk.
         * This connection owns the job and joins it before backend replacement. */
        MdAgentTreeJob *initial_tree = host_send_initial_tree(&ctx);

        /* ── Streaming loop ─────────────────────────────────────── */
        printf("host: streaming to client #%u (Ctrl+C to stop)\n", client_num);

        uint32_t last_stats_ms = md_stream_now_ms();
        uint32_t last_ping_ms = last_stats_ms;

        /* Adaptive bitrate controller for this connection */
        MdBitrateCtrlConfig br_cfg = {
            .max_bitrate = bitrate,
            .min_bitrate = bitrate / 16,  /* 1/16 of max as floor */
            .initial_bitrate = bitrate,
        };
        MdBitrateCtrl *br_ctrl = md_bitrate_ctrl_create(&br_cfg);
        uint32_t last_br = bitrate;

        while (host_running() && !atomic_load(&ctx.capture_failed) &&
               (!ctx.access || md_host_access_valid(ctx.access, ctx.pending_req.request_id)) &&
               md_stream_is_connected(client)) {
            char *clipboard_text=NULL; size_t clipboard_size=0;
            int clipboard_result=md_clipboard_poll(ctx.clipboard,&clipboard_text,&clipboard_size);
            if (clipboard_result==1) {
                pthread_mutex_lock(&ctx.client_mu);
                if ((!ctx.access || md_host_access_valid(ctx.access,ctx.pending_req.request_id)) &&
                    md_stream_has_capability(client,MD_CAP_CLIPBOARD)) {
                    if (md_stream_send(client,MD_PKT_CLIPBOARD,ctx.pkt_seq++,(uint8_t*)clipboard_text,(uint32_t)clipboard_size)) md_stream_close(client);
                    else fprintf(stderr,"host: clipboard sent (%zu bytes)\n",clipboard_size);
                }
                pthread_mutex_unlock(&ctx.client_mu); free(clipboard_text);
            } else if (clipboard_result<0) fprintf(stderr,"host: clipboard read rejected (size, UTF-8 or platform unavailable)\n");
            char *tree = NULL;
            if (initial_tree && md_agent_tree_take(initial_tree, &tree) == 1) {
                free(md_agent_tree_finish(initial_tree));
                initial_tree = NULL;
                if (tree) {
                    pthread_mutex_lock(&ctx.client_mu);
                    if ((!ctx.access || md_host_access_valid(ctx.access, ctx.pending_req.request_id)) &&
                        md_stream_has_capability(client, MD_CAP_AGENT))
                        md_stream_send(client, MD_PKT_UI_TREE, ctx.pkt_seq++,
                                       (const uint8_t *)tree, (uint32_t)strlen(tree));
                    pthread_mutex_unlock(&ctx.client_mu);
                    free(tree);
                }
            }
            /* Never leave a second connection waiting to inherit this grant. */
            MdStream *extra = md_stream_server_accept(srv, 1);
            if (extra) {
                host_bind_candidate(&ctx, extra);
                md_stream_destroy(extra);
            }
            /* Check for incoming packets from client (non-blocking) */
            MdPacketHeader hdr;
            uint8_t *payload = NULL;
            int ret = md_stream_recv(client, &hdr, &payload, 10 /* 10ms poll */);

            if (ret == 0) {
                /* Do not publish a pre-action snapshot after an action update.
                 * Join outside client_mu so video continues during the wait. */
                if (hdr.type == MD_PKT_ACTION && initial_tree) {
                    free(md_agent_tree_finish(initial_tree));
                    initial_tree = NULL;
                }
                pthread_mutex_lock(&ctx.client_mu);
                handle_client_packet(&ctx, &hdr, payload);
                pthread_mutex_unlock(&ctx.client_mu);
                free(payload);
            } else if (ret < 0) {
                /* Connection lost */
                break;
            }
            /* ret == 1: timeout, no data — continue */

            uint32_t now = md_stream_now_ms();

            /* Send periodic ping for RTT measurement */
            if (now - last_ping_ms >= 1000) {
                pthread_mutex_lock(&ctx.client_mu);
                md_stream_send_ping(client);
                pthread_mutex_unlock(&ctx.client_mu);
                last_ping_ms = now;

                /* Feed RTT to bitrate controller */
                if (br_ctrl && encoder) {
                    MdStreamStats st;
                    md_stream_get_stats(client, &st);
                    if (st.avg_rtt_ms > 0) {
                        pthread_mutex_lock(&ctx.client_mu);
                        md_bitrate_ctrl_set_max(br_ctrl, ctx.quality.bitrate);
                        pthread_mutex_unlock(&ctx.client_mu);
                        uint32_t new_br = md_bitrate_ctrl_update(
                            br_ctrl, st.avg_rtt_ms, now);
                        if (new_br != last_br) {
                            host_capture_sync_request_bitrate(&ctx.sync,
                                                              new_br);
                            last_br = new_br;
                            printf("host: bitrate adjusted to %u bps "
                                   "(rtt_avg=%ums, action=%s)\n",
                                   new_br, st.avg_rtt_ms,
                                   md_bitrate_ctrl_get_action(br_ctrl)
                                       == MD_BITRATE_DECREASE ? "decrease"
                                   : md_bitrate_ctrl_get_action(br_ctrl)
                                       == MD_BITRATE_INCREASE ? "increase"
                                   : "hold");
                        }
                    }
                }
            }

            /* Print stats every 5 seconds */
            if (now - last_stats_ms >= 5000) {
                MdStreamStats stats;
                md_stream_get_stats(client, &stats);

                pthread_mutex_lock(&ctx.client_mu);
                uint32_t encoded=ctx.frames_encoded, sent=ctx.frames_sent;
                double avg_encode_ms = ctx.frames_encoded > 0
                    ? (double)ctx.total_encode_us / ctx.frames_encoded / 1000.0
                    : 0.0;
                pthread_mutex_unlock(&ctx.client_mu);

                printf("host: [#%u] frames=%u sent=%u encode_avg=%.1fms "
                       "rtt=%ums rtt_avg=%ums tx=%.1fMB rx=%.1fKB\n",
                       client_num,
                       encoded, sent,
                       avg_encode_ms,
                       stats.last_rtt_ms, stats.avg_rtt_ms,
                       (double)stats.bytes_sent / (1024.0 * 1024.0),
                       (double)stats.bytes_recv / 1024.0);

                last_stats_ms = now;
            }
        }

        pthread_mutex_lock(&ctx.client_mu);
        md_stream_close(client);
        pthread_mutex_unlock(&ctx.client_mu);
        free(md_agent_tree_finish(initial_tree));

        /* Client disconnected — clean up per-connection resources */
        md_bitrate_ctrl_destroy(br_ctrl);
        br_ctrl = NULL;

        /* The next session's quality revision retunes the persistent encoder.
         * Never restore a host ceiling over a still-applied client ceiling. */

        /* Log disconnection */
        if (session_log) {
            char disc_detail[128];
            snprintf(disc_detail, sizeof(disc_detail),
                     "disconnected after %u frames", ctx.frames_sent);
            md_session_log_event(session_log, MD_SESSION_LOG_DISCONNECT,
                                 session.session_id,
                                 session.peer_npub[0] ? session.peer_npub : NULL,
                                 disc_detail);
        }

        printf("host: client #%u disconnected (sent %u frames)\n",
               client_num, ctx.frames_sent);

        /* Do NOT flush the encoder here: md_encoder_flush() sends an EOF to the
         * persistent encoder, which then refuses further frames and leaves the
         * next client with frames=0. Any buffered packets would only go to the
         * client that just left. The next client gets a forced keyframe above. */

        host_finish_client(&ctx, client);

    }

    if (atomic_load(&ctx.capture_failed)) {
        fprintf(stderr, "ERROR: capture failed permanently; ending host session\n");
        exit_status = 1;
        g_running = 0;
    }

    md_host_access_lifecycle(&access, managed, "stopping");
    md_host_lifecycle_event("stopping", 0);

    /* ── Shutdown ────────────────────────────────────────────────── */
    printf("\nhost: shutting down...\n");
    atomic_store(&ctx.capture_stopping, true);

    if (capture) {
        if (cap_thread_started) {
            pthread_join(cap_thread, NULL);
            capture=cap_thr_ctx.capture;
        }
        md_capture_destroy(capture);
        free(cap_thr_ctx.owned_output);
    }

    free(ctx.quality_pixels);
    free(ctx.cached_frame.data);
    md_encoder_destroy(encoder);

    if (agent) {
        printf("host: handled %u agent actions\n", md_agent_get_action_count(agent));
        md_agent_destroy(agent);
    }
    if (a11y)
        md_a11y_destroy(a11y);
    if (input)
        md_input_destroy(input);

    md_stream_server_destroy(srv);

    md_nostr_stop(nostr);
    md_management_stop(management);
    md_nostr_destroy(nostr);
    if (ctx.session_req_ch) {
        go_channel_close(ctx.session_req_ch);
        go_channel_unref(ctx.session_req_ch);
    }
    md_session_log_destroy(session_log);
    md_signer_destroy(signer);
    md_account_startup_clear(&account);

    md_host_access_destroy(&access);
    pthread_cond_destroy(&ctx.capture_control_cv);
    pthread_mutex_destroy(&ctx.client_mu);

    printf("host: done. served %u client(s)\n", client_num);
    return exit_status;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc == 2 && !strcmp(argv[1], "--managed-stop")) return md_host_lifecycle_stop();
#endif
    int status = host_run(argc, argv);
    /* Drain the process-wide network runtime before ExitProcess kills its
     * threads. DLL atexit handlers run too late to join them on Windows. */
    md_nostr_shutdown();
    return md_host_lifecycle_finish(status);
}
