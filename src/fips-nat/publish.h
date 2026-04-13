/*
 * fips-nat — publish.h
 * Nostr transport address publication via nostrc.
 */
#ifndef MD_PUBLISH_H
#define MD_PUBLISH_H

#include "nostr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Publish FIPS transport address to Nostr relays via nostrc bridge. */
int md_publish_transport(MdNostr *nostr, const char *fips_addr);

#ifdef __cplusplus
}
#endif

#endif /* MD_PUBLISH_H */
