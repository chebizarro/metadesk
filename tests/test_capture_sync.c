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
    int moves;
    double gx, gy;
} MockInput;

static int mi_move(MdInputCtx *ctx, int x, int y) {
    MockInput *mi = ctx->backend_data;
    md_input_map_to_output(&ctx->config, x, y, &mi->gx, &mi->gy);
    mi->moves++;
    return 0;
}

static int mi_button(MdInputCtx *ctx, int button, int pressed) {
    (void)ctx; (void)button; (void)pressed;
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

/* Desktop point that the frame centre of g maps to. */
static void expect_center(const Geometry *g, double *gx, double *gy) {
    MdInputConfig c = {
        .screen_width = g->w, .screen_height = g->h,
        .output_x = g->rect.x, .output_y = g->rect.y,
        .output_width = g->rect.width, .output_height = g->rect.height,
    };
    md_input_map_to_output(&c, (int)g->w / 2, (int)g->h / 2, gx, gy);
}

/* ── Packet sink: what the client would decode ────────────────── */

#define MAX_SIZES 16

typedef struct {
    MdDecoder *dec;
    int packets;
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
            if (sink && sync->resizes != before) sink->want_key = true;
            MD_CHECK(md_encoder_submit(sync->encoder, f.data, f.stride,
                                       MD_PIX_FMT_BGRA, (int64_t)f.seq,
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
    host_capture_sync_init(&sync, enc, &inp, &mu, G0.w, G0.h, &G0.rect);

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

    md_decoder_destroy(sink.dec);
    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    printf("OK (%d packets, %d keyframes)\n", sink.packets, sink.keys);
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
    host_capture_sync_init(&sync, enc, NULL, NULL, G0.w, G0.h, &G0.rect);

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
    host_capture_sync_init(&sync, NULL, &inp, NULL, G0.w, G0.h, &G0.rect);

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
    host_capture_sync_init(&sync, enc, NULL, NULL, G0.w, G0.h, NULL);

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

/* md-f1oo: the display moves, then changes mode, while the stream keeps
 * running. Nothing restarts; the next frame's reconciliation follows both. */
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
    HostCaptureSync sync;
    host_capture_sync_init(&sync, enc, &inp, &mu, G0.w, G0.h, &G0.rect);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);

    /* Rearranged: same frame size, the display now right of main. */
    const Geometry moved = { G0.w, G0.h, { 1512, -100, 320, 180 } };
    capture_republish(&mc, &moved);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);
    MD_CHECK(mc.starts == 1);
    MD_CHECK(sync.bounds_updates == 1 && sync.resizes == 0);
    MD_CHECK(md_input_mouse_move(&inp, 320, 180) == 0);
    MD_CHECK(mi.gx == 1512 + 160 && mi.gy == -100 + 90);

    /* Mode change in place: a new size and rect on the same stream. */
    capture_republish(&mc, &G1);
    MD_CHECK(pump(&sync, &cap, &sink, 2) == 2);
    MD_CHECK(mc.starts == 1);
    MD_CHECK(sync.bounds_updates == 2 && sync.resizes == 1);
    uint32_t ew = 0, eh = 0;
    MD_CHECK(md_encoder_get_size(enc, &ew, &eh) == 0 && ew == 480 && eh == 270);
    MD_CHECK(md_input_mouse_move(&inp, 240, 135) == 0);
    MD_CHECK(mi.gx == -960 && mi.gy == 660);

    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    printf("OK\n");
}

static double abs_d(double v) { return v < 0 ? -v : v; }

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
    HostCaptureSync sync;
    host_capture_sync_init(&sync, NULL, &inp, NULL, cw, ch, &r);
    MD_CHECK(pump(&sync, &cap, NULL, 1) == 0); /* no encoder: never submits */
    MD_CHECK(sync.bounds_updates == 0);        /* nothing to reconcile */

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

    md_capture_stop(&cap);
    printf("OK\n");
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
    host_capture_sync_init(&sync, NULL, &inp, &mu, G0.w, G0.h, &G0.rect);
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
 * and a packet thread (stream -> MD_PKT_INPUT -> input, under the lock).
 * Client side (this thread): decode, follow the size change, click the
 * centre of the new frame, and check where it landed on the desktop. */

typedef struct {
    HostCaptureSync *sync;
    MdCaptureCtx    *cap;
    MdStream        *peer;      /* host end */
    pthread_mutex_t *mu;        /* client_mu: sends + input dispatch */
    MdInput         *input;
    atomic_bool      stop;
    uint32_t         seq;
    int              keys_after_resize;
    bool             armed;
    bool             first_after_resize_key;
    double           gx, gy;    /* where the client's click landed */
    int              inputs;
} E2E;

static void e2e_on_encoded(const MdEncodedPacket *pkt, void *ud) {
    E2E *e = ud;
    if (e->armed) {
        e->first_after_resize_key = pkt->is_key;
        e->armed = false;
    }
    pthread_mutex_lock(e->mu);
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
            md_encoder_submit(e->sync->encoder, f.data, f.stride,
                              MD_PIX_FMT_BGRA, (int64_t)f.seq,
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
            uint16_t tlen = 0;
            pthread_mutex_lock(e->mu);
            if (md_input_event_unpack(payload, hdr.payload_len, &kind, &button,
                                      &x, &y, &text, &tlen) == 0 &&
                kind == MD_INPUT_EV_MOVE) {
                md_input_mouse_move(e->input, x, y);
                MockInput *mi = e->input->backend_data;
                e->gx = mi->gx;
                e->gy = mi->gy;
                e->inputs++;
            }
            pthread_mutex_unlock(e->mu);
        }
        free(payload);
    }
    return NULL;
}

