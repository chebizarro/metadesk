/*
 * fips-nat — punch.h
 * UDP hole punch coordinator.
 */
#ifndef MD_PUNCH_H
#define MD_PUNCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Attempt UDP hole punch to peer. */
int md_punch_peer(const char *peer_addr, int peer_port);

#ifdef __cplusplus
}
#endif

#endif /* MD_PUNCH_H */
