/*
 * metadesk — signer.c
 * Pluggable signing interface with vtable dispatch.
 *
 * This file implements:
 *   1. The vtable dispatch layer (md_signer_*() functions)
 *   2. The direct-key backend (in-process secp256k1 via nostrc)
 *   3. Creation stubs for NIP-46, NIP-55L, NIP-5F backends
 *      (full implementations in separate beads)
 *   4. Auto-detection logic
 *
 * The direct-key backend delegates all crypto to nostrc:
 *   - nostr_key_get_public() for pubkey derivation
 *   - nostr_event_sign() for event signing
 *   - nostr_nip44_encrypt()/decrypt() for NIP-44
 *
 * Remote backends (NIP-46/55L/5F) are compiled conditionally
 * and linked when their nostrc NIP libraries are available.
 */
#include "signer.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>

/* ══════════════════════════════════════════════════════════════
 * Direct-key backend
 * ══════════════════════════════════════════════════════════════ */

/* Backend state for direct-key signer */
typedef struct {
    char  sk_hex[65];   /* 64 hex chars + null, mlock'd */
    char *pk_hex;       /* derived public key             */
} DirectKeyState;

/* ── Direct-key operations ───────────────────────────────────── */

static int direct_get_pubkey(MdSigner *s, char **out) {
    if (!s || !s->backend || !out)
        return MD_SIGNER_ERR_INVALID;

    DirectKeyState *dk = s->backend;
    if (!dk->pk_hex)
        return MD_SIGNER_ERR_NO_KEY;

    *out = strdup(dk->pk_hex);
    return *out ? MD_SIGNER_OK : MD_SIGNER_ERR_INTERNAL;
}

static int direct_sign_event(MdSigner *s, const char *event_json,
                             char **out_signed_json) {
    if (!s || !s->backend || !event_json || !out_signed_json)
        return MD_SIGNER_ERR_INVALID;

    DirectKeyState *dk = s->backend;

    /* Parse JSON into a NostrEvent */
    NostrEvent *ev = nostr_event_from_json(event_json);
    if (!ev)
        return MD_SIGNER_ERR_INVALID;

    /* Ensure pubkey is set */
    if (!nostr_event_get_pubkey(ev) || strlen(nostr_event_get_pubkey(ev)) == 0)
        nostr_event_set_pubkey(ev, dk->pk_hex);

    /* Sign the event (computes id and sig) */
    int ret = nostr_event_sign(ev, dk->sk_hex);
    if (ret != 0) {
        nostr_event_free(ev);
        return MD_SIGNER_ERR_CRYPTO;
    }

    /* Serialize back to JSON */
    char *json = nostr_event_to_json(ev);
    nostr_event_free(ev);

    if (!json)
        return MD_SIGNER_ERR_INTERNAL;

    *out_signed_json = json;
    return MD_SIGNER_OK;
}

static int direct_nip44_encrypt(MdSigner *s, const char *peer_pubkey_hex,
                                const char *plaintext, char **out_ciphertext) {
    if (!s || !s->backend || !peer_pubkey_hex || !plaintext || !out_ciphertext)
        return MD_SIGNER_ERR_INVALID;

    DirectKeyState *dk = s->backend;

    char *cipher = NULL;
    int ret = nostr_nip44_encrypt(dk->sk_hex, peer_pubkey_hex,
                                  plaintext, &cipher);
    if (ret != 0 || !cipher)
        return MD_SIGNER_ERR_CRYPTO;

    *out_ciphertext = cipher;
    return MD_SIGNER_OK;
}

static int direct_nip44_decrypt(MdSigner *s, const char *peer_pubkey_hex,
                                const char *ciphertext, char **out_plaintext) {
    if (!s || !s->backend || !peer_pubkey_hex || !ciphertext || !out_plaintext)
        return MD_SIGNER_ERR_INVALID;

    DirectKeyState *dk = s->backend;

    char *plain = NULL;
    int ret = nostr_nip44_decrypt(dk->sk_hex, peer_pubkey_hex,
                                  ciphertext, &plain);
    if (ret != 0 || !plain)
        return MD_SIGNER_ERR_CRYPTO;

    *out_plaintext = plain;
    return MD_SIGNER_OK;
}

