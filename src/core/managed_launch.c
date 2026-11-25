#include "managed_launch.h"
#include "session.h"

#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <errno.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* A public address plus the largest allowed explicit relay set, including
 * room for JSON escapes. A truncated configuration is never parsed. */
#define WIRE_MAX (3u * (MD_MANAGED_ADDRESS_MAX + MD_RELAY_TARGET_MAX * (MD_RELAY_URL_MAX + 1)) + 1024u)

#ifdef __linux__
static bool send_packet(int fd, const void *data, size_t len) {
    ssize_t n;
    do n = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT); while (n < 0 && errno == EINTR);
    return n >= 0 && (size_t)n == len;
}

static ssize_t receive_packet(int fd, void *data, size_t cap, int flags) {
    ssize_t n;
    do n = recv(fd, data, cap, MSG_TRUNC | flags); while (n < 0 && errno == EINTR);
    if (n > 0 && (size_t)n > cap) { errno = EMSGSIZE; return -1; }
    return n;
}

#else
#include "md_time.h"
#include <poll.h>

/* Darwin AF_UNIX has no SOCK_SEQPACKET. The initial configuration is a
 * network-order uint32 length plus exactly that many JSON bytes. Each later
 * message is a single byte (3 version bits, 5 message bits), so concurrent
 * client status producers cannot interleave parts of a message. */
static bool transfer_config(int fd, void *data, size_t len, bool writing, uint64_t deadline) {
    char *cursor = data;
    while (len) {
        ssize_t n = writing ? send(fd, cursor, len, MSG_DONTWAIT) : recv(fd, cursor, len, MSG_DONTWAIT);
        if (n > 0) { cursor += n; len -= (size_t)n; continue; }
        if (!n) { errno = EPROTO; return false; }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        uint64_t now = md_now_ms();
        if (now >= deadline) { errno = ETIMEDOUT; return false; }
        struct pollfd p = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int ready = poll(&p, 1, (int)(deadline - now));
        if (!ready) { errno = ETIMEDOUT; return false; }
        if (ready < 0 && errno != EINTR) return false;
    }
    return true;
}

static bool send_packet(int fd, void *data, size_t len) {
    uint32_t wire_size = htonl((uint32_t)len);
    uint64_t deadline = md_now_ms() + 5000;
    if (transfer_config(fd, &wire_size, sizeof(wire_size), true, deadline) &&
        transfer_config(fd, data, len, true, deadline)) return true;
    /* The peer must not interpret a later command as the remaining JSON. */
    shutdown(fd, SHUT_WR);
    return false;
}

static ssize_t receive_packet(int fd, void *data, size_t cap, int flags) {
    (void)flags; /* Only the initial configuration uses this framing. */
    uint32_t wire_size;
    uint64_t deadline = md_now_ms() + 5000;
    if (!transfer_config(fd, &wire_size, sizeof(wire_size), false, deadline)) return -1;
    uint32_t size = ntohl(wire_size);
    if (!size || size > cap) { errno = EMSGSIZE; return -1; }
    return transfer_config(fd, data, size, false, deadline) ? (ssize_t)size : -1;
}
#endif

static bool full_key(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}

static bool launch_valid(const MdManagedLaunch *c) {
    unsigned char ip[16];
    const uint8_t allowed = MD_CAP_VIDEO | MD_CAP_INPUT | MD_CAP_AGENT;
    if (!c || !memchr(c->address, 0, sizeof(c->address)) ||
        !memchr(c->pin, 0, sizeof(c->pin)) || !memchr(c->signer, 0, sizeof(c->signer)) ||
        !memchr(c->route, 0, sizeof(c->route)) || !full_key(c->pin) || !full_key(c->signer) || !c->port ||
        !c->relay_count || c->relay_count > MD_RELAY_TARGET_MAX ||
        !(c->capabilities & MD_CAP_VIDEO) || (c->capabilities & ~allowed) ||
        (*c->route && inet_pton(AF_INET, c->route, ip) != 1 &&
                      inet_pton(AF_INET6, c->route, ip) != 1)) return false;
    /* Parent resolves NIP-05 before review. Do not perform a second lookup,
     * or allow other NIP-19 types or raw route strings as the identity. */
    if (strchr(c->address, '@') || (!full_key(c->address) &&
        strncmp(c->address, "npub1", 5) && strncmp(c->address, "nprofile1", 9))) return false;
    MdIdentity id = {0};
    MdResolveResult r = md_identity_resolve(c->address, c->pin, &id);
    md_identity_clear(&id);
    return r == MD_RESOLVE_OK;
}

