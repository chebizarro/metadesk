#include "credential_store.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

struct MdCredentialStore { MdCredentialStoreOps ops; void *context; };
void md_credential_wipe(void *p, size_t n) {
    volatile unsigned char *b = p;
    while (n--) *b++ = 0;
}
void md_credential_free(char *p) {
    if (p) { md_credential_wipe(p, strlen(p)); free(p); }
}
static bool hexkey(const char *s) {
    return s && strnlen(s, 65) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static bool valid(const MdNip46Credentials *c) {
    if (!c || !hexkey(c->user_pubkey) || !hexkey(c->remote_pubkey) ||
        !hexkey(c->client_secret) || strspn(c->client_secret, "0") == 64 ||
        strcmp(c->client_secret, "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141") >= 0 ||
        !c->relays || !c->relay_count || c->relay_count > 16) return false;
    for (size_t i = 0; i < c->relay_count; ++i) {
        const char *s = c->relays[i];
        if (!s || strlen(s) > 2048) return false;
        size_t prefix = strncmp(s, "wss://", 6) == 0 ? 6 :
                        strncmp(s, "ws://", 5) == 0 ? 5 : 0;
        if (!prefix || !s[prefix] || s[prefix] == '/' || s[prefix] == '?' ||
            s[prefix] == '#' || strchr(s, '@') || strchr(s, '#')) return false;
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
            if (*p <= 32 || *p == 127) return false;
    }
    return true;
}
void md_nip46_credentials_free(MdNip46Credentials *c) {
    if (!c) return;
    for (size_t i = 0; i < c->relay_count; ++i) free(c->relays[i]);
    free(c->relays);
    md_credential_wipe(c, sizeof(*c)); free(c);
}
/* Wipe all string copies, including malformed/unrecognized input fields. */
static void wipe_json(cJSON *j) {
    for (cJSON *p = j; p; p = p->next) {
        if (p->valuestring) md_credential_wipe(p->valuestring, strlen(p->valuestring));
        if (p->child) wipe_json(p->child);
    }
}
int md_nip46_credentials_parse(const char *json, MdNip46Credentials **out) {
    if (!out) return MD_CREDENTIAL_INVALID;
    *out = NULL;
    if (!json || strlen(json) > 65536 || strstr(json, "\\u0000")) return MD_CREDENTIAL_INVALID;
    cJSON *j = cJSON_ParseWithOpts(json, NULL, true);
    MdNip46Credentials *c = calloc(1, sizeof(*c));
    int rc = MD_CREDENTIAL_INVALID;
    if (!j || !c || !cJSON_IsObject(j) || cJSON_GetArraySize(j) != 5) goto done;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "version");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "user_pubkey");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "remote_pubkey");
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "client_secret");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "relays");
    if (!cJSON_IsNumber(v) || v->valuedouble != 1 || !cJSON_IsString(u) ||
        !cJSON_IsString(r) || !cJSON_IsString(s) || !cJSON_IsArray(a) ||
        !hexkey(u->valuestring) || !hexkey(r->valuestring) || !hexkey(s->valuestring)) goto done;
    memcpy(c->user_pubkey, u->valuestring, 65);
    memcpy(c->remote_pubkey, r->valuestring, 65);
    memcpy(c->client_secret, s->valuestring, 65);
    int count = cJSON_GetArraySize(a);
    if (count < 1 || count > 16) goto done;
    c->relays = calloc((size_t)count, sizeof(char *));
    if (!c->relays) { rc = MD_CREDENTIAL_FAILED; goto done; }
    for (int i = 0; i < count; ++i) {
        const cJSON *url = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsString(url)) goto done;
        c->relays[i] = strdup(url->valuestring);
        if (!c->relays[i]) { rc = MD_CREDENTIAL_FAILED; goto done; }
        c->relay_count++;
    }
    if (!valid(c)) goto done;
    *out = c; c = NULL; rc = 0;