typedef struct {
    uint32_t w, h;
    int frames_at_first, frames_at_new;
    uint32_t first_w, first_h;
} ClientView;

static void e2e_on_decoded(const MdDecodedFrame *f, void *ud) {
    ClientView *v = ud;
    if (!v->first_w) { v->first_w = f->width; v->first_h = f->height; }
    v->w = f->width;
    v->h = f->height;
    if (f->width == v->first_w && f->height == v->first_h) v->frames_at_first++;
    else v->frames_at_new++;
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
    host_capture_sync_init(&sync, enc, &inp, &mu, G0.w, G0.h, &G0.rect);
    E2E e = { .sync = &sync, .cap = &cap, .peer = peer, .mu = &mu, .input = &inp };
    atomic_init(&e.stop, false);

    pthread_t tc, tp;
    REQUIRE(pthread_create(&tc, NULL, e2e_capture_thread, &e) == 0);
    REQUIRE(pthread_create(&tp, NULL, e2e_packet_thread, &e) == 0);

    MdDecoder *dec = md_decoder_create();
    REQUIRE(dec != NULL);
    ClientView v = {0};
    bool changed_requested = false, clicked = false;
    /* Event-driven: each step reacts to what the client decoded. The
     * packet budget only bounds a broken run. */
    for (int budget = 0; budget < 2000 && !clicked; budget++) {
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int rc = md_stream_recv(client, &hdr, &payload, 5000);
        if (rc != 0) { free(payload); break; }
        if (hdr.type == MD_PKT_VIDEO_FRAME) {
            md_decoder_submit(dec, payload, hdr.payload_len, 0);
            md_decoder_poll(dec, e2e_on_decoded, &v);
        }
        free(payload);
        if (!changed_requested && v.frames_at_first > 0) {
            capture_change(&mc, &G1);   /* host display changes mode */
            changed_requested = true;
        }
        if (v.frames_at_new > 0 && !clicked) {
            /* As the client's map_to_remote: the decoded frame's centre. */
            uint8_t buf[16];
            size_t n = md_input_event_pack(buf, sizeof(buf), MD_INPUT_EV_MOVE, 0,
                                           (int16_t)(v.w / 2), (int16_t)(v.h / 2),
                                           NULL, 0);
            MD_CHECK(md_stream_send(client, MD_PKT_INPUT, 1, buf, (uint32_t)n) == 0);
            clicked = true;
        }
    }
    MD_CHECK(clicked);

    /* Wait for the host to dispatch that click: its reply is the next
     * video packet after it, so drain until the packet thread counted it. */
    for (int budget = 0; budget < 2000; budget++) {
        pthread_mutex_lock(&mu);
        int inputs = e.inputs;
        pthread_mutex_unlock(&mu);
        if (inputs > 0) break;
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        if (md_stream_recv(client, &hdr, &payload, 5000) != 0) { free(payload); break; }
        free(payload);
    }

    /* The packet thread polls the stop flag between bounded recvs, so it
     * exits on a live connection. Only then shut the client end: a capture
     * send blocked on a full socket fails instead of hanging the join, and
     * no recv runs concurrently with that failing send (MdStream's
     * connected flag is not synchronized between send and recv). */
    atomic_store(&e.stop, true);
    pthread_join(tp, NULL);
    md_stream_close(client);
    pthread_join(tc, NULL);

    MD_CHECK(v.first_w == 640 && v.first_h == 360);
    MD_CHECK(v.w == 480 && v.h == 270);
    MD_CHECK(sync.resizes == 1);
    MD_CHECK(e.first_after_resize_key);
    MD_CHECK(e.inputs == 1);
    /* Decoded 480x270 centre (240,135) -> new display's centre. */
    MD_CHECK(e.gx == -960 && e.gy == 660);

    md_decoder_destroy(dec);
    md_encoder_destroy(enc);
    md_capture_stop(&cap);
    md_stream_destroy(client);
    md_stream_destroy(peer);
    md_stream_server_destroy(srv);
    printf("OK (%d frames before, %d after)\n", v.frames_at_first, v.frames_at_new);
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
    test_concurrent_dispatch();
    test_end_to_end_stream();
    return md_test_report();
}
