/*
 * metadesk — signer.h
 * Pluggable signing interface for Nostr identity operations.
 *
 * metadesk needs four signing operations:
 *   1. get_pubkey   — retrieve the user's x-only public key (hex)
 *   2. sign_event   — sign a Nostr event JSON (produce signed JSON)
 *   3. nip44_encrypt — NIP-44 encrypt plaintext for a peer
 *   4. nip44_decrypt — NIP-44 decrypt ciphertext from a peer
 *
 * These operations are dispatched through a vtable, allowing multiple
 * backends without changing calling code:
 *
 *   Backend         Key Location         Transport
 *   ─────────────   ──────────────────   ─────────────────────
 *   direct-key      in-process memory    none (local secp256k1)
 *   NIP-46          remote bunker        relay (kind:24133 RPC)
 *   NIP-55L         local daemon         D-Bus IPC
 *   NIP-5F          local daemon         Unix domain socket
 *
 * Spec §7 security model: the direct-key backend is used when the
 * secret key is retrieved from 1Password Connect (secrets.c). The
 * remote backends (NIP-46/55L/5F) never expose the key to metadesk.
 *
 * Thread safety: signer operations may block (network I/O, D-Bus).
 * Callers must not call from latency-sensitive paths (capture callback).
 * The nostr.c bridge calls signing from its worker thread.
 */
#ifndef MD_SIGNER_H
#define MD_SIGNER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Signer types ────────────────────────────────────────────── */

typedef enum {
    MD_SIGNER_DIRECT_KEY = 0,  /* in-process secp256k1 signing        */
    MD_SIGNER_NIP46,           /* NIP-46 Nostr Connect (remote bunker) */
    MD_SIGNER_NIP55L,          /* NIP-55L D-Bus local signer daemon    */
    MD_SIGNER_NIP5F,           /* NIP-5F Unix socket local signer      */
} MdSignerType;

/* ── Error codes ─────────────────────────────────────────────── */

typedef enum {
    MD_SIGNER_OK             =  0,
    MD_SIGNER_ERR_NOT_INIT   = -1,  /* signer not initialized            */
    MD_SIGNER_ERR_CONNECT    = -2,  /* failed to connect to backend      */
    MD_SIGNER_ERR_TIMEOUT    = -3,  /* backend operation timed out       */
    MD_SIGNER_ERR_DENIED     = -4,  /* signing request denied by user    */
    MD_SIGNER_ERR_INVALID    = -5,  /* invalid argument or malformed data */
    MD_SIGNER_ERR_CRYPTO     = -6,  /* cryptographic operation failed    */
    MD_SIGNER_ERR_INTERNAL   = -7,  /* internal/unexpected error         */
    MD_SIGNER_ERR_NO_KEY     = -8,  /* no key available in backend       */
} MdSignerError;

/* ── Vtable (operation dispatch) ─────────────────────────────── */

/* Forward declaration */
typedef struct MdSigner MdSigner;

/*
 * Signer operations vtable. Each backend implements these four functions.
 * All return MdSignerError (0 = success, <0 = error).
 * Output strings are heap-allocated; caller must free.
 */
typedef struct {
    /*
     * Get the user's x-only public key as 64-char hex string.
     * out_pubkey_hex: receives malloc'd hex string. Caller frees.
     */
    int (*get_pubkey)(MdSigner *s, char **out_pubkey_hex);

    /*
     * Sign a Nostr event.
     * event_json: unsigned event JSON (kind, content, tags, created_at, pubkey).
     * out_signed_json: receives malloc'd signed event JSON (with id, sig). Caller frees.
     */
    int (*sign_event)(MdSigner *s, const char *event_json, char **out_signed_json);

    /*
     * NIP-44 encrypt plaintext for a peer.
     * peer_pubkey_hex: 64-char hex pubkey of recipient.
     * plaintext: UTF-8 string to encrypt.
     * out_ciphertext: receives malloc'd base64 ciphertext. Caller frees.
     */
    int (*nip44_encrypt)(MdSigner *s, const char *peer_pubkey_hex,
                         const char *plaintext, char **out_ciphertext);

    /*
     * NIP-44 decrypt ciphertext from a peer.
     * peer_pubkey_hex: 64-char hex pubkey of sender.
     * ciphertext: base64-encoded NIP-44 ciphertext.
     * out_plaintext: receives malloc'd UTF-8 plaintext. Caller frees.
     */
    int (*nip44_decrypt)(MdSigner *s, const char *peer_pubkey_hex,
                         const char *ciphertext, char **out_plaintext);

    /*
     * Destroy backend-specific state. Called by md_signer_destroy().
     * The MdSigner struct itself is freed by the base destroy.
     */
    void (*destroy)(MdSigner *s);

} MdSignerOps;

