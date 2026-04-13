/*
 * fips-nat — stun.c
 * STUN discovery stub.
 */
#include "stun.h"

int md_stun_discover(const char *stun_server, char *buf, int buf_len) {
    (void)stun_server; (void)buf; (void)buf_len;
    /* TODO: libnice STUN (Phase 2) */
    return -1;
}
