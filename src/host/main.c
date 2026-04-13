/*
 * metadesk-host — main entry point.
 * Screen capture, encoding, AT-SPI2 tree, session management.
 */
#include "capture.h"
#include "encode.h"
#include "atspi.h"
#include "input.h"
#include "session.h"
#include "packet.h"
#include "nostr.h"
#include "secrets.h"

#include <stdio.h>
#include <stdlib.h>
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

    printf("metadesk-host v0.1.0 starting...\n");

    /* TODO: Initialize subsystems
     * 1. Load config
     * 2. Retrieve secrets from 1Password Connect
     * 3. Initialize session state machine
     * 4. Start PipeWire capture
     * 5. Initialize encoder
     * 6. Start AT-SPI2 tree walker
     * 7. Listen on port 7700 for client connections
     * 8. Main loop: capture → encode → send frames
     */

    MdSession session;
    md_session_init(&session);

    printf("metadesk-host: session initialized, waiting for connections on :7700\n");

    while (g_running) {
        /* TODO: event loop (M1.3+) */
        break; /* stub: exit immediately */
    }

    printf("metadesk-host shutting down.\n");
    return 0;
}
