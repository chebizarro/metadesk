#import "hosts_launch.h"
#include "md_test.h"
#include "nostr.h"
#include "session.h"
#include <nostr/nip19/nip19.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { MD_CHECK(x); if (md_test_failures) return md_test_report(); } while (0)
static NSString *const A = @"79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static NSString *const B = @"f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";
static NSString *const C = @"e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13";
static NSString *const Name = @"desk@example.com";

static int Unit(void) {
    CHECK(MDClassifyHostInput(A) == MDHostPublic);
    CHECK(MDClassifyHostInput([@"nostr:" stringByAppendingString:MDHostNpub(A)]) == MDHostPublic);
    CHECK(MDClassifyHostInput(@"nprofile1fixture") == MDHostPublic);
    CHECK(MDClassifyHostInput(@"desk@example.com") == MDHostName);
    CHECK(MDClassifyHostInput(@"\u00a0 NOSTR: NsEc1never-copy-me") == MDHostSecret);
    CHECK(MDClassifyHostInput(@" ncryptsec1never-copy-me") == MDHostSecret);
    CHECK(MDClassifyHostInput(@"127.0.0.1") == MDHostInvalid);
    CHECK(MDClassifyHostInput(@"::1") == MDHostInvalid);
    CHECK(MDClassifyHostInput(@"note1fixture") == MDHostInvalid);
    CHECK(MDHostRouteValid(@"127.0.0.1") && MDHostRouteValid(@"::1") && !MDHostRouteValid(@"localhost"));
    CHECK([MDHostFingerprint(A) isEqual:@"79be 667e f9dc bbac"]);
    CHECK([MDHostBoundText(@"ab\u202ecd", 4) isEqual:@"ab\ufffdc…"]);
    CHECK([MDHostBoundText(@"🙂", 1) isEqual:@"…"]);
    CHECK([MDHostLaunchText(MD_LAUNCH_FAILED, MD_LAUNCH_SIGNER) containsString:@"signer"]);
    CHECK([MDHostLaunchText(MD_LAUNCH_APPROVAL, 0) containsString:@"approval"]);
    CHECK(MDHostLaunchOutcome(MD_LAUNCH_FAILED, MD_LAUNCH_BIND) == MD_RECENT_BIND);
    CHECK([MDHostLaunchDiagnostic(MD_LAUNCH_RELAY_AUTH) containsString:@"authentication"]);
    CHECK([MDHostLaunchDiagnostic(MD_LAUNCH_RELAY_CLOSED) containsString:@"closed"]);
    CHECK([MDHostLaunchDiagnostic(MD_LAUNCH_RELAY_REJECTED) containsString:@"rejected"]);
    CHECK(!MDHostClientPath(@"relative/manager", nil, @":.:relative"));
    return md_test_report();
}

