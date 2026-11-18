#include "accounts.h"
#include <stdatomic.h>
#include <pthread.h>
#ifndef _WIN32
#include <cjson/cJSON.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ACCOUNTS 128
#define METADATA_FILE "management.json"
struct MdAccountWatch {
    MdAccountBinding *binding;
    void (*revoked)(void *);
    void *data;
    struct MdAccountWatch *next;
};
struct MdAccountBinding {
    atomic_uint refs;
    atomic_bool revoked;
    uint64_t generation;
    MdAccountRole role;
    pthread_mutex_t watch_mu;
    MdAccountWatch *watches;
    MdSigner *signer;
    MdSignerCancel *cancel;
};
typedef struct { MdAccount info; MdSigner *signer; } Record;
struct MdAccounts {
    int dirfd, lockfd;
    size_t count;
    Record records[MAX_ACCOUNTS];
    int roles[MD_ACCOUNT_ROLE_COUNT];
    MdAccountBinding *bindings[MD_ACCOUNT_ROLE_COUNT];
    uint64_t generation;
    MdCredentialStore *store;
    bool snapshot;
};
static bool key_valid(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static bool label_valid(const char *s) {
    if (!s || strlen(s) > 128) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
static bool role_valid(MdAccountRole r) { return r >= 0 && r < MD_ACCOUNT_ROLE_COUNT; }
const char *md_accounts_backend_name(MdSignerType t) {
    switch (t) {
    case MD_SIGNER_DIRECT_KEY: return "direct";
    case MD_SIGNER_NIP46: return "nip46";
    case MD_SIGNER_NIP55L: return "nip55l";
    case MD_SIGNER_NIP5F: return "nip5f";
    default: return NULL;
    }
}
int md_accounts_parse_backend(const char *s, MdSignerType *out) {
    if (!s || !out) return MD_ACCOUNTS_INVALID;
    for (int t = MD_SIGNER_DIRECT_KEY; t <= MD_SIGNER_NIP5F; ++t)
        if (!strcmp(s, md_accounts_backend_name((MdSignerType)t))) { *out = (MdSignerType)t; return 0; }
    return MD_ACCOUNTS_INVALID;
}
static int find(const MdAccounts *a, MdSignerType t, const char *pk) {
    if (!a || !md_accounts_backend_name(t) || !key_valid(pk)) return MD_ACCOUNTS_INVALID;
    for (size_t i = 0; i < a->count; ++i)
        if (a->records[i].info.backend == t && !strcmp(pk, a->records[i].info.pubkey)) return (int)i;
    return MD_ACCOUNTS_NOT_FOUND;
}
size_t md_accounts_count(const MdAccounts *a) { return a ? a->count : 0; }
const MdAccount *md_accounts_at(const MdAccounts *a, size_t i) {
    return a && i < a->count ? &a->records[i].info : NULL;
}
int md_accounts_add(MdAccounts *a, MdSignerType t, const char *pk, const char *label) {
    int i = find(a, t, pk);
    if (i >= 0) return MD_ACCOUNTS_EXISTS;
    if (i != MD_ACCOUNTS_NOT_FOUND || !label_valid(label) || a->count == MAX_ACCOUNTS)
        return MD_ACCOUNTS_INVALID;
    Record *r = &a->records[a->count++];
    r->info.backend = t; memcpy(r->info.pubkey, pk, 65); strcpy(r->info.label, label);
    return 0;
}
int md_accounts_label(MdAccounts *a, MdSignerType t, const char *pk, const char *label) {
    int i = find(a, t, pk);
    if (i < 0) return i;
    if (!label_valid(label)) return MD_ACCOUNTS_INVALID;
    strcpy(a->records[i].info.label, label); return 0;
}
MdAccountBinding *md_account_binding_ref(MdAccountBinding *b) {
    if (b) atomic_fetch_add(&b->refs, 1);
    return b;
}
void md_account_binding_release(MdAccountBinding *b) {
    if (!b || atomic_fetch_sub(&b->refs, 1) != 1) return;
    pthread_mutex_destroy(&b->watch_mu);
    md_signer_cancel_free(b->cancel); md_signer_destroy(b->signer); free(b);
}
static void revoke_role(MdAccounts *a, MdAccountRole role) {
    MdAccountBinding *b = a->bindings[role];
    a->bindings[role] = NULL;
    if (b) {
        pthread_mutex_lock(&b->watch_mu);
        atomic_store(&b->revoked, true); md_signer_cancel(b->cancel);
        for (MdAccountWatch *w = b->watches; w; w = w->next) w->revoked(w->data);
        pthread_mutex_unlock(&b->watch_mu);
        md_account_binding_release(b);
    }
}
static void release_idle_signers(MdAccounts *a) {
    for (size_t i = 0; i < a->count; ++i) {
        bool active = false;
        for (int r = 0; r < MD_ACCOUNT_ROLE_COUNT; ++r)
            if (a->roles[r] == (int)i && a->bindings[r]) active = true;
        if (!active) {
            md_signer_destroy(a->records[i].signer);
            a->records[i].signer = NULL;
        }
    }
}
int md_accounts_remove(MdAccounts *a, MdSignerType t, const char *pk) {
    int i = find(a, t, pk);
    if (i < 0) return i;
    if (t == MD_SIGNER_NIP46) {
        int rc = md_credential_store_remove(a->store, pk);
        if (rc) return rc;
    }
    for (int role = 0; role < MD_ACCOUNT_ROLE_COUNT; ++role) {
        if (a->roles[role] == i) { revoke_role(a, role); a->roles[role] = -1; }
        else if (a->roles[role] > i) a->roles[role]--;
    }
    md_signer_destroy(a->records[i].signer);
    memmove(&a->records[i], &a->records[i + 1], (a->count - (size_t)i - 1) * sizeof(Record));
    memset(&a->records[--a->count], 0, sizeof(Record)); return 0;
}
const MdAccount *md_accounts_role(const MdAccounts *a, MdAccountRole role) {
    return a && role_valid(role) && a->roles[role] >= 0 ?
        &a->records[a->roles[role]].info : NULL;
}
static int check_signer(MdSigner *s, MdSignerType t, const char *pk) {
    if (!s) return MD_ACCOUNTS_NOT_READY;
    if (md_signer_get_type(s) != t) return MD_ACCOUNTS_INVALID;
    int rc = md_signer_check_connection(s);
    char *live = NULL;
    if (!rc) rc = md_signer_get_pubkey(s, &live);
    if (!rc && strcmp(pk, live)) rc = MD_SIGNER_ERR_KEY_CHANGED;
    free(live); return rc;
}
int md_accounts_bind_role(MdAccounts *a, MdAccountRole role, MdSignerType t, const char *pk, MdSigner *s) {
    if (!a || !role_valid(role)) return MD_ACCOUNTS_INVALID;
    int i = find(a, t, pk);
    if (i < 0) return i;
    Record *r = &a->records[i];
    /* A shared account keeps its existing backend, never silently replaces a
     * live signer underneath the other role. Validate any supplied candidate. */
    int rc = check_signer(s ? s : r->signer, t, pk);
    if (rc) return rc;
    if (r->signer && r->signer != s && (rc = check_signer(r->signer, t, pk))) return rc;
    if (a->roles[role] == i && a->bindings[role]) return 0;
    MdAccountBinding *b = calloc(1, sizeof(*b));
    if (!b) return MD_ACCOUNTS_IO;
    b->cancel = md_signer_cancel_new();
    if (!b->cancel) { free(b); return MD_ACCOUNTS_IO; }
    if (!r->signer) r->signer = md_signer_ref(s);
    b->signer = md_signer_ref(r->signer);
    atomic_init(&b->refs, 1); atomic_init(&b->revoked, false);
    pthread_mutex_init(&b->watch_mu, NULL);
    b->role = role;
    b->generation = ++a->generation;
    revoke_role(a, role); a->roles[role] = i; a->bindings[role] = b;
    release_idle_signers(a); return 0;
}
int md_accounts_unbind_role(MdAccounts *a, MdAccountRole role) {
    if (!a || !role_valid(role)) return MD_ACCOUNTS_INVALID;
    revoke_role(a, role); a->roles[role] = -1; release_idle_signers(a); return 0;
}
MdAccountBinding *md_accounts_acquire_role(MdAccounts *a, MdAccountRole role) {
    return a && role_valid(role) ? md_account_binding_ref(a->bindings[role]) : NULL;
}
bool md_account_binding_is_current(const MdAccountBinding *b) { return b && !atomic_load(&b->revoked); }
uint64_t md_account_binding_generation(const MdAccountBinding *b) { return b ? b->generation : 0; }
MdAccountRole md_account_binding_role(const MdAccountBinding *b) { return b ? b->role : MD_ACCOUNT_ROLE_COUNT; }
MdAccountWatch *md_account_binding_watch(MdAccountBinding *b, void (*revoked)(void *), void *data) {
    if (!b || !revoked) return NULL;
    MdAccountWatch *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->binding = md_account_binding_ref(b); w->revoked = revoked; w->data = data;
    pthread_mutex_lock(&b->watch_mu);
    w->next = b->watches; b->watches = w;
    if (atomic_load(&b->revoked)) revoked(data);
    pthread_mutex_unlock(&b->watch_mu);
    return w;
}
void md_account_binding_unwatch(MdAccountWatch *w) {
    if (!w) return;
    MdAccountBinding *b = w->binding;
    pthread_mutex_lock(&b->watch_mu);
    MdAccountWatch **p = &b->watches;
    while (*p && *p != w) p = &(*p)->next;
    if (*p) *p = w->next;
    pthread_mutex_unlock(&b->watch_mu);
    free(w); md_account_binding_release(b);
}
MdSigner *md_account_binding_signer(const MdAccountBinding *b) { return b ? b->signer : NULL; }
const MdSignerCancel *md_account_binding_cancel(const MdAccountBinding *b) { return b ? b->cancel : NULL; }
static int finish(MdAccountBinding *b, int rc, char **out) {
    if (!md_account_binding_is_current(b)) { md_credential_free(*out); *out = NULL; return MD_SIGNER_ERR_CANCELLED; }
    return rc;
}
int md_account_binding_sign_event(MdAccountBinding *b, const char *json, char **out) {
    if (!out) return MD_ACCOUNTS_INVALID;
    *out = NULL;
    if (!b) return MD_ACCOUNTS_NOT_READY;
    return finish(b, md_signer_sign_event_cancellable(b->signer, json, out, b->cancel), out);
}
int md_account_binding_encrypt(MdAccountBinding *b, const char *peer, const char *text, char **out) {
    if (!out) return MD_ACCOUNTS_INVALID;
    *out = NULL;
    if (!b) return MD_ACCOUNTS_NOT_READY;
    return finish(b, md_signer_nip44_encrypt_cancellable(b->signer, peer, text, out, b->cancel), out);
}
int md_account_binding_decrypt(MdAccountBinding *b, const char *peer, const char *text, char **out) {
    if (!out) return MD_ACCOUNTS_INVALID;
    *out = NULL;
    if (!b) return MD_ACCOUNTS_NOT_READY;
    return finish(b, md_signer_nip44_decrypt_cancellable(b->signer, peer, text, out, b->cancel), out);
}
int md_accounts_check_fips_host(const MdAccounts *a, const char *node) {
    if (!key_valid(node)) return MD_ACCOUNTS_INVALID;
    const MdAccount *host = md_accounts_role(a, MD_ACCOUNT_HOST);
    if (!host) return MD_ACCOUNTS_NOT_READY;
    return strcmp(node, host->pubkey) ? MD_ACCOUNTS_FIPS_KEY_MISMATCH : 0;
}

#ifndef _WIN32
/* Walk directories with openat/O_NOFOLLOW, including parent components. The
 * final private directory and all files must be owned by the current user. */
static int private_directory(const char *path) {
    if (!path || path[0] != '/') return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *copy = strdup(path), *save = NULL;
    if (fd < 0 || !copy) { if (fd >= 0) close(fd); free(copy); return -1; }
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); fd = -1; break; }
        if (mkdirat(fd, part, 0700) && errno != EEXIST) { close(fd); fd = -1; break; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd); fd = next;
        if (fd < 0) break;
    }
    free(copy);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_uid != getuid() || (st.st_mode & 07777) != 0700)) {
        close(fd); fd = -1;
    }
    return fd;
}
static bool private_file(int fd) {
    struct stat st;
    return !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
           st.st_nlink == 1 && (st.st_mode & 07777) == 0600;
}
static char *default_directory(void) {
    const char *home = getenv("HOME");
#ifdef __APPLE__
    const char *base = home, *suffix = "/Library/Application Support/metadesk";
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *base = xdg && *xdg ? xdg : home;
    const char *suffix = xdg && *xdg ? "/metadesk" : "/.config/metadesk";
#endif
    if (!base || base[0] != '/') return NULL;
    size_t n = strlen(base) + strlen(suffix) + 1;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%s%s", base, suffix);
    return path;
}
static int parse_pair(const cJSON *j, MdSignerType *t, const char **pk) {
    if (!cJSON_IsObject(j)) return MD_ACCOUNTS_INVALID;
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(j, "backend");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "pubkey");
    if (!cJSON_IsString(b) || !cJSON_IsString(p) || !key_valid(p->valuestring) ||
        md_accounts_parse_backend(b->valuestring, t)) return MD_ACCOUNTS_INVALID;
    *pk = p->valuestring; return 0;
}
static int load(MdAccounts *a) {
    int fd = openat(a->dirfd, METADATA_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : MD_ACCOUNTS_IO;
    struct stat st;
    if (!private_file(fd) || fstat(fd, &st) || st.st_size <= 0 || st.st_size > 65536) {
        close(fd); return MD_ACCOUNTS_IO;
    }
    size_t size = (size_t)st.st_size, used = 0;
    char *text = calloc(size + 1, 1);
    if (!text) { close(fd); return MD_ACCOUNTS_IO; }
    while (used < size) {
        ssize_t n = read(fd, text + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    cJSON *j = used == size && !memchr(text, 0, size) && !strstr(text, "\\u0000") ?
        cJSON_ParseWithOpts(text, NULL, true) : NULL;
    free(text);
    int rc = MD_ACCOUNTS_INVALID;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "version");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(j, "accounts");
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(j, "roles");
    if (!cJSON_IsObject(j) || cJSON_GetArraySize(j) != 3 || !cJSON_IsNumber(v) ||
        v->valuedouble != 1 || !cJSON_IsArray(list) || !cJSON_IsObject(roles) ||
        cJSON_GetArraySize(roles) != 2) goto done;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        MdSignerType t; const char *pk;
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (parse_pair(item, &t, &pk) || cJSON_GetArraySize(item) != 3 || !cJSON_IsString(label) ||
            md_accounts_add(a, t, pk, label->valuestring)) goto done;
    }
    for (int role = 0; role < MD_ACCOUNT_ROLE_COUNT; ++role) {
        item = cJSON_GetObjectItemCaseSensitive(roles, role == MD_ACCOUNT_CLIENT ? "client" : "host");
        if (cJSON_IsNull(item)) continue;
        MdSignerType t; const char *pk;
        if (parse_pair(item, &t, &pk) || cJSON_GetArraySize(item) != 2) goto done;
        int i = find(a, t, pk);
        if (i < 0) goto done;
        a->roles[role] = i;
    }
    rc = 0;
done:
    cJSON_Delete(j); return rc;
}
int md_accounts_open(const char *dir, MdCredentialStore *store, MdAccounts **out) {
    if (!out) return MD_ACCOUNTS_INVALID;
    *out = NULL;
    MdAccounts *a = calloc(1, sizeof(*a));
    if (!a) return MD_ACCOUNTS_IO;
    a->dirfd = a->lockfd = -1; a->roles[0] = a->roles[1] = -1; a->store = store;
    char *default_dir = dir ? NULL : default_directory();
    a->dirfd = private_directory(dir ? dir : default_dir); free(default_dir);
    int rc = MD_ACCOUNTS_IO;
    if (a->dirfd < 0) goto failed;
    a->lockfd = openat(a->dirfd, ".management.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (a->lockfd < 0 || !private_file(a->lockfd)) goto failed;
    if (flock(a->lockfd, LOCK_EX | LOCK_NB)) { rc = MD_ACCOUNTS_BUSY; goto failed; }
    rc = load(a);
    if (rc) goto failed;
    *out = a; return 0;
failed:
    md_accounts_destroy(a); return rc;
}
#else
int md_accounts_open(const char *dir, MdCredentialStore *store, MdAccounts **out) {
    (void)dir; (void)store;
    if (!out) return MD_ACCOUNTS_INVALID;
    *out = NULL; return MD_ACCOUNTS_UNSUPPORTED;
}
#endif
int md_accounts_open_snapshot(const char *dir, MdCredentialStore *store, MdAccounts **out) {
    int rc = md_accounts_open(dir, store, out);
    if (rc) return rc;
    (*out)->snapshot = true;
#ifndef _WIN32
    close((*out)->lockfd); (*out)->lockfd = -1;
#endif
    return 0;
}
int md_accounts_restore_role(MdAccounts *a, MdAccountRole role, MdAccountBinding **out) {
    if (!a || !out || !role_valid(role)) return MD_ACCOUNTS_INVALID;
    *out = NULL;
    const MdAccount *info = md_accounts_role(a, role);
    if (!info) return MD_ACCOUNTS_NOT_FOUND;
    MdSigner *s = NULL;
    int rc = md_accounts_bind_role(a, role, info->backend, info->pubkey, NULL);
    if (rc == MD_ACCOUNTS_NOT_READY) {
        rc = md_accounts_connect_signer(info->backend, info->pubkey, a->store, &s);
        if (!rc) rc = md_accounts_bind_role(a, role, info->backend, info->pubkey, s);
        md_signer_destroy(s);
    }
    if (!rc) *out = md_accounts_acquire_role(a, role);
    return rc;
}
void md_accounts_destroy(MdAccounts *a) {
    if (!a) return;
    for (int r = 0; r < MD_ACCOUNT_ROLE_COUNT; ++r) revoke_role(a, r);
    for (size_t i = 0; i < a->count; ++i) md_signer_destroy(a->records[i].signer);
#ifndef _WIN32
    if (a->lockfd >= 0) close(a->lockfd);
    if (a->dirfd >= 0) close(a->dirfd);
#endif
    free(a);
}
#ifndef _WIN32
static cJSON *pair(const MdAccount *info) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    if (!cJSON_AddStringToObject(j, "backend", md_accounts_backend_name(info->backend)) ||
        !cJSON_AddStringToObject(j, "pubkey", info->pubkey)) { cJSON_Delete(j); return NULL; }
    return j;
}
int md_accounts_save(MdAccounts *a) {
    if (!a) return MD_ACCOUNTS_INVALID;
    if (a->snapshot) return MD_ACCOUNTS_UNSUPPORTED;
    cJSON *j = cJSON_CreateObject(), *list = cJSON_CreateArray(), *roles = cJSON_CreateObject();
    if (!j || !list || !roles) { cJSON_Delete(j); cJSON_Delete(list); cJSON_Delete(roles); return MD_ACCOUNTS_IO; }
    bool ok = cJSON_AddNumberToObject(j, "version", 1) != NULL;
    if (!cJSON_AddItemToObject(j, "accounts", list)) { cJSON_Delete(list); list = NULL; ok = false; }
    if (!cJSON_AddItemToObject(j, "roles", roles)) { cJSON_Delete(roles); roles = NULL; ok = false; }
    for (size_t i = 0; ok && i < a->count; ++i) {
        cJSON *item = pair(&a->records[i].info);
        if (!item || !cJSON_AddStringToObject(item, "label", a->records[i].info.label) ||
            !cJSON_AddItemToArray(list, item)) { cJSON_Delete(item); ok = false; }
    }
    for (int r = 0; ok && r < MD_ACCOUNT_ROLE_COUNT; ++r) {
        const MdAccount *info = md_accounts_role(a, r);
        cJSON *item = info ? pair(info) : cJSON_CreateNull();
        if (!item || !cJSON_AddItemToObject(roles, r == MD_ACCOUNT_CLIENT ? "client" : "host", item)) {
            cJSON_Delete(item); ok = false;
        }
    }
    char *text = ok ? cJSON_PrintUnformatted(j) : NULL; cJSON_Delete(j);
    if (!text) return MD_ACCOUNTS_IO;
    char tmp[80]; int fd = -1;
    for (unsigned i = 0; i < 100; ++i) {
        snprintf(tmp, sizeof(tmp), ".management.%ld.%u.tmp", (long)getpid(), i);
        fd = openat(a->dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    int rc = MD_ACCOUNTS_IO;
    if (fd < 0) { free(text); return rc; }
    size_t n = strlen(text), used = 0;
    while (used < n) {
        ssize_t wrote = write(fd, text + used, n - used);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) break;
        used += (size_t)wrote;
    }
    free(text);
    bool synced = used == n && fsync(fd) == 0;
    if (close(fd)) synced = false;
    if (synced && !renameat(a->dirfd, tmp, a->dirfd, METADATA_FILE) && !fsync(a->dirfd)) rc = 0;
    if (rc) unlinkat(a->dirfd, tmp, 0);
    return rc;
}
#else
int md_accounts_save(MdAccounts *a) { (void)a; return MD_ACCOUNTS_UNSUPPORTED; }
#endif
const char *md_accounts_error_string(int rc) {
    if (rc <= MD_CREDENTIAL_NOT_FOUND) return md_credential_error_string(rc);
    switch (rc) {
    case 0: return "success";
    case MD_ACCOUNTS_INVALID: return "invalid account metadata or argument (lowercase 64-character hex pubkey required)";
    case MD_ACCOUNTS_NOT_FOUND: return "account not found";
    case MD_ACCOUNTS_EXISTS: return "account already exists";
    case MD_ACCOUNTS_IO: return "cannot read/write private account metadata (requires owned 0700 directory and 0600 files)";
    case MD_ACCOUNTS_BUSY: return "account metadata is in use by another process";
    case MD_ACCOUNTS_UNSUPPORTED: return "account persistence is currently supported on Linux and macOS only";
    case MD_ACCOUNTS_NOT_READY: return "role has no verified signer; supply or unlock its backend";
    case MD_ACCOUNTS_FIPS_KEY_MISMATCH: return "FIPS hosting refused: host-role pubkey differs from the FIPS node key";
    case MD_SIGNER_ERR_KEY_CHANGED: return "signer identity differs from the pinned account; rebinding refused";
    case MD_SIGNER_ERR_CANCELLED: return "role generation revoked";
    default: return "signer connection or identity check failed";
    }
}
