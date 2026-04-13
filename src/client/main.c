/*
 * metadesk-client — main entry point.
 * Receive → decode → display pipeline over TCP.
 *
 * Phase 1 flow:
 *   1. Connect TCP to host on port 7700
 *   2. Initialize H.264 decoder
 *   3. Create SDL2 window for display
 *   4. Main loop:
 *      a. Receive framed packets from host
 *      b. Decode H.264 video frames
 *      c. Display RGBA frames via SDL2
 *      d. Handle ping/pong keepalive
 *   5. Clean shutdown on SIGINT/SIGTERM or window close
 */
#include "fips_addr.h"
#include "session.h"
#include "packet.h"
#include "stream.h"
#include "decode.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── State ───────────────────────────────────────────────────── */

typedef struct {
    MdRenderer   *renderer;
    uint32_t      frames_decoded;
    uint32_t      frames_displayed;
    int64_t       total_decode_us;
} ClientCtx;

/* ── Decode callback: display frame ──────────────────────────── */

static void on_decoded(const MdDecodedFrame *frame, void *userdata) {
    ClientCtx *ctx = userdata;
    ctx->frames_decoded++;

    if (ctx->renderer) {
        int ret = md_renderer_present(ctx->renderer, frame->data,
                                      frame->width, frame->height);
        if (ret == 0)
            ctx->frames_displayed++;
    }
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s HOST|--npub NPUB [OPTIONS]\n\n", argv0);
    fprintf(stderr, "Connect to a metadesk host and display remote desktop.\n\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  HOST               Host address (IP or hostname)\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --npub NPUB        Connect via FIPS mesh (npub1xxx)\n");
    fprintf(stderr, "  --port PORT        Host port (default: %d)\n", MD_STREAM_PORT);
    fprintf(stderr, "  --no-display       Decode only, no SDL2 window\n");
    fprintf(stderr, "  --timeout MS       Connect timeout (default: 5000)\n");
    fprintf(stderr, "  -h, --help         Show this help\n");
}