static void direct_destroy(MdSigner *s) {
    if (!s || !s->backend) return;

    DirectKeyState *dk = s->backend;

    /* Zero secret key before freeing */
    memset(dk->sk_hex, 0, sizeof(dk->sk_hex));
    munlock(dk->sk_hex, sizeof(dk->sk_hex));

    free(dk->pk_hex);

    memset(dk, 0, sizeof(DirectKeyState));
    free(dk);
}

static const MdSignerOps direct_ops = {
    .get_pubkey    = direct_get_pubkey,
    .sign_event    = direct_sign_event,
    .nip44_encrypt = direct_nip44_encrypt,
    .nip44_decrypt = direct_nip44_decrypt,
    .destroy       = direct_destroy,
};

MdSigner *md_signer_create_direct(const char *sk_hex) {
    if (!sk_hex || strlen(sk_hex) != 64)
        return NULL;

    DirectKeyState *dk = calloc(1, sizeof(DirectKeyState));
    if (!dk) return NULL;

    /* Copy secret key into mlock'd buffer */
    memcpy(dk->sk_hex, sk_hex, 64);
    dk->sk_hex[64] = '\0';

    if (mlock(dk->sk_hex, sizeof(dk->sk_hex)) < 0) {
        fprintf(stderr, "signer: WARNING — mlock failed: %s\n", strerror(errno));
    }

    /* Derive public key */
    dk->pk_hex = nostr_key_get_public(dk->sk_hex);
    if (!dk->pk_hex) {
        memset(dk->sk_hex, 0, sizeof(dk->sk_hex));
        free(dk);
        return NULL;
    }

    MdSigner *s = calloc(1, sizeof(MdSigner));
    if (!s) {
        memset(dk->sk_hex, 0, sizeof(dk->sk_hex));
        free(dk->pk_hex);
        free(dk);
        return NULL;
    }

    s->type    = MD_SIGNER_DIRECT_KEY;
    s->ops     = &direct_ops;
    s->backend = dk;

    /* Cache pubkey */
    s->pubkey_hex = strdup(dk->pk_hex);

    fprintf(stderr, "signer: direct-key backend ready (pk=%.*s...)\n",
            8, dk->pk_hex);
    return s;
}

/* ══════════════════════════════════════════════════════════════
 * NIP-46 backend (Nostr Connect / remote bunker)
 * Full implementation in metadesk-pna.
 * ══════════════════════════════════════════════════════════════ */

#ifdef MD_SIGNER_ENABLE_NIP46
#include <nostr/nip46/nip46_client.h>
#include <nostr/nip46/nip46_uri.h>

typedef struct {
    NostrNip46Session *session;
    char              *remote_pubkey_hex;
} Nip46State;

static int nip46_get_pubkey(MdSigner *s, char **out) {
    if (!s || !s->backend || !out)
        return MD_SIGNER_ERR_INVALID;
    Nip46State *st = s->backend;

    char *pk = NULL;
    int ret = nostr_nip46_client_get_public_key_rpc(st->session, &pk);
    if (ret != 0 || !pk)
        return MD_SIGNER_ERR_CONNECT;

    *out = pk;
    return MD_SIGNER_OK;
}

static int nip46_sign_event(MdSigner *s, const char *event_json,
                            char **out_signed_json) {
    if (!s || !s->backend || !event_json || !out_signed_json)
        return MD_SIGNER_ERR_INVALID;
    Nip46State *st = s->backend;

    char *signed_json = NULL;
    int ret = nostr_nip46_client_sign_event(st->session, event_json, &signed_json);
    if (ret != 0 || !signed_json)
        return MD_SIGNER_ERR_CONNECT;

    *out_signed_json = signed_json;
    return MD_SIGNER_OK;
}

static int nip46_nip44_encrypt(MdSigner *s, const char *peer, const char *pt,
                               char **out) {
    if (!s || !s->backend || !peer || !pt || !out)
        return MD_SIGNER_ERR_INVALID;
    Nip46State *st = s->backend;

    char *ct = NULL;
    int ret = nostr_nip46_client_nip44_encrypt_rpc(st->session, peer, pt, &ct);
    if (ret != 0 || !ct)
        return MD_SIGNER_ERR_CONNECT;

    *out = ct;
    return MD_SIGNER_OK;
}

