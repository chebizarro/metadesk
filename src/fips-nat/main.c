/*
 * fips-nat — legacy NAT traversal daemon.
 * STUN discovery + Nostr signaling + UDP hole punching.
 *
 * DEPRECATED: FIPS v0.3+ owns discovery, STUN/TURN, and traversal.
 * This daemon remains for legacy testing only and is not the recommended
 * metadesk runtime path.
 *
 * Lifecycle:
 *   1. Parse CLI args (signer, relays, stun server, ports)
 *   2. Initialize signer → Nostr bridge
 *   3. Run STUN discovery to find our reflexive address
 *   4. Keep legacy NAT endpoint JSON available over IPC
 *   5. Listen on local IPC for commands from metadesk-host
 *   6. Handle punch requests, re-discovery, status queries
 *
 * IPC protocol (JSON over md_ipc_*):
 *   Commands (host → fips-nat):
 *     {"cmd":"status"}
 *     {"cmd":"discover"}
 *     {"cmd":"punch","peer_ip":"...","peer_port":N,"session_id":"hex32"}
 *     {"cmd":"shutdown"}
 *
 *   Responses (fips-nat → host):
 *     {"ok":true, ...}     (fields vary by command)
 *     {"ok":false,"error":"reason"}
 */
#include "stun.h"
#include "publish.h"
#include "punch.h"
#include "turn.h"
#include "fipsnat_ipc.h"

#include "nostr.h"
#include "signer.h"
#include "ipc.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* ── Globals ─────────────────────────────────────────────────── */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── Daemon state ────────────────────────────────────────────── */

typedef struct {
    MdStunResult    stun;           /* current reflexive address       */
    MdNatEndpoint   endpoint;       /* published NAT endpoint          */
    bool            stun_ok;        /* true if STUN succeeded          */
    bool            published;      /* true if endpoint published      */
    MdNostr        *nostr;          /* Nostr bridge (may be NULL)      */
    const char     *stun_server;    /* STUN server hostname            */
    uint16_t        stun_port;      /* STUN server port                */
    uint16_t        fips_port;      /* FIPS daemon port                */
    uint16_t        punch_port;     /* local punch bind port           */
    MdTurnConfig    turn;           /* TURN relay config (may be empty)*/
    bool            turn_enabled;   /* true if TURN credentials given  */
} FipsNatCtx;

/* ── IPC command dispatch ────────────────────────────────────── */

static char *handle_status(FipsNatCtx *ctx) {
    return md_fipsnat_ipc_status_response(
        ctx->stun_ok, ctx->published, &ctx->endpoint);
}

static char *handle_discover(FipsNatCtx *ctx) {
    int rc = md_stun_discover_full(
        ctx->stun_server, ctx->stun_port,
        &ctx->stun, MD_STUN_DEFAULT_TIMEOUT);

    if (rc < 0) {
        ctx->stun_ok = false;
        return md_fipsnat_ipc_error_response("STUN discovery failed");
    }

    ctx->stun_ok = true;
    ctx->endpoint.stun = ctx->stun;
    ctx->endpoint.fips_port = ctx->fips_port;
    ctx->endpoint.punch_port = ctx->punch_port;

    /* Re-publish updated endpoint */
    if (ctx->nostr) {
        if (md_publish_nat_endpoint(ctx->nostr, &ctx->endpoint) == 0)
            ctx->published = true;
    }

    return md_fipsnat_ipc_discover_response(&ctx->stun);
}

