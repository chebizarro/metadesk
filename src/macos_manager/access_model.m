#import "access_model.h"
#include "md_time.h"
#include <errno.h>
#include <jansson.h>
#include <math.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdatomic.h>
#include <unistd.h>

static BOOL Dict(id v) { return [v isKindOfClass:NSDictionary.class]; }
static BOOL Bool(id v) { return [v isKindOfClass:NSNumber.class] && CFGetTypeID((__bridge CFTypeRef)v) == CFBooleanGetTypeID(); }
static BOOL Uint(id v, double max) {
    if (![v isKindOfClass:NSNumber.class] || Bool(v)) return NO;
    double n = [v doubleValue]; return n >= 0 && n <= max && floor(n) == n;
}
static BOOL Str(id v, NSUInteger max) { return [v isKindOfClass:NSString.class] && [v length] <= max; }
static BOOL Hex(id v, NSUInteger n) {
    if (!Str(v, n) || [v length] != n) return NO;
    const char *p = [v UTF8String]; return p && strspn(p, "0123456789abcdef") == n;
}
static BOOL Request(id v) {
    if (!Dict(v)) return NO;
    NSDictionary *q = v;
    return Hex(q[@"peer"], 64) && Hex(q[@"request_id"], 32) && Uint(q[@"requested"], UINT32_MAX) &&
        Uint(q[@"granted"], UINT32_MAX) && Uint(q[@"expires_at"], 9007199254740991.0) &&
        [@[@"pending", @"remembering", @"approved"] containsObject:q[@"state"] ?: NSNull.null];
}
NSDictionary *MDParseHostFrame(NSData *data) {
    if (!data.length || data.length > 8192 || memchr(data.bytes, 0, data.length)) return nil;
    json_t *strict = json_loadb(data.bytes, data.length, JSON_REJECT_DUPLICATES, NULL);
    BOOL valid = json_is_object(strict); json_decref(strict);
    if (!valid) return nil;
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    if (!Dict(value)) return nil;
    NSDictionary *j = value;
    if (!Uint(j[@"v"], 1) || ![j[@"v"] isEqual:@1] || !Bool(j[@"ok"]) || !Hex(j[@"generation"], 32) ||
        !Uint(j[@"revision"], 9007199254740991.0) || (j[@"supported"] && !Uint(j[@"supported"], UINT32_MAX))) return nil;
    NSArray *requests = j[@"requests"];
    if (![requests isKindOfClass:NSArray.class] || requests.count > 16) return nil;
    NSMutableSet *ids = [NSMutableSet set];
    for (NSDictionary *request in requests) {
        if (!Request(request) || [ids containsObject:request[@"request_id"]]) return nil;
        [ids addObject:request[@"request_id"]];
    }
    id s = j[@"session"];
    if (s != NSNull.null && !(Request(s) && Bool(s[@"revoked"]) && Str(s[@"session_id"], 63) && [s[@"session_id"] length])) return nil;
    if (j[@"clients"] || j[@"policy_version"] || j[@"policy_available"]) {
        if (![j[@"clients"] isKindOfClass:NSArray.class] || !Uint(j[@"policy_version"], 9007199254740991.0) || !Bool(j[@"policy_available"])) return nil;
        [ids removeAllObjects];
        for (NSDictionary *client in j[@"clients"]) {
            if (!Dict(client) || !Hex(client[@"peer"], 64) || [ids containsObject:client[@"peer"]] ||
                !Str(client[@"caps"], 63) || !Bool(client[@"review_required"])) return nil;
            [ids addObject:client[@"peer"]];
        }
    }
    if (j[@"host"]) {
        NSDictionary *h = j[@"host"];
        if (!Dict(h) || !Bool(h[@"managed"]) || !Uint(h[@"pid"], INT_MAX) || !Str(h[@"mode"], 32) || !Str(h[@"state"], 32)) return nil;
    }
    return j;
}