static int nip46_nip44_decrypt(MdSigner *s, const char *peer, const char *ct,
                               char **out) {
    if (!s || !s->backend || !peer || !ct || !out)
        return MD_SIGNER_ERR_INVALID;
    Nip46State *st = s->backend;

    char *pt = NULL;
    int ret = nostr_nip46_client_nip44_decrypt_rpc(st->session, peer, ct, &pt);
    if (ret != 0 || !pt)
        return MD_SIGNER_ERR_CONNECT;

    *out = pt;
    return MD_SIGNER_OK;
}

static void nip46_destroy(MdSigner *s) {
    if (!s || !s->backend) return;
    Nip46State *st = s->backend;

    if (st->session) {
        nostr_nip46_client_stop(st->session);
        nostr_nip46_session_free(st->session);
    }
    free(st->remote_pubkey_hex);
    free(st);
}

static const MdSignerOps nip46_ops = {
    .get_pubkey    = nip46_get_pubkey,
    .sign_event    = nip46_sign_event,
    .nip44_encrypt = nip46_nip44_encrypt,
    .nip44_decrypt = nip46_nip44_decrypt,
    .destroy       = nip46_destroy,
};

MdSigner *md_signer_create_nip46(const char *bunker_uri,
                                 uint32_t timeout_ms) {
    if (!bunker_uri)
        return NULL;

    /* Parse bunker:// URI */
    NostrNip46BunkerURI parsed;
    if (nostr_nip46_uri_parse_bunker(bunker_uri, &parsed) != 0) {
        fprintf(stderr, "signer: invalid bunker URI\n");
        return NULL;
    }

    /* Create NIP-46 client session */
    NostrNip46Session *session = nostr_nip46_client_new();
    if (!session) {
        nostr_nip46_uri_bunker_free(&parsed);
        return NULL;
    }

    /* Connect to bunker */
    if (nostr_nip46_client_connect(session, bunker_uri, NULL) != 0) {
        fprintf(stderr, "signer: NIP-46 connect failed\n");
        nostr_nip46_session_free(session);
        nostr_nip46_uri_bunker_free(&parsed);
        return NULL;
    }

    if (timeout_ms > 0)
        nostr_nip46_client_set_timeout(session, timeout_ms);

    /* Start persistent relay connection */
    if (nostr_nip46_client_start(session) != 0) {
        fprintf(stderr, "signer: NIP-46 relay connection failed\n");
        nostr_nip46_session_free(session);
        nostr_nip46_uri_bunker_free(&parsed);
        return NULL;
    }

    /* Send connect RPC handshake */
    char *connect_result = NULL;
    if (nostr_nip46_client_connect_rpc(session, parsed.secret, NULL,
                                       &connect_result) != 0) {
        fprintf(stderr, "signer: NIP-46 connect RPC failed\n");
        nostr_nip46_client_stop(session);
        nostr_nip46_session_free(session);
        nostr_nip46_uri_bunker_free(&parsed);
        return NULL;
    }
    free(connect_result);

    Nip46State *st = calloc(1, sizeof(Nip46State));
    if (!st) {
        nostr_nip46_client_stop(session);
        nostr_nip46_session_free(session);
        nostr_nip46_uri_bunker_free(&parsed);
        return NULL;
    }
    st->session = session;
    st->remote_pubkey_hex = strdup(parsed.remote_signer_pubkey_hex);
    nostr_nip46_uri_bunker_free(&parsed);

    MdSigner *s = calloc(1, sizeof(MdSigner));
    if (!s) {
        nip46_destroy(s);
        return NULL;
    }
    s->type    = MD_SIGNER_NIP46;
    s->ops     = &nip46_ops;
    s->backend = st;

    /* Fetch and cache pubkey */
    char *pk = NULL;
    if (nip46_get_pubkey(s, &pk) == MD_SIGNER_OK) {
        s->pubkey_hex = pk;
        fprintf(stderr, "signer: NIP-46 backend ready (pk=%.*s...)\n", 8, pk);
    } else {
        fprintf(stderr, "signer: NIP-46 backend ready (pubkey pending)\n");
    }

    return s;
}

#else /* !MD_SIGNER_ENABLE_NIP46 */

MdSigner *md_signer_create_nip46(const char *bunker_uri,
                                 uint32_t timeout_ms) {
    (void)bunker_uri; (void)timeout_ms;
    fprintf(stderr, "signer: NIP-46 backend not compiled\n");
    return NULL;
}

#endif /* MD_SIGNER_ENABLE_NIP46 */

