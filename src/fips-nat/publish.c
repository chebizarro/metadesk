/*
 * fips-nat — publish.c
 * Nostr publication via nostrc bridge.
 */
#include "publish.h"

int md_publish_transport(MdNostr *nostr, const char *fips_addr) {
    if (!nostr || !fips_addr) return -1;
    return md_nostr_publish_transport(nostr, fips_addr);
}
