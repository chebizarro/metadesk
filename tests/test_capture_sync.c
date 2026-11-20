/*
 * metadesk — tests/test_capture_sync.c
 * md-ezto / md-pvlj: the host's encoder size and input bounds follow the
 * capture across restarts, mode changes and display rearrangement.
 *
 * A mock capture backend changes its frame size and published output
 * rectangle mid-stream (a "lost" duplication that the pump restarts, as the
 * host watchdog does). The real encoder/decoder run, and a mock input
 * backend records where frame points land on the desktop.
 *
 * md-f1oo: a backend may also republish its rectangle (and size) from
 * get_frame without a restart, as ScreenCaptureKit does when the captured
 * display moves or changes mode. md-n9p2: a size-capped capture delivers
 * frames smaller than the display; input maps onto it unchanged.
 *
 * md-383x: input the client mapped onto a frame it was still showing when
 * the host changed geometry lands where that frame showed, or is dropped,
 * never interpreted against the new frame.
 *
 * md-f7uc: a packet is attributed to the geometry its frame was captured
 * under, also when an asynchronous encoder emits it after the change (a
 * mock delay line here, and VideoToolbox itself on macOS).
 */
#include "capture_sync.h"
#include "action.h"
#include "decode.h"
#include "input_event.h"
#include "packet.h"
#include "stream.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "md_test.h"

#define REQUIRE(cond) do { \
    int ok_ = !!(cond); \
    MD_CHECK_MSG(ok_, #cond); \
    if (!ok_) exit(md_test_report()); \
} while (0)

/* ── Geometries ───────────────────────────────────────────────── */

typedef struct {
    uint32_t w, h;
    MdCaptureRect rect;
} Geometry;

/* G0: a 2x HiDPI display at the origin (frame pixels, rect in points).
 * G1: a mode change onto a display left of the main one; the odd height is
 *     cropped to an even encoder size while input keeps the true height.
 * G2: the same mode, the display rearranged to the right (rect only). */
static const Geometry G0 = { 640, 360, { 0, 0, 320, 180 } };
static const Geometry G1 = { 480, 271, { -1920, 120, 1920, 1084 } };
static const Geometry G2 = { 480, 271, { 3000, 0, 1920, 1084 } };
/* GS: G0's display at 1x (the same output rectangle at half the size).
 * GB: another display in G0's mode: the same frame size, elsewhere. */
static const Geometry GS = { 320, 180, { 0, 0, 320, 180 } };
static const Geometry GB = { 640, 360, { 320, 0, 320, 180 } };

static uint8_t g_pixels[640 * 360 * 4];

/* ── Mock capture backend ─────────────────────────────────────── */

typedef struct {
    Geometry   cur;          /* published by start */
    Geometry   next;         /* what the next start publishes */
    atomic_bool lose;        /* next get_frame reports a lost duplication */
    bool       lost;
    /* Next get_frame republishes this geometry without a restart. */
    Geometry   republish;
    atomic_bool republish_req;
    uint32_t   seq;
    uint32_t   starts;
    uint32_t   frame_pause_us; /* emulated frame interval */
} MockCap;

static int mc_init(MdCaptureCtx *ctx) { (void)ctx; return 0; }

static int mc_start(MdCaptureCtx *ctx) {
    MockCap *m = ctx->backend_data;
    m->cur = m->next;
    m->lost = false;
    m->starts++;
    ctx->width = m->cur.w;
    ctx->height = m->cur.h;
    ctx->output_rect = m->cur.rect;
    ctx->active = true;
    return 0;
}

static int mc_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms) {
    (void)timeout_ms;
    MockCap *m = ctx->backend_data;
    if (atomic_exchange(&m->lose, false)) {
        m->lost = true;         /* like DXGI access loss: restart re-reads */
        ctx->active = false;
    }
    if (m->lost || !ctx->active) return MD_CAPTURE_ERROR;
    if (atomic_exchange(&m->republish_req, false)) {
        /* As SCK: published on the get_frame thread with the frame. */
        m->cur = m->republish;
        ctx->width = m->cur.w;
        ctx->height = m->cur.h;
        ctx->output_rect = m->cur.rect;
    }
    if (m->frame_pause_us) usleep(m->frame_pause_us);
    memset(out, 0, sizeof(*out));
    out->width = m->cur.w;
    out->height = m->cur.h;
    out->stride = m->cur.w * 4;
    out->format = MD_PIX_CAPTURE_BGRA;
    out->buf_type = MD_BUF_CPU;
    out->data = g_pixels;
    out->data_size = (size_t)out->stride * out->height;
    out->seq = m->seq++;
    return 0;
}

static void mc_release(MdCaptureCtx *ctx, MdFrame *f) { (void)ctx; (void)f; }
static void mc_stop(MdCaptureCtx *ctx) { ctx->active = false; }
static void mc_destroy(MdCaptureCtx *ctx) { (void)ctx; }

static const MdCaptureBackend mock_capture = {
    .init = mc_init, .start = mc_start, .get_frame = mc_get_frame,
    .release_frame = mc_release, .stop = mc_stop, .destroy = mc_destroy,
};

static void capture_open(MdCaptureCtx *cap, MockCap *m, const Geometry *g) {
    memset(cap, 0, sizeof(*cap));
    memset(m, 0, sizeof(*m));
    atomic_init(&m->lose, false);
    atomic_init(&m->republish_req, false);
    m->next = *g;
    cap->vtable = &mock_capture;
    cap->backend_data = m;
    REQUIRE(md_capture_start(cap) == 0);
}

/* Lose the duplication; the restart publishes g. */
static void capture_change(MockCap *m, const Geometry *g) {
    m->next = *g;
    atomic_store(&m->lose, true);
}

/* The stream keeps running; the next frame carries g. */
static void capture_republish(MockCap *m, const Geometry *g) {
    m->republish = *g;
    atomic_store(&m->republish_req, true);
}

/* ── Mock input backend ───────────────────────────────────────── */

typedef struct {
    int moves, clicks;
    double gx, gy;
} MockInput;

static int mi_move(MdInputCtx *ctx, int x, int y) {
    MockInput *mi = ctx->backend_data;
    md_input_map_to_output(&ctx->config, x, y, &mi->gx, &mi->gy);
    mi->moves++;
    return 0;
}

static int mi_button(MdInputCtx *ctx, int button, int pressed) {
    MockInput *mi = ctx->backend_data;
    (void)button;
    if (!pressed) mi->clicks++;
    return 0;
}

static const MdInputBackend mock_input_backend = {
    .mouse_move = mi_move, .mouse_button = mi_button,
};

static void input_open(MdInput *inp, MockInput *mi, const Geometry *g) {
    memset(inp, 0, sizeof(*inp));
    memset(mi, 0, sizeof(*mi));
    inp->vtable = &mock_input_backend;
    inp->backend_data = mi;
    inp->ready = true;
    inp->config.screen_width = g->w;
    inp->config.screen_height = g->h;
    inp->config.output_x = g->rect.x;
    inp->config.output_y = g->rect.y;
    inp->config.output_width = g->rect.width;
    inp->config.output_height = g->rect.height;
}

/* Desktop point that frame point (x, y) of g maps to. */
static void desktop_point(const Geometry *g, int x, int y,
                          double *gx, double *gy) {
    MdInputConfig c = {
        .screen_width = g->w, .screen_height = g->h,
        .output_x = g->rect.x, .output_y = g->rect.y,
        .output_width = g->rect.width, .output_height = g->rect.height,
    };
    md_input_map_to_output(&c, x, y, gx, gy);
}

/* Desktop point that the frame centre of g maps to. */
static void expect_center(const Geometry *g, double *gx, double *gy) {
    desktop_point(g, (int)g->w / 2, (int)g->h / 2, gx, gy);
}

/* ── Packet sink: what the client would decode ────────────────── */

#define MAX_SIZES 16

typedef struct {
    MdDecoder *dec;
    int packets;
    /* Each frame's tag by pts (the mock's frame seq), as pump submitted it,
     * and the tag of the frame being submitted now. */
    uint64_t frame_tag[64];
    uint64_t submit_tag;
    int untagged;         /* packets without their frame's tag */
    int late;             /* emitted while a later generation submitted */
    int keys;
    bool want_key;        /* armed by a resize/request */
    bool key_ok;          /* the first packet after arming was a keyframe */
    int  armed_checks;
    uint32_t sizes_w[MAX_SIZES], sizes_h[MAX_SIZES];
    int n_sizes;
    int decoded;
} Sink;

