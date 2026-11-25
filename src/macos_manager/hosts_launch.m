#import "hosts_launch.h"
#include "session.h"
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

NSString *MDHostCapsText(uint8_t caps) {
    if ((caps & (MD_CAP_AGENT | MD_CAP_INPUT)) == (MD_CAP_AGENT | MD_CAP_INPUT)) return @"Video, accessibility and input";
    if (caps & MD_CAP_AGENT) return @"Video and accessibility";
    if (caps & MD_CAP_INPUT) return @"Video and input";
    return @"Video (view only)";
}
NSString *MDHostLaunchText(MdLaunchMessage m, MdLaunchMessage phase) {
    static NSString *const progress[] = {@"Opening video client…", @"Checking host identity…", @"Connecting to signer…", @"Connecting to relays…",
        @"Checking FIPS route…", @"Waiting for approval…", @"Opening stream…", @"Binding session…", @"Opening video…", @"Connected in video window"};
    static NSString *const failed[] = {@"Could not start metadesk-client. Check the app bundle or PATH.", @"Host identity could not be verified", @"The client signer is unavailable",
        @"The signaling relays could not be reached", @"The FIPS route is unavailable", @"No approval received or request declined", @"The stream could not be opened",
        @"The host did not confirm the session", @"Video could not be opened"};
    if ((unsigned)m < sizeof(progress)/sizeof(*progress)) return progress[m];
    if (m == MD_LAUNCH_FAILED) return (unsigned)phase < sizeof(failed)/sizeof(*failed) ? failed[phase] : @"Connection failed";
    if (m == MD_LAUNCH_CANCELLED) return @"Connection cancelled";
    if (m == MD_LAUNCH_ENDED) return @"Video session ended";
    return @"";
}
NSString *MDHostLaunchDiagnostic(MdLaunchMessage m) {
    switch (m) {
        case MD_LAUNCH_RELAY_AUTH: return @"A signaling relay requires authentication.";
        case MD_LAUNCH_RELAY_CLOSED: return @"A signaling relay closed the subscription.";
        case MD_LAUNCH_RELAY_REJECTED: return @"A signaling relay rejected an event.";
        default: return @"";
    }
}
MdRecentOutcome MDHostLaunchOutcome(MdLaunchMessage m, MdLaunchMessage phase) {
    static const MdRecentOutcome outcomes[] = {MD_RECENT_RESOLVE, MD_RECENT_RESOLVE, MD_RECENT_SIGNER,
        MD_RECENT_RELAY, MD_RECENT_FIPS, MD_RECENT_NEGOTIATION, MD_RECENT_FIPS, MD_RECENT_BIND, MD_RECENT_MEDIA};
    return m == MD_LAUNCH_CONNECTED ? MD_RECENT_CONNECTED : (unsigned)phase < sizeof(outcomes)/sizeof(*outcomes) ?
        outcomes[phase] : MD_RECENT_RESOLVE;
}
static NSString *ClientIn(NSString *directory) {
    if (!directory.isAbsolutePath) return nil;
    NSString *candidate = [directory stringByAppendingPathComponent:@"metadesk-client"];
    struct stat st;
    return !stat(candidate.fileSystemRepresentation, &st) && S_ISREG(st.st_mode) && !access(candidate.fileSystemRepresentation, X_OK) ? candidate : nil;
}
NSString *MDHostClientPath(NSString *manager, NSString *bundle, NSString *path) {
    NSString *program = ClientIn(manager.stringByDeletingLastPathComponent);
    if (program) return program;
    if (bundle.isAbsolutePath) for (NSString *relative in @[@"Contents/MacOS", @"Contents/Helpers"]) {
        program = ClientIn([bundle stringByAppendingPathComponent:relative]); if (program) return program;
    }
    for (NSString *directory in [path componentsSeparatedByString:@":"]) {
        program = ClientIn(directory); if (program) return program;
    }
    return nil;
}

@interface MDHostsLaunch () {
    dispatch_queue_t _queue;
    dispatch_source_t _source;
    dispatch_group_t _channelDrain;
    void (^_update)(MdLaunchMessage);
    int _fd;
    MdLaunchMessage _phase;
    unsigned _diagnostics;
    BOOL _final, _active, _cancelled, _closed;
}
@end

