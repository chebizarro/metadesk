/* md-ah9.11 throwaway NIP-5F crypto worker for client-fixture.py; see
 * client-validation.md. One JSON-RPC request on stdin, one result on stdout.
 * The 32-byte hex key is read from a private file (argv[1]), never argv/env.
 * Test-only: real deployments use a managed signer, not a key on disk.
 * Build: cc -O2 -Wall -o nip5f-crypto nip5f-crypto.c -I<src>/src/core
 *        $(pkg-config --cflags --libs libcjson) -L<bundle>/opt/metadesk/lib -lmetadesk */
#include "signer.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_key(const char *path, char key[65]) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(key, 1, 64, f);
    fclose(f);
    key[64] = 0;
    return n == 64 && strspn(key, "0123456789abcdef") == 64 ? 0 : -1;
}

int main(int argc, char **argv) {
    char key[65];
    if (argc != 2 || read_key(argv[1], key)) return 2;
    MdSigner *s = md_signer_create_direct(key);
    memset(key, 0, sizeof(key));
    static char buffer[131073];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, stdin);
    buffer[n] = 0;
    cJSON *rpc = cJSON_Parse(buffer);
    cJSON *method = cJSON_GetObjectItemCaseSensitive(rpc, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(rpc, "params");
    cJSON *result = NULL;
    char *out = NULL;
    int rc = -1;
    if (s && cJSON_IsString(method)) {
        const char *m = method->valuestring;
        if (!strcmp(m, "get_public_key")) {
            rc = md_signer_get_pubkey(s, &out);
        } else if (!strcmp(m, "sign_event")) {
            char *event = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(params, "event"));
            if (event) rc = md_signer_sign_event(s, event, &out);
            free(event);
            if (!rc) result = cJSON_Parse(out);
        } else if (!strcmp(m, "nip44_encrypt") || !strcmp(m, "nip44_decrypt")) {
            bool encrypt = !strcmp(m, "nip44_encrypt");
            cJSON *peer = cJSON_GetObjectItemCaseSensitive(params, "peer_pub");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(params, encrypt ? "plaintext" : "cipher_b64");
            if (cJSON_IsString(peer) && cJSON_IsString(text))
                rc = encrypt ? md_signer_nip44_encrypt(s, peer->valuestring, text->valuestring, &out)
                             : md_signer_nip44_decrypt(s, peer->valuestring, text->valuestring, &out);
        }
    }
    if (!rc && !result) result = cJSON_CreateString(out);
    char *json = !rc && result ? cJSON_PrintUnformatted(result) : NULL;
    if (json) puts(json);
    free(json);
    free(out);
    cJSON_Delete(result);
    cJSON_Delete(rpc);
    md_signer_destroy(s);
    return json ? 0 : 1;
}
