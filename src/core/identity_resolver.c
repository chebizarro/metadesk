#include "identity_resolver.h"
#include "discovery_http.h"
#include <nip05.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void md_identity_scrub_environment(void) {
#ifdef _WIN32
    _putenv_s("NIP05_ALLOW_INSECURE", "");
    _putenv_s("NIP05_TIMEOUT_MS", "");
#else
    unsetenv("NIP05_ALLOW_INSECURE");
    unsetenv("NIP05_TIMEOUT_MS");
#endif
}

void md_identity_clear(MdIdentity *id) {
    for (size_t i = 0; i < id->hint_count; ++i) free(id->hints[i]);
    memset(id, 0, sizeof(*id));
}

static bool hex_key(const char *s, char out[65]) {
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
        out[i] = c;
    }
    out[64] = '\0';
    return true;
}

static bool direct_key(const char *s, char out[65]) {
    if (hex_key(s, out)) return true;
    uint8_t key[32];
    if (nostr_nip19_decode_npub(s, key) != 0) return false;
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(key); ++i) {
        out[i * 2] = hex[key[i] >> 4];
        out[i * 2 + 1] = hex[key[i] & 15];
    }
    out[64] = '\0';
    return true;
}

char *md_identity_npub(const MdIdentity *id) {
    unsigned char key[32];
    char *npub = NULL;
    if (!id || !nostr_hex2bin(key, id->pubkey, sizeof(key)) ||
        nostr_nip19_encode_npub(key, &npub)) return NULL;
    return npub;
}

static const char *strip_uri(const char *s) {
    return s && !strncmp(s, "nostr:", 6) ? s + 6 : s;
}

static bool parse_identifier(const char *input, char **name, char **domain) {
    const char *at = input ? strchr(input, '@') : NULL;
    return at && at != input && strlen(input) <= 320 && strlen(at + 1) <= 253 &&
        nostr_nip05_parse_identifier(input, name, domain) == 0;
}

static MdResolveResult resolved(const char *key, const char *pin, MdIdentityHint source,
                                char **hints, size_t count, MdIdentity *out) {
    char pubkey[65], expected[65];
    if (!hex_key(key, pubkey)) return MD_RESOLVE_INVALID;
    if (pin && !direct_key(strip_uri(pin), expected)) return MD_RESOLVE_INVALID;
    if (pin && strcmp(expected, pubkey)) return MD_RESOLVE_PIN_MISMATCH;
    memcpy(out->pubkey, pubkey, sizeof(pubkey));
    out->hint_source = source;
    for (size_t i = 0; i < count && out->hint_count < MD_RELAY_TARGET_MAX; ++i) {
        if (!hints[i] || strlen(hints[i]) > MD_RELAY_URL_MAX) continue;
        char *hint = strdup(hints[i]);
        if (hint) out->hints[out->hint_count++] = hint;
    }
    return MD_RESOLVE_OK;
}

static MdResolveResult resolve_json(const char *name, const char *json,
                                    const char *pin, MdIdentity *out) {
    char *key = NULL, **hints = NULL;
    size_t count = 0;
    int rc = nostr_nip05_resolve_from_json(name, json, &key, &hints, &count, NULL);
    MdResolveResult result = rc ? MD_RESOLVE_NOT_FOUND :
        resolved(key, pin, MD_HINT_NIP05, hints, count, out);
    for (size_t i = 0; i < count; ++i) free(hints[i]);
    free(hints);
    free(key);
    return result;
}

MdResolveResult md_identity_from_nip05_json(const char *input, const char *json,
                                            const char *pin, MdIdentity *out) {
    memset(out, 0, sizeof(*out));
    char *name = NULL, *domain = NULL;
    if (!json || !parse_identifier(strip_uri(input), &name, &domain)) return MD_RESOLVE_INVALID;
    MdResolveResult result = resolve_json(name, json, pin, out);
    free(name);
    free(domain);
    return result;
}

MdResolveResult md_identity_resolve(const char *input, const char *pin, MdIdentity *out) {
    memset(out, 0, sizeof(*out));
    input = strip_uri(input);
    char key[65];
    if (!input || !*input || strlen(input) > 8192 ||
        (pin && !direct_key(strip_uri(pin), key))) return MD_RESOLVE_INVALID;
    if (direct_key(input, key)) return resolved(key, pin, MD_HINT_NONE, NULL, 0, out);
    NostrProfilePointer *profile = NULL;
    if (nostr_nip19_decode_nprofile(input, &profile) == 0 && profile) {
        MdResolveResult result = resolved(profile->public_key, pin, MD_HINT_NPROFILE,
                                          profile->relays, profile->relays_count, out);
        nostr_profile_pointer_free(profile);
        return result;
    }
    char *name = NULL, *domain = NULL;
    if (!parse_identifier(input, &name, &domain)) return MD_RESOLVE_INVALID;
    char *escaped = md_discovery_http_init() ? curl_easy_escape(NULL, name, 0) : NULL;
    char url[1024];
    bool unsafe = false;
    int len = escaped ? snprintf(url, sizeof(url), "https://%s/.well-known/nostr.json?name=%s", domain, escaped) : -1;
    curl_free(escaped);
    char *json = len > 0 && (size_t)len < sizeof(url) ?
        md_discovery_http_get(url, "Accept: application/json", true, 10000, &unsafe) : NULL; /* NIP-05 forbids redirects. */
    MdResolveResult result = json ? resolve_json(name, json, pin, out) :
        unsafe ? MD_RESOLVE_UNSAFE : MD_RESOLVE_FETCH_FAILED;
    free(json);
    free(name);
    free(domain);
    return result;
}

const char *md_resolve_error(MdResolveResult result) {
    switch (result) {
    case MD_RESOLVE_OK: return "resolved";
    case MD_RESOLVE_INVALID: return "invalid host identity or pin (expected npub, nprofile, 64-hex or name@domain; pin must be npub or hex)";
    case MD_RESOLVE_FETCH_FAILED: return "NIP-05 HTTPS lookup failed";
    case MD_RESOLVE_UNSAFE: return "NIP-05 refused an unsafe destination";
    case MD_RESOLVE_NOT_FOUND: return "NIP-05 document does not resolve this name";
    case MD_RESOLVE_PIN_MISMATCH: return "host key does not match the pin; connection refused";
    }
    return "unknown resolution failure";
}
