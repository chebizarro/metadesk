/*
 * metadesk — fips_addr.c
 * FIPS address derivation and DNS resolution.
 *
 * Derives fd00::/8 ULA IPv6 addresses from Nostr public keys.
 * Uses FFmpeg's libavutil SHA-256 implementation (already a dependency).
 *
 * The bech32 decoder handles npub → raw 32-byte pubkey conversion.
 * The address derivation matches the Rust reference implementation:
 *   FipsAddress = 0xfd || SHA-256(pubkey)[0..15]
 */
#include "fips_addr.h"

#include <libavutil/sha.h>
#include <libavutil/mem.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ── Bech32 decoder (for npub → raw bytes) ───────────────────── */

static const char BECH32_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static int bech32_char_to_val(char c) {
    const char *p = strchr(BECH32_CHARSET, c);
    if (!p || c == '\0') return -1;
    return (int)(p - BECH32_CHARSET);
}

/*
 * Decode a bech32/bech32m string.
 * hrp_out: receives the human-readable part (must be >= hrp_max bytes)
 * data_out: receives the 5-bit values (must be large enough)
 * data_len: set to number of 5-bit values on output
 * Returns 0 on success, -1 on error.
 */
static int bech32_decode(const char *input,
                         char *hrp_out, size_t hrp_max,
                         uint8_t *data_out, size_t *data_len) {
    size_t input_len = strlen(input);
    if (input_len < 8 || input_len > 90)
        return -1;

    /* Find the last '1' separator */
    const char *sep = NULL;
    for (size_t i = input_len; i > 0; i--) {
        if (input[i - 1] == '1') {
            sep = &input[i - 1];
            break;
        }
    }
    if (!sep) return -1;

    size_t hrp_len = (size_t)(sep - input);
    if (hrp_len < 1 || hrp_len >= hrp_max)
        return -1;

    memcpy(hrp_out, input, hrp_len);
    hrp_out[hrp_len] = '\0';

    /* Lowercase the HRP for comparison */
    for (size_t i = 0; i < hrp_len; i++)
        hrp_out[i] = (char)tolower((unsigned char)hrp_out[i]);

    /* Decode data part (after '1', includes 6-char checksum) */
    const char *data_str = sep + 1;
    size_t data_str_len = input_len - hrp_len - 1;
    if (data_str_len < 6)
        return -1;

    /* Decode all 5-bit values (including checksum) */
    size_t total_vals = data_str_len;
    uint8_t *vals = calloc(total_vals, 1);
    if (!vals) return -1;

    for (size_t i = 0; i < total_vals; i++) {
        int v = bech32_char_to_val(tolower((unsigned char)data_str[i]));
        if (v < 0) {
            free(vals);
            return -1;
        }
        vals[i] = (uint8_t)v;
    }

    /* Strip 6-byte checksum, output data values */
    size_t payload_len = total_vals - 6;
    memcpy(data_out, vals, payload_len);
    *data_len = payload_len;

    free(vals);
    return 0;
}

/*
 * Convert 5-bit bech32 values to 8-bit bytes.
 * Returns number of output bytes, or -1 on error.
 */