static int Bootstrap(NSString *file) {
    NSString *error = nil;
    NSFileManager *fm = NSFileManager.defaultManager;
    CHECK([MDHostsBootstrap(nil, &error) isEqual:@""] && !error);
    CHECK(![fm fileExistsAtPath:file]);
    CHECK([MDHostsBootstrap(@"WSS://Relay.Example:443/a%2Fb wss://relay.example/a%2Fb\nws://localhost", &error)
          isEqual:@"wss://relay.example/a%2Fb ws://localhost/"]);
    CHECK([MDHostsBootstrap(nil, &error) isEqual:@"wss://relay.example/a%2Fb ws://localhost/"] && !error);
    CHECK([@"[relays]\nbootstrap=wss://relay.example/a%2Fb;ws://localhost/;\n" isEqual:
          [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:NULL]]);
    struct stat st;
    CHECK(!stat(file.fileSystemRepresentation, &st) && (st.st_mode & 0777) == 0600);
    CHECK(!MDHostsBootstrap(@"ws://user:secret@localhost", &error) && error.length);
    CHECK([MDHostsBootstrap(nil, NULL) isEqual:@"wss://relay.example/a%2Fb ws://localhost/"]);
    CHECK([MDHostsBootstrap(@"", NULL) isEqual:@""] && [MDHostsBootstrap(nil, NULL) isEqual:@""]);
    CHECK([@"[relays]\nbootstrap=https://invalid/;\n" writeToFile:file atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    error = nil;
    CHECK(!MDHostsBootstrap(nil, &error) && error.length);
    return md_test_report();
}

static MDHostReview *Resolved(MDHostsBook *book, NSString *key, NSString *name) {
    MdIdentity identity = {.hint_source = name ? MD_HINT_NIP05 : MD_HINT_NONE};
    strlcpy(identity.pubkey, key.UTF8String, sizeof(identity.pubkey));
    return [book reviewIdentity:&identity alias:name route:@"" error:NULL];
}
static MDHostsBook *Await(MDHostsWorker *worker, dispatch_semaphore_t changed, BOOL (^ready)(MDHostsBook *)) {
    dispatch_time_t limit = dispatch_time(DISPATCH_TIME_NOW, 8000000000LL);
    for (;;) {
        @autoreleasepool {
            MDHostsBook *book = [worker snapshot];
            if (ready(book)) return book;
        }
        if (dispatch_semaphore_wait(changed, limit)) return nil;
    }
}
static NSDictionary *Row(MDHostsBook *book, NSString *key) {
    for (NSDictionary *row in book.rows) if ([row[@"key"] isEqual:key]) return row;
    return nil;
}
static BOOL Signal(NSString *message) {
    puts(message.UTF8String); fflush(stdout);
    int c = getchar(); return c == '\n';
}
static BOOL SendRemote(MdSigner *signer, NSString *owner, int version, time_t advance, NSString *label) {
    NSString *document = [NSString stringWithFormat:@"{\"schema_version\":%d,\"hosts\":{\"%@\":{\"label\":\"%@\"}}}", version, C, label];
    NostrEvent *event = md_private_document_build(signer, NULL, owner.UTF8String, 30078,
        MD_STORE_MANAGEMENT, document.UTF8String, time(NULL) + advance);
    char *wire = NULL;
    BOOL ok = event && !md_nostr_sign_event_json(signer, event, &wire);
    nostr_event_free(event);
    if (ok) ok = Signal([@"event " stringByAppendingString:[NSString stringWithUTF8String:wire]]);
    free(wire);
    return ok;
}

static NSString *ProfileWith(NSString *key, char *hint) {
    NostrProfilePointer ptr = {.public_key = (char *)key.UTF8String, .relays = &hint, .relays_count = 1};
    char *encoded = NULL;
    if (nostr_nip19_encode_nprofile(&ptr, &encoded)) return nil;
    NSString *value = @(encoded);
    free(encoded);
    return value;
}
static int Live(MDHostsWorker *worker, MdSigner *signer, NSString *directory, NSString *relay, NSString *unreachable, dispatch_semaphore_t changed) {
    NSString *error = nil;
    MdManagedLaunch launch;
    CHECK([worker snapshot].state == MDHostsNoRelays && ![worker snapshot].canReviewKey);
    CHECK(![worker resolve:A route:@"" cancelled:NULL data:NULL error:&error] && [error containsString:@"choose relays"]);
    CHECK(![worker resolve:ProfileWith(A, "wss://93.184.216.34/") route:@"" cancelled:NULL data:NULL error:&error] &&
        [error containsString:@"choose relays"]);
    [worker startRelays:unreachable];
    CHECK(![worker snapshot].canReview);
    MDHostReview *offline = [worker resolve:MDHostNpub(A) route:@"" cancelled:NULL data:NULL error:&error];
    CHECK(offline && [worker launchConfig:&launch review:offline error:&error] && launch.relay_count == 1);
    CHECK(![worker resolve:Name route:@"" cancelled:NULL data:NULL error:&error] && [error containsString:@"Wait for your host list"]);
    CHECK(Signal(@"offline-review-ready"));
    CHECK(([worker startRelays:[NSString stringWithFormat:@"%@ %@", relay, unreachable]]));
    CHECK(Signal(@"review-bridge-started"));
    CHECK(![worker snapshot].canReview);
    MDHostReview *early = [worker resolve:A route:@"" cancelled:NULL data:NULL error:&error];
    CHECK(early && [worker launchConfig:&launch review:early error:&error] && [worker saveReview:early confirmation:nil].length);
    CHECK(![worker resolve:Name route:@"" cancelled:NULL data:NULL error:&error] && [error containsString:@"Wait for your host list"]);
    CHECK(Signal(@"review-before-eose"));
    MDHostsBook *book = Await(worker, changed, ^BOOL(MDHostsBook *b) { return b.canReview; });
    CHECK(book && !book.canEdit);
    MDHostReview *review = [worker resolve:A route:@"127.0.0.1" cancelled:NULL data:NULL error:NULL];
    CHECK([worker launchConfig:&launch review:review error:NULL]);
    CHECK(launch.relay_count == 2 && !strcmp(launch.pin, A.UTF8String) && !strcmp(launch.route, "127.0.0.1") && launch.capabilities == MD_CAP_VIDEO);
    CHECK(Signal(@"review-history-ready"));
    CHECK([worker startRelays:relay]);
    CHECK(Signal(@"edit-bridge-started"));
    book = Await(worker, changed, ^BOOL(MDHostsBook *b) { return b.canEdit && [b.status isEqual:@"Address book synced."]; });
    CHECK(book && book.rows.count == 0);
    CHECK(Signal(@"edit-history-ready"));
    review = Resolved(book, A, Name);
    CHECK(review && review.alias && !review.oldKey && review.npub);
    CHECK([worker saveReview:review confirmation:nil].length == 0);
    book = [worker snapshot];
    CHECK(Row(book, A) && [[book addressForKey:A] isEqual:Name]);
    CHECK(!Resolved(book, A, nil)); /* A raw key cannot bypass its saved NIP-05 review. */
    MDHostReview *stale = Resolved(book, C, nil);
    CHECK(stale);
    CHECK([worker editKey:A book:book label:@"Alpha" notes:@"Encrypted \u202ememo" favorite:NO tagIDs:[NSSet set] newTag:@"" remove:NO].length == 0);
    CHECK(![worker launchConfig:&launch review:stale error:NULL]);
    book = [worker snapshot];
    CHECK([Row(book, A)[@"notes"] isEqual:@"Encrypted \ufffdmemo"]);
    CHECK([worker editKey:A book:book label:Row(book, A)[@"label"] notes:Row(book, A)[@"notes"] favorite:YES tagIDs:[NSSet set] newTag:@"Work" remove:NO].length == 0);
    book = [worker snapshot];
    NSDictionary *row = Row(book, A);
    CHECK([row[@"favorite"] boolValue] && [row[@"tags"] isEqual:@[@"Work"]]);
    CHECK([book rowsMatching:@"MEMO" tag:[row[@"tagIDs"] firstObject]].count == 1);
    CHECK([book rowsMatching:@"unmatched" tag:nil].count == 0);
    review = Resolved(book, B, Name);
    CHECK([review.oldKey isEqual:A] && [book canReplaceReview:review]);
    CHECK(![worker launchConfig:&launch review:review error:NULL]);
    CHECK(![book canConfirmReview:review fingerprint:@"mistyped"]);
    CHECK([worker saveReview:review confirmation:@"mistyped"].length > 0);
    CHECK(Row([worker snapshot], A) && !Row([worker snapshot], B));
    CHECK([worker saveReview:review confirmation:review.fingerprint].length == 0);
    book = [worker snapshot]; row = Row(book, B);
    CHECK(row && !Row(book, A) && [row[@"label"] isEqual:@"Alpha"] && [row[@"notes"] isEqual:@"Encrypted \ufffdmemo"]);
    CHECK([row[@"favorite"] boolValue] && [row[@"tags"] isEqual:@[@"Work"]]);

    NSString *importPath = [directory stringByAppendingPathComponent:@"import.json"];
    NSURL *file = [NSURL fileURLWithPath:importPath];
    NSString *input = [NSString stringWithFormat:@"{\"schema\":\"metadesk-hosts/1\",\"hosts\":[{\"pubkey\":\"%@\"},{\"pubkey\":\"%@\",\"label\":\"Imported\",\"requested_caps\":[\"video\",\"input\"]}]}", B, C];
    CHECK([input writeToURL:file atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    NSArray *import = [worker readImport:file book:book];
    CHECK(import.count == 1 && [import[0][@"npub"] isEqual:MDHostNpub(C)] && [import[0][@"caps"] containsString:@"input"]);
    CHECK([worker applyImport:import selected:[NSSet setWithObject:C] book:book].length == 0);
    book = [worker snapshot];
    CHECK(Resolved(book, C, nil).caps == (MD_CAP_INPUT | MD_CAP_VIDEO));
    NSString *outPath = [directory stringByAppendingPathComponent:@"export.json"];
    CHECK([[worker exportBook:book to:[NSURL fileURLWithPath:outPath]] containsString:@"exported"]);
    struct stat st; CHECK(!stat(outPath.fileSystemRepresentation, &st) && (st.st_mode & 0777) == 0600);
    CHECK([[NSString stringWithContentsOfFile:outPath encoding:NSUTF8StringEncoding error:NULL] containsString:@"Encrypted \u202ememo"]);
    [worker recordKey:C owner:book.owner outcome:MD_RECENT_CONNECTED];
    book = [worker snapshot];
    CHECK([[book.quick valueForKey:@"key"] isEqual:(@[B, C])] && [Row(book, C)[@"lastConnected"] longLongValue] > 0);

    CHECK(Signal(@"reject-hosts"));
    NSSet *tags = [NSSet setWithArray:Row(book, B)[@"tagIDs"]];
    CHECK([[worker editKey:B book:book label:@"" notes:@"" favorite:NO tagIDs:tags newTag:@"" remove:YES] containsString:@"pending"]);
    book = [worker snapshot];
    CHECK(!Row(book, B) && !Resolved(book, B, nil) && !Resolved(book, B, Name));
    CHECK([worker retryIssue:@MD_STORE_HOSTS book:book].length == 0);
    book = [worker snapshot];
    CHECK(!Row(book, B) && Row(book, C));

    CHECK(Signal(@"reject-meta"));
    CHECK([[worker editKey:C book:book label:@"Local draft" notes:@"" favorite:NO tagIDs:[NSSet set] newTag:@"" remove:NO] containsString:@"pending"]);
    book = [worker snapshot];
    CHECK(SendRemote(signer, book.owner, 1, 20, @"Remote winner"));
    book = Await(worker, changed, ^BOOL(MDHostsBook *b) { return [b conflictText:@MD_STORE_MANAGEMENT] != nil; });
    CHECK(book && !book.canReview && !book.canEdit && [[book conflictText:@MD_STORE_MANAGEMENT] containsString:@"Local draft"]);
    CHECK([worker resolveIssue:@MD_STORE_MANAGEMENT book:book reapply:YES].length == 0);
    book = [worker snapshot];
    CHECK([Row(book, C)[@"label"] isEqual:@"Local draft"]);
    CHECK(Signal(@"reject-meta"));
    CHECK([[worker editKey:C book:book label:@"Second draft" notes:@"" favorite:NO tagIDs:[NSSet set] newTag:@"" remove:NO] containsString:@"pending"]);
    CHECK(SendRemote(signer, book.owner, 1, 30, @"Keep this remote winner"));
    book = Await(worker, changed, ^BOOL(MDHostsBook *b) { return [b conflictText:@MD_STORE_MANAGEMENT] != nil; });
    CHECK(book && [worker resolveIssue:@MD_STORE_MANAGEMENT book:book reapply:NO].length == 0);
    CHECK([Row([worker snapshot], C)[@"label"] isEqual:@"Keep this remote winner"]);
    CHECK([worker startRelays:@""]);
    book = [worker snapshot];
    CHECK(Row(book, C) && !book.canReview && !book.canEdit && [book.status containsString:@"Offline"]);
    CHECK([worker startRelays:relay]);
    CHECK(Signal(@"reload-bridge-started"));
    book = Await(worker, changed, ^BOOL(MDHostsBook *b) { return b.canEdit; });
    CHECK(book && SendRemote(signer, book.owner, 2, 40, @"Unknown schema"));
    book = Await(worker, changed, ^BOOL(MDHostsBook *b) { return [b.status containsString:@"read-only"]; });
    CHECK(book && !book.canReviewKey && !book.canReview && !book.canEdit && !book.canExport);
    /* An absent pin in unreadable metadata is not an unseen key. */
    CHECK(![worker resolve:A route:@"" cancelled:NULL data:NULL error:&error] && [error containsString:@"read-only"]);
    CHECK(![worker resolve:Name route:@"" cancelled:NULL data:NULL error:&error] && [error containsString:@"read-only"]);
    return md_test_report();
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 1) return Unit();
        if (argc == 3 && !strcmp(argv[1], "--bootstrap")) return Bootstrap(@(argv[2]));
        CHECK(argc == 4);
        NSString *directory = [NSString stringWithUTF8String:argv[1]];
        MdAccounts *accounts = NULL;
        MdSigner *signer = md_signer_create_direct("0000000000000000000000000000000000000000000000000000000000000002");
        char *owner = NULL;
        CHECK(signer && !md_signer_get_pubkey(signer, &owner));
        CHECK(!md_accounts_open(directory.fileSystemRepresentation, NULL, &accounts));
        CHECK(!md_accounts_add(accounts, MD_SIGNER_DIRECT_KEY, owner, "Fixture only"));
        CHECK(!md_accounts_bind_role(accounts, MD_ACCOUNT_CLIENT, MD_SIGNER_DIRECT_KEY, owner, signer));
        MdAccountBinding *binding = md_accounts_acquire_role(accounts, MD_ACCOUNT_CLIENT);
        dispatch_semaphore_t changed = dispatch_semaphore_create(0);
        MDHostsWorker *worker = [[MDHostsWorker alloc] initWithChange:^{ dispatch_semaphore_signal(changed); }];
        int status = 1;
        MD_CHECK([worker openBinding:binding directory:directory]);
        if (!md_test_failures) status = Live(worker, signer, directory, [NSString stringWithUTF8String:argv[2]], [NSString stringWithUTF8String:argv[3]], changed);
        [worker close]; md_account_binding_release(binding); md_accounts_destroy(accounts);
        md_signer_destroy(signer); free(owner); md_nostr_shutdown();
        return status;
    }
}