NSArray<NSDictionary *> *MDLiveRequests(NSDictionary *snapshot, NSTimeInterval now) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSDictionary *q in snapshot[@"requests"]) if ([q[@"expires_at"] doubleValue] > now) [out addObject:q];
    return out;
}
NSString *MDRequestToken(NSDictionary *snapshot, NSDictionary *request) {
    if (!Hex(snapshot[@"generation"], 32) || !Hex(request[@"request_id"], 32)) return nil;
    return [NSString stringWithFormat:@"%@/%@", snapshot[@"generation"], request[@"request_id"]];
}
NSSet<NSString *> *MDRequestNotices(NSDictionary *s, NSTimeInterval now) {
    NSMutableSet *tokens=[NSMutableSet set];
    for (NSDictionary *r in MDLiveRequests(s,now)) if ([r[@"state"] isEqual:@"pending"]) {
        NSString *token=MDRequestToken(s,r);
        if (token) [tokens addObject:token];
    }
    return tokens;
}
NSDictionary *MDRequestForToken(NSDictionary *s, NSString *token, NSTimeInterval now) {
    for (NSDictionary *r in MDLiveRequests(s, now))
        if ([r[@"state"] isEqual:@"pending"] && [MDRequestToken(s, r) isEqual:token]) return r;
    return nil;
}
uint32_t MDApprovalAllowed(NSDictionary *s, NSDictionary *r) {
    return [r[@"requested"] unsignedIntValue] & [s[@"supported"] unsignedIntValue] & MD_HOST_ACCESS_CAPS;
}
uint32_t MDApprovalDefault(NSDictionary *s, NSDictionary *r) { return MDApprovalAllowed(s, r) & MD_CAP_VIDEO; }
static const uint32_t Bits[] = {MD_CAP_VIDEO, MD_CAP_AGENT, MD_CAP_INPUT, MD_CAP_MONITOR, MD_CAP_CLIPBOARD};
NSString *MDAccessCaps(uint32_t caps) {
    NSArray *names = @[@"View screen", @"Read accessibility data", @"Control keyboard and pointer", @"Monitor selection", @"Clipboard"];
    NSMutableArray *text = [NSMutableArray array];
    for (NSUInteger i = 0; i < names.count; ++i) if (caps & Bits[i]) [text addObject:names[i]];
    if (caps & ~MD_HOST_ACCESS_CAPS) [text addObject:[NSString stringWithFormat:@"Unknown (0x%x)", caps & ~MD_HOST_ACCESS_CAPS]];
    return text.count ? [text componentsJoinedByString:@", "] : @"None";
}
BOOL MDPolicyCaps(NSString *text, uint32_t *out) {
    if ([text isEqual:@"none"]) { *out = 0; return YES; }
    NSArray *canonical = @[@"video", @"agent", @"input", @"monitor", @"clipboard"];
    NSUInteger next = 0; uint32_t bits = 0;
    for (NSString *token in [text componentsSeparatedByString:@","]) {
        NSUInteger index = [canonical indexOfObject:token];
        if (index == NSNotFound || index < next) return NO;
        bits |= Bits[index]; next = index + 1;
    }
    if (!bits) return NO;
    *out = bits; return YES;
}
static NSMutableDictionary *Mutation(NSDictionary *s, NSString *method) {
    if (![s[@"ok"] boolValue] || !s[@"generation"] || !s[@"revision"]) return nil;
    return [@{@"v":@1, @"method":method, @"generation":s[@"generation"], @"revision":s[@"revision"]} mutableCopy];
}
NSDictionary *MDApprovalCommand(NSDictionary *s, NSDictionary *r, NSString *d, uint32_t caps, BOOL remember, NSTimeInterval now) {
    if (!r || ![MDRequestForToken(s, MDRequestToken(s, r), now) isEqual:r]) return nil;
    BOOL approve = [d isEqual:@"approve"];
    if ((!approve && ![d isEqual:@"deny"]) || (approve && (!caps || (caps & ~MDApprovalAllowed(s, r))))) return nil;
    NSMutableDictionary *j = Mutation(s, @"requests.decide");
    j[@"decision"] = d; j[@"request_id"] = r[@"request_id"];
    if (approve) { j[@"caps"] = @(caps); j[@"remember"] = @(remember); }
    return j;
}
NSDictionary *MDPolicyCommand(NSDictionary *s, NSDictionary *p, NSDictionary *c, uint32_t caps, BOOL remove, NSString *sessionID) {
    if (!c || ![p[@"policy_available"] boolValue] || ![p[@"generation"] isEqual:s[@"generation"]] ||
        ![p[@"clients"] containsObject:c] || (caps & ~MD_HOST_ACCESS_CAPS)) return nil;
    uint32_t old;
    if (!remove && ([c[@"review_required"] boolValue] || !MDPolicyCaps(c[@"caps"], &old))) return nil;
    NSDictionary *session = Dict(s[@"session"]) ? s[@"session"] : nil;
    if (sessionID && (!remove || ![session[@"session_id"] isEqual:sessionID] || ![session[@"peer"] isEqual:c[@"peer"]])) return nil;
    NSMutableDictionary *j = Mutation(s, remove ? @"access.remove" : @"access.set");
    j[@"peer"] = c[@"peer"]; j[@"policy_version"] = p[@"policy_version"];
    if (!remove) j[@"caps"] = @(caps);
    return j;
}
NSDictionary *MDKickCommand(NSDictionary *s, NSDictionary *session) {
    if (!Dict(session) || ![s[@"session"] isEqual:session] || [session[@"revoked"] boolValue]) return nil;
    NSMutableDictionary *j = Mutation(s, @"session.kick"); j[@"session_id"] = session[@"session_id"]; return j;
}