static char *handle_punch(FipsNatCtx *ctx, const MdFipsnatPunchReq *req) {
    MdPunchConfig cfg = {0};
    strncpy(cfg.peer_ip, req->peer_ip, sizeof(cfg.peer_ip) - 1);
    cfg.peer_port = req->peer_port;
    cfg.local_bind_port = 0;  /* ephemeral */
    cfg.timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : MD_PUNCH_DEFAULT_TIMEOUT;
    cfg.probe_interval_ms = MD_PUNCH_DEFAULT_INTERVAL;
    memcpy(cfg.session_id, req->session_id, 16);

    /* Try direct UDP hole punch first */
    MdPunchResult result = {0};
    int rc = md_punch_execute(&cfg, &result);

    if (rc == 0) {
        return md_fipsnat_ipc_punch_response(&result);
    }

    /* Direct punch failed — try TURN relay fallback */
    if (!ctx->turn_enabled) {
        return md_fipsnat_ipc_error_response(
            "hole punch failed and no TURN relay configured");
    }

    fprintf(stderr, "fips-nat: direct punch failed, trying TURN relay...\n");

    MdTurnConfig turn_cfg = ctx->turn;
    strncpy(turn_cfg.peer_ip, req->peer_ip, sizeof(turn_cfg.peer_ip) - 1);
    turn_cfg.peer_port = req->peer_port;

    MdTurnAlloc turn_alloc = {0};
    rc = md_turn_allocate(&turn_cfg, &turn_alloc);

    if (rc < 0) {
        return md_fipsnat_ipc_error_response(
            "hole punch and TURN relay both failed");
    }

    /* Build a result from the TURN allocation.
     * The fd is the TCP socket to the TURN server — data flows
     * through ChannelData framing, but the host can still use it
     * as a connected transport. */
    result.fd = turn_alloc.fd;
    strncpy(result.peer_ip, turn_alloc.relay_ip, sizeof(result.peer_ip) - 1);
    result.peer_port = turn_alloc.relay_port;
    result.local_port = 0;
    result.rtt_ms = 0; /* relay RTT not measured here */

    /* Detach fd so md_turn_close doesn't close it */
    turn_alloc.fd = -1;
    turn_alloc.allocated = false; /* prevent release on close */
    md_turn_close(&turn_alloc);

    fprintf(stderr, "fips-nat: TURN relay active: %s:%u\n",
            result.peer_ip, result.peer_port);

    return md_fipsnat_ipc_punch_response(&result);
}

static char *dispatch_command(FipsNatCtx *ctx, const char *json) {
    MdFipsnatCmd cmd;
    int rc = md_fipsnat_ipc_parse_command(json, &cmd);
    if (rc < 0) {
        return md_fipsnat_ipc_error_response("invalid command JSON");
    }

    switch (cmd.type) {
    case MD_FIPSNAT_CMD_STATUS:
        return handle_status(ctx);

    case MD_FIPSNAT_CMD_DISCOVER:
        return handle_discover(ctx);

    case MD_FIPSNAT_CMD_PUNCH:
        return handle_punch(ctx, &cmd.punch);

    case MD_FIPSNAT_CMD_SHUTDOWN:
        g_running = 0;
        return md_fipsnat_ipc_ok_response("shutting down");

    default:
        return md_fipsnat_ipc_error_response("unknown command");
    }
}

