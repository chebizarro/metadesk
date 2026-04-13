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

    printf("fips-nat v0.1.0 starting...\n");
    /* TODO: Phase 2 implementation */

    while (g_running) {
        break;
    }

    printf("fips-nat shutting down.\n");
    return 0;
}
