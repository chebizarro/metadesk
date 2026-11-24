#include "managed_store.h"
#include "platform.h"
#include "quality.h"
#include <nostr-kinds.h>
#include <nostr/nip51/nip51.h>
#include <nostr/nip78/nip78.h>
#include <jansson.h>
#include <openssl/sha.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win_private.h"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define EVENT_LIMIT (MD_STORE_MAX_CONTENT + 1024)
#define FILE_LIMIT (2 * EVENT_LIMIT + 4096)
#define RECENT_LIMIT 100
#define PEOPLE NOSTR_KIND_CATEGORIZED_PEOPLE_LIST
#define APPDATA NOSTR_KIND_APPLICATION_SPECIFIC_DATA

struct MdManagedStore {
    MdAccountBinding *binding;
    char owner[65];
    json_t *ready;
    bool latch_disabled;
#ifdef _WIN32
    MdWinDirectory *dir;
    HANDLE lock;
#else
    int dir, lock;
#endif
};

static bool hex(const char *s, size_t n) {
    return s && strlen(s) == n && strspn(s, "0123456789abcdef") == n;
}
static bool uuid(const char *s) {
    if (!s || strlen(s) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!strchr("0123456789abcdef", s[i])) return false;
    }
    return true;
}
static int doc_kind(const char *d) {
    if (!d) return 0;
    if (!strcmp(d, MD_STORE_MANAGEMENT)) return APPDATA;
    if (!strcmp(d, MD_STORE_HOSTS) || !strcmp(d, MD_STORE_FAVORITES) ||
        (!strncmp(d, MD_STORE_TAG_PREFIX, sizeof(MD_STORE_TAG_PREFIX)-1) &&
         uuid(d + sizeof(MD_STORE_TAG_PREFIX)-1))) return PEOPLE;
    return 0;
}
static void wipe(char *s) {
    if (s) { md_secure_zero(s, strlen(s)); free(s); }
}
static bool ciphertext(const char *s) {
    if (!s || s[0] != 'A' || s[1] < 'g' || s[1] > 'v') return false;
    size_t n = strlen(s);
    if (n < 132 || n > MD_STORE_MAX_CONTENT || n % 4) return false;
    size_t prefix = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
    return n - prefix <= 2 && strspn(s + prefix, "=") == n - prefix;
}

static NostrEvent *encrypted_event(const char *owner, int kind, const char *d,
                                   const char *cipher, int64_t at) {
    if (!ciphertext(cipher)) return NULL;
    if (kind == APPDATA) return nostr_nip78_build_event(owner, d, cipher, at);
    if (kind != PEOPLE) return NULL;
    NostrListDocument *list = nostr_nip51_document_new(kind, d);
    if (!list) return NULL;
    NostrEvent *event = nostr_nip51_document_build_unsigned(list, owner, at, cipher);
    nostr_nip51_document_free(list);
    return event;
}

NostrEvent *md_private_document_build(MdSigner *signer, const MdSignerCancel *cancel,
    const char *owner, int kind, const char *d, const char *plain, int64_t at) {
    if (!plain || strlen(plain) > MD_STORE_MAX_PLAINTEXT || (kind != PEOPLE && kind != APPDATA)) return NULL;
    char *cipher = NULL;
    NostrEvent *event = NULL;
    if (!md_signer_nip44_encrypt_cancellable(signer, owner, plain, &cipher, cancel))
        event = encrypted_event(owner, kind, d, cipher, at);
    free(cipher);
    return event;
}

static bool current(MdManagedStore *s) { return s && md_account_binding_is_current(s->binding); }

#ifndef _WIN32
static bool owned(int fd, bool directory) {
    struct stat st;
    return fd >= 0 && !fstat(fd, &st) && st.st_uid == geteuid() &&
        (st.st_mode & 07777) == (directory ? 0700 : 0600) &&
        (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode) && st.st_nlink == 1);
}
#endif

