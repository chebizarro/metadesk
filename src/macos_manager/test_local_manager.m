#import "access_model.h"
#import "identity_model.h"
#import "hosts_model.h"
#include "md_test.h"
#include <time.h>

#define KEY "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define OTHER "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
#define GEN "abababababababababababababababab"
#define REQUEST "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
#define CHECK(x) do { MD_CHECK(x); if (md_test_failures) return md_test_report(); } while (0)

static int Decisions(void) {
    NSDictionary *r=@{@"request_id":@REQUEST,@"peer":@KEY,@"requested":@(MD_CAP_VIDEO|MD_CAP_INPUT|MD_CAP_AGENT),@"granted":@0,@"state":@"pending",@"expires_at":@100};
    NSDictionary *s=@{@"ok":@YES,@"generation":@GEN,@"revision":@5,@"requests":@[r],@"supported":@(MD_CAP_VIDEO|MD_CAP_AGENT)};
    NSString *token=MDRequestToken(s,r);
    CHECK([MDRequestForToken(s,token,99) isEqual:r]);
    CHECK(!MDRequestForToken(s,token,100));
    CHECK([MDRequestNotices(s,99) containsObject:token] && !MDRequestNotices(s,100).count);
    NSMutableDictionary *cancel=[s mutableCopy]; cancel[@"requests"]=@[];
    CHECK(!MDRequestForToken(cancel,token,99) && !MDRequestNotices(cancel,99).count);
    cancel=[s mutableCopy]; cancel[@"generation"]=@"00000000000000000000000000000000";
    CHECK(!MDRequestForToken(cancel,token,99));
    CHECK(MDApprovalDefault(s,r)==MD_CAP_VIDEO && MDApprovalAllowed(s,r)==(MD_CAP_VIDEO|MD_CAP_AGENT));
    CHECK(!MDApprovalCommand(s,r,@"approve",MD_CAP_INPUT,NO,99));
    CHECK(!MDApprovalCommand(s,r,@"approve",0,NO,99));
    CHECK(!MDApprovalCommand(s,r,@"approve",MD_CAP_VIDEO,NO,100));
    NSDictionary *approve=MDApprovalCommand(s,r,@"approve",MD_CAP_AGENT,YES,99);
    CHECK([approve[@"caps"] unsignedIntValue]==MD_CAP_AGENT && [approve[@"remember"] boolValue]);
    NSDictionary *deny=MDApprovalCommand(s,r,@"deny",0,NO,99);
    CHECK(deny && !deny[@"caps"] && !deny[@"remember"] && [deny[@"decision"] isEqual:@"deny"]);
    uint32_t bits;
    CHECK(MDPolicyCaps(@"video,input",&bits) && bits==(MD_CAP_VIDEO|MD_CAP_INPUT));
    CHECK(!MDPolicyCaps(@"input,video",&bits) && !MDPolicyCaps(@"video,unknown",&bits));
    CHECK([MDAccessCaps(1u<<30) containsString:@"Unknown"]);
    CHECK(!MDParseHostFrame([@"{\"v\":1,\"v\":1}" dataUsingEncoding:NSUTF8StringEncoding]));
    return md_test_report();
}

