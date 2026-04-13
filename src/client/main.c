/*
 * metadesk-client — main entry point.
 * Human video client with SDL2 display and Dear ImGui overlay.
 */
#include "session.h"
#include "packet.h"
#include "decode.h"
#include "render.h"

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

    printf("metadesk-client v0.1.0 starting...\n");

    /* TODO: Initialize subsystems
     * 1. Parse args (host npub)
     * 2. Negotiate session with host via Nostr DM
     * 3. Connect TCP to host fd00::npub:7700
     * 4. Initialize decoder
     * 5. Create SDL2 window + ImGui overlay
     * 6. Main loop: receive → decode → display
     */

    MdSession session;
    md_session_init(&session);

    printf("metadesk-client: stub — no connection yet.\n");

    while (g_running) {
        /* TODO: event loop (M1.3+) */
        break;
    }

    printf("metadesk-client shutting down.\n");
    return 0;
}
