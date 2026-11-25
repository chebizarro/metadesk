#import "hosts_launch.h"
#include "md_test.h"
#include "session.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEY "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define CHECK(x) do { MD_CHECK(x); if (md_test_failures) return md_test_report(); } while (0)

static int Child(void) {
    CHECK(!getenv("MD_HOSTS_FIXTURE_SECRET"));
    CHECK(md_managed_launch_prepare_stream(3));
    MdManagedLaunch c;
    CHECK(md_managed_launch_read(3, &c));
    CHECK(!strcmp(c.pin, KEY) && c.capabilities == MD_CAP_VIDEO);
    CHECK(md_managed_launch_message_write(3, MD_LAUNCH_SIGNER));
    CHECK(md_managed_launch_message_write(3, MD_LAUNCH_RELAY_AUTH));
    CHECK(md_managed_launch_message_write(3, MD_LAUNCH_RELAY_CLOSED));
    CHECK(md_managed_launch_message_write(3, MD_LAUNCH_RELAY_REJECTED));
    /* Duplicate diagnostics must not enqueue a message per byte on AppKit's queue. */
    for (unsigned i = 0; i < 10000; ++i) {
        if (!md_managed_launch_message_write(3, MD_LAUNCH_RELAY_REJECTED)) break;
    }
    MdLaunchMessage command;
    CHECK(md_managed_launch_message_read(3, &command, false) == 1 && command == MD_LAUNCH_CANCEL);
    /* SIGPIPE suppression and framing also covered by the port's C fixture. */
    CHECK(md_managed_launch_message_write(3, MD_LAUNCH_CANCELLED));
    return md_test_report();
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 3 && !strcmp(argv[1], "--managed-session-fd") && !strcmp(argv[2], "3")) return Child();
        CHECK(argc == 2);
        NSString *root = [NSString stringWithUTF8String:argv[1]];
        NSString *bundle = [root stringByAppendingPathComponent:@"Fixture.app"];
        NSString *helpers = [bundle stringByAppendingPathComponent:@"Contents/Helpers"];
        NSString *directory = [root stringByAppendingPathComponent:@"bin"];
        NSFileManager *files = NSFileManager.defaultManager;
        CHECK([files createDirectoryAtPath:helpers withIntermediateDirectories:YES attributes:nil error:NULL]);
        CHECK([files createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL]);
        char *bin = realpath(argv[0], NULL);
        CHECK(bin);
        NSString *selfPath = [NSString stringWithUTF8String:bin]; free(bin);
        NSString *manager = [directory stringByAppendingPathComponent:@"manager"];
        NSString *helperClient = [helpers stringByAppendingPathComponent:@"metadesk-client"];
        NSString *pathClient = [root stringByAppendingPathComponent:@"metadesk-client"];
        NSString *siblingClient = [directory stringByAppendingPathComponent:@"metadesk-client"];
        CHECK(!symlink(selfPath.fileSystemRepresentation, pathClient.fileSystemRepresentation));
        CHECK(!MDHostClientPath(manager, bundle, root));
        CHECK([MDHostClientPath(manager, nil, root) isEqual:pathClient]);
        CHECK([@"not executable" writeToFile:helperClient atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
        CHECK(chmod(helperClient.fileSystemRepresentation, 0600) == 0);
        CHECK(!MDHostClientPath(manager, bundle, root));
        CHECK(!unlink(helperClient.fileSystemRepresentation));
        CHECK(!symlink(selfPath.fileSystemRepresentation, helperClient.fileSystemRepresentation));
        CHECK([MDHostClientPath(manager, bundle, root) isEqual:helperClient]);
        CHECK(!symlink(selfPath.fileSystemRepresentation, siblingClient.fileSystemRepresentation));
        CHECK([MDHostClientPath(manager, bundle, root) isEqual:helperClient]);
        CHECK([MDHostClientPath(manager, nil, root) isEqual:siblingClient]);
        CHECK(!setenv("MD_HOSTS_FIXTURE_SECRET", "dummy", 1));
        __block unsigned events = 0;
        __block MDHostsLaunch *launch;
        launch = [[MDHostsLaunch alloc] initWithUpdate:^(MdLaunchMessage m) {
            MD_CHECK(NSThread.isMainThread);
            ++events;
            if (m == MD_LAUNCH_RELAY_REJECTED) [launch control];
            if (m == MD_LAUNCH_CANCELLED || m == MD_LAUNCH_FAILED) {
                MD_CHECK(m == MD_LAUNCH_CANCELLED);
                MD_CHECK(events == 5);
                [launch close:^{ launch = nil; exit(md_test_report()); }];
            }
        }];
        MdManagedLaunch config = {.address = KEY, .pin = KEY, .signer = KEY, .capabilities = MD_CAP_VIDEO, .port = 7700,
            .relays = {"ws://127.0.0.1:1"}, .relay_count = 1};
        [launch start:&config manager:manager bundle:bundle path:root];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8000000000LL), dispatch_get_main_queue(), ^{
            MD_CHECK(false); exit(md_test_report());
        });
        /* dispatch_main does not promise the callbacks run on pthread main on
         * Darwin. Run the main CFRunLoop without starting or linking AppKit. */
        CFRunLoopRun();
        return 1;
    }
}