/* ── Usage ───────────────────────────────────────────────────── */

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [OPTIONS]\n\n", argv0);
    fprintf(stderr, "Legacy NAT traversal daemon for metadesk (deprecated).\n");
    fprintf(stderr, "FIPS daemon discovery/traversal is the supported runtime path.\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --stun-server HOST  STUN server (default: %s)\n",
            MD_STUN_DEFAULT_SERVER);
    fprintf(stderr, "  --stun-port PORT    STUN port (default: %d)\n",
            MD_STUN_DEFAULT_PORT);
    fprintf(stderr, "  --fips-port PORT    FIPS daemon port (default: 2121)\n");
    fprintf(stderr, "  --punch-port PORT   Local punch port (default: ephemeral)\n");
    fprintf(stderr, "  --ipc-name NAME     IPC endpoint name (default: fips-nat)\n");
    fprintf(stderr, "\nSigner options (choose one):\n");
    fprintf(stderr, "  --bunker URI        NIP-46 Nostr Connect bunker URI\n");
    fprintf(stderr, "  --dbus-signer       Use NIP-55L D-Bus signer daemon\n");
    fprintf(stderr, "  --socket-signer [PATH]  Use NIP-5F Unix socket signer\n");
    fprintf(stderr, "  --auto-signer       Auto-detect local signer\n");
    fprintf(stderr, "  --relay URL         Relay URL (repeatable)\n");
    fprintf(stderr, "  --no-publish        Skip unsupported legacy Nostr NAT publication (STUN only)\n");
    fprintf(stderr, "\nTURN relay fallback:\n");
    fprintf(stderr, "  --turn-server HOST  TURN server (e.g. turn.sharegap.net)\n");
    fprintf(stderr, "  --turn-port PORT    TURN port (default: %d)\n",
            MD_TURN_DEFAULT_PORT);
    fprintf(stderr, "  --turn-user USER    TURN username\n");
    fprintf(stderr, "  --turn-pass PASS    TURN password\n");
    fprintf(stderr, "  -h, --help          Show this help\n");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Defaults */
    const char *stun_server = NULL;   /* NULL → default */
    uint16_t    stun_port = 0;        /* 0 → default    */
    uint16_t    fips_port = 2121;
    uint16_t    punch_port = 0;       /* 0 → ephemeral  */
    const char *ipc_name = "fips-nat";
    const char *bunker_uri = NULL;
    const char *socket_path = NULL;
    bool        use_dbus_signer = false;
    bool        auto_signer = false;
    bool        no_publish = false;
    const char *relay_urls[16];
    int         relay_count = 0;
    const char *turn_server = NULL;
    uint16_t    turn_port = 0;
    const char *turn_user = NULL;
    const char *turn_pass = NULL;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stun-server") == 0 && i + 1 < argc)
            stun_server = argv[++i];
        else if (strcmp(argv[i], "--stun-port") == 0 && i + 1 < argc)
            stun_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--fips-port") == 0 && i + 1 < argc)
            fips_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--punch-port") == 0 && i + 1 < argc)
            punch_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--ipc-name") == 0 && i + 1 < argc)
            ipc_name = argv[++i];
        else if (strcmp(argv[i], "--bunker") == 0 && i + 1 < argc)
            bunker_uri = argv[++i];
        else if (strcmp(argv[i], "--dbus-signer") == 0)
            use_dbus_signer = true;
        else if (strcmp(argv[i], "--socket-signer") == 0) {
            socket_path = (i + 1 < argc && argv[i + 1][0] != '-')
                ? argv[++i] : NULL;
        }
        else if (strcmp(argv[i], "--auto-signer") == 0)
            auto_signer = true;
        else if (strcmp(argv[i], "--relay") == 0 && i + 1 < argc) {
            if (relay_count < 16)
                relay_urls[relay_count++] = argv[++i];
        }
        else if (strcmp(argv[i], "--no-publish") == 0)
            no_publish = true;
        else if (strcmp(argv[i], "--turn-server") == 0 && i + 1 < argc)
            turn_server = argv[++i];
        else if (strcmp(argv[i], "--turn-port") == 0 && i + 1 < argc)
            turn_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--turn-user") == 0 && i + 1 < argc)
            turn_user = argv[++i];
        else if (strcmp(argv[i], "--turn-pass") == 0 && i + 1 < argc)
            turn_pass = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "fips-nat: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* ── Banner ──────────────────────────────────────────────── */
    printf("fips-nat v0.1.0\n");
    printf("  stun:     %s:%u\n",
           stun_server ? stun_server : MD_STUN_DEFAULT_SERVER,
           stun_port   ? stun_port   : MD_STUN_DEFAULT_PORT);
    printf("  fips:     port %u\n", fips_port);
    printf("  punch:    port %s\n", punch_port ? "fixed" : "ephemeral");
    printf("  ipc:      %s\n", ipc_name);
    printf("\n");

    /* ── Daemon context ──────────────────────────────────────── */
    FipsNatCtx ctx = {
        .stun_server = stun_server,
        .stun_port   = stun_port,
        .fips_port   = fips_port,
        .punch_port  = punch_port,
    };

    /* ── TURN relay config ────────────────────────────────────── */
    if (turn_server) {
        strncpy(ctx.turn.server, turn_server, sizeof(ctx.turn.server) - 1);
        ctx.turn.port = turn_port;
        if (turn_user)
            strncpy(ctx.turn.username, turn_user, sizeof(ctx.turn.username) - 1);
        if (turn_pass)
            strncpy(ctx.turn.password, turn_pass, sizeof(ctx.turn.password) - 1);
        ctx.turn_enabled = true;
        printf("fips-nat: TURN fallback: %s:%u\n",
               turn_server, turn_port ? turn_port : MD_TURN_DEFAULT_PORT);
    }

    /* ── Signer initialization ───────────────────────────────── */
    MdSigner *signer = NULL;
    if (bunker_uri) {
        printf("fips-nat: connecting to NIP-46 bunker...\n");
        signer = md_signer_create_nip46(bunker_uri, 30000);
    } else if (use_dbus_signer) {
        printf("fips-nat: connecting to NIP-55L D-Bus signer...\n");
        signer = md_signer_create_nip55l();
    } else if (socket_path || auto_signer) {
        if (socket_path) {
            printf("fips-nat: connecting to NIP-5F socket signer...\n");
            signer = md_signer_create_nip5f(socket_path);
        }
        if (!signer && auto_signer) {
            printf("fips-nat: auto-detecting signer...\n");
            signer = md_signer_auto_detect();
        }
    }

    if (signer) {
        printf("fips-nat: signer ready (%s)\n",
               md_signer_type_name(md_signer_get_type(signer)));
    } else if (bunker_uri || use_dbus_signer) {
        fprintf(stderr, "fips-nat: ERROR: requested signer backend not available\n");
        return 1;
    }

    /* ── Nostr bridge (if signer available and publish enabled) ─ */
    MdNostr *nostr = NULL;
    if (signer && !no_publish) {
        if (relay_count == 0) {
            relay_urls[0] = "wss://relay.sharegap.net";
            relay_count = 1;
        }

        MdNostrCallbacks nostr_cbs = {0};
        MdNostrConfig nostr_cfg = {
            .signer     = signer,
            .relay_urls = relay_urls,
            .relay_count = relay_count,
        };

        nostr = md_nostr_create(&nostr_cfg, &nostr_cbs);
        if (nostr) {
            ctx.nostr = nostr;
            printf("fips-nat: nostr bridge ready (%d relay%s)\n",
                   relay_count, relay_count != 1 ? "s" : "");
        } else {
            fprintf(stderr, "fips-nat: WARNING: nostr bridge creation failed, "
                    "STUN-only mode\n");
        }
    } else if (!signer) {
        printf("fips-nat: no signer — running in STUN-only mode "
               "(no Nostr publication)\n");
    }

    /* ── STUN discovery ──────────────────────────────────────── */
    printf("fips-nat: discovering reflexive address via STUN...\n");
    int stun_rc = md_stun_discover_full(
        stun_server, stun_port, &ctx.stun, MD_STUN_DEFAULT_TIMEOUT);

    if (stun_rc == 0) {
        ctx.stun_ok = true;
        printf("fips-nat: reflexive address: %s:%u%s\n",
               ctx.stun.ip, ctx.stun.port,
               ctx.stun.is_ipv6 ? " (IPv6)" : "");

        /* Build endpoint */
        ctx.endpoint.stun = ctx.stun;
        ctx.endpoint.fips_port = fips_port;
        ctx.endpoint.punch_port = punch_port;

        /* Publish to Nostr */
        if (nostr) {
            if (md_publish_nat_endpoint(nostr, &ctx.endpoint) == 0) {
                ctx.published = true;
                printf("fips-nat: NAT endpoint published to relays\n");
            } else {
                fprintf(stderr, "fips-nat: WARNING: failed to publish NAT endpoint\n");
            }
        }
    } else {
        fprintf(stderr, "fips-nat: WARNING: STUN discovery failed — "
                "NAT traversal may not work\n");
    }

    /* ── IPC server ──────────────────────────────────────────── */
    MdIpcServer *ipc = md_ipc_listen(ipc_name);
    if (!ipc) {
        fprintf(stderr, "fips-nat: ERROR: failed to create IPC server '%s'\n",
                ipc_name);
        if (nostr) md_nostr_destroy(nostr);
        else if (signer) md_signer_destroy(signer);
        return 1;
    }
    printf("fips-nat: IPC listening on %s\n", md_ipc_server_path(ipc));
    printf("fips-nat: ready (Ctrl+C to stop)\n\n");

    /* ── Main loop: accept IPC connections, dispatch commands ── */
    while (g_running) {
        /* Accept with 1-second timeout so we can check g_running */
        MdIpcConn *conn = md_ipc_accept(ipc, 1000);
        if (!conn)
            continue;

        printf("fips-nat: IPC client connected\n");

        /* Handle commands on this connection until disconnect */
        while (g_running && md_ipc_is_connected(conn)) {
            char buf[MD_IPC_MAX_MSG];
            int n = md_ipc_recv(conn, buf, sizeof(buf) - 1, 1000);

            if (n <= 0) {
                if (n == 0) break;  /* peer disconnect */
                continue;           /* timeout, retry  */
            }
            buf[n] = '\0';

            /* Dispatch and respond */
            char *resp = dispatch_command(&ctx, buf);
            if (resp) {
                md_ipc_send(conn, resp, strlen(resp));
                free(resp);
            }
        }

        md_ipc_close(conn);
        printf("fips-nat: IPC client disconnected\n");
    }

    /* ── Shutdown ────────────────────────────────────────────── */
    printf("\nfips-nat: shutting down...\n");
    md_ipc_server_destroy(ipc);
    if (nostr) md_nostr_destroy(nostr);
    else if (signer) md_signer_destroy(signer);

    printf("fips-nat: done.\n");
    return 0;
}