static int FakeKey(MdSigner *signer,char **out) { (void)signer; *out=strdup(KEY); return *out ? 0 : MD_SIGNER_ERR_INTERNAL; }
static int Connect(MdSignerType type,const char *pk,MdCredentialStore *store,MdSigner **out) {
    (void)pk; (void)store;
    static const MdSignerOps ops={.get_pubkey=FakeKey};
    *out=md_signer_create_custom(type,&ops,NULL); return *out ? 0 : MD_SIGNER_ERR_INTERNAL;
}
static BOOL Node(char out[65],void *key) { if (!key) return NO; strlcpy(out,key,65); return YES; }
static BOOL Cancel(void *unused) { (void)unused; return YES; }
static int Removed;
static int ReadPair(void *p,const char *key,char **payload) { (void)p; (void)key; *payload=NULL; return MD_CREDENTIAL_NOT_FOUND; }
static int NoWrite(void *p,const char *key,const char *payload) { (void)p; (void)key; (void)payload; MD_CHECK(false); return MD_CREDENTIAL_FAILED; }
static int RemoveOnly(void *unused,const char *key) { (void)unused; MD_CHECK(!strcmp(key,KEY)); ++Removed; return 0; }
static int Metadata(void) {
    NSString *directory = [@(getenv("HOME")) stringByAppendingPathComponent:@"meta-edits"];
    CHECK(MDIdentityAdd(directory,MD_SIGNER_DIRECT_KEY,@KEY,@"Direct").length);
    for (NSUInteger i=0; i<2; ++i) {
        MdSignerType type = i ? MD_SIGNER_NIP46 : MD_SIGNER_NIP5F;
        if (!MDIdentitySupportsBackend(type)) continue;
        CHECK(MDIdentityAdd(directory,type,@"nsec1fixture-not-a-key",@"Bad key").length);
        CHECK(MDIdentityAdd(directory,type,MDHostNpub(@KEY),@"ncryptsec1fixture-not-a-key").length);
        CHECK(!MDIdentityAdd(directory,type,MDHostNpub(@KEY),@"Office").length);
        CHECK(MDIdentityAdd(directory,type,@KEY,@"Duplicate").length);
        NSDictionary *account = MDIdentityAccounts(directory,NULL).lastObject;
        CHECK([account[@"key"] isEqual:@KEY] && [account[@"backend"] intValue] == (int)type);
        CHECK(!MDIdentityLabel(directory,account,@"Office Mac").length);
        account = MDIdentityAccounts(directory,NULL).lastObject;
        CHECK([account[@"label"] isEqual:@"Office Mac"]);
        CHECK(![account[@"host"] boolValue] && ![account[@"client"] boolValue]);
        if (type != MD_SIGNER_NIP46) continue;

        MdCredentialStore *store = md_credential_store_memory();
        CHECK(store);
        const char *secret = "0000000000000000000000000000000000000000000000000000000000000001";
        char json[512];
        size_t len = (size_t)snprintf(json,sizeof(json),"{\"version\":1,\"user_pubkey\":\"%s\",\"remote_pubkey\":\"%s\",\"client_secret\":\"%s\",\"relays\":[\"wss://fixture.invalid/\"]}",KEY,OTHER,secret)+1;
        CHECK(len < sizeof(json));
        const char malformed[] = "nsec1fixture-not-a-key";
        CHECK(MDIdentityImportPairing(directory,store,account,malformed,sizeof(malformed),NULL,NULL).length);
        CHECK(MDIdentityImportPairing(directory,store,account,json,len-1,NULL,NULL).length);
        CHECK(MDIdentityImportPairing(directory,store,account,json,len,Cancel,NULL).length);
        NSDictionary *wrong = @{@"key":@OTHER,@"backend":@(MD_SIGNER_NIP46)};
        CHECK([MDIdentityImportPairing(directory,store,wrong,json,len,NULL,NULL) containsString:@"different public key"]);
        MdNip46Credentials *restored = NULL;
        CHECK(md_credential_store_get(store,KEY,&restored)==MD_CREDENTIAL_NOT_FOUND && !restored);
        CHECK(MDIdentityImportPairing(directory,NULL,account,json,len,NULL,NULL).length);
        CHECK(MDIdentityImportPairing(directory,store,account,json,len,NULL,NULL).length == 0);
        md_credential_wipe(json,sizeof(json));
        CHECK(!md_credential_store_get(store,KEY,&restored) && restored && !strcmp(restored->client_secret,secret));
        md_nip46_credentials_free(restored);
        NSString *metadata = [NSString stringWithContentsOfFile:[directory stringByAppendingPathComponent:@"management.json"] encoding:NSUTF8StringEncoding error:NULL];
        CHECK(metadata.length && ![metadata containsString:@(secret)] && ![metadata containsString:@"client_secret"]);
        CHECK(![MDIdentityAccounts(directory,NULL).lastObject[@"host"] boolValue]);
        CHECK(![MDIdentityAccounts(directory,NULL).lastObject[@"client"] boolValue]);
        md_credential_store_destroy(store);
    }
    return 0;
}
static int Accounts(void) {
    if (Metadata()) return 1;
    MdAccounts *accounts=NULL;
    static const MdCredentialStoreOps storeOps={.get=ReadPair,.put=NoWrite,.remove=RemoveOnly};
    MdCredentialStore *store=md_credential_store_custom(&storeOps,NULL);
    CHECK(store && !md_accounts_open(NULL,store,&accounts));
    CHECK(!md_accounts_add(accounts,MD_SIGNER_NIP46,KEY,"Fixture remote"));
    CHECK(!md_accounts_add(accounts,MD_SIGNER_NIP5F,OTHER,"Fixture socket"));
    CHECK(!md_accounts_save(accounts));
    md_accounts_destroy(accounts);
    NSArray *rows=MDIdentityAccounts(nil,NULL);
    CHECK(rows.count==2);
    NSDictionary *remote=rows[0],*wrong=rows[1];
    CHECK([remote[@"key"] isEqual:@KEY] && [wrong[@"key"] isEqual:@OTHER]);
    CHECK([MDIdentityBind(nil,store,MD_ACCOUNT_CLIENT,wrong,@"",NO,Connect,Node,NULL,NULL) containsString:@"different public key"]);
    CHECK([MDIdentityBind(nil,store,MD_ACCOUNT_CLIENT,remote,@"",NO,Connect,Node,Cancel,NULL) containsString:@"cancelled"]);
    CHECK(!MDIdentityBind(nil,store,MD_ACCOUNT_CLIENT,remote,@"",NO,Connect,Node,NULL,NULL).length);
    CHECK([MDIdentityBind(nil,store,MD_ACCOUNT_CLIENT,nil,@"",NO,Connect,Node,NULL,NULL) containsString:@"changed elsewhere"]);
    CHECK([MDIdentityBind(nil,store,MD_ACCOUNT_HOST,remote,@"",YES,Connect,Node,NULL,NULL) containsString:@"show_status"]);
    CHECK([MDIdentityBind(nil,store,MD_ACCOUNT_HOST,remote,@"",YES,Connect,Node,NULL,OTHER) containsString:@"differs"]);
    CHECK(!MDIdentityBind(nil,store,MD_ACCOUNT_HOST,remote,@"",YES,Connect,Node,NULL,KEY).length);
    rows=MDIdentityAccounts(nil,NULL); remote=rows[0];
    CHECK([remote[@"client"] boolValue] && [remote[@"host"] boolValue]);
    CHECK([MDIdentityError(MD_CREDENTIAL_LOCKED,MD_SIGNER_NIP46) containsString:@"locked"]);
    CHECK([MDIdentityError(MD_SIGNER_ERR_CONNECT,MD_SIGNER_NIP5F) containsString:@"Start the signer"]);
    CHECK([MDIdentityError(MD_SIGNER_ERR_TIMEOUT,MD_SIGNER_NIP46) containsString:@"approval"]);
    CHECK(!MDIdentityRemove(nil,store,remote).length && Removed==1);
    md_credential_store_destroy(store);
    rows=MDIdentityAccounts(nil,NULL);
    CHECK(rows.count==1 && ![rows[0][@"client"] boolValue] && ![rows[0][@"host"] boolValue]);
    return md_test_report();
}
static void Watchdog(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,15000000000LL),dispatch_get_main_queue(),^{ MD_CHECK(false); exit(md_test_report()); });
}
static int IPC(NSString *path) {
    __block MDHostLink *link;
    __block NSUInteger step=0;
    link=[[MDHostLink alloc] initWithUpdate:^(NSString *status,NSDictionary *s,NSDictionary *p,NSDictionary *permissions,BOOL busy) {
        MD_CHECK(NSThread.isMainThread);
        if (busy) return;
        NSUInteger revision=[s[@"revision"] unsignedIntegerValue];
        NSTimeInterval now=NSDate.date.timeIntervalSince1970;
        NSDictionary *request=[MDLiveRequests(s,now) firstObject];
        NSDictionary *cmd=nil;
        if (step==0 && revision==3 && [p[@"policy_available"] boolValue]) {
            MD_CHECK(MDApprovalDefault(s,request)==MD_CAP_VIDEO);
            ++step; [link command:@{@"v":@1,@"method":@"host.permissions"}]; return;
        } else if (step==1 && revision==3 && permissions) {
            MD_CHECK([permissions[@"screen_recording"] boolValue] && ![permissions[@"accessibility"] boolValue]);
            ++step; [link command:MDApprovalCommand(s,request,@"approve",MD_CAP_VIDEO,YES,now) policyVersion:@7]; return;
        } else if (step==2 && revision==3 && [status containsString:@"changed"]) {
            ++step; [link command:MDApprovalCommand(s,request,@"approve",MD_CAP_VIDEO,YES,now) policyVersion:p[@"policy_version"]]; return;
        } else if (step==3 && revision==4) {
            MD_CHECK([status containsString:@"may already be durable"]);
            cmd=MDApprovalCommand(s,request,@"approve",MD_CAP_VIDEO,NO,now);
        } else if (step==4 && revision==5) {
            cmd=MDKickCommand(s,s[@"session"]);
        } else if (step==5 && revision==6) {
            cmd=MDPolicyCommand(s,p,[p[@"clients"] firstObject],0,NO,nil);
        } else if (step==6 && revision==7) {
            NSString *sid=s[@"session"][@"session_id"];
            MD_CHECK(!MDPolicyCommand(s,p,[p[@"clients"] firstObject],0,YES,@"stale-session"));
            cmd=MDPolicyCommand(s,p,[p[@"clients"] firstObject],0,YES,sid);
        } else if (step==7 && !s && [status containsString:@"generation"]) {
            MD_CHECK(!p);
            [link close:^{ link=nil; exit(md_test_report()); }]; return;
        } else return;
        MD_CHECK(cmd != nil); ++step; [link command:cmd];
    }];
    [link connect:path]; Watchdog(); CFRunLoopRun(); return 1;
}
int main(int argc,char **argv) {
    @autoreleasepool {
        if (argc==1) return Decisions();
        CHECK(argc==2);
        if (!strcmp(argv[1],"--accounts")) return Accounts();
        return IPC(@(argv[1]));
    }
}