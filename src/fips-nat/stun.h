/*
 * fips-nat — stun.h
 * STUN address discovery via libnice.
 */
#ifndef MD_STUN_H
#define MD_STUN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Discover public address via STUN. Writes IP string to buf. */
int md_stun_discover(const char *stun_server, char *buf, int buf_len);

#ifdef __cplusplus
}
#endif

#endif /* MD_STUN_H */