static void on_decoded(const MdDecodedFrame *f, void *ud) {
    Sink *s = ud;
    s->decoded++;
    if (s->n_sizes == 0 || s->sizes_w[s->n_sizes - 1] != f->width ||
        s->sizes_h[s->n_sizes - 1] != f->height) {
        if (s->n_sizes < MAX_SIZES) {
            s->sizes_w[s->n_sizes] = f->width;
            s->sizes_h[s->n_sizes] = f->height;
            s->n_sizes++;
        }
    }
}

static void on_encoded(const MdEncodedPacket *pkt, void *ud) {
    Sink *s = ud;
    s->packets++;
    if (!pkt->tag || pkt->tag != s->frame_tag[pkt->pts % 64]) s->untagged++;
    if (pkt->tag != s->submit_tag) s->late++;
    if (pkt->is_key) s->keys++;
    if (s->want_key) {
        s->key_ok = pkt->is_key;
        s->armed_checks++;
        s->want_key = false;
    }
    if (s->dec) {
        md_decoder_submit(s->dec, pkt->data, pkt->size, pkt->pts);
        md_decoder_poll(s->dec, on_decoded, s);
    }
}

/* One iteration of the host capture loop (src/host/main.c): a broken
 * stream is restarted, every frame is reconciled, matching frames encode. */
static int pump(HostCaptureSync *sync, MdCaptureCtx *cap, Sink *sink, int frames) {
    int encoded = 0;
    for (int guard = 0; encoded < frames && guard < frames * 4 + 8; guard++) {
        MdFrame f;
        int rc = md_capture_get_frame(cap, &f, 100);
        if (rc == MD_CAPTURE_ERROR) {
            md_capture_stop(cap);
            md_capture_start(cap);
            continue;
        }
        if (rc != 0) continue;
        uint32_t before = sync->resizes;
        if (host_capture_sync_frame(sync, cap, &f) && sync->encoder) {
            const uint64_t tag = host_capture_sync_frame_tag(sync);
            if (sink) {
                if (sync->resizes != before) sink->want_key = true;
                sink->frame_tag[f.seq % 64] = tag;
                sink->submit_tag = tag;
            }
            MD_CHECK(md_encoder_submit_tagged(sync->encoder, f.data, f.stride,
                                       MD_PIX_FMT_BGRA, (int64_t)f.seq, tag,
                                       on_encoded, sink) == 0);
            encoded++;
        }
        md_capture_release_frame(cap, &f);
    }
    return encoded;
}

static MdEncoder *encoder_for(const Geometry *g) {
    MdEncoderConfig cfg = {
        .width = g->w & ~1u, .height = g->h & ~1u,
        .bitrate = 2000000, .fps = 30,
    };
    MdEncoder *enc = md_encoder_create(&cfg);
    REQUIRE(enc != NULL);
    return enc;
}

/* ── Tests ────────────────────────────────────────────────────── */

static void test_restart_reconfigures(void) {
    printf("  test_restart_reconfigures... ");
    md_test_name = "restart_reconfigures";
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &G0);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    MdEncoder *enc = encoder_for(&G0);
    Sink sink = { .dec = md_decoder_create() };
    REQUIRE(sink.dec != NULL);

    HostCaptureSync sync;
    host_capture_sync_init(&sync, enc, &inp, &mu, NULL, G0.w, G0.h, &G0.rect);

    /* Steady state: nothing to reconcile. */
    MD_CHECK(pump(&sync, &cap, &sink, 4) == 4);
    MD_CHECK(sync.resizes == 0 && sync.bounds_updates == 0);
    MD_CHECK(sink.packets > 0);

    /* Restart into a new mode on another display. */
    capture_change(&mc, &G1);
    MD_CHECK(pump(&sync, &cap, &sink, 6) == 6);
    MD_CHECK(mc.starts == 2);
    MD_CHECK(sync.resizes == 1);
    MD_CHECK(sync.bounds_updates == 1);
    uint32_t ew = 0, eh = 0;
    MD_CHECK(md_encoder_get_size(enc, &ew, &eh) == 0);
    MD_CHECK(ew == 480 && eh == 270);          /* odd edge cropped (md-b7j) */
    MD_CHECK(sink.armed_checks == 1 && sink.key_ok);
    MD_CHECK(inp.config.screen_width == 480 && inp.config.screen_height == 271);
    MD_CHECK(inp.config.output_x == -1920 && inp.config.output_width == 1920);

    double ex, ey;
    expect_center(&G1, &ex, &ey);
    MD_CHECK(md_input_mouse_move(&inp, 240, 135) == 0);
    MD_CHECK(mi.gx == ex && mi.gy == ey);
    MD_CHECK(mi.gx == -960 && mi.gy == 660);

    /* Rearranged, same mode: input retargets, the encoder is untouched. */
    capture_change(&mc, &G2);
    MD_CHECK(pump(&sync, &cap, &sink, 3) == 3);
    MD_CHECK(sync.resizes == 1);
    MD_CHECK(sync.bounds_updates == 2);
    MD_CHECK(md_input_mouse_move(&inp, 240, 135) == 0);
    MD_CHECK(mi.gx == 3000 + 960 && mi.gy == 540);

    /* The decoder followed the size change mid-stream. */
    md_decoder_flush(sink.dec, on_decoded, &sink);
    MD_CHECK(sink.n_sizes == 2);
    MD_CHECK(sink.sizes_w[0] == 640 && sink.sizes_h[0] == 360);
    MD_CHECK(sink.sizes_w[sink.n_sizes - 1] == 480 &&
             sink.sizes_h[sink.n_sizes - 1] == 270);

    /* Each packet carried its own frame's generation, including any the
     * encoder emitted after the rearrangement (md-f7uc). */
    MD_CHECK(sink.untagged == 0);

    md_decoder_destroy(sink.dec);
    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    printf("OK (%d packets, %d keyframes, %d emitted after a change)\n",
           sink.packets, sink.keys, sink.late);
}

/* Keyframe and bitrate requests from other threads are applied by the
 * capture thread, which owns the encoder. */
static void test_requests_apply_on_capture_thread(void) {
    printf("  test_requests_apply_on_capture_thread... ");
    md_test_name = "requests";
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    MdEncoder *enc = encoder_for(&G0);
    Sink sink = {0};
    HostCaptureSync sync;
    host_capture_sync_init(&sync, enc, NULL, NULL, NULL, G0.w, G0.h, &G0.rect);

    MD_CHECK(pump(&sync, &cap, &sink, 3) == 3);
    host_capture_sync_request_bitrate(&sync, 1500000);
    host_capture_sync_request_keyframe(&sync);
    MD_CHECK(md_encoder_get_bitrate(enc) == 2000000); /* not yet applied */
    const int keys_before = sink.keys;
    MD_CHECK(pump(&sync, &cap, &sink, 3) == 3);
    MD_CHECK(md_encoder_get_bitrate(enc) == 1500000);
    MD_CHECK(sink.keys > keys_before);

    /* A resize keeps the adapted bitrate. */
    capture_change(&mc, &G1);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);
    MD_CHECK(sync.resizes == 1);
    MD_CHECK(md_encoder_get_bitrate(enc) == 1500000);

    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    printf("OK\n");
}

/* Degenerate frames are dropped without retargeting anything, and a
 * missing encoder still keeps input current. */
static void test_degenerate_frames(void) {
    printf("  test_degenerate_frames... ");
    md_test_name = "degenerate";
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &G0);
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    HostCaptureSync sync;
    host_capture_sync_init(&sync, NULL, &inp, NULL, NULL, G0.w, G0.h, &G0.rect);

    MdFrame tiny = { .width = 16, .height = 16 };
    MD_CHECK(!host_capture_sync_frame(&sync, &cap, &tiny));
    MD_CHECK(sync.bounds_updates == 0 && inp.config.screen_width == G0.w);
    MD_CHECK(!host_capture_sync_frame(&sync, &cap, NULL));

    /* Without an encoder frames are never submittable, input still follows. */
    capture_change(&mc, &G1);
    MD_CHECK(pump(&sync, &cap, NULL, 1) == 0);
    MD_CHECK(sync.bounds_updates == 1);
    MD_CHECK(inp.config.screen_width == G1.w);

    /* A backend that publishes no rectangle (PipeWire) clears it: legacy
     * frame == global mapping. */
    cap.output_rect = (MdCaptureRect){0};
    MdFrame f = { .width = G1.w, .height = G1.h };
    MD_CHECK(!host_capture_sync_frame(&sync, &cap, &f));
    MD_CHECK(sync.bounds_updates == 2 && inp.config.output_width == 0);
    md_capture_stop(&cap);
    printf("OK\n");
}