/* ── Timing helper ───────────────────────────────────────────── */

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Default options */
    const char *host = NULL;
    const char *npub = NULL;
    uint16_t port = MD_STREAM_PORT;
    bool do_display = true;
    uint32_t timeout_ms = 5000;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-display") == 0)
            do_display = false;
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout_ms = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--npub") == 0 && i + 1 < argc)
            npub = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        else if (argv[i][0] != '-' && !host)
            host = argv[i];
    }

    if (!host && !npub) {
        fprintf(stderr, "ERROR: host address or --npub required\n\n");
        usage(argv[0]);
        return 1;
    }

    printf("metadesk-client v0.1.0\n");
    if (npub) {
        printf("  fips:      %.*s...\n", 12, npub);
    } else {
        printf("  host:      %s:%u\n", host, port);
    }
    printf("  display:   %s\n", do_display ? "SDL2" : "disabled");
    printf("  timeout:   %u ms\n", timeout_ms);
    printf("\n");

    /* Initialize session state */
    MdSession session;
    md_session_init(&session);

    /* Connect to host */
    MdStream *stream = NULL;
    if (npub) {
        /* FIPS mesh connect: resolve npub → fd00::/8 → TCP */
        printf("client: connecting via FIPS to %.*s...\n", 12, npub);
        stream = md_stream_connect_fips(npub, port, timeout_ms);
        if (!stream) {
            fprintf(stderr, "ERROR: FIPS connect failed for %.*s...\n", 12, npub);
            return 1;
        }
    } else {
        printf("client: connecting to %s:%u...\n", host, port);
        stream = md_stream_connect(host, port, timeout_ms);
        if (!stream) {
            fprintf(stderr, "ERROR: failed to connect to %s:%u\n", host, port);
            return 1;
        }
    }
    printf("client: connected\n");

    /* Create decoder */
    MdDecoder *decoder = md_decoder_create();
    if (!decoder) {
        fprintf(stderr, "ERROR: failed to create H.264 decoder\n");
        md_stream_destroy(stream);
        return 1;
    }

    /* Create renderer (SDL2 window) */
    ClientCtx ctx = { 0 };
    if (do_display) {
        ctx.renderer = md_renderer_create(1920, 1080, "metadesk");
        if (!ctx.renderer) {
            fprintf(stderr, "WARNING: SDL2 renderer unavailable, decode-only mode\n");
        }
    }

    /* ── Main loop ───────────────────────────────────────────── */
    printf("client: receiving stream... (Ctrl+C to stop)\n\n");

    uint32_t pkt_seq = 0;
    uint32_t last_stats_ms = md_stream_now_ms();
    uint32_t video_packets = 0;

    while (g_running && md_stream_is_connected(stream)) {
        /* Check SDL events (window close, etc.) */
        if (ctx.renderer) {
            if (md_renderer_poll_events(ctx.renderer) < 0) {
                printf("client: window closed\n");
                break;
            }
        }

        /* Receive next packet */
        MdPacketHeader hdr;
        uint8_t *payload = NULL;
        int ret = md_stream_recv(stream, &hdr, &payload, 16 /* ~1 frame at 60fps */);

        if (ret == 1) {
            /* Timeout — no data yet, loop back */
            continue;
        }
        if (ret < 0) {
            /* Connection lost */
            fprintf(stderr, "client: connection lost\n");
            break;
        }

        /* Handle packet by type */
        switch (hdr.type) {
        case MD_PKT_VIDEO_FRAME: {
            video_packets++;

            int64_t dec_start = now_us();

            ret = md_decoder_submit(decoder, payload, hdr.payload_len, hdr.sequence);
            if (ret == 0) {
                md_decoder_poll(decoder, on_decoded, &ctx);
            }

            int64_t dec_elapsed = now_us() - dec_start;
            ctx.total_decode_us += dec_elapsed;
            break;
        }

        case MD_PKT_PING:
            /* Reply with pong */
            md_stream_send(stream, MD_PKT_PONG, pkt_seq++, NULL, 0);
            break;

        case MD_PKT_PONG:
            md_stream_handle_pong(stream, &hdr);
            break;

        case MD_PKT_UI_TREE:
        case MD_PKT_UI_TREE_DELTA:
            /* TODO: parse and display UI tree (M1.6/M1.7) */
            break;

        case MD_PKT_SESSION_INFO:
            fprintf(stderr, "client: received session info\n");
            break;

        default:
            fprintf(stderr, "client: unknown packet type 0x%02x\n", hdr.type);
            break;
        }

        free(payload);

        /* Print stats every 5 seconds */
        uint32_t now = md_stream_now_ms();
        if (now - last_stats_ms >= 5000) {
            MdStreamStats stats;
            md_stream_get_stats(stream, &stats);

            double avg_decode_ms = ctx.frames_decoded > 0
                ? (double)ctx.total_decode_us / ctx.frames_decoded / 1000.0
                : 0.0;

            printf("client: packets=%u decoded=%u displayed=%u "
                   "decode_avg=%.1fms rtt=%ums rx=%.1fMB\n",
                   video_packets, ctx.frames_decoded, ctx.frames_displayed,
                   avg_decode_ms,
                   stats.avg_rtt_ms,
                   (double)stats.bytes_recv / (1024.0 * 1024.0));

            last_stats_ms = now;
        }
    }

    /* ── Shutdown ────────────────────────────────────────────── */
    printf("\nclient: shutting down...\n");

    md_decoder_flush(decoder, on_decoded, &ctx);
    md_decoder_destroy(decoder);
    md_stream_destroy(stream);

    if (ctx.renderer)
        md_renderer_destroy(ctx.renderer);

    printf("client: done. decoded %u frames, displayed %u\n",
           ctx.frames_decoded, ctx.frames_displayed);
    return 0;
}