MdManagedStore *md_managed_store_open(MdAccountBinding *binding, const char *path) {
    if (!path || !binding || md_account_binding_role(binding) != MD_ACCOUNT_CLIENT ||
        !md_account_binding_is_current(binding)) return NULL;
    char *owner = NULL;
    if (md_signer_get_pubkey(md_account_binding_signer(binding), &owner) || !hex(owner, 64)) {
        free(owner); return NULL;
    }
    MdManagedStore *s = calloc(1, sizeof(*s));
    if (!s) { free(owner); return NULL; }
    memcpy(s->owner, owner, 65);
    free(owner);
    s->binding = md_account_binding_ref(binding);
    s->ready = json_object();
#ifdef _WIN32
    s->lock = INVALID_HANDLE_VALUE;
    MdWinDirectory *parent = md_win_directory(path, false);
    if (parent) {
        size_t n = strlen(path) + 66;
        char *child = malloc(n);
        if (child) {
            snprintf(child, n, "%s\\%s", path, s->owner);
            s->dir = md_win_directory(child, true);
            free(child);
        }
        md_win_directory_close(parent);
    }
    if (s->dir) s->lock = md_win_file(s->dir, "store.lock", GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, 0);
    bool locked = s->lock != INVALID_HANDLE_VALUE;
#else
    s->dir = s->lock = -1;
    int parent = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (owned(parent, true) && (!mkdirat(parent, s->owner, 0700) || errno == EEXIST)) {
        s->dir = openat(parent, s->owner, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (owned(s->dir, true) && !fsync(parent))
            s->lock = openat(s->dir, "store.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    }
    if (parent >= 0) close(parent);
    bool locked = owned(s->lock, false) && !flock(s->lock, LOCK_EX | LOCK_NB);
#endif
    if (!locked || !s->ready || !current(s)) { md_managed_store_close(s); return NULL; }
    return s;
}
void md_managed_store_close(MdManagedStore *s) {
    if (!s) return;
#ifdef _WIN32
    if (s->lock != INVALID_HANDLE_VALUE) CloseHandle(s->lock);
    md_win_directory_close(s->dir);
#else
    if (s->lock >= 0) close(s->lock);
    if (s->dir >= 0) close(s->dir);
#endif
    json_decref(s->ready);
    md_account_binding_release(s->binding);
    free(s);
}

/* One atomic record per coordinate: never write a receipt and its winning
 * event separately. After a crash, either the previous state or the whole
 * replacement survives. Only authenticated encrypted events enter it. */
/* Nostr coordinates contain ':' which is not a valid Windows filename. */
static void file_name(const char *name, char out[64]) {
    size_t i = 0;
    for (; name[i]; ++i) out[i] = name[i] == ':' ? '_' : name[i];
    out[i] = 0;
}
static bool write_json(MdManagedStore *s, const char *coordinate, json_t *j) {
    char name[64];
    file_name(coordinate, name);
    if (!current(s)) return false;
    char *str = json_dumps(j, JSON_COMPACT | JSON_ENCODE_ANY);
    if (!str) return false;
    size_t n = strlen(str);
    bool ok = false;
#ifdef _WIN32
    ok = n <= FILE_LIMIT && md_win_replace(s->dir, name, str, n);
#else
    /* Exclusive owner lock also protects the fixed scratch path. */
    const char *tmp = "store.tmp";
    if (n <= FILE_LIMIT && (!unlinkat(s->dir, tmp, 0) || errno == ENOENT)) {
        int fd = openat(s->dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            size_t used = 0;
            while (used < n) {
                ssize_t k = write(fd, str + used, n - used);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) break;
                used += (size_t)k;
            }
            bool synced = used == n && !fsync(fd);
            if (close(fd)) synced = false;
            ok = synced && current(s) && !renameat(s->dir, tmp, s->dir, name) && !fsync(s->dir);
            if (!ok) unlinkat(s->dir, tmp, 0);
        }
    }
#endif
    free(str);
    return ok;
}
static json_t *read_json(MdManagedStore *s, const char *coordinate, bool *missing) {
    char name[64];
    file_name(coordinate, name);
    *missing = false;
    unsigned char *buf = NULL;
    size_t used = 0;
#ifdef _WIN32
    HANDLE f = md_win_file(s->dir, name, GENERIC_READ, OPEN_EXISTING, FILE_SHARE_READ);
    if (f == INVALID_HANDLE_VALUE) { *missing = GetLastError() == ERROR_FILE_NOT_FOUND; return NULL; }
    bool ok = md_win_read(f, &buf, &used, FILE_LIMIT);
    CloseHandle(f);
    if (!ok) { free(buf); return NULL; }
#else
    int f = openat(s->dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (f < 0) { *missing = errno == ENOENT; return NULL; }
    struct stat st;
    if (owned(f, false) && !fstat(f, &st) && st.st_size > 0 && st.st_size <= FILE_LIMIT) {
        size_t n = (size_t)st.st_size;
        buf = malloc(n);
        while (buf && used < n) {
            ssize_t k = read(f, buf + used, n - used);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            used += (size_t)k;
        }
        if (used != n) { free(buf); buf = NULL; }
    }
    close(f);
#endif
    json_t *j = buf ? json_loadb((const char *)buf, used, JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, NULL) : NULL;
    free(buf);
    return j;
}

static const char *field(json_t *j, const char *name) { return json_string_value(json_object_get(j, name)); }
static bool text(json_t *v, size_t max) { return json_is_string(v) && json_string_length(v) <= max; }
static bool tree(json_t *v) {
    const char *s = json_string_value(v);
    return s && (!strcmp(s, "compact") || !strcmp(s, "json"));
}
static bool relay(const char *s) {
    if (!s || strlen(s) > 2048) return false;
    if (!strncmp(s, "wss://", 6)) s += 6;
    else if (!strncmp(s, "ws://", 5)) s += 5;
    else return false;
    /* No credentials, query parameters, fragments, or escaped userinfo. */
    if (!*s || *s == '/' || strpbrk(s, "@?#%\\")) return false;
    for (; *s; ++s) if ((unsigned char)*s <= 32 || (unsigned char)*s >= 127) return false;
    return true;
}
static bool nip05(json_t *v) {
    if (json_is_null(v)) return true;
    const char *s = json_string_value(v);
    if (!s || strlen(s) > 320) return false;
    size_t local = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789_.-");
    if (!local || s[local] != '@') return false;
    s += local + 1;
    if (!*s || strlen(s) > 253) return false;
    while (*s) {
        size_t n = strspn(s, "abcdefghijklmnopqrstuvwxyz0123456789-");
        if (!n || n > 63 || *s == '-' || s[n-1] == '-') return false;
        s += n;
        if (*s && (*s++ != '.' || !*s)) return false;
    }
    return true;
}
static bool capability(const char *s) {
    return s && (!strcmp(s,"video") || !strcmp(s,"agent") || !strcmp(s,"input") ||
                 !strcmp(s,"monitor") || !strcmp(s,"clipboard"));
}
static bool strings(json_t *v, size_t max, bool (*valid)(const char *)) {
    if (!json_is_array(v) || json_array_size(v) > max) return false;
    size_t i;
    json_t *x;
    json_array_foreach(v, i, x) {
        const char *s = json_string_value(x);
        if (!valid(s)) return false;
        for (size_t k = 0; k < i; ++k) if (json_equal(x, json_array_get(v, k))) return false;
    }
    return true;
}
static bool metadata(json_t *item) {
    if (!json_is_object(item)) return false;
    const char *key;
    json_t *v;
    json_object_foreach(item, key, v) {
        if (!strcmp(key,"label") && !text(v,128)) return false;
        if (!strcmp(key,"notes") && !text(v,2048)) return false;
        if (!strcmp(key,"nip05") && !nip05(v)) return false;
        if (!strcmp(key,"relay_hints") && !strings(v,16,relay)) return false;
        if (!strcmp(key,"requested_caps") && !strings(v,5,capability)) return false;
        if (!strcmp(key,"tree_format") && !tree(v)) return false;
        if (!strcmp(key,"port") && (!json_is_integer(v) || json_integer_value(v) < 1 || json_integer_value(v) > 65535)) return false;
        if (!strcmp(key,"quality")) {
            char *wire = json_dumps(v, JSON_COMPACT | JSON_ENCODE_ANY);
            cJSON *host = cJSON_CreateObject(), *q = wire ? cJSON_Parse(wire) : NULL;
            bool attached = host && q && cJSON_AddItemToObject(host,"quality",q);
            MdQuality quality;
            bool has = false;
            bool valid = attached && !md_quality_read(host,&quality,&has) && has;
            if (!attached) cJSON_Delete(q);
            cJSON_Delete(host);
            free(wire);
            if (!valid) return false;
        }
    }
    return true;
}
static MdStoreResult schema(int kind, json_t *doc) {
    size_t i;
    json_t *item;
    if (kind == PEOPLE) {
        if (!json_is_array(doc) || json_array_size(doc) > NOSTR_LIST_MAX_TAGS) return MD_STORE_INVALID;
        json_t *seen = json_object();
        if (!seen) return MD_STORE_IO;
        bool valid = true;
        json_array_foreach(doc, i, item) {
            size_t k;
            json_t *v;
            const char *tag = json_string_value(json_array_get(item, 0));
            if (!json_is_array(item) || !tag || !*tag || json_array_size(item) > NOSTR_LIST_MAX_FIELDS) { valid = false; break; }
            json_array_foreach(item, k, v) if (!text(v, NOSTR_LIST_MAX_FIELD_BYTES)) { valid = false; break; }
            if (!valid) break;
            if (!strcmp(tag,"p")) {
                const char *pk = json_string_value(json_array_get(item, 1));
                if (json_array_size(item) != 2 || !hex(pk,64) || json_object_get(seen,pk) ||
                    json_object_size(seen) == MD_STORE_MAX_HOSTS || json_object_set(seen,pk,json_true())) { valid = false; break; }
            }
        }
        json_decref(seen);
        return valid ? MD_STORE_OK : MD_STORE_INVALID;
    }
    json_t *version = json_object_get(doc,"schema_version");
    if (!json_is_integer(version) || json_integer_value(version) < 1) return MD_STORE_INVALID;
    if (json_integer_value(version) != 1) return MD_STORE_READ_ONLY;
    json_t *hosts = json_object_get(doc,"hosts"), *tags = json_object_get(doc,"tags");
    json_t *pref = json_object_get(doc,"preferences");
    if ((hosts && !json_is_object(hosts)) || json_object_size(hosts) > MD_STORE_MAX_HOSTS ||
        (tags && !json_is_object(tags)) || (pref && !json_is_object(pref))) return MD_STORE_INVALID;
    const char *key;
    json_object_foreach(hosts,key,item) if (!hex(key,64) || !metadata(item)) return MD_STORE_INVALID;
    json_object_foreach(tags,key,item) {
        json_t *deleted = json_object_get(item,"deleted");
        if (!uuid(key) || !json_is_object(item) || !text(json_object_get(item,"label"),64) ||
            (deleted && !json_is_boolean(deleted))) return MD_STORE_INVALID;
    }
    item = json_object_get(pref,"default_requested_caps");
    if (item && !strings(item,5,capability)) return MD_STORE_INVALID;
    item = json_object_get(pref,"default_tree_format");
    return !item || tree(item) ? MD_STORE_OK : MD_STORE_INVALID;
}

static NostrEvent *parse_event(const char *wire, const char *owner, int kind, const char *d) {
    if (!wire || strlen(wire) > EVENT_LIMIT) return NULL;
    json_t *strict = json_loads(wire, JSON_REJECT_DUPLICATES, NULL);
    if (!strict) return NULL;
    json_decref(strict);
    NostrEvent *e = nostr_event_new();
    if (!e) return NULL;
    if (nostr_event_deserialize_signed(e, wire, NULL) == NOSTR_EVENT_VALIDATION_OK) {
        const char *pk = nostr_event_get_pubkey(e);
        NostrTags *tags = nostr_event_get_tags(e);
        NostrTag *tag = tags && nostr_tags_size(tags) == 1 ? nostr_tags_get(tags,0) : NULL;
        char id[65];
        if (pk && !strcmp(pk,owner) && nostr_event_get_kind(e) == kind &&
            nostr_event_get_created_at(e) >= 0 && tag && nostr_tag_size(tag) == 2 &&
            nostr_tag_get(tag,0) && !strcmp(nostr_tag_get(tag,0),"d") &&
            nostr_tag_get(tag,1) && !strcmp(nostr_tag_get(tag,1),d) &&
            ciphertext(nostr_event_get_content(e)) &&
            nostr_event_validate(e,id) == NOSTR_EVENT_VALIDATION_OK) return e;
    }
    nostr_event_free(e);
    return NULL;
}

static MdStoreResult decode(MdManagedStore *s, int kind, NostrEvent *e, json_t **doc) {
    *doc = NULL;
    char *plain = NULL;
    if (md_account_binding_decrypt(s->binding,s->owner,nostr_event_get_content(e),&plain) || !plain) {
        wipe(plain); return current(s) ? MD_STORE_SIGNER : MD_STORE_REVOKED;
    }
    if (strlen(plain) <= MD_STORE_MAX_CONTENT) *doc = json_loads(plain,JSON_REJECT_DUPLICATES,NULL);
    wipe(plain);
    return schema(kind,*doc);
}

/* Journal loading also verifies disk envelopes. Never recover an unreadable
 * journal as an empty baseline: that could overwrite newer data on a relay. */
typedef struct {
    json_t *j, *relays, *superseded;
    NostrEvent *winner, *draft;
    const char *base;
    size_t acked;
    bool exposed;
} Journal;
static void dispose(Journal *j) {
    nostr_event_free(j->winner); nostr_event_free(j->draft); json_decref(j->j);
}
/* Only called with envelopes admitted by parse_event(), which validates
 * the declared ID against the canonical hash and verifies the signature. */
static const char *id(const NostrEvent *e) { return e ? e->id : ""; }
static bool load(MdManagedStore *s, const char *d, Journal *j) {
    bool missing;
    j->j = read_json(s,d,&missing);
    if (missing) j->j = json_object();
    if (!json_is_object(j->j)) return false;
    const char *wire = field(j->j,"event"), *pending = field(j->j,"draft");
    if (json_object_get(j->j,"event") && !(j->winner = parse_event(wire,s->owner,doc_kind(d),d))) return false;
    j->relays = json_object_get(j->j,"relays");
    j->base = field(j->j,"base");
    j->superseded = json_object_get(j->j,"superseded");
    json_t *echo = json_object_get(j->j,"echo");
    if (!json_object_get(j->j,"draft"))
        return !json_object_get(j->j,"base") && !j->relays && !j->superseded && !echo;
    if (echo && !json_is_true(echo)) return false;
    j->exposed = json_is_true(echo);
    if (j->superseded && (!json_is_array(j->superseded) ||
                          json_array_size(j->superseded) > MD_STORE_MAX_RELAYS)) return false;
    size_t index;
    json_t *item;
    json_array_foreach(j->superseded,index,item) if (!hex(json_string_value(item),64)) return false;
    j->draft = parse_event(pending,s->owner,doc_kind(d),d);
    if (!j->draft || !j->base || (*j->base && !hex(j->base,64)) ||
        !json_is_object(j->relays) || !json_object_size(j->relays) || json_object_size(j->relays) > MD_STORE_MAX_RELAYS) return false;
    const char *key;
    json_t *v;
    json_object_foreach(j->relays,key,v) {
        if (!hex(key,64) || !json_is_integer(v) || json_integer_value(v) < MD_STORE_RELAY_WAITING ||
            json_integer_value(v) > MD_STORE_RELAY_ACKNOWLEDGED) return false;
        if (json_integer_value(v) == MD_STORE_RELAY_ACKNOWLEDGED) ++j->acked;
        if (json_integer_value(v) != MD_STORE_RELAY_WAITING) j->exposed = true;
    }
    return j->acked < json_object_size(j->relays);
}
static bool superseded(const Journal *j, const char *event_id) {
    size_t i;
    json_t *item;
    json_array_foreach(j->superseded,i,item)
        if (!strcmp(json_string_value(item),event_id)) return true;
    return false;
}
static MdStoreResult status(const Journal *j) {
    if (!j->draft) return MD_STORE_OK;
    const char *expected = j->acked ? id(j->draft) : j->base;
    if (strcmp(expected,id(j->winner))) return MD_STORE_CONFLICT;
    return j->acked ? MD_STORE_PARTIAL : MD_STORE_PENDING;
}
static MdStoreResult access_doc(MdManagedStore *s, const char *d) {
    return !current(s) ? MD_STORE_REVOKED : !doc_kind(d) ? MD_STORE_INVALID : MD_STORE_OK;
}

char *md_managed_store_filter(MdManagedStore *s, const char *d) {
    if (access_doc(s,d) != MD_STORE_OK) return NULL;
    json_t *j = json_pack("{s:[i],s:[s],s:[s],s:i}","kinds",doc_kind(d),"authors",s->owner,"#d",d,"limit",1);
    char *wire = j ? json_dumps(j,JSON_COMPACT) : NULL;
    json_decref(j);
    return wire;
}
MdStoreResult md_managed_store_backfill(MdManagedStore *s, const char *d, bool done) {
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    if (!done) { json_object_del(s->ready,d); return MD_STORE_OK; }
    return json_object_set(s->ready,d,json_true()) ? MD_STORE_IO : MD_STORE_OK;
}
MdStoreResult md_managed_store_receive(MdManagedStore *s, const char *d, const char *wire, int64_t now) {
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    int kind = doc_kind(d);
    NostrEvent *e = parse_event(wire,s->owner,kind,d);
    if (!e) return MD_STORE_INVALID;
    Journal j = {0};
    int64_t at = nostr_event_get_created_at(e);
    if (now < 0 || (at > now && at - now > 300)) r = MD_STORE_CLOCK;
    else if (!load(s,d,&j)) r = MD_STORE_IO;
    /* Echoes never count as OK, but they prove a draft escaped. */
    else if (!strcmp(id(e),id(j.draft))) {
        r = MD_STORE_UNCHANGED;
        if (!json_is_true(json_object_get(j.j,"echo")) &&
            (json_object_set(j.j,"echo",json_true()) || !write_json(s,d,j.j))) r = MD_STORE_IO;
    } else if (superseded(&j,id(e)) || (j.winner &&
        (at < nostr_event_get_created_at(j.winner) ||
         (at == nostr_event_get_created_at(j.winner) && strcmp(id(e),id(j.winner)) >= 0)))) r = MD_STORE_UNCHANGED;
    else {
        /* Retain the authenticated envelope even if its payload is unreadable.
         * A locked signer must not cause editing against an older revision. */
        char *canonical = nostr_event_serialize_compact(e);
        if (!canonical || json_object_set_new(j.j,"event",json_string(canonical)) || !write_json(s,d,j.j)) r = MD_STORE_IO;
        else r = j.draft ? MD_STORE_CONFLICT : MD_STORE_OK;
        free(canonical);
    }
    dispose(&j);
    nostr_event_free(e);
    return current(s) ? r : MD_STORE_REVOKED;
}

MdStoreResult md_managed_store_read(MdManagedStore *s, const char *d,
    char revision[65], char **plain, char **draft) {
    if (plain) *plain = NULL;
    if (draft) *draft = NULL;
    if (revision) revision[0] = 0;
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    Journal j = {0};
    json_t *winner = NULL, *pending = NULL;
    if (!load(s,d,&j)) { r = MD_STORE_IO; goto done; }
    r = j.winner ? decode(s,doc_kind(d),j.winner,&winner) : MD_STORE_OK;
    if (r != MD_STORE_OK && r != MD_STORE_READ_ONLY) goto done;
    if (r == MD_STORE_OK || j.draft) r = status(&j);
    if (j.draft && draft) {
        MdStoreResult draft_status = decode(s,doc_kind(d),j.draft,&pending);
        if (draft_status != MD_STORE_OK) { r = draft_status; goto done; }
    }
    if (!current(s)) { r = MD_STORE_REVOKED; goto done; }
    if (plain && winner) *plain = json_dumps(winner,JSON_COMPACT);
    if (draft && pending) *draft = json_dumps(pending,JSON_COMPACT);
    if ((plain && winner && !*plain) || (draft && pending && !*draft)) {
        if (plain) { wipe(*plain); *plain = NULL; }
        if (draft) { wipe(*draft); *draft = NULL; }
        r = MD_STORE_IO;
    } else if (revision) strcpy(revision,id(j.winner));
done:
    json_decref(winner); json_decref(pending); dispose(&j);
    return r;
}

static bool listed(const char *const *keys, const char *key) {
    for (; *keys; ++keys) if (!strcmp(*keys,key)) return true;
    return false;
}
static bool retain_fields(json_t *new, json_t *old, const char *const *known) {
    const char *key;
    json_t *v;
    json_object_foreach(old,key,v) {
        if (!listed(known,key) && !json_object_get(new,key) && json_object_set(new,key,v)) return false;
    }
    return true;
}
static bool retain(int kind, json_t *new, json_t *old) {
    if (kind == PEOPLE) {
        size_t i, k;
        json_t *v, *w;
        json_array_foreach(old,i,v) {
            if (!strcmp(json_string_value(json_array_get(v,0)),"p")) continue;
            bool present = false;
            json_array_foreach(new,k,w) if (json_equal(v,w)) { present = true; break; }
            if (!present && json_array_append(new,v)) return false;
        }
        return true;
    }
    const char *const root_keys[] = {"schema_version","hosts","tags","preferences",NULL};
    const char *const host_keys[] = {"label","notes","nip05","relay_hints","requested_caps","tree_format","port",NULL};
    const char *const tag_keys[] = {"label","deleted",NULL};
    const char *const pref_keys[] = {"default_requested_caps","default_tree_format",NULL};
    if (!retain_fields(new,old,root_keys)) return false;
    const char *key;
    json_t *v;
    json_object_foreach(json_object_get(new,"hosts"),key,v)
        if (!retain_fields(v,json_object_get(json_object_get(old,"hosts"),key),host_keys)) return false;
    json_object_foreach(json_object_get(new,"tags"),key,v)
        if (!retain_fields(v,json_object_get(json_object_get(old,"tags"),key),tag_keys)) return false;
    json_t *prefs = json_object_get(old,"preferences");
    if (prefs && !json_object_get(new,"preferences") && json_object_set_new(new,"preferences",json_object())) return false;
    return retain_fields(json_object_get(new,"preferences"),prefs,pref_keys);
}
static bool relay_key(const char *target, char key[65]) {
    if (!relay(target)) return false;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (!SHA256((const unsigned char *)target,strlen(target),digest)) return false;
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(key + 2*i,3,"%02x",(unsigned)digest[i]);
    return true;
}

static bool next_timestamp(const Journal *j, int64_t now, int64_t *at) {
    int64_t previous = j->winner ? nostr_event_get_created_at(j->winner) : -1;
    if (j->draft && nostr_event_get_created_at(j->draft) > previous)
        previous = nostr_event_get_created_at(j->draft);
    if (now < 0 || previous == INT64_MAX || (previous >= now && previous - now >= 300)) return false;
    *at = previous >= now ? previous + 1 : now;
    return true;
}

/* Keep earlier superseded IDs until a newer draft gets fully acknowledged.
 * An echo before that ACK cannot undo the explicit conflict decision. */
static MdStoreResult save_draft(MdManagedStore *s, const char *d, Journal *j,
                                NostrEvent *event, json_t *relays) {
    MdStoreResult r = MD_STORE_SIGNER;
    char *unsigned_wire = event ? nostr_event_serialize_compact(event) : NULL;
    char *signed_wire = NULL, *canonical = NULL;
    NostrEvent *signed_event = NULL;
    if (!unsigned_wire || md_account_binding_sign_event(s->binding,unsigned_wire,&signed_wire)) goto done;
    signed_event = parse_event(signed_wire,s->owner,doc_kind(d),d);
    if (!signed_event || nostr_event_get_created_at(signed_event) != nostr_event_get_created_at(event) ||
        strcmp(nostr_event_get_content(signed_event),nostr_event_get_content(event))) goto done;
    r = MD_STORE_IO;
    if (j->draft && j->exposed && !superseded(j,id(j->draft))) {
        if (!j->superseded) j->superseded = json_object_set_new(j->j,"superseded",json_array()) ?
            NULL : json_object_get(j->j,"superseded");
        if (!j->superseded) goto done;
        if (json_array_size(j->superseded) == MD_STORE_MAX_RELAYS) { r = MD_STORE_LIMIT; goto done; }
        if (json_array_append_new(j->superseded,json_string(id(j->draft)))) goto done;
    }
    json_object_del(j->j,"echo");
    canonical = nostr_event_serialize_compact(signed_event);
    if (canonical && !json_object_set_new(j->j,"draft",json_string(canonical)) &&
        !json_object_set_new(j->j,"base",json_string(id(j->winner))) && !json_object_set(j->j,"relays",relays) &&
        write_json(s,d,j->j)) r = MD_STORE_PENDING;
done:
    free(unsigned_wire); free(signed_wire); free(canonical);
    nostr_event_free(signed_event);
    return current(s) ? r : MD_STORE_REVOKED;
}

static MdStoreResult stage(MdManagedStore *s, const char *d, const char *base,
    const char *plain, const char *const *targets, size_t count, int64_t now, bool reapply) {
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    if (!base || (*base && !hex(base,64)) || !plain ||
        !targets || !count || count > MD_STORE_MAX_RELAYS) return MD_STORE_INVALID;
    if (strlen(plain) > MD_STORE_MAX_CONTENT) return MD_STORE_LIMIT;
    if (!json_is_true(json_object_get(s->ready,d))) return MD_STORE_NOT_READY;
    Journal j = {0};
    json_t *candidate = NULL, *old = NULL, *relays = NULL;
    NostrEvent *unsigned_event = NULL;
    char *encoded = NULL;
    if (!load(s,d,&j)) { r = MD_STORE_IO; goto done; }
    if (strcmp(base,id(j.winner))) { r = MD_STORE_CONFLICT; goto done; }
    if (reapply && status(&j) != MD_STORE_CONFLICT) { r = MD_STORE_UNCHANGED; goto done; }
    if (!reapply && j.draft) { r = status(&j); goto done; }
    int64_t at;
    if (!next_timestamp(&j,now,&at)) { r = MD_STORE_CLOCK; goto done; }
    int kind = doc_kind(d);
    candidate = json_loads(plain,JSON_REJECT_DUPLICATES,NULL);
    r = schema(kind,candidate);
    if (r != MD_STORE_OK) goto done;
    if (j.winner) {
        r = decode(s,kind,j.winner,&old);
        if (r != MD_STORE_OK) goto done;
        if (!retain(kind,candidate,old)) { r = MD_STORE_IO; goto done; }
        r = schema(kind,candidate);
        if (r != MD_STORE_OK) goto done;
        if (json_equal(candidate,old)) { r = MD_STORE_UNCHANGED; goto done; }
    }
    relays = json_object();
    if (!relays) { r = MD_STORE_IO; goto done; }
    for (size_t i = 0; i < count; ++i) {
        char key[65];
        if (!relay_key(targets[i],key) || json_object_get(relays,key)) { r = MD_STORE_INVALID; goto done; }
        if (json_object_set_new(relays,key,json_integer(MD_STORE_RELAY_WAITING))) { r = MD_STORE_IO; goto done; }
    }
    encoded = json_dumps(candidate,JSON_COMPACT);
    if (!encoded) { r = MD_STORE_IO; goto done; }
    if (strlen(encoded) > MD_STORE_MAX_PLAINTEXT) { r = MD_STORE_LIMIT; goto done; }
    unsigned_event = md_private_document_build(md_account_binding_signer(s->binding),
        md_account_binding_cancel(s->binding),s->owner,kind,d,encoded,at);
    r = save_draft(s,d,&j,unsigned_event,relays);
done:
    json_decref(relays); json_decref(candidate); json_decref(old); dispose(&j);
    wipe(encoded);
    nostr_event_free(unsigned_event);
    return current(s) ? r : MD_STORE_REVOKED;
}

MdStoreResult md_managed_store_stage(MdManagedStore *s, const char *d, const char *base,
    const char *plain, const char *const *targets, size_t count, int64_t now) {
    return stage(s,d,base,plain,targets,count,now,false);
}
MdStoreResult md_managed_store_reapply(MdManagedStore *s, const char *d, const char *base,
    const char *plain, const char *const *targets, size_t count, int64_t now) {
    return stage(s,d,base,plain,targets,count,now,true);
}

MdStoreResult md_managed_store_outgoing(MdManagedStore *s, const char *d, const char *target, char **wire) {
    if (!wire) return MD_STORE_INVALID;
    *wire = NULL;
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    char key[65];
    if (!relay_key(target,key)) return MD_STORE_INVALID;
    if (!json_is_true(json_object_get(s->ready,d))) return MD_STORE_NOT_READY;
    Journal j = {0};
    if (!load(s,d,&j)) r = MD_STORE_IO;
    else if ((r = status(&j)) == MD_STORE_PENDING || r == MD_STORE_PARTIAL) {
        json_t *ack = json_object_get(j.relays,key);
        if (!ack) r = MD_STORE_INVALID;
        else if (json_integer_value(ack) == MD_STORE_RELAY_ACKNOWLEDGED) r = MD_STORE_UNCHANGED;
#ifndef _WIN32
        else if (fsync(s->dir)) r = MD_STORE_IO;
#endif
        else if (json_integer_value(ack) == MD_STORE_RELAY_WAITING &&
                 (json_object_set_new(j.relays,key,json_integer(MD_STORE_RELAY_UNKNOWN)) ||
                  !write_json(s,d,j.j))) r = MD_STORE_IO;
        else if (!(*wire = strdup(field(j.j,"draft")))) r = MD_STORE_IO;
    }
    dispose(&j);
    if (current(s)) return r;
    free(*wire); *wire = NULL;
    return MD_STORE_REVOKED;
}
static void drop_draft(json_t *j) {
    json_object_del(j,"draft"); json_object_del(j,"base"); json_object_del(j,"relays");
    json_object_del(j,"echo"); json_object_del(j,"superseded");
}
MdStoreResult md_managed_store_record_result(MdManagedStore *s, const char *d,
    const char *target, const char *event_id, MdStoreRelayResult outcome) {
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    char key[65];
    if (!relay_key(target,key) || !hex(event_id,64) || outcome <= MD_STORE_RELAY_WAITING ||
        outcome > MD_STORE_RELAY_ACKNOWLEDGED) return MD_STORE_INVALID;
    Journal j = {0};
    if (!load(s,d,&j)) r = MD_STORE_IO;
    else if (!j.draft || strcmp(event_id,id(j.draft))) r = MD_STORE_UNCHANGED;
    else if ((r = status(&j)) != MD_STORE_CONFLICT) {
        json_t *ack = json_object_get(j.relays,key);
        if (!ack) r = MD_STORE_INVALID;
        else if (json_integer_value(ack) != MD_STORE_RELAY_ACKNOWLEDGED &&
                 json_integer_value(ack) != outcome) {
            if (json_object_set_new(j.relays,key,json_integer(outcome))) r = MD_STORE_IO;
            else if (outcome == MD_STORE_RELAY_ACKNOWLEDGED) {
                if (json_object_set(j.j,"event",json_object_get(j.j,"draft"))) r = MD_STORE_IO;
                else {
                    r = ++j.acked == json_object_size(j.relays) ? MD_STORE_OK : MD_STORE_PARTIAL;
                    if (r == MD_STORE_OK) drop_draft(j.j);
                }
            }
            if (r != MD_STORE_IO && !write_json(s,d,j.j)) r = MD_STORE_IO;
        }
    }
    dispose(&j);
    return current(s) ? r : MD_STORE_REVOKED;
}
MdStoreResult md_managed_store_relay_result(MdManagedStore *s, const char *d,
    const char *target, MdStoreRelayResult *out) {
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    char key[65];
    if (!out || !relay_key(target,key)) return MD_STORE_INVALID;
    Journal j = {0};
    if (!load(s,d,&j)) r = MD_STORE_IO;
    else {
        json_t *result = json_object_get(j.relays,key);
        if (result) *out = (MdStoreRelayResult)json_integer_value(result);
        else r = MD_STORE_UNCHANGED;
    }
    dispose(&j);
    return current(s) ? r : MD_STORE_REVOKED;
}
MdStoreResult md_managed_store_keep_remote(MdManagedStore *s, const char *d,
                                            const char *base, int64_t now) {
    MdStoreResult r = access_doc(s,d);
    if (r != MD_STORE_OK) return r;
    if (!hex(base,64)) return MD_STORE_INVALID;
    Journal j = {0};
    json_t *targets = NULL;
    NostrEvent *event = NULL;
    if (!load(s,d,&j)) { r = MD_STORE_IO; goto done; }
    if (strcmp(base,id(j.winner))) { r = MD_STORE_CONFLICT; goto done; }
    if (status(&j) != MD_STORE_CONFLICT) { r = MD_STORE_UNCHANGED; goto done; }
    if (!j.exposed && !json_array_size(j.superseded)) {
        drop_draft(j.j);
        r = write_json(s,d,j.j) ? MD_STORE_OK : MD_STORE_IO;
        goto done;
    }
    if (!json_is_true(json_object_get(s->ready,d))) { r = MD_STORE_NOT_READY; goto done; }
    int64_t at;
    if (!next_timestamp(&j,now,&at)) { r = MD_STORE_CLOCK; goto done; }
    targets = json_object();
    if (!targets) { r = MD_STORE_IO; goto done; }
    const char *key;
    json_t *result;
    json_object_foreach(j.relays,key,result) {
        if (json_object_set_new(targets,key,json_integer(MD_STORE_RELAY_WAITING))) { r = MD_STORE_IO; goto done; }
    }
    /* NIP-44 authenticates content independently of the Nostr timestamp.
     * Re-signing the winner's ciphertext preserves its exact plaintext,
     * including a future schema, without materializing it. */
    event = encrypted_event(s->owner,doc_kind(d),d,nostr_event_get_content(j.winner),at);
    r = save_draft(s,d,&j,event,targets);
done:
    nostr_event_free(event); json_decref(targets); dispose(&j);
    return current(s) ? r : MD_STORE_REVOKED;
}

bool md_managed_store_unattended(MdManagedStore *s) {
    if (!current(s) || s->latch_disabled) return false;
    bool missing;
    json_t *j = read_json(s,"local-unattended",&missing);
    bool enabled = json_is_true(j);
    json_decref(j);
    return enabled && current(s);
}
MdStoreResult md_managed_store_set_unattended(MdManagedStore *s, bool enabled) {
    if (!current(s)) return MD_STORE_REVOKED;
    if (!enabled) s->latch_disabled = true;
    if (!write_json(s,"local-unattended",json_boolean(enabled))) return MD_STORE_IO;
    s->latch_disabled = !enabled;
    return MD_STORE_OK;
}

static json_t *recents(MdManagedStore *s) {
    bool missing;
    json_t *j = read_json(s,"local-recents",&missing);
    if (missing) return json_array();
    if (!json_is_array(j) || json_array_size(j) > RECENT_LIMIT) { json_decref(j); return NULL; }
    size_t i;
    json_t *v;
    json_array_foreach(j,i,v) {
        json_t *at = json_object_get(v,"at"), *outcome = json_object_get(v,"outcome");
        if (!hex(field(v,"pubkey"),64) || !json_is_integer(at) || json_integer_value(at) < 0 ||
            !json_is_integer(outcome) || json_integer_value(outcome) < 0 ||
            json_integer_value(outcome) > MD_RECENT_MEDIA || json_object_size(v) != 3) {
            json_decref(j); return NULL;
        }
    }
    return j;
}
char *md_managed_store_recents(MdManagedStore *s) {
    if (!current(s)) return NULL;
    json_t *j = recents(s);
    char *result = j && current(s) ? json_dumps(j,JSON_COMPACT) : NULL;
    json_decref(j);
    return result;
}
MdStoreResult md_managed_store_record(MdManagedStore *s, const char *pk, int64_t at, MdRecentOutcome outcome) {
    if (!current(s)) return MD_STORE_REVOKED;
    if (!hex(pk,64) || at < 0 || (unsigned)outcome > MD_RECENT_MEDIA) return MD_STORE_INVALID;
    json_t *j = recents(s);
    if (!j) return MD_STORE_IO;
    json_t *entry = json_pack("{s:s,s:I,s:i}","pubkey",pk,"at",(json_int_t)at,"outcome",(int)outcome);
    bool ok = entry && !json_array_insert_new(j,0,entry);
    if (ok && json_array_size(j) > RECENT_LIMIT) json_array_remove(j,RECENT_LIMIT);
    ok = ok && write_json(s,"local-recents",j);
    json_decref(j);
    return ok ? MD_STORE_OK : MD_STORE_IO;
}

static MdStoreResult exchange(const char *input, char **result, bool importing) {
    if (!result) return MD_STORE_INVALID;
    *result = NULL;
    if (!input || strlen(input) > MD_STORE_MAX_CONTENT) return MD_STORE_INVALID;
    json_t *doc = json_loads(input,JSON_REJECT_DUPLICATES,NULL);
    json_t *hosts = importing ? json_object_get(doc,"hosts") : doc;
    json_t *out = json_array(), *seen = json_object(), *wrapper = NULL;
    MdStoreResult r = MD_STORE_INVALID;
    if (!out || !seen) { r = MD_STORE_IO; goto done; }
    const char *format = field(doc,"schema");
    if (importing && (!format || strcmp(format,"metadesk-hosts/1"))) {
        if (format && !strncmp(format,"metadesk-hosts/",15)) r = MD_STORE_READ_ONLY;
        goto done;
    }
    if (!json_is_array(hosts) || json_array_size(hosts) > MD_STORE_MAX_HOSTS) goto done;
    const char *const fields[] = {"pubkey","label","nip05","relay_hints","requested_caps","tags","notes",NULL};
    size_t i;
    json_t *v;
    json_array_foreach(hosts,i,v) {
        const char *pk = field(v,"pubkey");
        json_t *tags = json_object_get(v,"tags");
        if (!hex(pk,64) || json_object_get(seen,pk) || (tags && !strings(tags,MD_STORE_MAX_HOSTS,uuid))) goto done;
        json_t *safe = json_object();
        if (!safe) { r = MD_STORE_IO; goto done; }
        for (size_t k = 0; fields[k]; ++k) {
            json_t *vfield = json_object_get(v,fields[k]);
            if (vfield && json_object_set(safe,fields[k],vfield)) { json_decref(safe); r = MD_STORE_IO; goto done; }
        }
        if (!metadata(safe)) { json_decref(safe); goto done; }
        if (json_array_append_new(out,safe) || json_object_set(seen,pk,json_true())) { r = MD_STORE_IO; goto done; }
    }
    if (!importing) wrapper = json_pack("{s:s,s:O}","schema","metadesk-hosts/1","hosts",out);
    *result = importing || wrapper ? json_dumps(importing ? out : wrapper,JSON_COMPACT) : NULL;
    r = *result ? MD_STORE_OK : MD_STORE_IO;
    if (*result && strlen(*result) > MD_STORE_MAX_CONTENT) { wipe(*result); *result = NULL; r = MD_STORE_INVALID; }
done:
    json_decref(wrapper); json_decref(doc); json_decref(out); json_decref(seen);
    return r;
}
MdStoreResult md_managed_store_import(const char *text, char **candidates) { return exchange(text,candidates,true); }
MdStoreResult md_managed_store_export(const char *candidates, char **text) { return exchange(candidates,text,false); }