bool md_managed_launch_write(int fd, const MdManagedLaunch *c) {
    if (!launch_valid(c)) return false;
    json_t *relays = json_array();
    if (!relays) return false;
    bool ok = true;
    for (size_t i = 0; i < c->relay_count && ok; ++i)
        ok = *c->relays[i] && memchr(c->relays[i], 0, sizeof(c->relays[i])) &&
             json_array_append_new(relays, json_string(c->relays[i])) == 0;
    json_t *j = ok ? json_pack("{s:i,s:s,s:s,s:s,s:s,s:i,s:i,s:O}",
        "v", MD_MANAGED_LAUNCH_VERSION, "address", c->address, "pin", c->pin,
        "signer", c->signer, "route", c->route, "port", c->port, "caps", c->capabilities, "relays", relays) : NULL;
    json_decref(relays);
    char *wire = j ? json_dumps(j, JSON_COMPACT) : NULL;
    json_decref(j);
    ok = wire && strlen(wire) <= WIRE_MAX && send_packet(fd, wire, strlen(wire));
    free(wire);
    return ok;
}

static bool copy_string(json_t *j, char *out, size_t cap) {
    const char *s = json_string_value(j);
    size_t n = json_string_length(j);
    if (!s || n >= cap) return false;
    memcpy(out, s, n + 1);
    return true;
}

bool md_managed_launch_read(int fd, MdManagedLaunch *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    char *wire = malloc(WIRE_MAX);
    if (!wire) return false;
    ssize_t n = receive_packet(fd, wire, WIRE_MAX, 0);
    json_t *j = n > 0 ? json_loadb(wire, (size_t)n, JSON_REJECT_DUPLICATES, NULL) : NULL;
    free(wire);
    json_t *relays = json_object_get(j, "relays");
    json_int_t port = json_integer_value(json_object_get(j, "port"));
    out->relay_count = json_array_size(relays);
    json_int_t caps = json_integer_value(json_object_get(j, "caps"));
    bool ok = json_object_size(j) == 8 && caps > 0 && caps <= UINT8_MAX &&
        json_integer_value(json_object_get(j, "v")) == MD_MANAGED_LAUNCH_VERSION &&
        port > 0 && port <= UINT16_MAX && out->relay_count <= MD_RELAY_TARGET_MAX &&
        copy_string(json_object_get(j, "address"), out->address, sizeof(out->address)) &&
        copy_string(json_object_get(j, "pin"), out->pin, sizeof(out->pin)) &&
        copy_string(json_object_get(j, "signer"), out->signer, sizeof(out->signer)) &&
        copy_string(json_object_get(j, "route"), out->route, sizeof(out->route));
    if (ok) { out->port = (uint16_t)port; out->capabilities = (uint8_t)caps; }
    for (size_t i = 0; i < out->relay_count && ok; ++i)
        ok = copy_string(json_array_get(relays, i), out->relays[i], sizeof(out->relays[i])) && *out->relays[i];
    json_decref(j);
    if (ok && launch_valid(out)) return true;
    memset(out, 0, sizeof(*out));
    return false;
}

bool md_managed_launch_message_write(int fd, MdLaunchMessage msg) {
    if (msg < MD_LAUNCH_RESOLVE || msg > MD_LAUNCH_RELAY_REJECTED) return false;
#ifdef __linux__
    unsigned char wire[] = {MD_MANAGED_LAUNCH_VERSION, (unsigned char)msg};
    return send_packet(fd, wire, sizeof(wire));
#else
    _Static_assert(MD_MANAGED_LAUNCH_VERSION <= 7 && MD_LAUNCH_RELAY_REJECTED <= 31,
                   "managed launch message no longer fits in one byte");
    unsigned char wire = (MD_MANAGED_LAUNCH_VERSION << 5) | (unsigned char)msg;
    ssize_t n;
    do n = send(fd, &wire, sizeof(wire), MSG_DONTWAIT); while (n < 0 && errno == EINTR);
    return n == sizeof(wire);
#endif
}

int md_managed_launch_message_read(int fd, MdLaunchMessage *out, bool nonblocking) {
    if (!out) { errno = EINVAL; return -1; }
#ifdef __linux__
    unsigned char wire[2];
    ssize_t n = receive_packet(fd, wire, sizeof(wire), nonblocking ? MSG_DONTWAIT : 0);
    if (n <= 0) return (int)n;
    unsigned version = wire[0], message = n == sizeof(wire) ? wire[1] : 0;
#else
    unsigned char wire;
    ssize_t n;
    do n = recv(fd, &wire, sizeof(wire), nonblocking ? MSG_DONTWAIT : 0); while (n < 0 && errno == EINTR);
    if (n <= 0) return (int)n;
    unsigned version = wire >> 5, message = wire & 31;
#endif
    if (version != MD_MANAGED_LAUNCH_VERSION ||
        message < MD_LAUNCH_RESOLVE || message > MD_LAUNCH_RELAY_REJECTED) {
        errno = EPROTO;
        return -1;
    }
    *out = (MdLaunchMessage)message;
    return 1;
}
#endif
