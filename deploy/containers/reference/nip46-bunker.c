/* md-ah9.11 throwaway NIP-46 bunker for client-fixture.py; see client-validation.md.
 * argv: KEYFILE RELAY_URL. The 32-byte hex user key comes from a private file;
 * the transport key and connect secret are random per run. Prints one
 * bunker:// URI on stdout; the transport key that last presented that secret
 * is the only client served (one host/client process at a time). Exits on
 * SIGTERM/SIGINT. Test-only: real
 * deployments use a managed remote signer. Build with the link line of the
 * meson access_bunker_fixture target (-Dsigner_nip46=true). */
#include "nostr.h"
#include <nostr/nip46/nip46_bunker.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    MdSigner *signer;
    char secret[33];
    char owner[65];
    pthread_mutex_t mu;
} State;

static int random_bytes(unsigned char *out, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(out, 1, n, f) : 0;
    if (f) fclose(f);
    return got == n ? 0 : -1;
}

static void request(NostrNip46BunkerRequest *req, void *data) {
    State *s = data;
    const NostrNip46Request *rpc = nostr_nip46_bunker_request_rpc(req);
    const char *client = nostr_nip46_bunker_request_client(req);
    pthread_mutex_lock(&s->mu);
    bool owner = s->owner[0] && !strcmp(s->owner, client);
    if (!strcmp(rpc->method, "connect")) {
        bool ok = rpc->n_params >= 2 && !strcmp(rpc->params[1], s->secret);
        if (ok) snprintf(s->owner, sizeof(s->owner), "%s", client);
        pthread_mutex_unlock(&s->mu);
        nostr_nip46_bunker_request_complete(req, ok ? "\"ack\"" : NULL, ok ? NULL : "unauthorized");
        fprintf(stderr, "bunker: connect %s\n", ok ? "accepted" : "refused");
        return;
    }
    pthread_mutex_unlock(&s->mu);
    if (!owner) {
        nostr_nip46_bunker_request_complete(req, NULL, "unauthorized");
        return;
    }
    char *out = NULL;
    int rc = -1;
    if (!strcmp(rpc->method, "get_public_key")) rc = md_signer_get_pubkey(s->signer, &out);
    else if (!strcmp(rpc->method, "sign_event") && rpc->n_params == 1)
        rc = md_signer_sign_event(s->signer, rpc->params[0], &out);
    else if (!strcmp(rpc->method, "nip44_encrypt") && rpc->n_params == 2)
        rc = md_signer_nip44_encrypt(s->signer, rpc->params[0], rpc->params[1], &out);
    else if (!strcmp(rpc->method, "nip44_decrypt") && rpc->n_params == 2)
        rc = md_signer_nip44_decrypt(s->signer, rpc->params[0], rpc->params[1], &out);
    cJSON *value = !rc && out ? cJSON_CreateString(out) : NULL;
    char *json = value ? cJSON_PrintUnformatted(value) : NULL;
    nostr_nip46_bunker_request_complete(req, json, json ? NULL : "unsupported or failed");
    free(json);
    cJSON_Delete(value);
    free(out);
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGINT);
    pthread_sigmask(SIG_BLOCK, &stop, NULL); /* inherited by library threads */
    char key[65] = {0};
    FILE *f = fopen(argv[1], "r");
    size_t n = f ? fread(key, 1, 64, f) : 0;
    if (f) fclose(f);
    if (n != 64 || strspn(key, "0123456789abcdef") != 64) return 2;
    State s = {.signer = md_signer_create_direct(key), .mu = PTHREAD_MUTEX_INITIALIZER};
    memset(key, 0, sizeof(key));
    unsigned char transport[32], token[16];
    if (!s.signer || random_bytes(transport, sizeof(transport)) || random_bytes(token, sizeof(token))) return 1;
    for (size_t i = 0; i < sizeof(token); i++) snprintf(s.secret + 2 * i, 3, "%02x", token[i]);
    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    const char *relays[] = {argv[2]};
    if (!bunker || nostr_nip46_bunker_set_transport_key(bunker, transport) ||
        nostr_nip46_bunker_set_request_handler(bunker, request, &s) ||
        nostr_nip46_bunker_listen(bunker, relays, 1)) return 1;
    memset(transport, 0, sizeof(transport));
    char *remote = NULL;
    if (nostr_nip46_bunker_get_transport_pubkey(bunker, &remote)) return 1;
    printf("bunker://%s?relay=", remote);
    for (const unsigned char *p = (const unsigned char *)argv[2]; *p; p++) {
        if (strchr(":/?&=%#", *p)) printf("%%%02X", *p);
        else putchar(*p);
    }
    printf("&secret=%s\n", s.secret);
    fflush(stdout);
    int sig = 0;
    sigwait(&stop, &sig);
    nostr_nip46_bunker_stop(bunker);
    nostr_nip46_session_free(bunker);
    md_signer_destroy(s.signer);
    free(remote);
    return 0;
}