static NSDictionary *Call(NSString *path, NSDictionary *command) {
    NSData *request = [NSJSONSerialization dataWithJSONObject:command options:0 error:NULL];
    NSMutableData *wire = [request mutableCopy]; [wire appendBytes:"" length:1];
    cJSON *j = wire ? cJSON_Parse(wire.bytes) : NULL;
    cJSON *reply = j ? md_management_call(path.fileSystemRepresentation, j) : NULL;
    cJSON_Delete(j);
    char *text = reply ? cJSON_PrintUnformatted(reply) : NULL;
    cJSON_Delete(reply);
    NSDictionary *result = text ? MDParseHostFrame([NSData dataWithBytes:text length:strlen(text)]) : nil;
    cJSON_free(text); return result;
}
static NSDictionary *Query(NSString *method) { return @{@"v":@1, @"method":method}; }
static uint64_t Rev(NSDictionary *j) { return [j[@"revision"] unsignedLongLongValue]; }
static NSString *Failure(NSString *error) {
    if ([error isEqual:@"conflict"]) return @"Host or policy changed. Review current state before trying again.";
    if ([error isEqual:@"cancelled_or_expired_after_publish"]) return @"Request cancelled or expired after publication. Refresh private policy; the remembered grant may already be durable.";
    if ([error isEqual:@"publish_failed"]) return @"Publication was not confirmed. Refresh policy before retrying; a relay may already have the edit.";
    if ([error isEqual:@"policy_unavailable"]) return @"Private policy is unavailable.";
    if ([error isEqual:@"deny_delivery_failed"]) return @"Request was closed, but delivery of the refusal was not confirmed.";
    return @"The host did not complete the operation. Review current state before trying again.";
}

