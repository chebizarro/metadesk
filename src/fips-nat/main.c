/*
 * fips-nat — NAT traversal daemon.
 * STUN + Nostr signaling for hole punching.
 */
#include "stun.h"
#include "publish.h"
#include "punch.h"

#include <stdio.h>
#include <signal.h>

static volatile int g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    fprintf(stderr, "fips-nat v0.1.0\n");
    fprintf(stderr, "ERROR: NAT traversal daemon not yet implemented (Phase 2).\n");
    fprintf(stderr, "  STUN discovery, Nostr signaling, and UDP hole punching\n");
    fprintf(stderr, "  are planned for a future release.\n");
    return 1;
}
