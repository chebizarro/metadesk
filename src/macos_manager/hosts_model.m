#import "hosts_model.h"
#include "stream.h"
#include "md_time.h"
#include "native_relay_store.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <jansson.h>
#include <nostr/nip19/nip19.h>
#include <openssl/crypto.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *Text(const char *s) { return s ? [NSString stringWithUTF8String:s] : nil; }
static void Wipe(char *s) { if (s) { OPENSSL_cleanse(s, strlen(s)); free(s); } }
static json_t *Parse(char *s) { json_t *j = s ? json_loads(s, JSON_REJECT_DUPLICATES, NULL) : NULL; Wipe(s); return j; }
static NSString *Field(json_t *j, const char *f) { return Text(json_string_value(json_object_get(j, f))); }
NSString *MDHostTrim(NSString *s) { return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @""; }

/* Inspect the existing field contents before obtaining a UTF-8 or trimmed copy. */
static NSUInteger SkipSpace(NSString *value, NSUInteger i) {
    NSCharacterSet *spaces = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    while (i < value.length && [spaces characterIsMember:[value characterAtIndex:i]]) ++i;
    return i;
}
static BOOL PrefixAt(NSString *s, NSUInteger i, NSString *prefix) {
    return s.length - i >= prefix.length && [s compare:prefix options:NSCaseInsensitiveSearch | NSLiteralSearch
                                                range:NSMakeRange(i, prefix.length)] == NSOrderedSame;
}
MDHostInput MDClassifyHostInput(NSString *s) {
    NSUInteger i = SkipSpace(s, 0);
    if (PrefixAt(s, i, @"nostr:")) i = SkipSpace(s, i + 6);
    if (PrefixAt(s, i, @"nsec1") || PrefixAt(s, i, @"ncryptsec1")) return MDHostSecret;
    if (i == s.length || s.length > MD_MANAGED_ADDRESS_MAX) return MDHostInvalid;
    NSString *value = MDHostTrim(s);
    if ([value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > MD_MANAGED_ADDRESS_MAX) return MDHostInvalid;
    for (NSUInteger k = 0; k < value.length; ++k) if ([value characterAtIndex:k] == 0) return MDHostInvalid;
    if (PrefixAt(s, i, @"npub1") || PrefixAt(s, i, @"nprofile1")) return MDHostPublic;
    const char *p = value.UTF8String;
    if (p && strlen(p) == 64 && strspn(p, "0123456789abcdef") == 64) return MDHostPublic;
    char *alias = p && strchr(p, '@') ? md_identity_normalize_nip05(p) : NULL;
    if (alias) { free(alias); return MDHostName; }
    return MDHostInvalid;
}
BOOL MDHostRouteValid(NSString *s) {
    if (s.length > 46) return NO;
    unsigned char ip[16];
    const char *p = s.UTF8String;
    return !s.length || (p && strlen(p) == s.length && (inet_pton(AF_INET, p, ip) == 1 || inet_pton(AF_INET6, p, ip) == 1));
}
NSString *MDHostFingerprint(NSString *s) {
    char fp[MD_KEY_FINGERPRINT_SIZE];
    return md_key_fingerprint(s.UTF8String, fp) ? Text(fp) : nil;
}
NSString *MDHostNpub(NSString *s) {
    if (!MDHostFingerprint(s)) return nil;
    MdIdentity id = {0};
    memcpy(id.pubkey, s.UTF8String, sizeof(id.pubkey));
    char *npub = md_identity_npub(&id);
    NSString *out = Text(npub);
    free(npub);
    return out;
}
NSString *MDHostBoundText(NSString *s, NSUInteger limit) {
    if (!s.length) return @"";
    NSUInteger n = MIN(s.length, limit);
    if (n < s.length && n && CFStringIsSurrogateHighCharacter([s characterAtIndex:n-1])) --n;
    NSMutableString *out = [NSMutableString stringWithCapacity:n];
    for (NSUInteger i = 0; i < n; ++i) {
        unichar c = [s characterAtIndex:i];
        if ((c < 0x20 && c != '\n' && c != '\t') || (c >= 0x7f && c <= 0x9f) ||
            (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069)) c = 0xfffd;
        [out appendString:[NSString stringWithCharacters:&c length:1]];
    }
    if (n < s.length) [out appendString:@"…"];
    return out;
}

@interface MDDoc : NSObject {
@public
    char revision[65];
    json_t *winner, *draft;
    MdStoreResult result;
    MdStoreSync sync;
    BOOL reviewReady;
}
@end
@implementation MDDoc
- (void)dealloc { md_managed_store_discard_plaintext(winner); md_managed_store_discard_plaintext(draft); }
@end

static NSString *const Hosts = @MD_STORE_HOSTS, *const Meta = @MD_STORE_MANAGEMENT, *const Favorites = @MD_STORE_FAVORITES;
static NSArray *Base(void) { return @[Hosts, Meta, Favorites]; }
static NSString *Tag(NSString *uuid) { return [@MD_STORE_TAG_PREFIX stringByAppendingString:uuid]; }
static ssize_t Member(json_t *j, NSString *key) {
    const char *target = key.UTF8String;
    if (!target) return -1;
    size_t i; json_t *item;
    json_array_foreach(j, i, item) {
        const char *kind = json_string_value(json_array_get(item, 0)), *pk = json_string_value(json_array_get(item, 1));
        if (kind && pk && !strcmp(kind, "p") && !strcmp(pk, target)) return (ssize_t)i;
    }
    return -1;
}
static BOOL SetMember(json_t *j, NSString *key, BOOL add) {
    if (!json_is_array(j) || !MDHostFingerprint(key)) return NO;
    ssize_t i = Member(j, key);
    if (add) return i >= 0 || json_array_append_new(j, json_pack("[s,s]", "p", key.UTF8String)) == 0;
    while (i >= 0) {
        json_t *old = json_incref(json_array_get(j, (size_t)i));
        if (json_array_remove(j, (size_t)i)) { json_decref(old); return NO; }
        md_managed_store_discard_plaintext(old);
        i = Member(j, key);
    }
    return YES;
}
static MDDoc *ReadDoc(MdStoreController *store, NSString *coordinate) {
    MDDoc *d = [MDDoc new];
    char *a = NULL, *b = NULL;
    d->result = md_store_controller_read(store, coordinate.UTF8String, d->revision, &a, &b);
    d->winner = Parse(a); d->draft = Parse(b);
    d->sync = md_store_controller_sync(store, coordinate.UTF8String);
    d->reviewReady = md_store_controller_review_ready(store, coordinate.UTF8String);
    return d;
}
static BOOL Readable(MDDoc *d) {
    return d && (d->result == MD_STORE_OK || d->result == MD_STORE_PENDING || d->result == MD_STORE_PARTIAL);
}
static BOOL Writable(MDDoc *d) { return d && d->result == MD_STORE_OK && d->sync == MD_SYNC_READY; }
static BOOL SameDoc(MDDoc *a, MDDoc *b) {
    return a && b && a->result == b->result && !strcmp(a->revision, b->revision) &&
        ((!a->draft && !b->draft) || (a->draft && b->draft && json_equal(a->draft, b->draft)));
}
static NSString *StoreError(MdStoreResult result) {
    switch (result) {
        case MD_STORE_OK: case MD_STORE_UNCHANGED: return @"";
        case MD_STORE_PENDING: return @"Edit pending — no storage relay has accepted it yet.";
        case MD_STORE_PARTIAL: return @"Some storage relays have not accepted the edit.";
        case MD_STORE_CONFLICT: return @"The document changed. Review the conflict before editing again.";
        case MD_STORE_READ_ONLY: return @"The document is read-only in this version.";
        case MD_STORE_SIGNER: return @"The client signer could not read or sign the document.";
        case MD_STORE_NOT_READY: return @"Wait until all required fetch relays have caught up before editing.";
        case MD_STORE_LIMIT: return @"The address book is too large.";
        default: return @"Could not save the address book change.";
    }
}
static NSString *DocStatus(MDDoc *d) {
    if (d->result == MD_STORE_INVALID) return @"An encrypted document is invalid and will not be overwritten.";
    if (!Readable(d)) return StoreError(d->result);
    switch (d->sync) {
        case MD_SYNC_READY: return @"";
        case MD_SYNC_OFFLINE: return @"Offline — showing cached hosts.";
        case MD_SYNC_REFUSED: return @"An address book relay refused the subscription.";
        case MD_SYNC_AUTH_PENDING: return @"Waiting for relay authentication…";
        case MD_SYNC_AUTH_REFUSED: return @"An address book relay refused authentication.";
        case MD_SYNC_REPLAY_NEEDED: return @"Address book catch-up was incomplete. Refresh to replay it.";
        default: return @"Syncing address book…";
    }
}
static NSString *JSONText(json_t *j) {
    char *s = j ? json_dumps(j, JSON_COMPACT | JSON_ENCODE_ANY | JSON_ENSURE_ASCII) : NULL;
    NSString *value = Text(s);
    Wipe(s);
    return value;
}
static json_t *CopyDocument(MDDoc *d, BOOL meta) {
    return d && d->winner ? json_deep_copy(d->winner) : meta ? json_pack("{s:i,s:{}}", "schema_version", 1, "hosts") : json_array();
}
static json_t *Ensure(json_t *obj, const char *key) {
    json_t *j = json_object_get(obj, key);
    if (!j && !json_object_set_new(obj, key, json_object())) j = json_object_get(obj, key);
    return json_is_object(j) ? j : NULL;
}

@interface MDHostReview ()
@property(nonatomic, readwrite) NSString *key, *npub, *fingerprint, *alias, *oldKey, *route;
@property(nonatomic, readwrite) uint8_t caps;
@property(nonatomic) NSString *address, *verifiedOwner;
@property(nonatomic) NSArray<NSString *> *hints;
@property(nonatomic) NSDictionary<NSString *, MDDoc *> *documents;
@end
@implementation MDHostReview
@end

@interface MDHostsBook ()
@property(nonatomic, readwrite) NSString *owner, *unavailable;
@property(nonatomic) MDHostsState blocker;
@property(nonatomic) BOOL hasRelays;
@property(nonatomic) NSDictionary<NSString *, MDDoc *> *documents;
@property(nonatomic, readwrite) NSArray<NSDictionary *> *rows, *quick, *issues, *tags;
- (void)project:(json_t *)recents;
@end

static uint8_t Caps(json_t *items) {
    uint8_t caps = MD_CAP_VIDEO;
    size_t i; json_t *item;
    json_array_foreach(items, i, item) {
        const char *s = json_string_value(item);
        if (s && !strcmp(s, "input")) caps |= MD_CAP_INPUT;
        if (s && !strcmp(s, "agent")) caps |= MD_CAP_AGENT;
    }
    return caps;
}
static NSString *Profile(NSString *key, NSArray<NSString *> *hints) {
    char *safe[MD_RELAY_TARGET_MAX]; size_t n = 0;
    for (NSString *hint in hints) if (n < MD_RELAY_TARGET_MAX && [hint lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 255)
        safe[n++] = (char *)hint.UTF8String;
    NostrProfilePointer ptr = {.public_key = (char *)key.UTF8String, .relays = safe, .relays_count = n};
    char *out = NULL;
    if (nostr_nip19_encode_nprofile(&ptr, &out)) return key;
    NSString *address = Text(out); free(out);
    return [address lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= MD_MANAGED_ADDRESS_MAX ? address ?: key : key;
}
static NSArray *Hints(json_t *record) {
    NSMutableArray *hints = [NSMutableArray array];
    size_t i; json_t *j;
    json_array_foreach(json_object_get(record, "relay_hints"), i, j) {
        NSString *s = Text(json_string_value(j));
        if (s && hints.count < MD_RELAY_TARGET_MAX) [hints addObject:s];
    }
    return hints;
}
static json_t *HostRecords(MDDoc *d) { return d ? json_object_get(d->winner, "hosts") : NULL; }
static json_t *HostMeta(MDHostsBook *b, NSString *key) { return key ? json_object_get(HostRecords(b.documents[Meta]), key.UTF8String) : NULL; }
static BOOL Retired(MDHostsBook *b, NSString *key) {
    MDDoc *h = b.documents[Hosts], *m = b.documents[Meta];
    if (!h || !m) return YES;
    BOOL inHosts = Member(h->winner, key) >= 0, inMeta = HostMeta(b, key) != NULL;
    BOOL inHostsDraft = h->draft && Member(h->draft, key) >= 0;
    BOOL inMetaDraft = m->draft && json_object_get(json_object_get(m->draft, "hosts"), key.UTF8String);
    return (h->draft && inHosts != inHostsDraft) || (m->draft && inMeta != inMetaDraft);
}
static BOOL Visible(MDHostsBook *b, NSString *key) {
    MDDoc *d = b.documents[Hosts];
    return d && Member(d->winner, key) >= 0 && !Retired(b, key);
}
static BOOL Favorite(MDHostsBook *b, NSString *key) {
    MDDoc *d = b.documents[Favorites];
    return d && Member(d->winner, key) >= 0 && (!d->draft || Member(d->draft, key) >= 0);
}
static BOOL SameBook(MDHostsBook *a, MDHostsBook *b, BOOL all) {
    if (!a.owner || ![a.owner isEqual:b.owner]) return NO;
    NSArray *coords = all ? a.documents.allKeys : @[Hosts, Meta];
    if (all && coords.count != b.documents.count) return NO;
    for (NSString *d in coords) if (!SameDoc(a.documents[d], b.documents[d])) return NO;
    return YES;
}

@implementation MDHostsBook
- (BOOL)canReview {
    MDDoc *h = self.documents[Hosts], *m = self.documents[Meta];
    return self.hasRelays && Readable(h) && Readable(m) && h->reviewReady && m->reviewReady;
}
- (BOOL)canEdit { return Writable(self.documents[Hosts]) && Writable(self.documents[Meta]); }
- (BOOL)canExport { return Readable(self.documents[Hosts]) && Readable(self.documents[Meta]); }
- (NSString *)status {
    if (self.unavailable) return self.unavailable;
    if (!self.hasRelays) return @"Offline — showing cached hosts. Add explicit bootstrap relays to sync.";
    for (NSString *coordinate in Base()) {
        MDDoc *d = self.documents[coordinate];
        NSString *message = d ? DocStatus(d) : @"Opening the address book…";
        if (message.length) return message;
    }
    return @"Address book synced.";
}
- (MDHostsState)state {
    if (self.unavailable) return self.blocker ?: MDHostsCacheUnavailable;
    if (!self.documents) return MDHostsOpening;
    if (!self.hasRelays) return MDHostsNoRelays;
    for (NSString *coordinate in Base()) {
        MDDoc *d = self.documents[coordinate];
        if (!d) return MDHostsOpening;
        if (d->result == MD_STORE_INVALID || !Readable(d)) return MDHostsDocumentError;
        switch (d->sync) {
            case MD_SYNC_READY: continue;
            case MD_SYNC_OFFLINE: return MDHostsRelaysOffline;
            case MD_SYNC_REFUSED: case MD_SYNC_AUTH_REFUSED: return MDHostsRelayRefused;
            case MD_SYNC_AUTH_PENDING: return MDHostsAuthPending;
            case MD_SYNC_REPLAY_NEEDED: return MDHostsReplayNeeded;
            default: return MDHostsSyncing;
        }
    }
    return MDHostsSynced;
}
- (void)project:(json_t *)recents {
    NSMutableArray *rows = [NSMutableArray array], *tags = [NSMutableArray array], *issues = [NSMutableArray array];
    NSMutableDictionary *byKey = [NSMutableDictionary dictionary], *lastConnected = [NSMutableDictionary dictionary];
    size_t i; json_t *row;
    json_array_foreach(recents, i, row) {
        NSString *key = Field(row, "pubkey");
        if (key && !lastConnected[key] && json_integer_value(json_object_get(row, "outcome")) == MD_RECENT_CONNECTED)
            lastConnected[key] = @(json_integer_value(json_object_get(row, "at")));
    }
    MDDoc *meta = self.documents[Meta], *host = self.documents[Hosts];
    const char *uuid; json_t *tag;
    if (meta) json_object_foreach(json_object_get(meta->winner, "tags"), uuid, tag) {
        NSString *uid = Text(uuid), *label = Field(tag, "label");
        if (!uid || !label || json_is_true(json_object_get(tag, "deleted"))) continue;
        [tags addObject:@{@"id":uid, @"label":MDHostBoundText(label, 128), @"coordinate":Tag(uid)}];
    }
    [tags sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        NSComparisonResult r = [a[@"label"] localizedCaseInsensitiveCompare:b[@"label"]];
        return r == NSOrderedSame ? [a[@"id"] compare:b[@"id"]] : r;
    }];
    if (host) json_array_foreach(host->winner, i, row) {
        NSString *kind = Text(json_string_value(json_array_get(row, 0))), *pk = Text(json_string_value(json_array_get(row, 1)));
        NSString *fp = MDHostFingerprint(pk);
        if (![kind isEqual:@"p"] || !fp || byKey[pk] || !Visible(self, pk)) continue;
        json_t *record = HostMeta(self, pk);
        NSMutableArray *names = [NSMutableArray array], *ids = [NSMutableArray array];
        for (NSDictionary *entry in tags) {
            MDDoc *d = self.documents[entry[@"coordinate"]];
            if (d && Member(d->winner, pk) >= 0 && (!d->draft || Member(d->draft, pk) >= 0)) {
                [names addObject:entry[@"label"]]; [ids addObject:entry[@"id"]];
            }
        }
        NSString *label = MDHostBoundText(Field(record, "label"), 128);
        NSDictionary *hostRow = @{@"key":pk, @"title":label.length ? label : fp, @"label":label,
            @"alias":MDHostBoundText(Field(record, "nip05"), 320), @"notes":MDHostBoundText(Field(record, "notes"), 2048),
            @"tags":names, @"tagIDs":ids, @"favorite":@(Favorite(self, pk)), @"lastConnected":lastConnected[pk] ?: @0};
        [rows addObject:hostRow]; byKey[pk] = hostRow;
    }
    NSMutableArray *quick = [NSMutableArray array]; NSMutableSet *seen = [NSMutableSet set];
    for (NSDictionary *row in rows) if ([row[@"favorite"] boolValue]) { [quick addObject:row]; [seen addObject:row[@"key"]]; }
    NSUInteger recentCount = 0;
    json_array_foreach(recents, i, row) {
        NSString *pk = Field(row, "pubkey"); NSDictionary *saved = pk ? byKey[pk] : nil;
        if (!saved || [seen containsObject:pk]) continue;
        [quick addObject:saved]; [seen addObject:pk];
        if (++recentCount == 10) break;
    }
    for (NSString *coord in [self.documents.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        MDDoc *d = self.documents[coord];
        if (d->result != MD_STORE_OK && d->result != MD_STORE_UNCHANGED)
            [issues addObject:@{@"coordinate":coord, @"message":StoreError(d->result),
                @"conflict":@(d->result == MD_STORE_CONFLICT), @"retry":@(d->result == MD_STORE_PENDING || d->result == MD_STORE_PARTIAL),
                @"ready":@(d->sync == MD_SYNC_READY)}];
    }
    self.rows = rows; self.quick = quick; self.issues = issues; self.tags = tags;
}
- (NSArray<NSDictionary *> *)rowsMatching:(NSString *)query tag:(NSString *)tag {
    NSMutableArray *rows = [NSMutableArray array];
    for (NSDictionary *row in self.rows) {
        if (tag.length && ![row[@"tagIDs"] containsObject:tag]) continue;
        BOOL match = !query.length;
        for (NSString *k in @[@"title", @"key", @"alias", @"notes"]) if ([row[k] localizedCaseInsensitiveContainsString:query]) { match = YES; break; }
        if (match) [rows addObject:row];
    }
    return rows;
}
- (NSString *)addressForKey:(NSString *)key {
    if (!Visible(self, key)) return nil;
    json_t *record = HostMeta(self, key);
    if (!record) return nil; /* A partially applied multi-document edit is not a completed pin. */
    NSString *alias = Field(record, "nip05");
    return alias.length ? alias : Profile(key, Hints(record));
}
- (MDHostReview *)reviewIdentity:(const MdIdentity *)id alias:(NSString *)alias route:(NSString *)route error:(NSString **)error {
    NSString *key = Text(id->pubkey);
    if (!self.canReview || !MDHostFingerprint(key) || Retired(self, key) ||
        (Visible(self, key) != (HostMeta(self, key) != NULL))) {
        if (error) *error = @"Host review unavailable. Sync the book and reconcile pending changes.";
        return nil;
    }
    if ((id->hint_source == MD_HINT_NIP05) != (alias.length != 0)) {
        if (error) *error = @"The NIP-05 name could not be verified.";
        return nil;
    }
    NSString *stored = Field(HostMeta(self, key), "nip05"), *pin = nil;
    if (stored.length && ![stored isEqual:alias]) {
        if (error) *error = @"This key has a different saved NIP-05 name. Review the saved name instead.";
        return nil;
    }
    const char *pk; json_t *meta;
    if (alias.length) json_object_foreach(HostRecords(self.documents[Meta]), pk, meta) {
        if (![Field(meta, "nip05") isEqual:alias]) continue;
        if (pin || !Visible(self, Text(pk))) {
            if (error) *error = @"This saved name is ambiguous or has a pending key change. Reconcile the address book first.";
            return nil;
        }
        pin = Text(pk);
    }
    MdResolveResult result = md_identity_check_pin(id, pin.UTF8String);
    if (result != MD_RESOLVE_OK && result != MD_RESOLVE_PIN_MISMATCH) {
        if (error) *error = @"The saved host pin is invalid.";
        return nil;
    }
    NSMutableArray *hints = [NSMutableArray array];
    for (size_t i = 0; i < MIN(id->hint_count, MD_RELAY_TARGET_MAX); ++i) {
        NSString *hint = Text(id->hints[i]); if (hint) [hints addObject:hint];
    }
    MDHostReview *r = [MDHostReview new];
    r.key = key; r.npub = MDHostNpub(key); r.fingerprint = MDHostFingerprint(key); r.alias = alias; r.route = route ?: @"";
    r.oldKey = result == MD_RESOLVE_PIN_MISMATCH ? pin : nil;
    r.hints = hints; r.address = Profile(key, hints); r.verifiedOwner = self.owner; r.documents = self.documents;
    r.caps = Visible(self, key) ? Caps(json_object_get(HostMeta(self, key), "requested_caps")) : MD_CAP_VIDEO;
    return r;
}
- (BOOL)canReplaceReview:(MDHostReview *)review {
    if (!self.canEdit || !review.oldKey || !Writable(self.documents[Favorites])) return NO;
    for (NSDictionary *tag in self.tags) if (!Writable(self.documents[tag[@"coordinate"]])) return NO;
    return YES;
}
- (BOOL)canConfirmReview:(MDHostReview *)review fingerprint:(NSString *)value {
    return [self canReplaceReview:review] && [review.fingerprint isEqualToString:value];
}
- (NSString *)conflictText:(NSString *)coordinate {
    MDDoc *d = self.documents[coordinate];
    if (!d || d->result != MD_STORE_CONFLICT) return nil;
    return [NSString stringWithFormat:@"Remote version\n%@\n\nLocal draft\n%@", JSONText(d->winner) ?: @"Unavailable", JSONText(d->draft) ?: @"Unavailable"];
}
@end

/* Each draft owns exactly one independent plaintext tree. Stage only after
 * preparing every affected coordinate and checking its write barrier. The
 * separate relay documents do not form an atomic transaction. */
@interface MDHostEdits : NSObject
@property(nonatomic) NSMutableDictionary<NSString *, MDDoc *> *documents;
@property(nonatomic) NSMutableArray<NSString *> *order;
@end
@implementation MDHostEdits
- (instancetype)init {
    if ((self = [super init])) { self.documents = [NSMutableDictionary dictionary]; self.order = [NSMutableArray array]; }
    return self;
}
- (json_t *)edit:(NSString *)coordinate base:(MDDoc *)base {
    MDDoc *d = self.documents[coordinate];
    if (d) return d->winner;
    if (!Writable(base)) return NULL;
    d = [MDDoc new];
    memcpy(d->revision, base->revision, sizeof(d->revision));
    d->winner = CopyDocument(base, [coordinate isEqual:Meta]);
    if (!d->winner) return NULL;
    self.documents[coordinate] = d;
    [self.order addObject:coordinate];
    return d->winner;
}
@end

static NSString *const Changed = @"The address book changed. Open the review again.";
static NSString *const Wait = @"Wait for every affected address book collection to finish syncing.";
/* Mutate independent document copies only. Scrub detached nodes as well as
 * the tree that remains when the edit is finished. */
static BOOL ReplaceField(json_t *j, const char *field, json_t *value) {
    json_t *old = json_incref(json_object_get(j, field));
    int rc = json_object_set(j, field, value);
    if (rc) { json_decref(old); md_managed_store_discard_plaintext(value); }
    else { md_managed_store_discard_plaintext(old); json_decref(value); }
    return rc == 0;
}
static BOOL RemoveField(json_t *j, const char *field) {
    json_t *old = json_incref(json_object_get(j, field));
    if (!old) return YES;
    if (json_object_del(j, field)) { json_decref(old); return NO; }
    md_managed_store_discard_plaintext(old);
    return YES;
}
static BOOL SetText(json_t *j, const char *field, NSString *value) {
    const char *p = value.UTF8String;
    return j && p && [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] == strlen(p) &&
        ReplaceField(j, field, json_string(p));
}
static BOOL SetHints(json_t *record, NSArray<NSString *> *hints) {
    if (!hints.count) return YES;
    json_t *a = json_array();
    if (!a) return NO;
    for (NSString *url in hints) if (json_array_append_new(a, json_string(url.UTF8String))) {
        md_managed_store_discard_plaintext(a); return NO;
    }
    return ReplaceField(record, "relay_hints", a);
}

NSString *MDHostsBootstrap(NSString *input, NSString **error) {
    NSString *directory = MDRelayConfigDirectory();
    if (input) {
        NSMutableOrderedSet<NSString *> *urls = [NSMutableOrderedSet orderedSet];
        if (input.length > (MD_RELAY_URL_MAX + 1) * MD_RELAY_TARGET_MAX) {
            if (error) *error = @"The bootstrap relay list is too long.";
            return nil;
        }
        for (NSString *piece in [input componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]) {
            if (!piece.length) continue;
            const char *p = piece.UTF8String;
            NSString *url = p && strlen(p) == [piece lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ?
                MDRelayNormalize(p) : nil;
            if (!url || [url lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > MD_RELAY_URL_MAX) {
                if (error) *error = @"Use ws:// or wss:// URLs without credentials, queries or fragments. Enter international hostnames in ASCII punycode (xn--).";
                return nil;
            }
            [urls addObject:url];
        }
        if (urls.count > MD_RELAY_TARGET_MAX) {
            if (error) *error = @"Too many bootstrap relays.";
            return nil;
        }
        if (!MDRelaySave(directory, urls.array)) {
            if (error) *error = @"Could not save bootstrap relays. Check the private configuration directory.";
            return nil;
        }
        return [urls.array componentsJoinedByString:@" "];
    }

    NSArray<NSString *> *values = MDRelayLoad(directory);
    if (values) {
        if (values.count > MD_RELAY_TARGET_MAX) {
            if (error) *error = @"The saved bootstrap relay list exceeds the route limits.";
            return nil;
        }
        for (NSString *url in values) {
            if ([url lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > MD_RELAY_URL_MAX) {
                if (error) *error = @"The saved bootstrap relay list exceeds the route limits.";
                return nil;
            }
        }
        return [values componentsJoinedByString:@" "];
    }
    if (error) *error = @"Could not read saved bootstrap relays. Use ASCII punycode (xn--) for international hostnames. No relay connections were started.";
    return nil;
}

@interface MDHostsWorker () {
    MdAccountStartup _account;
    MdStoreController *_store;
    MdRelayPlan _relays;
    NSString *_owner, *_unavailable;
    MDHostsState _blocker;
    dispatch_block_t _changed;
}
- (void)notify;
@end
static void ControllerChanged(void *context) {
    @autoreleasepool { [(__bridge MDHostsWorker *)context notify]; }
}

@implementation MDHostsWorker
- (instancetype)initWithChange:(dispatch_block_t)changed {
    if ((self = [super init])) _changed = [changed copy];
    return self;
}
- (void)notify { if (_changed) _changed(); }
- (void)dealloc { NSCAssert(!_store && !_account.accounts, @"Close the Hosts worker on its owning queue first"); }
- (void)close {
    md_store_controller_close(_store); _store = NULL;
    md_account_startup_clear(&_account);
    md_relay_plan_clear(&_relays);
    _owner = nil; _unavailable = nil; _blocker = MDHostsOpening;
}
- (BOOL)attach:(MdAccountBinding *)binding directory:(NSString *)directory {
    MdSigner *signer = md_account_binding_signer(binding);
    _owner = signer ? Text(signer->pubkey_hex) : nil;
    _store = _owner && directory.isAbsolutePath ? md_store_controller_open(binding, directory.fileSystemRepresentation,
        ControllerChanged, (__bridge void *)self) : NULL;
    return _store != NULL;
}
- (BOOL)openBinding:(MdAccountBinding *)binding directory:(NSString *)directory {
    [self close];
    return [self attach:binding directory:directory];
}
- (BOOL)openDirectory:(NSString *)directory error:(NSString **)error {
    [self close];
    int rc = md_account_startup_open(MD_ACCOUNT_CLIENT, &_account);
    if (rc) {
        switch (rc) {
            case MD_CREDENTIAL_NOT_FOUND: case MD_CREDENTIAL_LOCKED: case MD_CREDENTIAL_UNAVAILABLE:
            case MD_CREDENTIAL_INVALID: case MD_CREDENTIAL_FAILED: case MD_CREDENTIAL_AMBIGUOUS:
                _unavailable = [NSString stringWithFormat:@"Local NIP-46 credential access failed: %s. Check Keychain and the saved pairing; no external signer was reached.",
                    md_credential_error_string(rc)];
                _blocker = MDHostsCredentialFailed;
                break;
            default:
                _unavailable = [NSString stringWithFormat:@"Client signer unavailable: %s. Check or unlock it outside Metadesk.", md_accounts_error_string(rc)];
                _blocker = MDHostsSignerUnavailable;
        }
    } else if (!_account.binding) { _unavailable = @"Assign a client role with the account CLI, then refresh."; _blocker = MDHostsNoClientRole; }
    else {
        if (!directory.length) {
            char *path = md_managed_store_default_directory(); directory = Text(path); free(path);
        }
        if (!directory.isAbsolutePath || ![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:@{NSFilePosixPermissions:@0700} error:NULL] || ![self attach:_account.binding directory:directory])
            { _unavailable = @"The encrypted cache is unavailable. Check the private state directory and other manager instances."; _blocker = MDHostsCacheUnavailable; }
    }
    if (error) *error = _unavailable;
    return _store != NULL;
}
- (BOOL)startRelays:(NSString *)urls {
    if (!_store) return NO;
    MdRelayPlan plan = {0};
    BOOL ok = urls.length <= (MD_RELAY_URL_MAX + 1) * MD_RELAY_TARGET_MAX;
    if (ok) for (NSString *piece in [urls componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]) {
        if (!piece.length) continue;
        const char *p = piece.UTF8String;
        if (!p || [piece lengthOfBytesUsingEncoding:NSUTF8StringEncoding] != strlen(p) || !md_relay_plan_add(&plan, p, MD_RELAY_CONFIGURED)) {
            ok = NO; break;
        }
    }
    if (ok) ok = md_store_controller_start(_store, plan.count ? &plan : NULL);
    if (ok) { md_relay_plan_clear(&_relays); _relays = plan; }
    else md_relay_plan_clear(&plan);
    return ok;
}
- (MDHostsBook *)snapshot {
    MDHostsBook *book = [MDHostsBook new];
    book.owner = _owner; book.unavailable = _unavailable; book.blocker = _blocker; book.hasRelays = _relays.count != 0;
    if (!_store) return book;
    NSMutableDictionary *docs = [NSMutableDictionary dictionary];
    for (NSString *coord in Base()) docs[coord] = ReadDoc(_store, coord);
    MDDoc *meta = docs[Meta];
    const char *uuid; json_t *tag;
    json_object_foreach(json_object_get(meta->winner, "tags"), uuid, tag) {
        if (json_is_true(json_object_get(tag, "deleted"))) continue;
        NSString *coordinate = Tag(Text(uuid));
        if (md_store_controller_watch(_store, coordinate.UTF8String)) docs[coordinate] = ReadDoc(_store, coordinate);
    }
    book.documents = docs;
    json_t *recents = Parse(md_store_controller_recents(_store));
    [book project:recents];
    md_managed_store_discard_plaintext(recents);
    return book;
}
- (MDHostReview *)resolve:(NSString *)value route:(NSString *)route cancelled:(bool (*)(void *))cancelled data:(void *)data error:(NSString **)error {
    MDHostInput input = MDClassifyHostInput(value);
    if ((input != MDHostPublic && input != MDHostName) || !MDHostRouteValid(route)) {
        if (error) *error = @"Enter a public host identity and, optionally, an IPv4 or IPv6 route.";
        return nil;
    }
    value = MDHostTrim(value);
    MdIdentity id = {0};
    MdResolveResult r = md_identity_resolve_cancellable(value.UTF8String, NULL, &id, cancelled, data);
    char *normalized = r == MD_RESOLVE_OK && id.hint_source == MD_HINT_NIP05 ? md_identity_normalize_nip05(value.UTF8String) : NULL;
    NSString *alias = Text(normalized); free(normalized);
    MDHostReview *review = r == MD_RESOLVE_OK ? [[self snapshot] reviewIdentity:&id alias:alias route:route error:error] : nil;
    if (r != MD_RESOLVE_OK && error) *error = Text(md_resolve_error(r));
    md_identity_clear(&id);
    return review;
}
- (BOOL)review:(MDHostReview *)r stillCurrent:(MDHostsBook *)book {
    return r && [book.owner isEqual:r.verifiedOwner] && book.canReview && !Retired(book, r.key) &&
        SameDoc(book.documents[Hosts], r.documents[Hosts]) && SameDoc(book.documents[Meta], r.documents[Meta]);
}
- (BOOL)launchConfig:(MdManagedLaunch *)c review:(MDHostReview *)r error:(NSString **)error {
    MDHostsBook *book = [self snapshot];
    if (!c || ![self review:r stillCurrent:book] || r.oldKey || !_relays.count) {
        if (error) *error = r.oldKey ? @"Host key changed. Keep the saved key or replace it after verifying the new fingerprint." : Changed;
        return NO;
    }
    memset(c, 0, sizeof(*c));
    strlcpy(c->address, r.address.UTF8String, sizeof(c->address));
    strlcpy(c->route, r.route.UTF8String, sizeof(c->route));
    strlcpy(c->pin, r.key.UTF8String, sizeof(c->pin));
    strlcpy(c->signer, book.owner.UTF8String, sizeof(c->signer));
    c->relay_count = _relays.count;
    for (size_t i = 0; i < _relays.count; ++i) strlcpy(c->relays[i], _relays.target[i].url, sizeof(c->relays[i]));
    json_int_t port = json_integer_value(json_object_get(HostMeta(book, r.key), "port"));
    c->port = port > 0 && port <= UINT16_MAX ? (uint16_t)port : MD_STREAM_PORT;
    c->capabilities = r.caps;
    return YES;
}
- (NSString *)stage:(MDHostEdits *)edits {
    NSMutableArray<NSString *> *pending = [NSMutableArray array];
    for (NSString *coord in edits.order) {
        MDDoc *d = edits.documents[coord];
        char *text = json_dumps(d->winner, JSON_COMPACT | JSON_ENCODE_ANY);
        MdStoreResult r = text ? md_store_controller_edit(_store, coord.UTF8String, MD_EDIT_STAGE, d->revision, text) : MD_STORE_IO;
        Wipe(text);
        if (r == MD_STORE_PENDING || r == MD_STORE_PARTIAL) [pending addObject:coord];
        else if (r != MD_STORE_OK && r != MD_STORE_UNCHANGED) {
            NSString *message = StoreError(r);
            return pending.count ? [message stringByAppendingString:@" Some collections already have pending drafts; review them separately."] : message;
        }
    }
    NSString *message = @"";
    for (NSString *coord in pending) {
        NSString *result = StoreError(md_store_controller_flush(_store, coord.UTF8String));
        if (!message.length && result.length) message = result;
    }
    return message;
}
- (NSString *)saveReview:(MDHostReview *)r confirmation:(NSString *)fingerprint {
    MDHostsBook *book = [self snapshot];
    if (![self review:r stillCurrent:book]) return Changed;
    if (!book.canEdit) return Wait;
    if (r.oldKey && ![book canConfirmReview:r fingerprint:fingerprint]) return @"Replacement requires the exact verified fingerprint and all memberships to be synced.";
    MDHostEdits *plan = [MDHostEdits new];
    json_t *meta = Ensure([plan edit:Meta base:book.documents[Meta]], "hosts");
    json_t *hosts = [plan edit:Hosts base:book.documents[Hosts]];
    json_t *record = Ensure(meta, r.key.UTF8String);
    if (!record || (r.alias && !SetText(record, "nip05", r.alias)) || !SetHints(record, r.hints) || !SetMember(hosts, r.key, YES))
        return @"Could not prepare the saved host change.";
    if (r.oldKey) {
        json_t *old = json_object_get(meta, r.oldKey.UTF8String);
        for (const char **field = (const char *[]){"label", "notes", NULL}; *field; ++field) {
            NSString *value = Field(old, *field);
            if (value && !SetText(record, *field, value)) return @"Could not copy the saved host details.";
        }
        if (!RemoveField(meta, r.oldKey.UTF8String) || !SetMember(hosts, r.oldKey, NO)) return @"Could not prepare replacement.";
        NSArray *coordinates = [@[Favorites] arrayByAddingObjectsFromArray:[book.tags valueForKey:@"coordinate"]];
        for (NSString *coord in coordinates) {
            MDDoc *d = book.documents[coord];
            if (!SameDoc(d, r.documents[coord]) || !Writable(d)) return Changed;
            if (Member(d->winner, r.oldKey) < 0) continue;
            json_t *list = [plan edit:coord base:d];
            if (!SetMember(list, r.oldKey, NO) || !SetMember(list, r.key, YES)) return @"Could not carry the saved memberships forward.";
        }
    }
    return [self stage:plan];
}
- (MDDoc *)watchMembership:(NSString *)coordinate deadline:(uint64_t)deadline {
    if (!md_store_controller_watch(_store, coordinate.UTF8String) ||
        md_store_controller_wait(_store, coordinate.UTF8String, deadline) != MD_SYNC_READY) return nil;
    return ReadDoc(_store, coordinate);
}
- (NSString *)editKey:(NSString *)key book:(MDHostsBook *)previous label:(NSString *)label notes:(NSString *)notes
             favorite:(BOOL)favorite tagIDs:(NSSet<NSString *> *)tagIDs newTag:(NSString *)newTag remove:(BOOL)remove {
    MDHostsBook *book = [self snapshot];
    if (!SameBook(previous, book, YES) || !Visible(book, key)) return Changed;
    if (!book.canEdit) return Wait;
    if ([newTag lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 64) return @"The tag is too long.";
    MDHostEdits *plan = [MDHostEdits new];
    json_t *meta = [plan edit:Meta base:book.documents[Meta]];
    json_t *records = Ensure(meta, "hosts");
    if (!records) return @"Could not prepare the saved host change.";
    if (remove) {
        json_t *members = [plan edit:Hosts base:book.documents[Hosts]];
        if (!SetMember(members, key, NO)) return @"Could not remove the saved host.";
        if (!RemoveField(records, key.UTF8String)) return @"Could not remove saved host details.";
    } else {
        json_t *record = Ensure(records, key.UTF8String);
        /* The editor uses the safe display projection. Changing a membership
         * must not rewrite an untouched label or note with its display text. */
        if (!record) return @"Could not prepare the saved host details.";
        if (![label isEqual:MDHostBoundText(Field(record, "label"), 128)] &&
            ([label lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 128 || !SetText(record, "label", label)))
            return @"Invalid or oversized host label.";
        if (![notes isEqual:MDHostBoundText(Field(record, "notes"), 2048)] &&
            ([notes lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 2048 || !SetText(record, "notes", notes)))
            return @"Invalid or oversized host notes.";
    }
    MDDoc *fav = book.documents[Favorites];
    if (!Readable(fav)) return Wait;
    if (favorite != (Member(fav->winner, key) >= 0) || remove) {
        if (!SetMember([plan edit:Favorites base:fav], key, !remove && favorite)) return Wait;
    }
    NSMutableSet *desired = [tagIDs mutableCopy] ?: [NSMutableSet set];
    uint64_t deadline = md_monotonic_ns() + 10000000000ULL;
    if (!remove && newTag.length) {
        NSString *id = nil;
        for (NSDictionary *t in book.tags) if ([t[@"label"] caseInsensitiveCompare:newTag] == NSOrderedSame) { id = t[@"id"]; break; }
        if (id) [desired addObject:id];
        else {
            id = NSUUID.UUID.UUIDString.lowercaseString;
            NSString *coordinate = Tag(id);
            MDDoc *tag = [self watchMembership:coordinate deadline:deadline];
            if (!Writable(tag) || *tag->revision) return Wait;
            json_t *entry = Ensure(Ensure(meta, "tags"), id.UTF8String);
            if (!SetText(entry, "label", newTag) || !SetMember([plan edit:coordinate base:tag], key, YES)) return Wait;
        }
    }
    for (NSDictionary *tag in book.tags) {
        NSString *coord = tag[@"coordinate"];
        MDDoc *d = book.documents[coord];
        if (!Readable(d)) return Wait;
        BOOL selected = !remove && [desired containsObject:tag[@"id"]];
        if (selected != (Member(d->winner, key) >= 0) && !SetMember([plan edit:coord base:d], key, selected)) return Wait;
    }
    return [self stage:plan];
}
- (NSString *)resolveIssue:(NSString *)coordinate book:(MDHostsBook *)old reapply:(BOOL)reapply {
    MDHostsBook *book = [self snapshot];
    MDDoc *d = book.documents[coordinate];
    if (![old.owner isEqual:book.owner] || !SameDoc(old.documents[coordinate], d) || d->result != MD_STORE_CONFLICT) return Changed;
    if (d->sync != MD_SYNC_READY) return Wait;
    char *draft = reapply && d->draft ? json_dumps(d->draft, JSON_COMPACT | JSON_ENCODE_ANY) : NULL;
    MdStoreResult r = reapply && !draft ? MD_STORE_INVALID : md_store_controller_edit(_store, coordinate.UTF8String,
        reapply ? MD_EDIT_REAPPLY : MD_EDIT_KEEP_REMOTE, d->revision, draft);
    Wipe(draft);
    if (r == MD_STORE_PENDING || r == MD_STORE_PARTIAL) r = md_store_controller_flush(_store, coordinate.UTF8String);
    return StoreError(r);
}
- (NSString *)retryIssue:(NSString *)coordinate book:(MDHostsBook *)old {
    MDHostsBook *book = [self snapshot];
    MDDoc *d = book.documents[coordinate];
    if (![old.owner isEqual:book.owner] || !SameDoc(old.documents[coordinate], d) ||
        (d->result != MD_STORE_PENDING && d->result != MD_STORE_PARTIAL)) return Changed;
    return d->sync == MD_SYNC_READY ? StoreError(md_store_controller_flush(_store, coordinate.UTF8String)) : Wait;
}
- (void)recordKey:(NSString *)key owner:(NSString *)owner outcome:(MdRecentOutcome)outcome {
    if (_store && [_owner isEqual:owner]) md_store_controller_record(_store, key.UTF8String, outcome);
}

/* The exchange codec validates all fields before any review. Keep normalized
 * candidates in owned, scrubbed Jansson trees; the UI only receives projections. */
- (NSArray<NSDictionary *> *)readImport:(NSURL *)url book:(MDHostsBook *)book {
    if (!url.isFileURL || !book.canEdit) return nil;
    int fd = open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return nil;
    struct stat st;
    char *buffer = NULL, *normalized = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= MD_STORE_MAX_CONTENT)
        buffer = malloc(MD_STORE_MAX_CONTENT + 1);
    size_t used = 0;
    if (buffer) while (used < MD_STORE_MAX_CONTENT + 1) {
        ssize_t n = read(fd, buffer + used, MD_STORE_MAX_CONTENT + 1 - used);
        if (!n) {
            if (used && !memchr(buffer, 0, used)) { buffer[used] = 0; md_managed_store_import(buffer, &normalized); }
            break;
        }
        if (n < 0) { if (errno == EINTR) continue; break; }
        used += (size_t)n;
    }
    close(fd);
    if (buffer) { OPENSSL_cleanse(buffer, MD_STORE_MAX_CONTENT + 1); free(buffer); }
    json_t *candidates = Parse(normalized);
    if (!candidates) return nil;
    NSMutableArray *rows = [NSMutableArray array];
    size_t i; json_t *item;
    json_array_foreach(candidates, i, item) {
        NSString *key = Field(item, "pubkey"), *npub = MDHostNpub(key);
        if (!npub || Member(book.documents[Hosts]->winner, key) >= 0 || HostMeta(book, key) || Retired(book, key)) continue;
        MDDoc *owned = [MDDoc new]; owned->winner = json_deep_copy(item);
        if (!owned->winner) continue;
        NSString *caps = JSONText(json_object_get(item, "requested_caps"));
        [rows addObject:@{@"key":key, @"npub":npub, @"fingerprint":MDHostFingerprint(key), @"record":owned,
            @"label":MDHostBoundText(Field(item, "label"), 128), @"alias":MDHostBoundText(Field(item, "nip05"), 320),
            @"caps":caps ? MDHostBoundText(caps, 120) : @"Video (view only)"}];
    }
    md_managed_store_discard_plaintext(candidates);
    return rows;
}
- (NSString *)applyImport:(NSArray<NSDictionary *> *)items selected:(NSSet<NSString *> *)keys book:(MDHostsBook *)old {
    MDHostsBook *book = [self snapshot];
    if (!SameBook(old, book, YES) || !book.canEdit) return Changed;
    if (!keys.count) return @"No imported keys selected.";
    MDHostEdits *plan = [MDHostEdits new];
    json_t *meta = [plan edit:Meta base:book.documents[Meta]];
    json_t *records = Ensure(meta, "hosts"), *members = [plan edit:Hosts base:book.documents[Hosts]];
    NSMutableSet *aliases = [NSMutableSet set];
    const char *pk; json_t *entry;
    json_object_foreach(records, pk, entry) {
        NSString *alias = Field(entry, "nip05"); if (alias.length) [aliases addObject:alias];
    }
    uint64_t deadline = md_monotonic_ns() + 10000000000ULL;
    NSMutableDictionary *additional = [NSMutableDictionary dictionary];
    for (NSDictionary *item in items) {
        NSString *key = item[@"key"];
        if (![keys containsObject:key]) continue;
        MDDoc *owned = item[@"record"];
        json_t *source = owned->winner;
        NSString *alias = Field(source, "nip05");
        if ((alias.length && [aliases containsObject:alias]) || HostMeta(book, key) || Member(members, key) >= 0)
            return @"Import blocked: a selected host conflicts with a saved or imported pin.";
        if (alias.length) [aliases addObject:alias];
        size_t i; json_t *tag;
        json_array_foreach(json_object_get(source, "tags"), i, tag) {
            NSString *id = Text(json_string_value(tag)), *coord = Tag(id);
            json_t *labels = Ensure(meta, "tags"), *entry = json_object_get(labels, id.UTF8String);
            if (json_is_true(json_object_get(entry, "deleted"))) return @"Import references a deleted tag.";
            MDDoc *d = book.documents[coord] ?: additional[coord];
            if (!d) {
                d = [self watchMembership:coord deadline:deadline];
                if (!Writable(d)) return Wait;
                additional[coord] = d;
            }
            if (!entry && !SetText(Ensure(labels, id.UTF8String), "label", id)) return Wait;
            if (!SetMember([plan edit:coord base:d], key, YES)) return Wait;
        }
        json_t *record = json_deep_copy(source);
        if (!record) return @"Could not prepare the import.";
        if (!RemoveField(record, "pubkey") || !RemoveField(record, "tags")) {
            md_managed_store_discard_plaintext(record);
            return @"Could not prepare the import.";
        }
        if (!ReplaceField(records, key.UTF8String, record) || !SetMember(members, key, YES)) return @"Could not prepare the import.";
    }
    return [self stage:plan];
}
- (NSString *)exportBook:(MDHostsBook *)book to:(NSURL *)url {
    if (!url.isFileURL || !book.canExport || !SameBook(book, [self snapshot], YES)) return Changed;
    for (NSDictionary *tag in book.tags) if (!Readable(book.documents[tag[@"coordinate"]])) return @"Cannot export unreadable tag memberships.";
    json_t *records = json_array();
    BOOL ok = records != NULL;
    for (NSDictionary *row in book.rows) {
        NSString *key = row[@"key"];
        json_t *meta = HostMeta(book, key), *entry = meta ? json_deep_copy(meta) : json_object();
        json_t *tags = json_array();
        ok = ok && entry && tags && SetText(entry, "pubkey", key);
        for (NSString *id in row[@"tagIDs"]) if (json_array_append_new(tags, json_string(id.UTF8String))) ok = NO;
        if (ok) ok = !json_object_set(entry, "tags", tags) && !json_array_append(records, entry);
        json_decref(tags); /* Shared with records while retained; scrub records below. */
        if (ok) json_decref(entry); else md_managed_store_discard_plaintext(entry);
        if (!ok) break;
    }
    char *items = ok ? json_dumps(records, JSON_COMPACT) : NULL, *text = NULL;
    md_managed_store_discard_plaintext(records);
    if (items) md_managed_store_export(items, &text);
    Wipe(items);
    if (!text) return @"Could not prepare the plaintext export.";
    NSString *template = [[url.path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@".metadesk-export-XXXXXX"];
    char *path = strdup(template.fileSystemRepresentation);
    int fd = path ? mkstemp(path) : -1;
    ok = fd >= 0;
    size_t done = 0, len = strlen(text);
    while (ok && done < len) {
        ssize_t n = write(fd, text + done, len - done);
        if (n > 0) done += (size_t)n;
        else if (!n || errno != EINTR) ok = NO;
    }
    if (ok && fsync(fd)) ok = NO;
    if (fd >= 0 && close(fd)) ok = NO;
    if (ok) ok = !rename(path, url.fileSystemRepresentation);
    if (!ok && fd >= 0) unlink(path);
    free(path); Wipe(text);
    return ok ? @"Plaintext address book exported." : @"Could not write the selected export file.";
}
@end
