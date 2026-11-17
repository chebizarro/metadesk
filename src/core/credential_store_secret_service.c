#include "credential_store.h"
#include <stdlib.h>
#include <string.h>
#ifdef MD_HAVE_LIBSECRET
#include <libsecret/secret.h>
static const SecretSchema schema = {
    .name = "org.metadesk.Nip46Credential", .flags = SECRET_SCHEMA_NONE,
    .attributes = {{"pubkey", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}},
};
static int map_error(GError *e) {
    int rc = MD_CREDENTIAL_FAILED;
    if (g_error_matches(e, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) rc = MD_CREDENTIAL_LOCKED;
    else if (e && e->domain == G_DBUS_ERROR) rc = MD_CREDENTIAL_UNAVAILABLE;
    g_clear_error(&e); return rc;
}
static int search(const char *pk, SecretService **service, GList **items) {
    GError *e = NULL;
    *items = NULL;
    *service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, NULL, &e);
    if (!*service) return e ? map_error(e) : MD_CREDENTIAL_UNAVAILABLE;
    GHashTable *attrs = secret_attributes_build(&schema, "pubkey", pk, NULL);
    /* No UNLOCK flag: a headless CLI must never open a desktop prompt. */
    *items = secret_service_search_sync(*service, &schema, attrs,
        SECRET_SEARCH_ALL | SECRET_SEARCH_LOAD_SECRETS, NULL, &e);
    g_hash_table_unref(attrs);
    if (e) { g_object_unref(*service); *service = NULL; return map_error(e); }
    return 0;
}
static int ss_get(void *ctx, const char *pk, char **out) {
    (void)ctx;
    SecretService *service = NULL; GList *items = NULL;
    int rc = search(pk, &service, &items);
    if (rc) return rc;
    rc = MD_CREDENTIAL_NOT_FOUND;
    if (items && items->next) rc = MD_CREDENTIAL_AMBIGUOUS;
    else if (items) {
        SecretItem *item = items->data;
        if (secret_item_get_locked(item)) rc = MD_CREDENTIAL_LOCKED;
        else {
            SecretValue *v = secret_item_get_secret(item);
            gsize size = 0;
            const char *text = v ? secret_value_get(v, &size) : NULL;
            if (text && size <= 65536 && !memchr(text, 0, size)) {
                *out = malloc(size + 1);
                if (*out) { memcpy(*out, text, size); (*out)[size] = 0; rc = 0; }
                else rc = MD_CREDENTIAL_FAILED;
            } else rc = MD_CREDENTIAL_INVALID;
            if (v) secret_value_unref(v);
        }
    }
    g_list_free_full(items, g_object_unref); g_object_unref(service); return rc;
}
static int ss_put(void *ctx, const char *pk, const char *payload) {
    (void)ctx;
    GError *e = NULL;
    SecretService *service = NULL; GList *items = NULL;
    int rc = search(pk, &service, &items);
    if (rc) return rc;
    /* Read and replace in the same all-collections scope. A changed default
     * keyring must not strand a stale item that future lookups could select. */
    if (items) {
        rc = MD_CREDENTIAL_AMBIGUOUS;
        if (!items->next) {
            SecretItem *item = items->data;
            rc = MD_CREDENTIAL_LOCKED;
            if (!secret_item_get_locked(item)) {
                SecretValue *value = secret_value_new(payload, -1, "application/json");
                rc = secret_item_set_secret_sync(item, value, NULL, &e) ? 0 :
                    e ? map_error(e) : MD_CREDENTIAL_FAILED;
                secret_value_unref(value);
            }
        }
        g_list_free_full(items, g_object_unref); g_object_unref(service); return rc;
    }
    SecretCollection *collection = secret_collection_for_alias_sync(service,
        SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, NULL, &e);
    if (!collection) { g_object_unref(service); return e ? map_error(e) : MD_CREDENTIAL_UNAVAILABLE; }
    rc = MD_CREDENTIAL_LOCKED;
    if (!secret_collection_get_locked(collection)) {
        GHashTable *attrs = secret_attributes_build(&schema, "pubkey", pk, NULL);
        SecretValue *value = secret_value_new(payload, -1, "application/json");
        SecretItem *item = secret_item_create_sync(collection, &schema, attrs,
            "Metadesk remote signer", value, SECRET_ITEM_CREATE_REPLACE, NULL, &e);
        rc = item ? 0 : e ? map_error(e) : MD_CREDENTIAL_FAILED;
        if (item) g_object_unref(item);
        secret_value_unref(value); g_hash_table_unref(attrs);
    }
    g_object_unref(collection); g_object_unref(service); return rc;
}
static int ss_remove(void *ctx, const char *pk) {
    (void)ctx;
    SecretService *service = NULL; GList *items = NULL;
    int rc = search(pk, &service, &items);
    if (rc) return rc;
    for (GList *p = items; p; p = p->next)
        if (secret_item_get_locked(p->data)) { rc = MD_CREDENTIAL_LOCKED; break; }
    for (GList *p = items; !rc && p; p = p->next) {
        GError *e = NULL;
        if (!secret_item_delete_sync(p->data, NULL, &e)) rc = e ? map_error(e) : MD_CREDENTIAL_FAILED;
    }
    g_list_free_full(items, g_object_unref); g_object_unref(service); return rc;
}
MdCredentialStore *md_credential_store_secret_service(void) {
    static const MdCredentialStoreOps ops = {ss_get, ss_put, ss_remove, NULL};
    return md_credential_store_custom(&ops, NULL);
}
#else
MdCredentialStore *md_credential_store_secret_service(void) { return NULL; }
#endif
