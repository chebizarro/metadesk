#include "accounts.h"
#include "md_test.h"
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <Security/Security.h>
#endif
#ifdef MD_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

static const char *sk1 = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *sk2 = "0000000000000000000000000000000000000000000000000000000000000002";
static const char *pk1 = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *pk2 = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";
static char *tempdir(void) {
    char *base = realpath("/tmp", NULL), *path = malloc(PATH_MAX);
    MD_CHECK(base && path);
    snprintf(path, PATH_MAX, "%s/md-accounts-XXXXXX", base); free(base);
    MD_CHECK(mkdtemp(path) != NULL); return path;
}
static void cleanup(char *dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/management.json", dir); unlink(path); rmdir(path);
    snprintf(path, sizeof(path), "%s/.management.lock", dir); unlink(path);
    MD_CHECK(rmdir(dir) == 0); free(dir);
}
static void write_metadata(const char *dir, const char *text) {
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/management.json", dir);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    MD_CHECK(fd >= 0);
    MD_CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text)); close(fd);
}
static MdNip46Credentials *credentials(void) {
    const char *json = "{\"version\":1,\"user_pubkey\":\"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798\","
        "\"remote_pubkey\":\"c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5\","
        "\"client_secret\":\"0000000000000000000000000000000000000000000000000000000000000002\","
        "\"relays\":[\"wss://relay.example.test\",\"ws://localhost:7777\"]}";
    MdNip46Credentials *c = NULL;
    MD_CHECK(md_nip46_credentials_parse(json, &c) == 0); return c;
}
static void credential_roundtrip(MdCredentialStore *store) {
    MD_CHECK(store != NULL);
    MdNip46Credentials *c = credentials(), *loaded = NULL;
    MD_CHECK(md_credential_store_remove(store, pk1) == 0);
    MD_CHECK(md_credential_store_get(store, pk1, &loaded) == MD_CREDENTIAL_NOT_FOUND);
    MD_CHECK(loaded == NULL);
    MD_CHECK(md_credential_store_put(store, c) == 0);
    MD_CHECK(md_credential_store_get(store, pk1, &loaded) == 0);
    if (loaded) {
        MD_CHECK(!strcmp(loaded->user_pubkey, pk1));
        MD_CHECK(!strcmp(loaded->remote_pubkey, pk2));
        MD_CHECK(!strcmp(loaded->client_secret, sk2));
        MD_CHECK(loaded->relay_count == 2);
    }
    md_nip46_credentials_free(loaded); loaded = NULL;
    memcpy(c->client_secret, sk1, 65);
    MD_CHECK(md_credential_store_put(store, c) == 0);
    MD_CHECK(md_credential_store_get(store, pk1, &loaded) == 0);
    if (loaded) MD_CHECK(!strcmp(loaded->client_secret, sk1));
    md_nip46_credentials_free(loaded); loaded = NULL;
    MD_CHECK(md_credential_store_remove(store, pk1) == 0);
    MD_CHECK(md_credential_store_remove(store, pk1) == 0);
    MD_CHECK(md_credential_store_get(store, pk1, &loaded) == MD_CREDENTIAL_NOT_FOUND);
    md_nip46_credentials_free(c);
}
#ifdef MD_HAVE_LIBSECRET
static void test_secret_service_collections(MdCredentialStore *store) {
    /* This function is called ONLY on the isolated fixture's private bus. */
    GError *error = NULL;
    SecretService *service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, NULL, &error);
    MD_CHECK(service && !error);
    if (!service) { g_clear_error(&error); return; }
    SecretCollection *original = secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT,
        SECRET_COLLECTION_NONE, NULL, &error);
    SecretCollection *session = secret_collection_for_alias_sync(service, SECRET_COLLECTION_SESSION,
        SECRET_COLLECTION_NONE, NULL, &error);
    MD_CHECK(original && session && !error);
    if (!original || !session) goto done;
    MdNip46Credentials *c = credentials(), *loaded = NULL;
    MD_CHECK(md_credential_store_put(store, c) == 0);
    MD_CHECK(secret_service_set_alias_sync(service, SECRET_COLLECTION_DEFAULT, session, NULL, &error));
    memcpy(c->client_secret, sk1, 65);
    MD_CHECK(md_credential_store_put(store, c) == 0);
    MD_CHECK(md_credential_store_get(store, pk1, &loaded) == 0);
    if (loaded) MD_CHECK(!strcmp(loaded->client_secret, sk1));
    md_nip46_credentials_free(loaded); loaded = NULL;
    /* Deliberate duplicate in another collection must fail closed, rather
     * than allowing arbitrary service ordering to choose the identity secret. */
    static const SecretSchema schema = {.name = "org.metadesk.Nip46Credential",
        .attributes = {{"pubkey", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}}};
    GHashTable *attrs = secret_attributes_build(&schema, "pubkey", pk1, NULL);
    char *payload = NULL; MD_CHECK(md_nip46_credentials_encode(c, &payload) == 0);
    SecretValue *value = secret_value_new(payload, -1, "application/json");
    SecretItem *duplicate = secret_item_create_sync(session, &schema, attrs,
        "Private duplicate fixture", value, SECRET_ITEM_CREATE_NONE, NULL, &error);
    MD_CHECK(duplicate && !error);
    MD_CHECK(md_credential_store_get(store, pk1, &loaded) == MD_CREDENTIAL_AMBIGUOUS && !loaded);
    MD_CHECK(md_credential_store_put(store, c) == MD_CREDENTIAL_AMBIGUOUS);
    MD_CHECK(md_credential_store_remove(store, pk1) == 0);
    MD_CHECK(md_credential_store_get(store, pk1, &loaded) == MD_CREDENTIAL_NOT_FOUND);
    MD_CHECK(secret_service_set_alias_sync(service, SECRET_COLLECTION_DEFAULT, original, NULL, &error));
    if (duplicate) g_object_unref(duplicate);
    secret_value_unref(value); g_hash_table_unref(attrs); md_credential_free(payload);
    md_nip46_credentials_free(c);