/* A size the encoder cannot open is dropped without a retry per frame, the
 * old encoder survives, and the size is retried once geometry changes. */
static void test_resize_failure_recovers(void) {
    printf("  test_resize_failure_recovers... ");
    md_test_name = "resize_failure";
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    cap.output_rect = (MdCaptureRect){0};
    MdEncoder *enc = encoder_for(&G0);
    HostCaptureSync sync;
    host_capture_sync_init(&sync, enc, NULL, NULL, NULL, G0.w, G0.h, NULL);

    /* Beyond FFmpeg's image size limit: every codec in the cascade refuses. */
    const MdFrame a = { .width = G0.w, .height = G0.h };
    const MdFrame huge = { .width = 65536, .height = 65536 };
    MD_CHECK(host_capture_sync_frame(&sync, &cap, &a));
    MD_CHECK(!host_capture_sync_frame(&sync, &cap, &huge));
    MD_CHECK(sync.failed_w == 65536 && sync.failed_h == 65536);
    MD_CHECK(!host_capture_sync_frame(&sync, &cap, &huge));
    uint32_t ew = 0, eh = 0;
    MD_CHECK(md_encoder_get_size(enc, &ew, &eh) == 0 && ew == G0.w && eh == G0.h);
    MD_CHECK(sync.resizes == 0);

    /* Back to a size the encoder has: frames flow, the failure is forgotten,
     * and the next change to the failed size tries again. */
    MD_CHECK(host_capture_sync_frame(&sync, &cap, &a));
    MD_CHECK(sync.failed_w == 0 && sync.failed_h == 0);
    MD_CHECK(!host_capture_sync_frame(&sync, &cap, &huge));
    MD_CHECK(sync.failed_w == 65536);

    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    printf("OK\n");
}

/* ── Helpers for input across geometry changes (md-383x) ──────── */

static bool rect_eq(const MdCaptureRect *a, const MdCaptureRect *b) {
    return a->x == b->x && a->y == b->y &&
           a->width == b->width && a->height == b->height;
}

/* The capture thread's first frame at g (no encoder: input still follows). */
static void apply_geometry(HostCaptureSync *sync, MdCaptureCtx *cap,
                           const Geometry *g) {
    cap->output_rect = g->rect;
    MdFrame f = { .width = g->w, .height = g->h };
    (void)host_capture_sync_frame(sync, cap, &f);
}

/* As host on_encoded, for n video packets of frames captured at the current
 * geometry (a synchronous encoder): each takes the next sequence. */
static void send_packets(HostCaptureSync *sync, uint32_t *seq, int n) {
    for (int i = 0; i < n; i++)
        host_capture_sync_packet_sent(sync, host_capture_sync_frame_tag(sync),
                                      (*seq)++);
}

/* The first sequence of the newest run of packets. */
static uint32_t run_start(const HostCaptureSync *sync) {
    REQUIRE(sync->runs_total > 0);
    return sync->runs[(sync->runs_total - 1) % HOST_CAPTURE_SYNC_RUNS].first_seq;
}

static HostPointMap map_on(HostCaptureSync *sync, const Geometry *shown,
                           uint32_t seq, int x, int y, int *ox, int *oy) {
    return host_capture_sync_map_point(sync, (uint16_t)(shown->w & ~1u),
                                       (uint16_t)(shown->h & ~1u), seq,
                                       x, y, ox, oy);
}

static double abs_d(double v) { return v < 0 ? -v : v; }

/* md-f1oo: the display moves, then changes mode, then rescales, while the
 * stream keeps running. Nothing restarts; the next frame's reconciliation
 * follows each, and records it in the packet-sequence history (md-383x):
 * points on frames sent before a republish are attributed to the geometry
 * they were sent under. */