/* ══════════════════════════════════════════════════════════════
 * NIP-55L backend (D-Bus local signer daemon)
 * Full implementation in metadesk-pmz.
 * ══════════════════════════════════════════════════════════════ */

#ifdef MD_SIGNER_ENABLE_NIP55L
#include <nostr/nip55l/signer_ops.h>

static int nip55l_get_pubkey(MdSigner *s, char **out) {
    (void)s;
    if (!out) return MD_SIGNER_ERR_INVALID;

    char *npub = NULL;
    int ret = nostr_nip55l_get_public_key(&npub);
    if (ret != 0 || !npub)
        return MD_SIGNER_ERR_CONNECT;

    *out = npub;
    return MD_SIGNER_OK;
}

static int nip55l_sign_event(MdSigner *s, const char *event_json,
                             char **out_signed_json) {
    (void)s;
    if (!event_json || !out_signed_json)
        return MD_SIGNER_ERR_INVALID;

    char *signed_json = NULL;
    int ret = nostr_nip55l_sign_event(event_json, "", "", &signed_json);
    if (ret != 0 || !signed_json)
        return MD_SIGNER_ERR_CONNECT;

    *out_signed_json = signed_json;
    return MD_SIGNER_OK;
}

static int nip55l_nip44_encrypt(MdSigner *s, const char *peer,
                                const char *pt, char **out) {
    (void)s;
    if (!peer || !pt || !out) return MD_SIGNER_ERR_INVALID;

    char *ct = NULL;
    int ret = nostr_nip55l_nip44_encrypt(pt, peer, "", &ct);
    if (ret != 0 || !ct)
        return MD_SIGNER_ERR_CRYPTO;

    *out = ct;
    return MD_SIGNER_OK;
}

static int nip55l_nip44_decrypt(MdSigner *s, const char *peer,
                                const char *ct, char **out) {
    (void)s;
    if (!peer || !ct || !out) return MD_SIGNER_ERR_INVALID;

    char *pt = NULL;
    int ret = nostr_nip55l_nip44_decrypt(ct, peer, "", &pt);
    if (ret != 0 || !pt)
        return MD_SIGNER_ERR_CRYPTO;

    *out = pt;
    return MD_SIGNER_OK;
}

static void nip55l_destroy(MdSigner *s) {
    (void)s;
    /* NIP-55L is stateless per-call; nothing to clean up */
}

static const MdSignerOps nip55l_ops = {
    .get_pubkey    = nip55l_get_pubkey,
    .sign_event    = nip55l_sign_event,
    .nip44_encrypt = nip55l_nip44_encrypt,
    .nip44_decrypt = nip55l_nip44_decrypt,
    .destroy       = nip55l_destroy,
};

MdSigner *md_signer_create_nip55l(void) {
    /* Probe: try to get pubkey to verify daemon is running */
    char *pk = NULL;
    int ret = nostr_nip55l_get_public_key(&pk);
    if (ret != 0 || !pk) {
        fprintf(stderr, "signer: NIP-55L D-Bus signer not available\n");
        return NULL;
    }

    MdSigner *s = calloc(1, sizeof(MdSigner));
    if (!s) { free(pk); return NULL; }

    s->type       = MD_SIGNER_NIP55L;
    s->ops        = &nip55l_ops;
    s->backend    = NULL;  /* stateless */
    s->pubkey_hex = pk;

    fprintf(stderr, "signer: NIP-55L D-Bus backend ready (pk=%.*s...)\n",
            8, pk);
    return s;
}

#else /* !MD_SIGNER_ENABLE_NIP55L */

MdSigner *md_signer_create_nip55l(void) {
    fprintf(stderr, "signer: NIP-55L backend not compiled\n");
    return NULL;
}

#endif /* MD_SIGNER_ENABLE_NIP55L */

/* ══════════════════════════════════════════════════════════════
 * NIP-5F backend (Unix domain socket signer)
 * Full implementation in metadesk-cf7.
 * ══════════════════════════════════════════════════════════════ */

#ifdef MD_SIGNER_ENABLE_NIP5F
#include <nostr/nip5f/nip5f.h>

typedef struct {
    void *conn;           /* nip5f client connection handle */
    char *socket_path;
} Nip5fState;