@implementation MDHostLink {
    dispatch_queue_t _queue;
    dispatch_group_t _drain;
    dispatch_source_t _source, _initial;
    void (^_update)(NSString *, NSDictionary *, NSDictionary *, NSDictionary *, BOOL);
    int _fd;
    BOOL _closed, _busy;
    atomic_uint_fast64_t _requestedEpoch;
    uint64_t _activeEpoch;
    NSString *_path, *_status;
    NSDictionary *_snapshot, *_policy, *_permissions;
    NSMutableData *_line;
}
- (instancetype)initWithUpdate:(void (^)(NSString *, NSDictionary *, NSDictionary *, NSDictionary *, BOOL))update {
    if ((self = [super init])) {
        _queue = dispatch_queue_create("metadesk.manager.host-ipc", DISPATCH_QUEUE_SERIAL);
        _drain = dispatch_group_create(); _fd = -1; _line = [NSMutableData data]; _update = [update copy];
        atomic_init(&_requestedEpoch,0);
    }
    return self;
}
- (void)emit {
    NSString *status = _status; NSDictionary *s = _snapshot, *p = _policy, *perms = _permissions; BOOL busy = _busy;
    void (^update)(NSString *, NSDictionary *, NSDictionary *, NSDictionary *, BOOL) = _update;
    uint64_t epoch=_activeEpoch;
    if (!_closed && update) dispatch_async(dispatch_get_main_queue(), ^{
        if (epoch == atomic_load(&self->_requestedEpoch)) update(status, s, p, perms, busy);
    });
}
- (void)stop {
    if (_initial) { dispatch_source_cancel(_initial); _initial = nil; }
    if (_source) { dispatch_source_cancel(_source); _source = nil; }
    else if (_fd >= 0) close(_fd);
    _fd = -1; _snapshot = nil; _policy = nil; _permissions = nil; _busy = NO; _line.length = 0;
}
- (void)fail:(NSString *)message { [self stop]; _status = message; [self emit]; }
- (BOOL)apply:(NSDictionary *)j {
    if (!j || (_snapshot && ![j[@"generation"] isEqual:_snapshot[@"generation"]])) {
        [self fail:@"Host reply invalid or host restarted. Reconnect before making decisions."]; return NO;
    }
    if (!_snapshot || Rev(j) >= Rev(_snapshot)) {
        NSMutableDictionary *s = [j mutableCopy];
        /* Command success is not the availability of the snapshot it carries. */
        s[@"ok"] = @YES; _snapshot = s;
    }
    return YES;
}
- (void)fetchPolicy {
    NSDictionary *p = Call(_path, Query(@"access.list"));
    if (p && ![p[@"generation"] isEqual:_snapshot[@"generation"]]) {
        [self fail:@"Host restarted. Reconnect before making decisions."]; return;
    }
    if (p && Rev(p) >= Rev(_snapshot) && ![self apply:p]) return;
    _policy = p[@"clients"] && [p[@"ok"] boolValue] ? p : nil;
}
- (BOOL)accept:(NSDictionary *)j {
    if (![j[@"ok"] boolValue]) { [self fail:@"Invalid or refused subscription snapshot."]; return NO; }
    BOOL first = !_snapshot;
    if (!first && ![j[@"generation"] isEqual:_snapshot[@"generation"]]) {
        [self fail:@"Host generation changed. Reconnect to review the new host."]; return NO;
    }
    if (!first && Rev(j) > Rev(_snapshot) + 1) {
        _busy = YES; _policy = nil; _status = @"Reconciling a host revision gap…"; [self emit];
        NSDictionary *fresh = Call(_path, Query(@"management.snapshot"));
        if (![fresh[@"ok"] boolValue] || ![fresh[@"generation"] isEqual:j[@"generation"]] || Rev(fresh) < Rev(j)) {
            [self fail:@"Could not reconcile host revisions. Reconnect before making decisions."]; return NO;
        }
        j = fresh;
    }
    if (![self apply:j]) return NO;
    if (_initial) { dispatch_source_cancel(_initial); _initial = nil; }
    if (first || _busy) [self fetchPolicy];
    if (_fd < 0) return NO;
    _busy = NO; _status = @"Connected to the local host"; return YES;
}
- (void)readReady {
    BOOL changed = NO;
    for (unsigned turn = 0; turn < 8 && _fd >= 0; ++turn) {
        char buffer[1024]; ssize_t n = recv(_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) { [self fail:@"Host disconnected — no approval authority is available."]; return; }
        for (ssize_t i = 0; i < n; ++i) {
            char c = buffer[i];
            if (!c || (c != '\n' && _line.length >= 8192)) { [self fail:@"Invalid or oversized host management frame."]; return; }
            if (c != '\n') { [_line appendBytes:&c length:1]; continue; }
            NSDictionary *j = MDParseHostFrame(_line); _line.length = 0;
            if (![self accept:j]) return;
            changed = YES;
        }
    }
    if (changed) [self emit];
}
- (void)connect:(NSString *)path {
    uint64_t epoch=atomic_fetch_add(&_requestedEpoch,1)+1;
    dispatch_async(_queue, ^{ @autoreleasepool {
        if (self->_closed || epoch != atomic_load(&self->_requestedEpoch)) return;
        [self stop]; self->_path = path; self->_activeEpoch=epoch;
        self->_status = @"Connecting to the local host…"; [self emit];
        int fd = path.isAbsolutePath ? md_management_connect(path.fileSystemRepresentation) : -1;
        if (fd < 0) { [self fail:@"Host unreachable or socket not trusted. Agent process state is unknown."]; return; }
        self->_fd = fd;
        static const char request[] = "{\"v\":1,\"method\":\"management.subscribe\"}\n";
        int noPipe = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noPipe, sizeof(noPipe))) {
            [self fail:@"Could not prepare the verified host socket."]; return;
        }
        uint64_t deadline = md_now_ms() + 5000;
        size_t sent = 0;
        while (sent < sizeof(request)-1) {
            ssize_t n = send(fd, request + sent, sizeof(request)-1-sent, MSG_DONTWAIT);
            if (n > 0) { sent += (size_t)n; continue; }
            uint64_t now = md_now_ms();
            struct pollfd p = {.fd=fd,.events=POLLOUT};
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
                now < deadline && poll(&p, 1, (int)(deadline-now)) > 0) continue;
            [self fail:@"Could not subscribe to the host."]; return;
        }
        self->_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, self->_queue);
        if (!self->_source) { [self fail:@"Could not watch the verified host socket."]; return; }
        dispatch_group_t drain = self->_drain;
        dispatch_group_enter(drain);
        dispatch_source_set_cancel_handler(self->_source, ^{ close(fd); dispatch_group_leave(drain); });
        dispatch_source_set_event_handler(self->_source, ^{ @autoreleasepool { if (self->_fd == fd) [self readReady]; } });
        dispatch_activate(self->_source);
        self->_initial = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self->_queue);
        dispatch_source_set_timer(self->_initial, dispatch_time(DISPATCH_TIME_NOW, 5000000000LL), DISPATCH_TIME_FOREVER, 0);
        dispatch_source_set_event_handler(self->_initial, ^{ if (!self->_snapshot) [self fail:@"The host did not supply an initial subscription snapshot."]; });
        dispatch_activate(self->_initial);
    } });
}
- (void)command:(NSDictionary *)command { [self command:command policyVersion:nil]; }
- (void)command:(NSDictionary *)command policyVersion:(NSNumber *)expected {
    if (!command) return;
    dispatch_async(_queue, ^{ @autoreleasepool {
        if (self->_closed || !self->_snapshot || self->_busy || self->_activeEpoch != atomic_load(&self->_requestedEpoch)) return;
        NSString *method = command[@"method"];
        BOOL list = [method isEqual:@"access.list"], permissions = [method isEqual:@"host.permissions"];
        BOOL valid = list || permissions || ([command[@"generation"] isEqual:self->_snapshot[@"generation"]] && Rev(command) == Rev(self->_snapshot));
        if (valid && [method isEqual:@"requests.decide"]) {
            NSString *token = [NSString stringWithFormat:@"%@/%@", command[@"generation"], command[@"request_id"]];
            NSTimeInterval now = NSDate.date.timeIntervalSince1970;
            NSDictionary *request = MDRequestForToken(self->_snapshot, token, now);
            valid = [command isEqual:MDApprovalCommand(self->_snapshot, request, command[@"decision"], [command[@"caps"] unsignedIntValue], [command[@"remember"] boolValue], now)] &&
                (![command[@"remember"] boolValue] || ([self->_policy[@"policy_available"] boolValue] &&
                    [expected isEqual:self->_policy[@"policy_version"]]));
        }
        if (valid && [method hasPrefix:@"access."] && !list) {
            NSDictionary *client = nil;
            for (NSDictionary *c in self->_policy[@"clients"]) if ([c[@"peer"] isEqual:command[@"peer"]]) { client=c; break; }
            valid = [command isEqual:MDPolicyCommand(self->_snapshot,self->_policy,client,[command[@"caps"] unsignedIntValue],
                [method isEqual:@"access.remove"],nil)];
        }
        if (valid && [method isEqual:@"session.kick"]) valid = [command isEqual:MDKickCommand(self->_snapshot,self->_snapshot[@"session"])];
        if (![@[@"access.list",@"access.set",@"access.remove",@"host.permissions",@"requests.decide",@"session.kick"] containsObject:method ?: NSNull.null]) valid=NO;
        if (!valid) { self->_status = Failure(@"conflict"); [self emit]; return; }
        self->_busy = YES; [self emit];
        if (list) {
            [self fetchPolicy]; self->_busy = NO; if (self->_fd >= 0) { self->_status = self->_policy ? @"Private policy refreshed" : @"Private policy unavailable"; [self emit]; }
            return;
        }
        NSDictionary *reply = Call(self->_path, command);
        self->_busy = NO;
        if (permissions) {
            if (reply && ![reply[@"generation"] isEqual:self->_snapshot[@"generation"]]) {
                [self fail:@"Host restarted. Reconnect before reviewing permissions."]; return;
            }
            NSDictionary *p = Dict(reply[@"permissions"]) ? reply[@"permissions"] : nil;
            self->_permissions = [reply[@"ok"] boolValue] && Bool(p[@"screen_recording"]) && Bool(p[@"accessibility"]) ? p : nil;
            [self emit]; return;
        }
        if (!reply) { [self fail:@"Host reply unavailable. An edit may already be durable; reconnect and review before retrying."]; return; }
        if (![self apply:reply]) return;
        NSString *outcome = [reply[@"ok"] boolValue] ? @"Host acknowledged the operation" : Failure(reply[@"error"]);
        self->_policy = nil;
        [self fetchPolicy];
        if (self->_fd >= 0) { self->_status = outcome; [self emit]; }
    } });
}
- (void)close:(dispatch_block_t)drained {
    atomic_fetch_add(&_requestedEpoch,1);
    dispatch_async(_queue, ^{
        self->_closed = YES; [self stop];
        if (drained) dispatch_group_notify(self->_drain, dispatch_get_main_queue(), drained);
    });
}
@end