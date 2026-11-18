/* Private NIP-46 signer for the opt-in full-process loopback session check. */
#include "nostr.h"
#include "fips_addr.h"
#include <nostr/nip46/nip46_bunker.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { MdSigner *signer; char *client_pk; } State;
static void request(NostrNip46BunkerRequest *request, void *data) {
    State *s = data;
    const NostrNip46Request *rpc = nostr_nip46_bunker_request_rpc(request);
    if (strcmp(nostr_nip46_bunker_request_client(request), s->client_pk)) {
        nostr_nip46_bunker_request_complete(request, NULL, "wrong fixture transport owner");
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
    nostr_nip46_bunker_request_complete(request, json, json ? NULL : "fixture signing failed");
    free(json); cJSON_Delete(value); free(out);
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "host") && strcmp(argv[1], "client"))) return 2;
    int role = !strcmp(argv[1], "host") ? 1 : 2;
    char user_sk[65], client_sk[65];
    snprintf(user_sk, sizeof(user_sk), "%064d", role);
    snprintf(client_sk, sizeof(client_sk), "%064d", role + 4);
    unsigned char remote_sk[32] = {0}; remote_sk[31] = (unsigned char)(role + 2);
    State s = {.signer = md_signer_create_direct(user_sk), .client_pk = md_nostr_get_pubkey(client_sk)};
    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    const char *relays[] = {argv[2]};
    if (!s.signer || !s.client_pk || !bunker ||
        nostr_nip46_bunker_set_transport_key(bunker, remote_sk) ||
        nostr_nip46_bunker_set_request_handler(bunker, request, &s) ||
        nostr_nip46_bunker_listen(bunker, relays, 1)) return 1;
    char *remote_pk = NULL, *user_pk = NULL;
    if (nostr_nip46_bunker_get_transport_pubkey(bunker, &remote_pk) ||
        md_signer_get_pubkey(s.signer, &user_pk)) return 1;
    char address[MD_FIPS_IPV6_STRLEN];
    if (md_fips_addr_from_pubkey_hex(user_pk, address, sizeof(address))) return 1;
    /* Only deterministic throwaway transport credentials; consumed on stdin by
     * account credentials-stdin, never a user's keyring or a public relay. */
    printf("{\"version\":1,\"user_pubkey\":\"%s\",\"remote_pubkey\":\"%s\",\"client_secret\":\"%s\",\"relays\":[\"%s\"]}\n%s\n",
           user_pk, remote_pk, client_sk, argv[2], address);
    fflush(stdout);
    (void)getchar();
    nostr_nip46_bunker_stop(bunker);
    nostr_nip46_session_free(bunker);
    md_signer_destroy(s.signer);
    free(s.client_pk); free(remote_pk); free(user_pk);
    return 0;
}
