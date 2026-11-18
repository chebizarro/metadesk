/* Throwaway-key crypto worker for the private NIP-5F fixture. No user keys. */
#include "signer.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "client"))) return 2;
    const char *key = !strcmp(argv[1], "host")
        ? "0000000000000000000000000000000000000000000000000000000000000001"
        : "0000000000000000000000000000000000000000000000000000000000000002";
    MdSigner *s = md_signer_create_direct(key);
    char buffer[131073];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, stdin);
    buffer[n] = 0;
    cJSON *rpc = cJSON_Parse(buffer);
    cJSON *method = cJSON_GetObjectItemCaseSensitive(rpc, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(rpc, "params");
    cJSON *result = NULL;
    char *out = NULL;
    int rc = -1;
    if (s && cJSON_IsString(method)) {
        if (!strcmp(method->valuestring, "get_public_key")) {
            rc = md_signer_get_pubkey(s, &out);
        } else if (!strcmp(method->valuestring, "sign_event")) {
            char *event = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(params, "event"));
            if (event) rc = md_signer_sign_event(s, event, &out);
            free(event);
            if (!rc) result = cJSON_Parse(out);
        } else {
            cJSON *peer = cJSON_GetObjectItemCaseSensitive(params, "peer_pub");
            bool encrypt = !strcmp(method->valuestring, "nip44_encrypt");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(params, encrypt ? "plaintext" : "cipher_b64");
            if (cJSON_IsString(peer) && cJSON_IsString(text))
                rc = encrypt ? md_signer_nip44_encrypt(s, peer->valuestring, text->valuestring, &out)
                             : md_signer_nip44_decrypt(s, peer->valuestring, text->valuestring, &out);
        }
    }
    if (!rc && !result) result = cJSON_CreateString(out);
    char *json = result ? cJSON_PrintUnformatted(result) : NULL;
    if (json) puts(json);
    free(json); free(out);
    cJSON_Delete(result); cJSON_Delete(rpc);
    md_signer_destroy(s);
    return rc || !json ? 1 : 0;
}
