#ifndef MD_MANAGED_LAUNCH_H
#define MD_MANAGED_LAUNCH_H

#include "identity_resolver.h"
#include <stdint.h>

/* Private local channel. Only the client endpoint is inherited. Version 1
 * carries public configuration, never signer credentials: the child
 * restores the OS-backed client binding and verifies its public key. */
#define MD_MANAGED_LAUNCH_VERSION 1
#define MD_MANAGED_ADDRESS_MAX 8192

typedef struct {
    char address[MD_MANAGED_ADDRESS_MAX + 1];
    char pin[65];
    char signer[65];
    char route[46]; /* optional IPv4/IPv6 literal, never a host identity */
    char relays[MD_RELAY_TARGET_MAX][MD_RELAY_URL_MAX + 1];
    size_t relay_count;
    uint16_t port;
    uint8_t capabilities; /* video is required; input and accessibility are opt-in */
} MdManagedLaunch;

typedef enum {
    MD_LAUNCH_RESOLVE = 1, MD_LAUNCH_SIGNER, MD_LAUNCH_RELAY,
    MD_LAUNCH_FIPS, MD_LAUNCH_APPROVAL, MD_LAUNCH_DIAL, MD_LAUNCH_BIND,
    MD_LAUNCH_MEDIA, MD_LAUNCH_CONNECTED, MD_LAUNCH_FAILED, MD_LAUNCH_CANCELLED,
    MD_LAUNCH_CANCEL, MD_LAUNCH_DISCONNECT, MD_LAUNCH_ENDED,
    MD_LAUNCH_RELAY_AUTH, MD_LAUNCH_RELAY_CLOSED, MD_LAUNCH_RELAY_REJECTED
} MdLaunchMessage;

#if defined(__linux__) || defined(__APPLE__)
/* Strict bounded JSON configuration, followed by typed versioned messages. */
bool md_managed_launch_write(int fd, const MdManagedLaunch *);
bool md_managed_launch_read(int fd, MdManagedLaunch *);
bool md_managed_launch_message_write(int fd, MdLaunchMessage);
/* 1 message, 0 EOF, -1 invalid or I/O error (errno=EAGAIN if not ready). */
int md_managed_launch_message_read(int fd, MdLaunchMessage *, bool nonblocking);
#endif

#ifdef __APPLE__
#include <sys/types.h>
/* Verify a connected same-user Unix stream and suppress SIGPIPE on it. */
bool md_managed_launch_prepare_stream(int fd);
/* Absolute executable path. Returns a POSIX error number or zero. On success
 * the caller owns the nonblocking parent socket and must reap the child,
 * including on write failure. Send configuration immediately after spawning. */
int md_managed_launch_spawn(const char *client, int *fd, pid_t *pid);
#endif
#endif