done:
    g_clear_error(&error);
    if (original) g_object_unref(original);
    if (session) g_object_unref(session);
    g_object_unref(service);
}
#endif
static void test_credentials(void) {
    MdCredentialStore *store = md_credential_store_memory(); credential_roundtrip(store);
    MdNip46Credentials *c = credentials(), *out = NULL;
    char *json = NULL;
    MD_CHECK(md_nip46_credentials_encode(c, &json) == 0);
    MD_CHECK(md_nip46_credentials_parse(json, &out) == 0);
    md_nip46_credentials_free(out); out = NULL; md_credential_free(json);
    memset(c->client_secret, '0', 64);
    MD_CHECK(md_credential_store_put(store, c) == MD_CREDENTIAL_INVALID);
    memset(c->client_secret, 'f', 64);
    MD_CHECK(md_credential_store_put(store, c) == MD_CREDENTIAL_INVALID);
    memcpy(c->client_secret, sk1, 65);
    memset(c->remote_pubkey, 'a', sizeof(c->remote_pubkey));
    MD_CHECK(md_credential_store_put(store, c) == MD_CREDENTIAL_INVALID);
    memcpy(c->remote_pubkey, pk2, 65);
    free(c->relays[0]); c->relays[0] = strdup("wss://secret@relay.example");
    MD_CHECK(md_credential_store_put(store, c) == MD_CREDENTIAL_INVALID);
    MD_CHECK(md_nip46_credentials_parse("{}", &out) == MD_CREDENTIAL_INVALID);
    MD_CHECK(md_nip46_credentials_parse("{\"version\":2}", &out) == MD_CREDENTIAL_INVALID);
    MD_CHECK(md_credential_store_get(NULL, pk1, &out) == MD_CREDENTIAL_UNAVAILABLE);
    MD_CHECK(md_credential_store_remove(store, "bad") == MD_CREDENTIAL_INVALID);
    md_nip46_credentials_free(c); md_credential_store_destroy(store);
}
static void revoked_count(void *data) { (*(unsigned *)data)++; }
static void test_roles_and_persistence(void) {
    char *dir = tempdir(); MdAccounts *a = NULL, *other = NULL;
    MdCredentialStore *store = md_credential_store_memory();
    MD_CHECK(md_accounts_open(dir, store, &a) == 0);
    MD_CHECK(md_accounts_open(dir, store, &other) == MD_ACCOUNTS_BUSY && !other);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, pk1, "Alice") == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, pk1, "again") == MD_ACCOUNTS_EXISTS);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, "bad", "bad") == MD_ACCOUNTS_INVALID);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, pk2, "Bob") == 0);
    MD_CHECK(md_accounts_label(a, MD_SIGNER_DIRECT_KEY, pk1, "Renamed") == 0);
    MD_CHECK(md_accounts_label(a, MD_SIGNER_DIRECT_KEY, pk1, "bad\nlabel") == MD_ACCOUNTS_INVALID);
    MdSigner *s1 = md_signer_create_direct(sk1), *s2 = md_signer_create_direct(sk2);
    MD_CHECK(s1 && s2);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk1, s1) == 0);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_CLIENT, MD_SIGNER_DIRECT_KEY, pk1, NULL) == 0);
    MdAccountBinding *host = md_accounts_acquire_role(a, MD_ACCOUNT_HOST);
    MdAccountBinding *client = md_accounts_acquire_role(a, MD_ACCOUNT_CLIENT);
    MD_CHECK(md_account_binding_signer(host) == md_account_binding_signer(client));
    MD_CHECK(md_account_binding_cancel(host) != md_account_binding_cancel(client));
    MD_CHECK(md_accounts_check_fips_host(a, pk1) == 0);
    MD_CHECK(md_accounts_check_fips_host(a, pk2) == MD_ACCOUNTS_FIPS_KEY_MISMATCH);
    MD_CHECK(md_accounts_check_fips_host(a, "bad") == MD_ACCOUNTS_INVALID);
    MD_CHECK(md_account_binding_role(host) == MD_ACCOUNT_HOST);
    MD_CHECK(md_account_binding_role(client) == MD_ACCOUNT_CLIENT);
    unsigned revoked = 0, client_revoked = 0;
    MdAccountWatch *watch = md_account_binding_watch(host, revoked_count, &revoked);
    MdAccountWatch *client_watch = md_account_binding_watch(client, revoked_count, &client_revoked);
    MD_CHECK(watch && client_watch);
    uint64_t generation = md_account_binding_generation(host);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk1, NULL) == 0);
    MdAccountBinding *same = md_accounts_acquire_role(a, MD_ACCOUNT_HOST);
    MD_CHECK(md_account_binding_generation(same) == generation); md_account_binding_release(same);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk2, s1) == MD_SIGNER_ERR_KEY_CHANGED);
    MD_CHECK(md_account_binding_is_current(host));
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk2, s2) == 0);
    MD_CHECK(!md_account_binding_is_current(host) && md_account_binding_is_current(client));
    MD_CHECK(revoked == 1 && client_revoked == 0);
    unsigned late = 0;
    MdAccountWatch *late_watch = md_account_binding_watch(host, revoked_count, &late);
    MD_CHECK(late_watch && late == 1);
    md_account_binding_unwatch(late_watch);
    md_account_binding_unwatch(watch);
    char *out = NULL;
    MD_CHECK(md_account_binding_encrypt(host, pk2, "old", &out) == MD_SIGNER_ERR_CANCELLED && !out);
    MD_CHECK(md_account_binding_encrypt(client, pk2, "still alive", &out) == 0 && out);
    free(out);
    MD_CHECK(md_accounts_save(a) == 0);
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/management.json", dir);
    struct stat st; MD_CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    FILE *file = fopen(path, "r"); char text[8192] = {0};
    MD_CHECK(file != NULL); if (file) { fread(text, 1, sizeof(text) - 1, file); fclose(file); }
    MD_CHECK(!strstr(text, sk1) && !strstr(text, sk2) && !strstr(text, "client_secret"));
    md_accounts_destroy(a); a = NULL;
    MD_CHECK(!md_account_binding_is_current(client));
    MD_CHECK(client_revoked == 1);
    md_account_binding_unwatch(client_watch);
    md_account_binding_release(host); md_account_binding_release(client);
    MD_CHECK(md_accounts_open_snapshot(dir, store, &a) == 0);
    MD_CHECK(md_accounts_open(dir, store, &other) == 0);
    md_accounts_destroy(other); other = NULL;
    MD_CHECK(md_accounts_save(a) == MD_ACCOUNTS_UNSUPPORTED);
    MdAccountBinding *restored = NULL;
    MD_CHECK(md_accounts_restore_role(a, MD_ACCOUNT_HOST, &restored) == MD_ACCOUNTS_NOT_READY && !restored);
    md_accounts_destroy(a);
    MD_CHECK(md_accounts_open(dir, store, &a) == 0);
    MD_CHECK(md_accounts_count(a) == 2);
    MD_CHECK(!strcmp(md_accounts_role(a, MD_ACCOUNT_HOST)->pubkey, pk2));
    MD_CHECK(!strcmp(md_accounts_role(a, MD_ACCOUNT_CLIENT)->label, "Renamed"));
    MD_CHECK(md_accounts_acquire_role(a, MD_ACCOUNT_HOST) == NULL);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk2, NULL) == MD_ACCOUNTS_NOT_READY);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk2, s2) == 0);
    MD_CHECK(md_accounts_remove(a, MD_SIGNER_DIRECT_KEY, pk1) == 0);
    MD_CHECK(!md_accounts_role(a, MD_ACCOUNT_CLIENT));
    MD_CHECK(!strcmp(md_accounts_role(a, MD_ACCOUNT_HOST)->pubkey, pk2));
    MD_CHECK(md_accounts_unbind_role(a, MD_ACCOUNT_HOST) == 0);
    MD_CHECK(md_accounts_check_fips_host(a, pk2) == MD_ACCOUNTS_NOT_READY);
    MD_CHECK(md_accounts_save(a) == 0);
    md_accounts_destroy(a); md_signer_destroy(s1); md_signer_destroy(s2);
    md_credential_store_destroy(store); cleanup(dir);
}
typedef struct {
    char pk[65]; bool block, entered, release;
    pthread_mutex_t mutex; pthread_cond_t cond;
} Fake;
static int fake_key(MdSigner *s, char **out) { *out = strdup(((Fake *)s->backend)->pk); return 0; }
static int fake_encrypt(MdSigner *s, const char *peer, const char *text, char **out) {
    (void)peer; Fake *f = s->backend;
    pthread_mutex_lock(&f->mutex);
    if (f->block) {
        f->entered = true; pthread_cond_broadcast(&f->cond);
        while (!f->release) pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex); *out = strdup(text); return 0;
}
static void fake_destroy(MdSigner *s) {
    Fake *f = s->backend; pthread_cond_destroy(&f->cond); pthread_mutex_destroy(&f->mutex); free(f);
}
static MdSigner *fake_signer(MdSignerType type, Fake **state) {
    Fake *f = calloc(1, sizeof(*f)); strcpy(f->pk, pk1);
    pthread_mutex_init(&f->mutex, NULL); pthread_cond_init(&f->cond, NULL);
    static const MdSignerOps ops = {.get_pubkey = fake_key, .nip44_encrypt = fake_encrypt,
        .nip44_decrypt = fake_encrypt, .destroy = fake_destroy};
    *state = f; return md_signer_create_custom(type, &ops, f);
}
typedef struct { MdAccountBinding *binding; int rc; char *out; } Work;
static void *encrypt_worker(void *ctx) {
    Work *w = ctx; w->rc = md_account_binding_encrypt(w->binding, pk2, "result", &w->out); return NULL;
}
static void test_inflight_switch(void) {
    char *dir = tempdir(); MdAccounts *a = NULL;
    MD_CHECK(md_accounts_open(dir, NULL, &a) == 0);
    Fake *f = NULL; MdSigner *s = fake_signer(MD_SIGNER_NIP55L, &f);
    MdSigner *s2 = md_signer_create_direct(sk2);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_NIP55L, pk1, "local") == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, pk2, "other") == 0);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_NIP55L, pk1, s) == 0);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP55L, pk1, NULL) == 0);
    Work w = {.binding = md_accounts_acquire_role(a, MD_ACCOUNT_HOST)};
    MdAccountBinding *client = md_accounts_acquire_role(a, MD_ACCOUNT_CLIENT);
    f->block = true; pthread_t thread; MD_CHECK(!pthread_create(&thread, NULL, encrypt_worker, &w));
    pthread_mutex_lock(&f->mutex);
    while (!f->entered) pthread_cond_wait(&f->cond, &f->mutex);
    pthread_mutex_unlock(&f->mutex);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_DIRECT_KEY, pk2, s2) == 0);
    MD_CHECK(!md_account_binding_is_current(w.binding) && md_account_binding_is_current(client));
    pthread_mutex_lock(&f->mutex); f->release = true; pthread_cond_broadcast(&f->cond); pthread_mutex_unlock(&f->mutex);
    pthread_join(thread, NULL);
    MD_CHECK(w.rc == MD_SIGNER_ERR_CANCELLED && !w.out);
    char *out = NULL;
    MD_CHECK(md_account_binding_encrypt(client, pk2, "shared survives", &out) == 0 && out);
    free(out);
    /* Explicit live rebind catches a backend that changed its key. */
    strcpy(f->pk, pk2);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP55L, pk1, NULL) == MD_SIGNER_ERR_KEY_CHANGED);
    MD_CHECK(!strcmp(md_accounts_role(a, MD_ACCOUNT_CLIENT)->pubkey, pk1));
    MD_CHECK(md_account_binding_encrypt(client, pk2, "refused", &out) == MD_SIGNER_ERR_KEY_CHANGED && !out);
    md_accounts_destroy(a);
    MD_CHECK(!md_account_binding_is_current(client));
    md_account_binding_release(w.binding); md_account_binding_release(client);
    md_signer_destroy(s); md_signer_destroy(s2); cleanup(dir);
}
static int fail_get(void *ctx, const char *pk, char **out) { (void)ctx; (void)pk; (void)out; return MD_CREDENTIAL_LOCKED; }
static int fail_put(void *ctx, const char *pk, const char *payload) { (void)ctx; (void)pk; (void)payload; return MD_CREDENTIAL_LOCKED; }
static int fail_remove(void *ctx, const char *pk) { (void)ctx; (void)pk; return MD_CREDENTIAL_LOCKED; }
static void test_remove_credentials(void) {
    char *dir = tempdir(); MdAccounts *a = NULL;
    MdCredentialStore *store = md_credential_store_memory();
    MdNip46Credentials *c = credentials(), *out = NULL;
    MD_CHECK(md_credential_store_put(store, c) == 0);
    MD_CHECK(md_accounts_open(dir, store, &a) == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_NIP46, pk1, "remote") == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_DIRECT_KEY, pk1, "direct") == 0);
    MD_CHECK(md_accounts_remove(a, MD_SIGNER_DIRECT_KEY, pk1) == 0);
    MD_CHECK(md_credential_store_get(store, pk1, &out) == 0); md_nip46_credentials_free(out); out = NULL;
    Fake *f = NULL; MdSigner *s = fake_signer(MD_SIGNER_NIP46, &f);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_CLIENT, MD_SIGNER_NIP46, pk1, s) == 0);
    MD_CHECK(md_accounts_bind_role(a, MD_ACCOUNT_HOST, MD_SIGNER_NIP46, pk1, NULL) == 0);
    MdAccountBinding *b = md_accounts_acquire_role(a, MD_ACCOUNT_HOST);
    MD_CHECK(md_accounts_remove(a, MD_SIGNER_NIP46, pk1) == 0);
    MD_CHECK(!md_account_binding_is_current(b) && !md_accounts_role(a, MD_ACCOUNT_CLIENT));
    MD_CHECK(md_credential_store_get(store, pk1, &out) == MD_CREDENTIAL_NOT_FOUND);
    MD_CHECK(md_signer_is_ready(s)); /* External signer key was not deleted or shut down. */
    md_account_binding_release(b); md_signer_destroy(s);
    md_accounts_destroy(a); md_credential_store_destroy(store);
    static const MdCredentialStoreOps ops = {fail_get, fail_put, fail_remove, NULL};
    store = md_credential_store_custom(&ops, NULL);
    MD_CHECK(md_accounts_open(dir, store, &a) == 0);
    MD_CHECK(md_accounts_add(a, MD_SIGNER_NIP46, pk1, "locked") == 0);
    MD_CHECK(md_accounts_remove(a, MD_SIGNER_NIP46, pk1) == MD_CREDENTIAL_LOCKED);
    MD_CHECK(md_accounts_count(a) == 1);
    md_accounts_destroy(a); md_credential_store_destroy(store); md_nip46_credentials_free(c); cleanup(dir);
}
static void test_storage_rejection(void) {
    char *dir = tempdir(), path[PATH_MAX], linkpath[PATH_MAX]; MdAccounts *a = NULL;
    MD_CHECK(md_accounts_open("relative", NULL, &a) == MD_ACCOUNTS_IO && !a);
    MD_CHECK(!chmod(dir, 0755)); MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_IO && !a);
    MD_CHECK(!chmod(dir, 0700));
    write_metadata(dir, "{\"version\":2,\"accounts\":[],\"roles\":{\"client\":null,\"host\":null}}");
    MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_INVALID && !a);
    write_metadata(dir, "{\"version\":1,\"accounts\":[],\"roles\":{\"client\":null,\"host\":null},\"client_secret\":\"no\"}");
    MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_INVALID && !a);
    write_metadata(dir, "{\"version\":1,\"accounts\":[],\"roles\":{\"client\":null,\"host\":null}}");
    snprintf(path, sizeof(path), "%s/management.json", dir);
    MD_CHECK(!chmod(path, 0644)); MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_IO && !a);
    MD_CHECK(!chmod(path, 0600));
    snprintf(linkpath, sizeof(linkpath), "%s/hardlink", dir);
    MD_CHECK(!link(path, linkpath)); MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_IO && !a);
    unlink(linkpath); unlink(path);
    MD_CHECK(!symlink(".management.lock", path));
    MD_CHECK(md_accounts_open(dir, NULL, &a) == MD_ACCOUNTS_IO && !a); unlink(path);
    MD_CHECK(md_accounts_open(dir, NULL, &a) == 0);
    MD_CHECK(!mkdir(path, 0700));
    MD_CHECK(md_accounts_save(a) == MD_ACCOUNTS_IO);
    DIR *d = opendir(dir); struct dirent *entry;
    while ((entry = readdir(d))) MD_CHECK(!strstr(entry->d_name, ".tmp"));
    closedir(d); md_accounts_destroy(a); cleanup(dir);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--secret-service")) {
        if (!getenv("MD_PRIVATE_KEYRING_FIXTURE")) { fprintf(stderr, "Private fixture required\n"); return 1; }
        MdCredentialStore *s = md_credential_store_secret_service(); credential_roundtrip(s);