static int nip5f_get_pubkey(MdSigner *s, char **out) {
    if (!s || !s->backend || !out) return MD_SIGNER_ERR_INVALID;
    Nip5fState *st = s->backend;

    char *pk = NULL;
    int ret = nostr_nip5f_client_get_public_key(st->conn, &pk);
    if (ret != 0 || !pk)
        return MD_SIGNER_ERR_CONNECT;

    *out = pk;
    return MD_SIGNER_OK;
}

static int nip5f_sign_event(MdSigner *s, const char *event_json,
                            char **out_signed_json) {
    if (!s || !s->backend || !event_json || !out_signed_json)
        return MD_SIGNER_ERR_INVALID;
    Nip5fState *st = s->backend;

    char *signed_json = NULL;
    /* NIP-5F sign_event takes event_json + pubkey_hex */
    int ret = nostr_nip5f_client_sign_event(st->conn, event_json, "",
                                            &signed_json);
    if (ret != 0 || !signed_json)
        return MD_SIGNER_ERR_CONNECT;

    *out_signed_json = signed_json;
    return MD_SIGNER_OK;
}

static int nip5f_nip44_encrypt(MdSigner *s, const char *peer,
                               const char *pt, char **out) {
    if (!s || !s->backend || !peer || !pt || !out)
        return MD_SIGNER_ERR_INVALID;
    Nip5fState *st = s->backend;

    char *ct = NULL;
    int ret = nostr_nip5f_client_nip44_encrypt(st->conn, peer, pt, &ct);
    if (ret != 0 || !ct)
        return MD_SIGNER_ERR_CRYPTO;

    *out = ct;
    return MD_SIGNER_OK;
}

static int nip5f_nip44_decrypt(MdSigner *s, const char *peer,
                               const char *ct, char **out) {
    if (!s || !s->backend || !peer || !ct || !out)
        return MD_SIGNER_ERR_INVALID;
    Nip5fState *st = s->backend;

    char *pt = NULL;
    int ret = nostr_nip5f_client_nip44_decrypt(st->conn, peer, ct, &pt);
    if (ret != 0 || !pt)
        return MD_SIGNER_ERR_CRYPTO;

    *out = pt;
    return MD_SIGNER_OK;
}

static void nip5f_destroy(MdSigner *s) {
    if (!s || !s->backend) return;
    Nip5fState *st = s->backend;

    if (st->conn)
        nostr_nip5f_client_close(st->conn);
    free(st->socket_path);
    free(st);
}

static const MdSignerOps nip5f_ops = {
    .get_pubkey    = nip5f_get_pubkey,
    .sign_event    = nip5f_sign_event,
    .nip44_encrypt = nip5f_nip44_encrypt,
    .nip44_decrypt = nip5f_nip44_decrypt,
    .destroy       = nip5f_destroy,
};

static const char *nip5f_default_socket_path(void) {
    static char path[512];
    const char *env = getenv("NOSTR_SIGNER_SOCK");
    if (env) return env;

    const char *home = getenv("HOME");
    if (!home) return NULL;

    snprintf(path, sizeof(path), "%s/.local/share/nostr/signer.sock", home);
    return path;
}

MdSigner *md_signer_create_nip5f(const char *socket_path) {
    const char *path = socket_path ? socket_path : nip5f_default_socket_path();
    if (!path) {
        fprintf(stderr, "signer: NIP-5F socket path not determined\n");
        return NULL;
    }

    void *conn = NULL;
    int ret = nostr_nip5f_client_connect(path, &conn);
    if (ret != 0 || !conn) {
        fprintf(stderr, "signer: NIP-5F connect failed: %s\n", path);
        return NULL;
    }

    /* Probe: get pubkey to verify signer is operational */
    char *pk = NULL;
    ret = nostr_nip5f_client_get_public_key(conn, &pk);
    if (ret != 0 || !pk) {
        fprintf(stderr, "signer: NIP-5F get_public_key failed\n");
        nostr_nip5f_client_close(conn);
        return NULL;
    }

    Nip5fState *st = calloc(1, sizeof(Nip5fState));
    if (!st) {
        free(pk);
        nostr_nip5f_client_close(conn);
        return NULL;
    }
    st->conn = conn;
    st->socket_path = strdup(path);

    MdSigner *s = calloc(1, sizeof(MdSigner));
    if (!s) {
        nip5f_destroy(s);
        free(pk);
        return NULL;
    }

    s->type       = MD_SIGNER_NIP5F;
    s->ops        = &nip5f_ops;
    s->backend    = st;
    s->pubkey_hex = pk;

    fprintf(stderr, "signer: NIP-5F backend ready (pk=%.*s..., socket=%s)\n",
            8, pk, path);
    return s;
}