/* ── Signer context ──────────────────────────────────────────── */

struct MdSigner {
    MdSignerType    type;
    const MdSignerOps *ops;
    void           *backend;    /* backend-specific opaque state */
    char           *pubkey_hex; /* cached pubkey (lazy-populated) */
};

/* ── Lifecycle ───────────────────────────────────────────────── */

/*
 * Create a direct-key signer from a hex secret key.
 * The key is copied into the signer and the caller's copy is NOT zeroed
 * (caller is responsible for zeroing their own buffer).
 * Uses nostrc for secp256k1 signing and NIP-44 encryption.
 */
MdSigner *md_signer_create_direct(const char *sk_hex);

/*
 * Create a NIP-46 signer from a bunker:// URI.
 * Connects to the remote signer via relay. Blocks until the initial
 * handshake completes or timeout_ms expires.
 * relay_urls: NULL-terminated array (overrides URI relays if non-NULL).
 */
MdSigner *md_signer_create_nip46(const char *bunker_uri,
                                 uint32_t timeout_ms);

/*
 * Create a NIP-55L signer (D-Bus).
 * Connects to the local org.nostr.Signer D-Bus service.
 * Returns NULL if the service is not available.
 */
MdSigner *md_signer_create_nip55l(void);

/*
 * Create a NIP-5F signer (Unix domain socket).
 * socket_path: path to signer socket, or NULL for default
 *              ($HOME/.local/share/nostr/signer.sock or $NOSTR_SIGNER_SOCK).
 * Returns NULL if the socket is not available.
 */
MdSigner *md_signer_create_nip5f(const char *socket_path);

/*
 * Auto-detect the best available signer backend.
 * Tries in order: NIP-5F socket → NIP-55L D-Bus → error.
 * Does NOT try NIP-46 (requires explicit bunker URI) or direct-key
 * (requires explicit secret key).
 * Returns NULL if no backend is available.
 */
MdSigner *md_signer_auto_detect(void);

/* ── Operations (dispatch through vtable) ────────────────────── */

/*
 * Get the user's public key (hex).
 * Result is cached after the first call.
 * out_pubkey_hex: receives malloc'd string. Caller frees.
 */
int md_signer_get_pubkey(MdSigner *s, char **out_pubkey_hex);

/*
 * Sign a Nostr event JSON.
 * event_json: unsigned event (must have kind, content, tags, created_at, pubkey).
 * out_signed_json: receives malloc'd signed event JSON. Caller frees.
 */
int md_signer_sign_event(MdSigner *s, const char *event_json,
                         char **out_signed_json);

/*
 * NIP-44 encrypt plaintext for a peer pubkey.
 */
int md_signer_nip44_encrypt(MdSigner *s, const char *peer_pubkey_hex,
                            const char *plaintext, char **out_ciphertext);

/*
 * NIP-44 decrypt ciphertext from a peer pubkey.
 */
int md_signer_nip44_decrypt(MdSigner *s, const char *peer_pubkey_hex,
                            const char *ciphertext, char **out_plaintext);

/* Get the signer backend type. */
MdSignerType md_signer_get_type(const MdSigner *s);

/* Get a human-readable name for the signer backend. */
const char *md_signer_type_name(MdSignerType type);

/* Check if the signer is ready (connected, key available). */
bool md_signer_is_ready(const MdSigner *s);

/* Destroy signer, zeroing any key material. */
void md_signer_destroy(MdSigner *s);

#ifdef __cplusplus
}
#endif

#endif /* MD_SIGNER_H */
