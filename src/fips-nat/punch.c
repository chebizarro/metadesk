/*
 * fips-nat — punch.c
 * Hole punch stub.
 */
#include "punch.h"

int md_punch_peer(const char *peer_addr, int peer_port) {
    (void)peer_addr; (void)peer_port;
    /* TODO: UDP hole punch (Phase 2) */
    return -1;
}