static int convert_bits(const uint8_t *data, size_t data_len,
                        int from_bits, int to_bits, bool pad,
                        uint8_t *out, size_t out_max) {
    int acc = 0;
    int bits = 0;
    size_t out_len = 0;
    int maxv = (1 << to_bits) - 1;

    for (size_t i = 0; i < data_len; i++) {
        if (data[i] >> from_bits)
            return -1;
        acc = (acc << from_bits) | data[i];
        bits += from_bits;
        while (bits >= to_bits) {
            bits -= to_bits;
            if (out_len >= out_max) return -1;
            out[out_len++] = (uint8_t)((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits > 0) {
            if (out_len >= out_max) return -1;
            out[out_len++] = (uint8_t)((acc << (to_bits - bits)) & maxv);
        }
    } else if (bits >= from_bits || ((acc << (to_bits - bits)) & maxv)) {
        return -1;
    }
    return (int)out_len;
}

/*
 * Decode an npub bech32 string to a 32-byte x-only public key.
 * Returns 0 on success, -1 on error.
 */
static int decode_npub(const char *npub, uint8_t *pubkey_out) {
    if (!npub || !pubkey_out) return -1;

    char hrp[16];
    uint8_t data5[64];
    size_t data5_len = 0;

    if (bech32_decode(npub, hrp, sizeof(hrp), data5, &data5_len) < 0)
        return -1;

    if (strcmp(hrp, "npub") != 0)
        return -1;

    uint8_t data8[40];
    int n = convert_bits(data5, data5_len, 5, 8, false, data8, sizeof(data8));
    if (n != 32)
        return -1;

    memcpy(pubkey_out, data8, 32);
    return 0;
}

/* ── Hex decoding ────────────────────────────────────────────── */

static int hex_to_byte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -1;

    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_to_byte(hex[i * 2]);
        int lo = hex_to_byte(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── Core derivation ─────────────────────────────────────────── */

int md_fips_addr_from_pubkey(const uint8_t *pubkey, size_t pubkey_len,
                             uint8_t *addr_out) {
    if (!pubkey || pubkey_len != 32 || !addr_out)
        return -1;

    /* SHA-256(pubkey) using FFmpeg's libavutil */
    struct AVSHA *sha = av_sha_alloc();
    if (!sha) return -1;

    av_sha_init(sha, 256);
    av_sha_update(sha, pubkey, 32);

    uint8_t hash[32];
    av_sha_final(sha, hash);
    av_free(sha);

    /* FipsAddress = 0xfd || hash[0..15] */
    addr_out[0] = MD_FIPS_PREFIX;
    memcpy(&addr_out[1], hash, 15);

    return 0;
}

int md_fips_addr_to_string(const uint8_t *addr, char *out, size_t out_len) {
    if (!addr || !out || out_len < MD_FIPS_IPV6_STRLEN)
        return -1;

    /* Format as IPv6 using inet_ntop */
    struct in6_addr in6;
    memcpy(&in6, addr, 16);

    if (!inet_ntop(AF_INET6, &in6, out, (socklen_t)out_len))
        return -1;

    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int md_fips_addr_from_npub(const char *npub,
                           char *ipv6_out, size_t ipv6_len) {
    if (!npub || !ipv6_out || ipv6_len < MD_FIPS_IPV6_STRLEN)
        return -1;

    /* Decode npub → 32-byte pubkey */
    uint8_t pubkey[32];
    if (decode_npub(npub, pubkey) < 0)
        return -1;

    /* Derive FIPS address */
    uint8_t addr[16];
    if (md_fips_addr_from_pubkey(pubkey, 32, addr) < 0)
        return -1;

    return md_fips_addr_to_string(addr, ipv6_out, ipv6_len);
}

int md_fips_addr_from_pubkey_hex(const char *pk_hex,
                                 char *ipv6_out, size_t ipv6_len) {
    if (!pk_hex || !ipv6_out || ipv6_len < MD_FIPS_IPV6_STRLEN)
        return -1;

    /* Decode hex → 32-byte pubkey */
    uint8_t pubkey[32];
    if (hex_decode(pk_hex, pubkey, 32) < 0)
        return -1;

    /* Derive FIPS address */
    uint8_t addr[16];
    if (md_fips_addr_from_pubkey(pubkey, 32, addr) < 0)
        return -1;

    return md_fips_addr_to_string(addr, ipv6_out, ipv6_len);
}

int md_fips_dns_name(const char *npub, char *dns_out, size_t dns_len) {
    if (!npub || !dns_out)
        return -1;

    size_t npub_len = strlen(npub);
    size_t suffix_len = strlen(MD_FIPS_DNS_SUFFIX);
    size_t total = npub_len + suffix_len + 1;

    if (dns_len < total)
        return -1;

    memcpy(dns_out, npub, npub_len);
    memcpy(dns_out + npub_len, MD_FIPS_DNS_SUFFIX, suffix_len);
    dns_out[npub_len + suffix_len] = '\0';

    return 0;
}

int md_fips_resolve(const char *npub,
                    char *ipv6_out, size_t ipv6_len) {
    if (!npub || !ipv6_out || ipv6_len < MD_FIPS_IPV6_STRLEN)
        return -1;

    /* Build DNS name: npub1xxx.fips */
    char dns_name[256];
    if (md_fips_dns_name(npub, dns_name, sizeof(dns_name)) < 0)
        return -1;

    /* Try DNS resolution first (primes identity cache) */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;

    int gai_ret = getaddrinfo(dns_name, NULL, &hints, &res);
    if (gai_ret == 0 && res) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)res->ai_addr;
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, ipv6_out, (socklen_t)ipv6_len)) {
            freeaddrinfo(res);
            return 0;
        }
        freeaddrinfo(res);
    }

    /* Fall back to direct computation */
    fprintf(stderr, "fips: DNS resolution failed for %s, computing directly\n",
            dns_name);
    return md_fips_addr_from_npub(npub, ipv6_out, ipv6_len);
}

bool md_fips_is_fips_addr(const char *ipv6_str) {
    if (!ipv6_str) return false;

    struct in6_addr in6;
    if (inet_pton(AF_INET6, ipv6_str, &in6) != 1)
        return false;

    return in6.s6_addr[0] == MD_FIPS_PREFIX;
}

bool md_fips_is_npub(const char *str) {
    if (!str) return false;
    return strncmp(str, "npub1", 5) == 0 && strlen(str) >= 59;
}