#ifdef MD_HAVE_LIBSECRET
        test_secret_service_collections(s);
#endif
        md_credential_store_destroy(s); return md_test_report();
    }
#ifdef __APPLE__
    if (argc == 2 && !strcmp(argv[1], "--keychain")) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        char *dir = tempdir(), path[PATH_MAX]; snprintf(path, sizeof(path), "%s/test.keychain", dir);
        CFArrayRef original = NULL; SecKeychainRef kc = NULL;
        MD_CHECK(SecKeychainCopySearchList(&original) == errSecSuccess);
        OSStatus status = SecKeychainCreate(path, 4, "test", false, NULL, &kc);
        /* SecKeychainCreate can add the new keychain to the search list.
         * Immediately restore the exact original list, including on failure. */
        if (original) MD_CHECK(SecKeychainSetSearchList(original) == errSecSuccess);
        MD_CHECK(status == errSecSuccess);
        if (!status) {
            MdCredentialStore *s = md_credential_store_keychain(path);
            credential_roundtrip(s); md_credential_store_destroy(s);
            MD_CHECK(SecKeychainDelete(kc) == errSecSuccess); CFRelease(kc);
        }
        if (original) CFRelease(original);
        cleanup(dir); return md_test_report();
#pragma clang diagnostic pop
    }
#endif
    RUN_TEST(test_credentials); RUN_TEST(test_roles_and_persistence);
    RUN_TEST(test_inflight_switch); RUN_TEST(test_remove_credentials); RUN_TEST(test_storage_rejection);
    return md_test_report();
}