static void test_republish_without_restart(void) {
    printf("  test_republish_without_restart... ");
    md_test_name = "republish";
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &G0);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    MdEncoder *enc = encoder_for(&G0);
    Sink sink = {0};
    uint32_t seq = 0;
    HostCaptureSync sync;
    host_capture_sync_init(&sync, enc, &inp, &mu, &seq, G0.w, G0.h, &G0.rect);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);
    send_packets(&sync, &seq, 2);              /* G0 frames: packets 0..1 */

    /* Rearranged: same frame size, the display now right of main. */
    const Geometry moved = { G0.w, G0.h, { 1512, -100, 320, 180 } };
    capture_republish(&mc, &moved);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);
    MD_CHECK(mc.starts == 1);
    MD_CHECK(sync.bounds_updates == 1 && sync.resizes == 0);
    send_packets(&sync, &seq, 2);              /* moved frames: 2..3 */
    MD_CHECK(run_start(&sync) == 2 && sync.runs_total == 2);
    int ox = -1, oy = -1;
    /* Same size, another rectangle: a G0-frame point cannot be placed. */
    MD_CHECK(map_on(&sync, &G0, 1, 320, 180, &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(map_on(&sync, &moved, 2, 320, 180, &ox, &oy) == HOST_POINT_CURRENT);
    MD_CHECK(md_input_mouse_move(&inp, ox, oy) == 0);
    MD_CHECK(mi.gx == 1512 + 160 && mi.gy == -100 + 90);

    /* Mode change in place: a new size and rect on the same stream. */
    capture_republish(&mc, &G1);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);
    MD_CHECK(mc.starts == 1);
    MD_CHECK(sync.bounds_updates == 2 && sync.resizes == 1);
    send_packets(&sync, &seq, 2);              /* G1 frames: 4..5 */
    MD_CHECK(run_start(&sync) == 4 && sync.runs_total == 3);
    uint32_t ew = 0, eh = 0;
    MD_CHECK(md_encoder_get_size(enc, &ew, &eh) == 0 && ew == 480 && eh == 270);
    MD_CHECK(map_on(&sync, &moved, 3, 320, 180, &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(map_on(&sync, &G1, 5, 240, 135, &ox, &oy) == HOST_POINT_CURRENT);
    MD_CHECK(md_input_click(&inp, ox, oy, 0) == 0);
    MD_CHECK(mi.gx == -960 && mi.gy == 660 && mi.clicks == 1);

    /* Rescaled in place (as a cap or HiDPI change): the same rectangle at
     * half the size, so G1-frame points still land where they showed. */
    const Geometry half = { 240, 136, G1.rect };
    capture_republish(&mc, &half);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);
    MD_CHECK(mc.starts == 1 && sync.resizes == 2);
    /* No half-size packet yet: the client can only show a G1 frame. */
    MD_CHECK(map_on(&sync, &G1, 4, 240, 135, &ox, &oy) == HOST_POINT_TRANSLATED);
    MD_CHECK(md_input_click(&inp, ox, oy, 0) == 0);
    MD_CHECK(abs_d(mi.gx - -960) <= 0.5 * 1920 / 240 &&
             abs_d(mi.gy - 660) <= 0.5 * 1084 / 136);
    MD_CHECK(map_on(&sync, &moved, 3, 320, 180, &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(sink.packets > 0 && sink.untagged == 0);

    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    printf("OK\n");
}

/* md-n9p2: a size-capped capture of a Retina display (3024x1964 pixels,
 * 1512x982 points) left of and above main, as --capture-max-size 1280x0
 * delivers it. The host wires input exactly as for a native capture (frame
 * size as screen_*, the output rect as output_*), and input lands where the
 * same spot of the native frame would: within one capped pixel. */
static void test_capped_capture_maps_input(void) {
    printf("  test_capped_capture_maps_input... ");
    md_test_name = "capped";
    const MdCaptureRect display = { -1512, -200, 1512, 982 };
    uint32_t w = 0, h = 0;
    REQUIRE(md_capture_fit_size(3024, 1964, 1280, 0, &w, &h) == 0);
    REQUIRE(w == 1280 && h == 831);
    const Geometry capped = { w, h, display };
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &capped);

    /* As host main.c: input from the capture's size and rect, nothing else. */
    uint32_t cw = 0, ch = 0;
    MdCaptureRect r = {0};
    REQUIRE(md_capture_get_size(&cap, &cw, &ch) == 0);
    REQUIRE(md_capture_get_output_rect(&cap, &r) == 0);
    const Geometry from_host = { cw, ch, r };
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &from_host);
    uint32_t seq = 0;
    HostCaptureSync sync;
    host_capture_sync_init(&sync, NULL, &inp, NULL, &seq, cw, ch, &r);
    MD_CHECK(pump(&sync, &cap, NULL, 1) == 0); /* no encoder: never submits */
    MD_CHECK(sync.bounds_updates == 0);        /* nothing to reconcile */
    send_packets(&sync, &seq, 3);              /* capped frames: 0..2 */

    /* md-383x: the client decodes the capped frame cropped to even (831 ->
     * 830 rows) and tags its points with that; they map as-is. */
    int ox = -1, oy = -1;
    MD_CHECK(map_on(&sync, &capped, 1, 640, 415, &ox, &oy) ==
             HOST_POINT_CURRENT);
    MD_CHECK(ox == 640 && oy == 415);

    const MdInputConfig native = {
        .screen_width = 3024, .screen_height = 1964,
        .output_x = display.x, .output_y = display.y,
        .output_width = display.width, .output_height = display.height,
    };
    const double tol_x = (double)display.width / w;   /* one capped pixel */
    const double tol_y = (double)display.height / h;
    static const int pts[][2] = {
        { 0, 0 }, { 640, 415 }, { 1279, 830 }, { 100, 700 }, { 1000, 50 },
    };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
        const int x = pts[i][0], y = pts[i][1];
        MD_CHECK(md_input_mouse_move(&inp, x, y) == 0);
        double nx = 0, ny = 0;
        md_input_map_to_output(&native, (int)(x * 3024.0 / w + 0.5),
                               (int)(y * 1964.0 / h + 0.5), &nx, &ny);
        MD_CHECK(abs_d(mi.gx - nx) <= tol_x && abs_d(mi.gy - ny) <= tol_y);
        MD_CHECK(mi.gx >= display.x && mi.gx < display.x + (int)display.width);
        MD_CHECK(mi.gy >= display.y && mi.gy < display.y + (int)display.height);
    }
    MD_CHECK(md_input_mouse_move(&inp, 0, 0) == 0);
    MD_CHECK(mi.gx == -1512 && mi.gy == -200);
    MD_CHECK(md_input_mouse_move(&inp, 640, 0) == 0);
    MD_CHECK(mi.gx == -756);                    /* exact half-way */

    /* Agent actions carry global points: the inverse mapping lands the
     * click back on that point, within one capped pixel. */
    MdAction act;
    memset(&act, 0, sizeof(act));
    act.type = MD_ACTION_CLICK;
    act.region[0] = -756;
    act.region[1] = 291;
    MD_CHECK(md_input_execute_action(&inp, &act) == 0);
    MD_CHECK(abs_d(mi.gx - -756) <= tol_x && abs_d(mi.gy - 291) <= tol_y);
    const int moves = mi.moves;
    act.region[0] = 100;                        /* on main, not captured */
    act.region[1] = 100;
    MD_CHECK(md_input_execute_action(&inp, &act) == -1);
    MD_CHECK(mi.moves == moves);

    /* md-f1oo while capped: the display moves right of main; the frame
     * keeps its capped size and only the rect is republished. */
    const Geometry moved = { w, h, { 3024, 0, 1512, 982 } };
    capture_republish(&mc, &moved);
    MD_CHECK(pump(&sync, &cap, NULL, 1) == 0);
    MD_CHECK(sync.bounds_updates == 1 && mc.starts == 1);
    MD_CHECK(inp.config.screen_width == w && inp.config.screen_height == h);
    MD_CHECK(md_input_mouse_move(&inp, 640, 0) == 0);
    MD_CHECK(mi.gx == 3024 + 756 && mi.gy == 0);
    send_packets(&sync, &seq, 3);              /* moved frames: 3..5 */
    MD_CHECK(run_start(&sync) == 3 && sync.runs_total == 2);
    /* A point on a frame from before the move cannot be placed. */
    MD_CHECK(map_on(&sync, &capped, 2, 640, 0, &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(map_on(&sync, &moved, 3, 640, 0, &ox, &oy) == HOST_POINT_CURRENT);

    /* The cap lifted in place: native pixels of the same rectangle. A point
     * on a capped frame is rescaled and lands where the capped frame showed
     * it, within half a native pixel. */
    const Geometry native_moved = { 3024, 1964, moved.rect };
    capture_republish(&mc, &native_moved);
    MD_CHECK(pump(&sync, &cap, NULL, 1) == 0);
    MD_CHECK(sync.bounds_updates == 2 && sync.runs_total == 2);
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
        const int x = pts[i][0], y = pts[i][1] > 829 ? 829 : pts[i][1];
        double wx = 0, wy = 0;
        desktop_point(&moved, x, y, &wx, &wy);
        MD_CHECK(map_on(&sync, &moved, 5, x, y, &ox, &oy) ==
                 HOST_POINT_TRANSLATED);
        MD_CHECK(md_input_mouse_move(&inp, ox, oy) == 0);
        MD_CHECK(abs_d(mi.gx - wx) <= 0.5 * 1512 / 3024 + 1e-9 &&
                 abs_d(mi.gy - wy) <= 0.5 * 982 / 1964 + 1e-9);
    }

    md_capture_stop(&cap);
    printf("OK\n");
}

/* ── Input across a resize (md-383x) ──────────────────────────── */

/* Every point of every frame the host ever sent, mapped after each change
 * in a series: it is used as-is only on the current geometry's frames,
 * rescaled from any earlier geometry showing the same output rectangle,
 * landing within half a current pixel of where the shown frame put it, and
 * otherwise dropped. */
static void test_points_follow_shown_frame(void) {
    printf("  test_points_follow_shown_frame... ");
    md_test_name = "points_follow_shown_frame";
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &G0);
    uint32_t seq = 0;
    HostCaptureSync sync;
    host_capture_sync_init(&sync, NULL, &inp, NULL, &seq, G0.w, G0.h, &G0.rect);

    /* Rescale, rescale back, another display of the same size, a mode
     * change, a rearrangement of the same size. */
    const Geometry *series[] = { &G0, &GS, &G0, &GB, &G1, &G2 };
    enum { N = sizeof(series) / sizeof(series[0]) };
    uint32_t first[N], end[N];
    int translated = 0, current = 0, dropped = 0;
    for (int step = 0; step < N; step++) {
        if (step) apply_geometry(&sync, &cap, series[step]);
        first[step] = seq;
        send_packets(&sync, &seq, 7);    /* frames sent at this geometry */
        end[step] = seq;
        MD_CHECK(sync.width == series[step]->w && run_start(&sync) == first[step]);
        const Geometry *cur = series[step];
        for (int h = 0; h <= step; h++) {
            const Geometry *shown = series[h];
            const uint32_t seqs[2] = { first[h], end[h] - 1 };
            const int xs[] = { 0, 1, (int)(shown->w & ~1u) / 3,
                               (int)shown->w / 2, (int)(shown->w & ~1u) - 1 };
            const int ys[] = { 0, (int)shown->h / 2, (int)(shown->h & ~1u) - 1 };
            for (int si = 0; si < 2; si++)
            for (size_t xi = 0; xi < sizeof(xs) / sizeof(xs[0]); xi++)
            for (size_t yi = 0; yi < sizeof(ys) / sizeof(ys[0]); yi++) {
                int ox = -1, oy = -1;
                HostPointMap m = map_on(&sync, shown, seqs[si], xs[xi], ys[yi],
                                        &ox, &oy);
                const bool rescalable = rect_eq(&shown->rect, &cur->rect);
                if (h == step) {
                    MD_CHECK(m == HOST_POINT_CURRENT);
                    MD_CHECK(ox == xs[xi] && oy == ys[yi]);
                    current++;
                } else if (rescalable) {
                    MD_CHECK(m == HOST_POINT_TRANSLATED);
                    MD_CHECK(ox >= 0 && ox < (int)cur->w);
                    MD_CHECK(oy >= 0 && oy < (int)cur->h);
                    /* Injected through the retargeted input, it lands where
                     * the shown frame put it. */
                    double wx, wy;
                    desktop_point(shown, xs[xi], ys[yi], &wx, &wy);
                    MD_CHECK(md_input_mouse_move(&inp, ox, oy) == 0);
                    const double tol_x = 0.5 * cur->rect.width / cur->w + 1e-9;
                    const double tol_y = 0.5 * cur->rect.height / cur->h + 1e-9;
                    MD_CHECK(mi.gx >= wx - tol_x && mi.gx <= wx + tol_x);
                    MD_CHECK(mi.gy >= wy - tol_y && mi.gy <= wy + tol_y);
                    translated++;
                } else {
                    MD_CHECK(m == HOST_POINT_DROPPED);
                    dropped++;
                }
            }
        }
    }
    /* G0->GS, GS->G0 and G0->G0 rescale; GB, G1 and G2 change the
     * rectangle. */
    MD_CHECK(translated == 3 * 2 * 5 * 3);
    MD_CHECK(sync.points_translated == (uint32_t)translated);
    MD_CHECK(sync.points_dropped == (uint32_t)dropped);
    MD_CHECK(current > 0 && dropped > 0);

    /* G2 has G1's size: only the sequence tells their frames apart. */
    int ox, oy;
    MD_CHECK(map_on(&sync, &G1, end[4] - 1, 240, 135, &ox, &oy) ==
             HOST_POINT_DROPPED);                     /* G1 -> G2 moved */
    MD_CHECK(map_on(&sync, &G2, first[5], 240, 135, &ox, &oy) ==
             HOST_POINT_CURRENT);

    /* Malformed or unknown tags. */
    MD_CHECK(host_capture_sync_map_point(&sync, 0, 0, first[5], 0, 0,
                                         &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(host_capture_sync_map_point(&sync, 480, 271, first[5], 0, 0,
                                         &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(map_on(&sync, &G2, seq, 0, 0, &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(map_on(&sync, &G2, seq + 1000, 0, 0, &ox, &oy) ==
             HOST_POINT_DROPPED);                     /* not sent yet */
    MD_CHECK(map_on(&sync, &G2, first[5], 480, 0, &ox, &oy) ==
             HOST_POINT_DROPPED);                     /* outside the frame */
    MD_CHECK(map_on(&sync, &G2, first[5], -1, 0, &ox, &oy) ==
             HOST_POINT_DROPPED);
    MD_CHECK(host_capture_sync_map_point(NULL, 480, 270, 0, 0, 0, &ox, &oy) ==
             HOST_POINT_DROPPED);

    /* A new stream restarts the counter: all it sees is the current
     * geometry, and nothing from the old stream is attributable. */
    seq = 0;
    host_capture_sync_reset_sequence(&sync);
    MD_CHECK(sync.runs_total == 0);
    send_packets(&sync, &seq, 3);
    MD_CHECK(map_on(&sync, &G2, 0, 240, 135, &ox, &oy) == HOST_POINT_CURRENT);
    MD_CHECK(map_on(&sync, &GS, 1, 160, 90, &ox, &oy) == HOST_POINT_DROPPED);
    md_capture_stop(&cap);
    printf("OK (%d current, %d rescaled, %d dropped)\n",
           current, translated, dropped);
}

/* Sequences compare modulo 2^32; without a sequence counter nothing is
 * attributable. */
static void test_point_sequence_edges(void) {
    printf("  test_point_sequence_edges... ");
    md_test_name = "point_sequence_edges";
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    uint32_t seq = UINT32_MAX - 3;
    HostCaptureSync sync;
    host_capture_sync_init(&sync, NULL, NULL, NULL, &seq, G0.w, G0.h, &G0.rect);
    send_packets(&sync, &seq, 10);              /* wraps to 6 */
    apply_geometry(&sync, &cap, &GS);
    send_packets(&sync, &seq, 4);
    int ox, oy;
    MD_CHECK(map_on(&sync, &G0, UINT32_MAX - 1, 100, 50, &ox, &oy) ==
             HOST_POINT_TRANSLATED);
    MD_CHECK(ox == 50 && oy == 25);
    MD_CHECK(map_on(&sync, &GS, 7, 10, 10, &ox, &oy) == HOST_POINT_CURRENT);
    MD_CHECK(map_on(&sync, &G0, UINT32_MAX - 5, 100, 50, &ox, &oy) ==
             HOST_POINT_DROPPED);               /* before G0's first frame */
    MD_CHECK(map_on(&sync, &GS, 10, 10, 10, &ox, &oy) == HOST_POINT_DROPPED);

    /* Without a counter no frame is attributable: every point drops. */
    HostCaptureSync bare;
    host_capture_sync_init(&bare, NULL, NULL, NULL, NULL, G0.w, G0.h, &G0.rect);
    MD_CHECK(map_on(&bare, &G0, 0, 100, 50, &ox, &oy) == HOST_POINT_DROPPED);
    apply_geometry(&bare, &cap, &GS);
    MD_CHECK(map_on(&bare, &GS, 0, 10, 10, &ox, &oy) == HOST_POINT_DROPPED);

    /* Only the newest runs are held: a frame older than all of them is
     * dropped, even on the same rectangle. */
    uint32_t s2 = 0;
    HostCaptureSync h;
    host_capture_sync_init(&h, NULL, NULL, NULL, &s2, G0.w, G0.h, &G0.rect);
    send_packets(&h, &s2, 1);                   /* G0 at packet 0 */
    for (uint32_t i = 0; i < HOST_CAPTURE_SYNC_RUNS; i++) {
        apply_geometry(&h, &cap, (i % 2) ? &G0 : &GS);
        send_packets(&h, &s2, 1);               /* GS at 1, G0 at 2, ... */
    }
    MD_CHECK(h.width == G0.w && s2 == HOST_CAPTURE_SYNC_RUNS + 1);
    MD_CHECK(map_on(&h, &G0, 0, 100, 50, &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(map_on(&h, &GS, 1, 50, 25, &ox, &oy) == HOST_POINT_TRANSLATED);
    MD_CHECK(ox == 100 && oy == 50);
    /* An earlier generation of the very same geometry: an exact rescale. */
    MD_CHECK(map_on(&h, &G0, 2, 100, 50, &ox, &oy) == HOST_POINT_TRANSLATED);
    MD_CHECK(ox == 100 && oy == 50);
    MD_CHECK(map_on(&h, &G0, s2 - 1, 100, 50, &ox, &oy) == HOST_POINT_CURRENT);

    /* A tag the sync no longer holds, or none, names no geometry: its
     * packets are attributable to nothing, though they match the frame. */
    const uint64_t stale = host_capture_sync_frame_tag(&h);
    for (uint32_t i = 0; i < HOST_CAPTURE_SYNC_GEOMETRIES; i++)
        apply_geometry(&h, &cap, (i % 2) ? &G0 : &GS);
    MD_CHECK(h.width == G0.w);
    host_capture_sync_packet_sent(&h, stale, s2++);
    MD_CHECK(map_on(&h, &G0, s2 - 1, 100, 50, &ox, &oy) == HOST_POINT_DROPPED);
    send_packets(&h, &s2, 1);
    MD_CHECK(map_on(&h, &G0, s2 - 1, 100, 50, &ox, &oy) == HOST_POINT_CURRENT);
    host_capture_sync_packet_sent(&h, 0, s2++);
    MD_CHECK(map_on(&h, &G0, s2 - 1, 100, 50, &ox, &oy) == HOST_POINT_DROPPED);
    MD_CHECK(host_capture_sync_frame_tag(NULL) == 0);
    host_capture_sync_packet_sent(NULL, 1, 0);  /* no sync: ignored */
    md_capture_stop(&cap);
    printf("OK\n");
}

/* ── An asynchronous encoder across geometry changes (md-f7uc) ── *
 * As VideoToolbox or AMF: a packet comes out after later frames went in, so
 * frames captured before a change are sent after the host applied it. A
 * delay line behind the real encoder holds each packet for ASYNC_DELAY more
 * submits and, across the rectangle-only change, releases neighbours out of
 * order; the flush drains it. Each packet is then sent as on_encoded does.
 * The truth is the capture side's: the geometry the mock delivered each
 * frame at, by pts. */

#define ASYNC_DELAY   2
#define ASYNC_PHASES  4
#define ASYNC_FRAMES  64        /* by pts, which is the mock's frame seq */
#define ASYNC_PACKETS 64

typedef struct {
    HostCaptureSync *sync;
    uint32_t        *seq;                     /* the host's counter */
    struct { int64_t pts; uint64_t tag; } held[16];
    int              n_held;
    bool             reorder;                 /* armed for one swap */
    /* Capture-side truth, per frame. */
    int              frame_phase[ASYNC_FRAMES];
    uint64_t         frame_tag[ASYNC_FRAMES];
    /* Per sent packet sequence: its frame. */
    int64_t          sent_pts[ASYNC_PACKETS];
    int              swapped;
} AsyncPipe;

static void async_on_encoded(const MdEncodedPacket *pkt, void *ud) {
    AsyncPipe *p = ud;
    REQUIRE(p->n_held < (int)(sizeof(p->held) / sizeof(p->held[0])));
    p->held[p->n_held].pts = pkt->pts;
    p->held[p->n_held].tag = pkt->tag;
    p->n_held++;
}

/* Send the held packet at index i, as host on_encoded would. */
static void async_send(AsyncPipe *p, int i) {
    const int64_t pts = p->held[i].pts;
    const uint64_t tag = p->held[i].tag;
    REQUIRE(pts >= 0 && pts < ASYNC_FRAMES && *p->seq < ASYNC_PACKETS);
    /* The encoder returned the tag of exactly the frame it encoded. */
    MD_CHECK(tag != 0 && tag == p->frame_tag[pts]);
    p->sent_pts[*p->seq] = pts;
    host_capture_sync_packet_sent(p->sync, tag, (*p->seq)++);
    memmove(&p->held[i], &p->held[i + 1],
            (size_t)(p->n_held - i - 1) * sizeof(p->held[0]));
    p->n_held--;
}

static void async_release(AsyncPipe *p, int keep) {
    while (p->n_held > keep) {
        /* Out of order, once: the first new-geometry packet overtakes the
         * old one ahead of it. */
        const bool swap = p->reorder && p->n_held >= 2 &&
            p->frame_phase[p->held[0].pts] != p->frame_phase[p->held[1].pts];
        if (swap) {
            p->swapped++;
            p->reorder = false;
        }
        async_send(p, swap ? 1 : 0);
    }
}

typedef struct {
    int current, translated, dropped;
} AsyncTally;

/* Every point of every packet sent so far, mapped now that phase `now`
 * (geometry phases[now]) is current: as-is on its own frames, rescaled from
 * any frame of the same output rectangle, dropped otherwise. */
static void async_check_all(AsyncPipe *p, MdInput *inp, MockInput *mi,
                            const Geometry *const *phases, int now,
                            AsyncTally *t) {
    const Geometry *cur = phases[now];
    for (uint32_t seq = 0; seq < *p->seq; seq++) {
        const int64_t pts = p->sent_pts[seq];
        const int ph = p->frame_phase[pts];
        const Geometry *shown = phases[ph];
        const int xs[] = { 0, (int)shown->w / 3, (int)(shown->w & ~1u) - 1 };
        const int ys[] = { 0, (int)shown->h / 2, (int)(shown->h & ~1u) - 1 };
        for (size_t xi = 0; xi < 3; xi++)
        for (size_t yi = 0; yi < 3; yi++) {
            int ox = -1, oy = -1;
            const HostPointMap m = map_on(p->sync, shown, seq, xs[xi], ys[yi],
                                          &ox, &oy);
            if (ph == now) {
                MD_CHECK(m == HOST_POINT_CURRENT);
                MD_CHECK(ox == xs[xi] && oy == ys[yi]);
                t->current++;
            } else if (cur->rect.width && rect_eq(&shown->rect, &cur->rect)) {
                MD_CHECK(m == HOST_POINT_TRANSLATED);
                double wx, wy;
                desktop_point(shown, xs[xi], ys[yi], &wx, &wy);
                MD_CHECK(md_input_mouse_move(inp, ox, oy) == 0);
                const double tol_x = 0.5 * cur->rect.width / cur->w + 1e-9;
                const double tol_y = 0.5 * cur->rect.height / cur->h + 1e-9;
                MD_CHECK(abs_d(mi->gx - wx) <= tol_x &&
                         abs_d(mi->gy - wy) <= tol_y);
                t->translated++;
            } else {
                MD_CHECK(m == HOST_POINT_DROPPED);
                t->dropped++;
            }
        }
    }
}

static void test_async_encoder_attribution(void) {
    printf("  test_async_encoder_attribution... ");
    md_test_name = "async_encoder";
    /* GB at half the size: the same rectangle, a pure rescale. */
    static const Geometry GBs = { 320, 180, { 320, 0, 320, 180 } };
    /* G0, then rearranged (same size: the encoder is not re-opened), then
     * rescaled (re-opened), then a mode change elsewhere (re-opened). */
    const Geometry *const phases[ASYNC_PHASES] = { &G0, &GB, &GBs, &G1 };
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &G0);
    MdEncoder *enc = encoder_for(&G0);
    uint32_t seq = 0;
    HostCaptureSync sync;
    host_capture_sync_init(&sync, enc, &inp, NULL, &seq, G0.w, G0.h, &G0.rect);
    AsyncPipe p = { .sync = &sync, .seq = &seq };
    AsyncTally t = {0};
    uint32_t seq_at_change[ASYNC_PHASES] = {0};
    int stragglers[ASYNC_PHASES] = {0};

    for (int ph = 0; ph < ASYNC_PHASES; ph++) {
        if (ph) capture_republish(&mc, phases[ph]);
        p.reorder = ph == 1;
        for (int i = 0; i < 6; i++) {
            MdFrame f;
            REQUIRE(md_capture_get_frame(&cap, &f, 100) == 0);
            REQUIRE(f.seq < ASYNC_FRAMES);
            REQUIRE(host_capture_sync_frame(&sync, &cap, &f));
            if (i == 0) seq_at_change[ph] = seq;  /* applied, as md-383x saw it */
            p.frame_phase[f.seq] = ph;
            p.frame_tag[f.seq] = host_capture_sync_frame_tag(&sync);
            MD_CHECK(md_encoder_submit_tagged(enc, f.data, f.stride,
                                              MD_PIX_FMT_BGRA, (int64_t)f.seq,
                                              p.frame_tag[f.seq],
                                              async_on_encoded, &p) == 0);
            md_capture_release_frame(&cap, &f);
            async_release(&p, ASYNC_DELAY);
        }
        if (ph == ASYNC_PHASES - 1) {
            MD_CHECK(md_encoder_flush(enc, async_on_encoded, &p) >= 0);
            async_release(&p, 0);
        }
        /* Earlier frames sent after this change was applied. */
        for (uint32_t s = seq_at_change[ph]; s < seq; s++)
            if (p.frame_phase[p.sent_pts[s]] < ph) stragglers[ph]++;
        /* Every packet's run must still be held for the expectation. */
        REQUIRE(sync.runs_total <= HOST_CAPTURE_SYNC_RUNS);
        async_check_all(&p, &inp, &mi, phases, ph, &t);
    }
    MD_CHECK(sync.resizes == 2 && sync.bounds_updates == 3);
    /* The window md-383x mis-mapped: G0 frames sent after the rearrangement
     * was applied, interleaved with GB ones. Each was checked dropped while
     * GB was current, as was every G0 point since. */
    MD_CHECK(stragglers[1] >= ASYNC_DELAY && p.swapped > 0);
    MD_CHECK(sync.runs_total > ASYNC_PHASES);  /* the reorder split runs */
    /* GB frames sent after the rescale were rescaled onto GBs; G0 ones,
     * on another rectangle, were dropped. */
    MD_CHECK(stragglers[2] >= 1 && t.translated > 0);
    MD_CHECK(t.current > 0 && t.dropped > 0);
    uint32_t sent_at[ASYNC_PHASES] = {0};
    for (uint32_t s = 0; s < seq; s++) sent_at[p.frame_phase[p.sent_pts[s]]]++;
    for (int ph = 0; ph < ASYNC_PHASES; ph++) MD_CHECK(sent_at[ph] > 0);

    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    printf("OK (%u packets, %d/%d/%d stragglers, %d swapped; %d current, "
           "%d rescaled, %d dropped)\n", seq, stragglers[1], stragglers[2],
           stragglers[3], p.swapped, t.current, t.translated, t.dropped);
}

/* ── Concurrency: geometry flips while input is dispatched ────── */

typedef struct {
    HostCaptureSync *sync;
    MdCaptureCtx *cap;
    MockCap *mc;
    atomic_bool stop;
    int flips;
} Flipper;

static void *flip_thread(void *arg) {
    Flipper *fl = arg;
    for (int i = 0; i < 200 && !atomic_load(&fl->stop); i++) {
        capture_change(fl->mc, (i % 2) ? &G0 : &G1);
        pump(fl->sync, fl->cap, NULL, 3);
        fl->flips++;
    }
    atomic_store(&fl->stop, true);
    return NULL;
}

static void test_concurrent_dispatch(void) {
    printf("  test_concurrent_dispatch... ");
    md_test_name = "concurrent";
    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &G0);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    HostCaptureSync sync;
    host_capture_sync_init(&sync, NULL, &inp, &mu, NULL, G0.w, G0.h, &G0.rect);
    Flipper fl = { .sync = &sync, .cap = &cap, .mc = &mc };
    atomic_init(&fl.stop, false);

    double c0x, c0y, c1x, c1y;
    expect_center(&G0, &c0x, &c0y);
    expect_center(&G1, &c1x, &c1y);

    pthread_t t;
    REQUIRE(pthread_create(&t, NULL, flip_thread, &fl) == 0);
    int moves = 0, torn = 0;
    while (!atomic_load(&fl.stop)) {
        /* As handle_client_packet: the frame centre under the input lock. */
        pthread_mutex_lock(&mu);
        int cx = (int)inp.config.screen_width / 2;
        int cy = (int)inp.config.screen_height / 2;
        md_input_mouse_move(&inp, cx, cy);
        bool g0 = mi.gx == c0x && mi.gy == c0y;
        bool g1 = mi.gx == c1x && mi.gy == c1y;
        pthread_mutex_unlock(&mu);
        if (!g0 && !g1) torn++;
        moves++;
    }
    pthread_join(t, NULL);
    MD_CHECK(torn == 0);
    MD_CHECK(fl.flips > 0 && sync.bounds_updates >= (uint32_t)fl.flips - 1);
    md_capture_stop(&cap);
    printf("OK (%d flips, %d moves)\n", fl.flips, moves);
}

/* ── End to end over a TCP stream ─────────────────────────────── *
 * Host side: a capture thread (mock capture -> sync -> encoder -> stream)
 * and a packet thread (stream -> MD_PKT_INPUT -> input, under the lock, as
 * handle_client_packet). Client side (this thread): decode, follow two
 * size changes, and click on the frame it shows, including in each window
 * where the host has changed geometry but the client still shows the old
 * frame (md-383x). Each click is checked where it landed on the desktop. */

#define E2E_MAX_CLICKS 8
#define E2E_RING       8192      /* frames / packets, beyond any run */

typedef struct {
    HostCaptureSync *sync;
    MdCaptureCtx    *cap;
    MdStream        *peer;      /* host end */
    pthread_mutex_t *mu;        /* client_mu: sends + input dispatch */
    MdInput         *input;
    atomic_bool      stop;
    uint32_t         seq;       /* frame-channel sequence, under mu */
    int              resizes_checked, resize_keys;
    bool             armed;
    /* Per dispatched click, under mu. */
    int              inputs;
    HostPointMap     result[E2E_MAX_CLICKS];
    double           gx[E2E_MAX_CLICKS], gy[E2E_MAX_CLICKS];
    /* Capture-side truth: the geometry each frame was delivered at, by pts
     * (capture thread only), and each sent packet's, by sequence (mu). */
    Geometry        *frame_geom;
    Geometry        *sent_geom;
} E2E;

static void e2e_on_encoded(const MdEncodedPacket *pkt, void *ud) {
    E2E *e = ud;
    if (e->armed) {
        e->resizes_checked++;
        if (pkt->is_key) e->resize_keys++;
        e->armed = false;
    }
    pthread_mutex_lock(e->mu);
    e->sent_geom[e->seq % E2E_RING] = e->frame_geom[pkt->pts % E2E_RING];
    host_capture_sync_packet_sent(e->sync, pkt->tag, e->seq);
    md_stream_send(e->peer, MD_PKT_VIDEO_FRAME, e->seq++, pkt->data,
                   (uint32_t)pkt->size);
    pthread_mutex_unlock(e->mu);
}

static void *e2e_capture_thread(void *arg) {
    E2E *e = arg;
    while (!atomic_load(&e->stop)) {
        MdFrame f;
        int rc = md_capture_get_frame(e->cap, &f, 100);
        if (rc == MD_CAPTURE_ERROR) {
            md_capture_stop(e->cap);
            md_capture_start(e->cap);
            continue;
        }
        if (rc != 0) continue;
        uint32_t before = e->sync->resizes;
        if (host_capture_sync_frame(e->sync, e->cap, &f)) {
            if (e->sync->resizes != before) e->armed = true;
            e->frame_geom[f.seq % E2E_RING] =
                (Geometry){ f.width, f.height, e->cap->output_rect };
            md_encoder_submit_tagged(e->sync->encoder, f.data, f.stride,
                                     MD_PIX_FMT_BGRA, (int64_t)f.seq,
                                     host_capture_sync_frame_tag(e->sync),
                                     e2e_on_encoded, e);
        }
        md_capture_release_frame(e->cap, &f);
    }
    return NULL;
}

static void *e2e_packet_thread(void *arg) {
    E2E *e = arg;
    while (!atomic_load(&e->stop)) {
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int rc = md_stream_recv(e->peer, &hdr, &payload, 50);
        if (rc < 0) break;
        if (rc == 0 && hdr.type == MD_PKT_INPUT) {
            uint8_t kind = 0, button = 0;
            int16_t x = 0, y = 0;
            const char *text = NULL;
            uint16_t tlen = 0, fw = 0, fh = 0;
            uint32_t fseq = 0;
            pthread_mutex_lock(e->mu);
            if (md_input_event_unpack(payload, hdr.payload_len, &kind, &button,
                                      &x, &y, &text, &tlen,
                                      &fw, &fh, &fseq) == 0 &&
                kind == MD_INPUT_EV_BUTTON && e->inputs < E2E_MAX_CLICKS) {
                int px = 0, py = 0;
                HostPointMap m = host_capture_sync_map_point(e->sync, fw, fh,
                                                             fseq, x, y,
                                                             &px, &py);
                MockInput *mi = e->input->backend_data;
                mi->gx = mi->gy = 1e9;
                if (m != HOST_POINT_DROPPED)
                    md_input_click(e->input, px, py, 0);
                e->result[e->inputs] = m;
                e->gx[e->inputs] = mi->gx;
                e->gy[e->inputs] = mi->gy;
                e->inputs++;
            }
            pthread_mutex_unlock(e->mu);
        }
        free(payload);
    }
    return NULL;
}

typedef struct {
    uint32_t w, h, seq;          /* the frame on screen */
    uint32_t sizes_w[MAX_SIZES], sizes_h[MAX_SIZES];
    int n_sizes;
} ClientView;

static void e2e_on_decoded(const MdDecodedFrame *f, void *ud) {
    ClientView *v = ud;
    v->w = f->width;
    v->h = f->height;
    v->seq = (uint32_t)f->pts;   /* submitted as the packet sequence */
    if ((v->n_sizes == 0 || v->sizes_w[v->n_sizes - 1] != f->width ||
         v->sizes_h[v->n_sizes - 1] != f->height) && v->n_sizes < MAX_SIZES) {
        v->sizes_w[v->n_sizes] = f->width;
        v->sizes_h[v->n_sizes] = f->height;
        v->n_sizes++;
    }
}

static bool shows(const ClientView *v, const Geometry *g) {
    return v->w == (g->w & ~1u) && v->h == (g->h & ~1u);
}

/* As the client's send_input: a click tagged with the frame on screen. */
static void client_click(MdStream *client, const ClientView *v, int x, int y) {
    uint8_t buf[MD_INPUT_EVENT_MAX_LEN];
    size_t n = md_input_event_pack(buf, sizeof(buf), MD_INPUT_EV_BUTTON, 1,
                                   (int16_t)x, (int16_t)y, NULL, 0,
                                   (uint16_t)v->w, (uint16_t)v->h, v->seq);
    MD_CHECK(n > 0);
    MD_CHECK(md_stream_send(client, MD_PKT_INPUT, 1, buf, (uint32_t)n) == 0);
}

static int locked_int(pthread_mutex_t *mu, const int *p) {
    pthread_mutex_lock(mu);
    int v = *p;
    pthread_mutex_unlock(mu);
    return v;
}

static uint32_t locked_u32(pthread_mutex_t *mu, const uint32_t *p) {
    pthread_mutex_lock(mu);
    uint32_t v = *p;
    pthread_mutex_unlock(mu);
    return v;
}

static void test_end_to_end_stream(void) {
    printf("  test_end_to_end_stream... ");
    md_test_name = "e2e";
    MdStreamServer *srv = md_stream_server_create("127.0.0.1", 0);
    REQUIRE(srv != NULL);
    MdStream *client = md_stream_connect("127.0.0.1", md_stream_server_port(srv), 3000);
    REQUIRE(client != NULL);
    MdStream *peer = md_stream_server_accept(srv, 3000);
    REQUIRE(peer != NULL);

    MdCaptureCtx cap; MockCap mc;
    capture_open(&cap, &mc, &G0);
    mc.frame_pause_us = 4000; /* ~250 fps cadence */
    MdInput inp; MockInput mi;
    input_open(&inp, &mi, &G0);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    MdEncoder *enc = encoder_for(&G0);
    HostCaptureSync sync;
    E2E e = { .sync = &sync, .cap = &cap, .peer = peer, .mu = &mu, .input = &inp,
              .frame_geom = calloc(E2E_RING, sizeof(Geometry)),
              .sent_geom = calloc(E2E_RING, sizeof(Geometry)) };
    REQUIRE(e.frame_geom && e.sent_geom);
    atomic_init(&e.stop, false);
    host_capture_sync_init(&sync, enc, &inp, &mu, &e.seq, G0.w, G0.h, &G0.rect);

    pthread_t tc, tp;
    REQUIRE(pthread_create(&tc, NULL, e2e_capture_thread, &e) == 0);
    REQUIRE(pthread_create(&tp, NULL, e2e_packet_thread, &e) == 0);

    MdDecoder *dec = md_decoder_create();
    REQUIRE(dec != NULL);
    ClientView v = {0};
    /* Clicks, in order:
     *   0  on G0 (100,50), sent once the host shows GS (a pure rescale,
     *      republished without a restart): rescaled onto GS, landing
     *      where G0 showed it;
     *   1  on GS (160,90), sent once the host shows G1 (a mode change):
     *      dropped;
     *   2  on G1's centre, once the client shows G1: as-is;
     *   3  on the centre of the frame on screen once the host has applied
     *      G2 (G1's mode, rearranged: the encoder is not re-opened): as-is
     *      if that frame was captured at G2, dropped if at G1, which an
     *      asynchronous encoder can send after the change (md-f7uc). */
    int phase = 0;
    uint32_t clicked_seq = 0;
    /* Event-driven: each step reacts to what the client received or
     * decoded. The packet budget only bounds a broken run. */
    for (int budget = 0; budget < 4000 && phase < 8; budget++) {
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int rc = md_stream_recv(client, &hdr, &payload, 5000);
        if (rc != 0) { free(payload); break; }
        if (hdr.type == MD_PKT_VIDEO_FRAME) {
            /* Before decoding: a packet sent after a change can be the new
             * IDR, so the old frame is still on screen. */
            const uint32_t applied = locked_u32(&mu, &sync.bounds_updates);
            if (phase == 1 && applied >= 1 && shows(&v, &G0)) {
                client_click(client, &v, 100, 50);
                phase = 2;
            } else if (phase == 3 && applied >= 2 && shows(&v, &GS)) {
                client_click(client, &v, 160, 90);
                phase = 4;
            } else if (phase == 6 && applied >= 3) {
                client_click(client, &v, (int)v.w / 2, (int)v.h / 2);
                clicked_seq = v.seq;
                phase = 7;
            }
            md_decoder_submit(dec, payload, hdr.payload_len, hdr.sequence);
            md_decoder_poll(dec, e2e_on_decoded, &v);
        }
        free(payload);
        const int inputs = locked_int(&mu, &e.inputs);
        if (phase == 0 && shows(&v, &G0)) {
            /* The display rescales in place: republished without a
             * restart, as ScreenCaptureKit does (md-f1oo). */
            capture_republish(&mc, &GS);
            phase = 1;
        } else if (phase == 2 && inputs >= 1 && shows(&v, &GS)) {
            capture_change(&mc, &G1);          /* host display changes mode */
            phase = 3;
        } else if (phase == 4 && inputs >= 2 && shows(&v, &G1)) {
            client_click(client, &v, (int)v.w / 2, (int)v.h / 2);
            phase = 5;
        } else if (phase == 5 && inputs >= 3) {
            capture_republish(&mc, &G2);       /* rearranged, same mode */
            phase = 6;
        } else if (phase == 7 && inputs >= 4) {
            phase = 8;
        }
    }
    MD_CHECK(phase == 8);

    /* The packet thread polls the stop flag between bounded recvs, so it
     * exits on a live connection. Only then shut the client end: a capture
     * send blocked on a full socket fails instead of hanging the join, and
     * no recv runs concurrently with that failing send (MdStream's
     * connected flag is not synchronized between send and recv). */
    atomic_store(&e.stop, true);
    pthread_join(tp, NULL);
    md_stream_close(client);
    pthread_join(tc, NULL);

    MD_CHECK(sync.resizes == 2);
    MD_CHECK(mc.starts == 2);                  /* only G1 restarted */
    MD_CHECK(e.resizes_checked == 2 && e.resize_keys == 2);
    MD_CHECK(e.inputs == 4);
    /* Click 3 follows the frame's capture geometry, not the send time. */
    const Geometry shown = e.sent_geom[clicked_seq % E2E_RING];
    const bool shown_g2 = rect_eq(&shown.rect, &G2.rect);
    MD_CHECK(shown_g2 || rect_eq(&shown.rect, &G1.rect));
    MD_CHECK(shown.w == G1.w && shown.h == G1.h);
    if (shown_g2) {
        double wx, wy;
        desktop_point(&G2, 240, 135, &wx, &wy);
        MD_CHECK(e.result[3] == HOST_POINT_CURRENT);
        MD_CHECK(e.gx[3] == wx && e.gy[3] == wy);
    } else {
        MD_CHECK(e.result[3] == HOST_POINT_DROPPED);
    }
    MD_CHECK(mi.clicks == 2 + shown_g2);
    /* G0 (100,50) is desktop (50,25); GS shows the same rectangle 1:1. */
    MD_CHECK(e.result[0] == HOST_POINT_TRANSLATED);
    MD_CHECK(e.gx[0] == 50 && e.gy[0] == 25);
    MD_CHECK(e.result[1] == HOST_POINT_DROPPED);
    /* Decoded 480x270 centre (240,135) -> new display's centre. */
    MD_CHECK(e.result[2] == HOST_POINT_CURRENT);
    MD_CHECK(e.gx[2] == -960 && e.gy[2] == 660);
    MD_CHECK(sync.points_translated == 1 &&
             sync.points_dropped == 1u + !shown_g2);

    /* The decoder followed both size changes mid-stream. */
    md_decoder_flush(dec, e2e_on_decoded, &v);
    MD_CHECK(v.n_sizes == 3);
    MD_CHECK(v.sizes_w[0] == 640 && v.sizes_h[0] == 360);
    MD_CHECK(v.sizes_w[1] == 320 && v.sizes_h[1] == 180);
    MD_CHECK(v.sizes_w[2] == 480 && v.sizes_h[2] == 270);

    md_decoder_destroy(dec);
    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    md_stream_destroy(client);
    md_stream_destroy(peer);
    md_stream_server_destroy(srv);
    free(e.frame_geom);
    free(e.sent_geom);
    printf("OK (%d clicks; the rearranged click was on a %s frame)\n",
           e.inputs, shown_g2 ? "G2" : "G1");
}

int main(void) {
    printf("test_capture_sync: encoder/input follow capture geometry\n");
    signal(SIGPIPE, SIG_IGN); /* as the host: a closed peer is EPIPE */
    for (size_t i = 0; i < sizeof(g_pixels); i += 4) {
        g_pixels[i] = (uint8_t)(i >> 4);
        g_pixels[i + 1] = 0x80;
        g_pixels[i + 2] = (uint8_t)(i >> 9);
        g_pixels[i + 3] = 0xFF;
    }
    test_restart_reconfigures();
    test_requests_apply_on_capture_thread();
    test_degenerate_frames();
    test_resize_failure_recovers();
    test_republish_without_restart();
    test_capped_capture_maps_input();
    test_points_follow_shown_frame();
    test_point_sequence_edges();
    test_async_encoder_attribution();
    test_concurrent_dispatch();
    test_end_to_end_stream();
    return md_test_report();
}