@implementation MDHostsLaunch
- (instancetype)initWithUpdate:(void (^)(MdLaunchMessage))update {
    if ((self = [super init])) {
        _queue = dispatch_queue_create("metadesk.macos-manager.launch", DISPATCH_QUEUE_SERIAL);
        _channelDrain = dispatch_group_create();
        _update = [update copy]; _fd = -1;
    }
    return self;
}
- (void)emit:(MdLaunchMessage)m {
    void (^update)(MdLaunchMessage) = _update;
    if (!_closed && update) dispatch_async(dispatch_get_main_queue(), ^{ update(m); });
}
- (void)disposeChannel {
    if (_fd < 0) return;
    if (_source) {
        shutdown(_fd, SHUT_RDWR);
        dispatch_source_cancel(_source); _source = nil;
    } else close(_fd);
    _fd = -1;
}
- (void)readReady {
    /* Budget work per source turn. Report monotonic phases and each relay
     * diagnostic at most once so a fast child cannot flood the main queue. */
    for (unsigned i = 0; i < 16 && _fd >= 0; ++i) {
        MdLaunchMessage m;
        int rc = md_managed_launch_message_read(_fd, &m, true);
        if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (rc <= 0 || m == MD_LAUNCH_CANCEL || m == MD_LAUNCH_DISCONNECT) {
            if (_active) [self emit:MD_LAUNCH_ENDED];
            else if (!_final) [self emit:_cancelled ? MD_LAUNCH_CANCELLED : MD_LAUNCH_FAILED];
            _active = NO; _final = YES;
            [self disposeChannel]; return;
        }
        if (m >= MD_LAUNCH_RELAY_AUTH) {
            unsigned bit = 1u << (m - MD_LAUNCH_RELAY_AUTH);
            if (!_final && !(_diagnostics & bit)) { _diagnostics |= bit; [self emit:m]; }
        } else if (m == MD_LAUNCH_ENDED) {
            if (_active) { _active = NO; [self emit:m]; [self disposeChannel]; }
        } else if (!_final && m >= MD_LAUNCH_CONNECTED) {
            _final = YES; _active = m == MD_LAUNCH_CONNECTED;
            [self emit:m]; if (!_active) [self disposeChannel];
        } else if (!_final && m > _phase) { _phase = m; [self emit:m]; }
    }
}
- (void)start:(const MdManagedLaunch *)config manager:(NSString *)manager bundle:(NSString *)bundle path:(NSString *)path {
    NSData *wire = [NSData dataWithBytes:config length:sizeof(*config)];
    dispatch_async(_queue, ^{
        if (self->_closed) return;
        NSBundle *mainBundle = NSBundle.mainBundle;
        NSString *program = MDHostClientPath(manager ?: mainBundle.executablePath, bundle ?: mainBundle.bundlePath,
            path ?: NSProcessInfo.processInfo.environment[@"PATH"]);
        pid_t pid;
        if (!program || md_managed_launch_spawn(program.fileSystemRepresentation, &self->_fd, &pid)) {
            [self emit:MD_LAUNCH_FAILED]; return;
        }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} });
        if (!md_managed_launch_write(self->_fd, wire.bytes)) {
            [self disposeChannel]; [self emit:MD_LAUNCH_FAILED]; return;
        }
        int fd = self->_fd;
        self->_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, self->_queue);
        if (!self->_source) { [self disposeChannel]; [self emit:MD_LAUNCH_FAILED]; return; }
        dispatch_source_set_event_handler(self->_source, ^{ [self readReady]; });
        dispatch_group_t drain = self->_channelDrain;
        dispatch_group_enter(drain);
        dispatch_source_set_cancel_handler(self->_source, ^{ close(fd); dispatch_group_leave(drain); });
        dispatch_activate(self->_source);
    });
}
- (void)control {
    dispatch_async(_queue, ^{
        if (self->_fd < 0) return;
        if (self->_active) md_managed_launch_message_write(self->_fd, MD_LAUNCH_DISCONNECT);
        else if (!self->_final) {
            self->_cancelled = YES;
            if (!md_managed_launch_message_write(self->_fd, MD_LAUNCH_CANCEL)) {
                [self disposeChannel]; self->_final = YES; [self emit:MD_LAUNCH_CANCELLED];
            }
        }
    });
}
- (void)close:(dispatch_block_t)completion {
    dispatch_async(_queue, ^{
        self->_closed = YES;
        if (self->_fd >= 0 && !self->_final) md_managed_launch_message_write(self->_fd, MD_LAUNCH_CANCEL);
        [self disposeChannel];
        if (completion) dispatch_group_notify(self->_channelDrain, dispatch_get_main_queue(), completion);
    });
}
@end