done:
    wipe_json(j); cJSON_Delete(j); md_nip46_credentials_free(c); return rc;
}
int md_nip46_credentials_encode(const MdNip46Credentials *c, char **out) {
    if (!out) return MD_CREDENTIAL_INVALID;
    *out = NULL;
    if (!valid(c)) return MD_CREDENTIAL_INVALID;
    cJSON *j = cJSON_CreateObject(), *a = cJSON_CreateArray();
    if (!j || !a) { cJSON_Delete(j); cJSON_Delete(a); return MD_CREDENTIAL_FAILED; }
    bool ok = cJSON_AddNumberToObject(j, "version", 1) &&
              cJSON_AddStringToObject(j, "user_pubkey", c->user_pubkey) &&
              cJSON_AddStringToObject(j, "remote_pubkey", c->remote_pubkey) &&
              cJSON_AddStringToObject(j, "client_secret", c->client_secret);
    for (size_t i = 0; ok && i < c->relay_count; ++i)
        ok = cJSON_AddItemToArray(a, cJSON_CreateString(c->relays[i]));
    if (!cJSON_AddItemToObject(j, "relays", a)) { cJSON_Delete(a); ok = false; }
    if (ok) *out = cJSON_PrintUnformatted(j);
    wipe_json(j); cJSON_Delete(j);
    return *out ? 0 : MD_CREDENTIAL_FAILED;
}
MdCredentialStore *md_credential_store_custom(const MdCredentialStoreOps *ops, void *context) {
    if (!ops || !ops->get || !ops->put || !ops->remove) return NULL;
    MdCredentialStore *s = malloc(sizeof(*s));
    if (s) { s->ops = *ops; s->context = context; }
    return s;
}
void md_credential_store_destroy(MdCredentialStore *s) {
    if (!s) return;
    if (s->ops.destroy) s->ops.destroy(s->context);
    free(s);
}
int md_credential_store_get(MdCredentialStore *s, const char *pk, MdNip46Credentials **out) {
    if (!out) return MD_CREDENTIAL_INVALID;
    *out = NULL;
    if (!hexkey(pk)) return MD_CREDENTIAL_INVALID;
    if (!s) return MD_CREDENTIAL_UNAVAILABLE;
    char *payload = NULL;
    int rc = s->ops.get(s->context, pk, &payload);
    if (!rc) rc = md_nip46_credentials_parse(payload, out);
    md_credential_free(payload);
    if (!rc && strcmp(pk, (*out)->user_pubkey)) {
        md_nip46_credentials_free(*out); *out = NULL; rc = MD_CREDENTIAL_INVALID;
    }
    return rc;
}
int md_credential_store_put(MdCredentialStore *s, const MdNip46Credentials *c) {
    if (!s) return MD_CREDENTIAL_UNAVAILABLE;
    char *payload = NULL;
    int rc = md_nip46_credentials_encode(c, &payload);
    if (!rc) rc = s->ops.put(s->context, c->user_pubkey, payload);
    md_credential_free(payload); return rc;
}
int md_credential_store_remove(MdCredentialStore *s, const char *pk) {
    if (!hexkey(pk)) return MD_CREDENTIAL_INVALID;
    return s ? s->ops.remove(s->context, pk) : MD_CREDENTIAL_UNAVAILABLE;
}
MdCredentialStore *md_credential_store_default(void) {
#ifdef __APPLE__
    return md_credential_store_keychain(NULL);
#else
    return md_credential_store_secret_service();
#endif
}
typedef struct Entry { char pk[65]; char *payload; struct Entry *next; } Entry;
typedef struct { Entry *head; } Memory;
static int memory_get(void *ctx, const char *pk, char **out) {
    for (Entry *e = ((Memory *)ctx)->head; e; e = e->next)
        if (!strcmp(pk, e->pk)) { *out = strdup(e->payload); return *out ? 0 : MD_CREDENTIAL_FAILED; }
    return MD_CREDENTIAL_NOT_FOUND;
}
static int memory_remove(void *ctx, const char *pk) {
    Entry **p = &((Memory *)ctx)->head;
    while (*p) {
        Entry *e = *p;
        if (!strcmp(pk, e->pk)) {
            *p = e->next; md_credential_free(e->payload); free(e); return 0;
        }
        p = &e->next;
    }
    return 0;
}
static int memory_put(void *ctx, const char *pk, const char *payload) {
    Entry *e = calloc(1, sizeof(*e));
    if (!e) return MD_CREDENTIAL_FAILED;
    e->payload = strdup(payload);
    if (!e->payload) { free(e); return MD_CREDENTIAL_FAILED; }
    memcpy(e->pk, pk, 65); memory_remove(ctx, pk);
    e->next = ((Memory *)ctx)->head; ((Memory *)ctx)->head = e; return 0;
}
static void memory_destroy(void *ctx) {
    Memory *m = ctx;
    while (m->head) memory_remove(m, m->head->pk);
    free(m);
}
MdCredentialStore *md_credential_store_memory(void) {
    static const MdCredentialStoreOps ops = {memory_get, memory_put, memory_remove, memory_destroy};
    Memory *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    MdCredentialStore *s = md_credential_store_custom(&ops, m);
    if (!s) free(m);
    return s;
}
const char *md_credential_error_string(int rc) {
    switch (rc) {
    case 0: return "success";
    case MD_CREDENTIAL_NOT_FOUND: return "no saved NIP-46 credentials for this account";
    case MD_CREDENTIAL_LOCKED: return "credential store is locked; unlock it and retry";
    case MD_CREDENTIAL_UNAVAILABLE: return "OS credential store unavailable (no plaintext fallback)";
    case MD_CREDENTIAL_INVALID: return "invalid NIP-46 credentials";
    case MD_CREDENTIAL_AMBIGUOUS: return "duplicate OS credential items; remove this remote account before re-importing pairing credentials";
    default: return "credential store operation failed";
    }
}