#else /* !MD_SIGNER_ENABLE_NIP5F */

MdSigner *md_signer_create_nip5f(const char *socket_path) {
    (void)socket_path;
    fprintf(stderr, "signer: NIP-5F backend not compiled\n");
    return NULL;
}

#endif /* MD_SIGNER_ENABLE_NIP5F */

/* ══════════════════════════════════════════════════════════════
 * Auto-detection
 * ══════════════════════════════════════════════════════════════ */

MdSigner *md_signer_auto_detect(void) {
    MdSigner *s;

    /* Try NIP-5F first (simplest, most secure for local) */
    s = md_signer_create_nip5f(NULL);
    if (s) return s;

    /* Try NIP-55L (D-Bus) */
    s = md_signer_create_nip55l();
    if (s) return s;

    fprintf(stderr, "signer: no local signer detected "
            "(tried NIP-5F socket, NIP-55L D-Bus)\n");
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 * Vtable dispatch layer
 * ══════════════════════════════════════════════════════════════ */

int md_signer_get_pubkey(MdSigner *s, char **out_pubkey_hex) {
    if (!s || !s->ops || !s->ops->get_pubkey || !out_pubkey_hex)
        return MD_SIGNER_ERR_NOT_INIT;

    /* Return cached value if available */
    if (s->pubkey_hex) {
        *out_pubkey_hex = strdup(s->pubkey_hex);
        return *out_pubkey_hex ? MD_SIGNER_OK : MD_SIGNER_ERR_INTERNAL;
    }

    int ret = s->ops->get_pubkey(s, out_pubkey_hex);
    if (ret == MD_SIGNER_OK && *out_pubkey_hex) {
        /* Cache for next time */
        s->pubkey_hex = strdup(*out_pubkey_hex);
    }
    return ret;
}

int md_signer_sign_event(MdSigner *s, const char *event_json,
                         char **out_signed_json) {
    if (!s || !s->ops || !s->ops->sign_event)
        return MD_SIGNER_ERR_NOT_INIT;
    return s->ops->sign_event(s, event_json, out_signed_json);
}

int md_signer_nip44_encrypt(MdSigner *s, const char *peer_pubkey_hex,
                            const char *plaintext, char **out_ciphertext) {
    if (!s || !s->ops || !s->ops->nip44_encrypt)
        return MD_SIGNER_ERR_NOT_INIT;
    return s->ops->nip44_encrypt(s, peer_pubkey_hex, plaintext, out_ciphertext);
}

int md_signer_nip44_decrypt(MdSigner *s, const char *peer_pubkey_hex,
                            const char *ciphertext, char **out_plaintext) {
    if (!s || !s->ops || !s->ops->nip44_decrypt)
        return MD_SIGNER_ERR_NOT_INIT;
    return s->ops->nip44_decrypt(s, peer_pubkey_hex, ciphertext, out_plaintext);
}

MdSignerType md_signer_get_type(const MdSigner *s) {
    return s ? s->type : MD_SIGNER_DIRECT_KEY;
}

const char *md_signer_type_name(MdSignerType type) {
    switch (type) {
    case MD_SIGNER_DIRECT_KEY: return "direct-key";
    case MD_SIGNER_NIP46:      return "NIP-46 (Nostr Connect)";
    case MD_SIGNER_NIP55L:     return "NIP-55L (D-Bus)";
    case MD_SIGNER_NIP5F:      return "NIP-5F (Unix socket)";
    default:                   return "unknown";
    }
}

bool md_signer_is_ready(const MdSigner *s) {
    if (!s || !s->ops)
        return false;

    /* A signer is ready if we can get a pubkey */
    if (s->pubkey_hex)
        return true;

    /* Try fetching (non-const cast for lazy init) */
    char *pk = NULL;
    int ret = s->ops->get_pubkey((MdSigner *)s, &pk);
    free(pk);
    return ret == MD_SIGNER_OK;
}

void md_signer_destroy(MdSigner *s) {
    if (!s) return;

    if (s->ops && s->ops->destroy)
        s->ops->destroy(s);

    free(s->pubkey_hex);

    memset(s, 0, sizeof(MdSigner));
    free(s);
}
