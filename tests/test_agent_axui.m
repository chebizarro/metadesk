/* Opt-in real Mac qualification. A child owns both AppKit windows; the
 * parent uses native AX IPC exactly as it would for a remote application.
 * Screen frames are encoded/decoded in memory and never written to disk. */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include "agent.h"
#include "capture.h"
#include "encode.h"
#include "decode.h"
#include "md_time.h"
#include "md_test.h"
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static NSWindow *target_window, *occluder_window;
static int target_clicks, occluder_clicks;
static MdA11yNode target_node;
@interface L12Actions : NSObject
- (void)target:(id)sender;
- (void)occluder:(id)sender;
@end
@implementation L12Actions
- (void)target:(id)sender { (void)sender; printf("TARGET %d\n", ++target_clicks); fflush(stdout); }
- (void)occluder:(id)sender { (void)sender; printf("OCCLUDER %d\n", ++occluder_clicks); fflush(stdout); }
@end

static void *fixture_commands(void *unused) {
    (void)unused;
    char command[32];
    while (fgets(command, sizeof(command), stdin)) {
        const char *value = command;
        dispatch_sync(dispatch_get_main_queue(), ^{
            if (!strcmp(value, "HIDE\n")) {
                [occluder_window orderOut:nil];
                [target_window makeKeyAndOrderFront:nil];
                [NSApp updateWindows];
                puts("READY");
            } else if (!strcmp(value, "COUNTS\n")) {
                printf("COUNTS %d %d\n", target_clicks, occluder_clicks);
            } else if (!strcmp(value, "QUIT\n")) {
                puts("BYE"); fflush(stdout); exit(0);
            }
            fflush(stdout);
        });
    }
    exit(1);
}

static int fixture_main(void) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    static L12Actions *actions;
    actions = [L12Actions new];
    target_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(150, 250, 360, 200)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    NSButton *button = [NSButton buttonWithTitle:@"L12 owned target" target:actions action:@selector(target:)];
    button.frame = NSMakeRect(40, 65, 260, 65);
    [target_window.contentView addSubview:button];
    target_window.title = @"L12 owned background";
    [target_window makeKeyAndOrderFront:nil];
    occluder_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(150, 250, 360, 200)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    NSButton *cover = [NSButton buttonWithTitle:@"L12 owned occluder" target:actions action:@selector(occluder:)];
    cover.frame = button.frame;
    [occluder_window.contentView addSubview:cover];
    occluder_window.title = @"L12 owned floating occluder";
    occluder_window.level = NSFloatingWindowLevel;
    [occluder_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    dispatch_async(dispatch_get_main_queue(), ^{
        puts("READY"); fflush(stdout);
        pthread_t thread;
        if (pthread_create(&thread, NULL, fixture_commands, NULL)) exit(2);
        pthread_detach(thread);
    });
    [NSApp run];
    return 1;
}

static AXUIElementRef find_button(AXUIElementRef elem, int depth) {
    if (depth > 12) return NULL;
    CFTypeRef title = NULL;
    AXUIElementCopyAttributeValue(elem, kAXTitleAttribute, &title);
    bool match = title && CFGetTypeID(title) == CFStringGetTypeID() && CFEqual(title, CFSTR("L12 owned target"));
    if (title) CFRelease(title);
    if (match) return (AXUIElementRef)CFRetain(elem);
    CFArrayRef children = NULL;
    AXUIElementCopyAttributeValue(elem, kAXChildrenAttribute, (CFTypeRef *)&children);
    AXUIElementRef result = NULL;
    if (children) {
        for (CFIndex i = 0; i < CFArrayGetCount(children) && !result; i++)
            result = find_button((AXUIElementRef)CFArrayGetValueAtIndex(children, i), depth + 1);
        CFRelease(children);
    }
    return result;
}

/* The controlled subtree uses real native identities, hit testing and action. */
static int owned_tree(MdA11yCtx *ctx, MdA11yNode **out) {
    (void)ctx;
    MdA11yNode *n = calloc(1, sizeof(*n));
    *n = target_node;
    n->id = strdup(target_node.id); n->role = strdup("button");
    n->label = strdup("L12 owned target");
    *out = n;
    return 0;
}

static bool fixture_line(int fd, char *line, size_t size) {
    size_t used = 0;
    int64_t deadline = md_now_ms() + 10000;
    while (used + 1 < size) {
        int64_t left = deadline - md_now_ms();
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (left <= 0 || poll(&pfd, 1, (int)left) <= 0) return false;
        char ch;
        if (read(fd, &ch, 1) != 1) return false;
        if (ch == '\n') { line[used] = 0; return true; }
        line[used++] = ch;
    }
    return false;
}

static void test_owned_clicks(pid_t fixture, int in, int out) {
    AXUIElementRef app = AXUIElementCreateApplication(fixture);
    AXUIElementRef button = find_button(app, 0);
    MD_CHECK(button);
    if (!button) { CFRelease(app); return; }
    char id[64];
    snprintf(id, sizeof(id), "ax%ld:%lx", (long)fixture, (unsigned long)CFHash(button));
    target_node.id = id;
    CFTypeRef p = NULL, s = NULL;
    AXUIElementCopyAttributeValue(button, kAXPositionAttribute, &p);
    AXUIElementCopyAttributeValue(button, kAXSizeAttribute, &s);
    CGPoint point = {0}; CGSize size = {0};
    MD_CHECK(p && s);
    AXValueGetValue(p, kAXValueCGPointType, &point);
    AXValueGetValue(s, kAXValueCGSizeType, &size);
    target_node.x = point.x; target_node.y = point.y; target_node.w = size.width; target_node.h = size.height;
    CFRelease(p); CFRelease(s); CFRelease(button);
    MdA11yCtx *a11y = md_a11y_create();
    MD_CHECK(a11y);
    const MdA11yBackend *native = a11y->vtable;
    MdA11yBackend backend = *native; backend.get_tree = owned_tree; a11y->vtable = &backend;
    MdInputConfig input_cfg = { .screen_width = 4096, .screen_height = 4096 };
    MdInput *input = md_input_create(&input_cfg);
    MD_CHECK(md_input_is_ready(input));
    MdAgentConfig cfg = { .a11y = a11y, .input = input, .tree_format = MD_TREE_FORMAT_COMPACT, .settle_ms = 1 };
    MdAgent *agent = md_agent_create(&cfg);
    char action[256], line[128];
    snprintf(action, sizeof(action), "{\"v\":1,\"action\":\"click\",\"target_id\":\"%s\",\"payload\":{}}", id);
    char *result = md_agent_handle_action_mcp(agent, (const uint8_t *)action, strlen(action));
    MD_CHECK(!result); free(result);
    MD_CHECK(write(in, "COUNTS\n", 7) == 7);
    MD_CHECK(fixture_line(out, line, sizeof(line)) && !strcmp(line, "COUNTS 0 0"));
    MD_CHECK(write(in, "HIDE\n", 5) == 5);
    MD_CHECK(fixture_line(out, line, sizeof(line)) && !strcmp(line, "READY"));
    /* Round-trip to the foreign application's AX server after its window
     * transition acknowledgement, rather than waiting a fixed focus sleep. */
    CFTypeRef focused = NULL;
    MD_CHECK(AXUIElementCopyAttributeValue(app, kAXFocusedWindowAttribute, &focused) == kAXErrorSuccess);
    if (focused) CFRelease(focused);
    /* Read the target again after the visibility transition, just as a real
     * agent gets a fresh tree. AppKit can replace an AX element identity. */
    button = find_button(app, 0);
    MD_CHECK(button);
    if (button) {
        snprintf(id, sizeof(id), "ax%ld:%lx", (long)fixture, (unsigned long)CFHash(button));
        CFRelease(button);
        snprintf(action, sizeof(action), "{\"v\":1,\"action\":\"click\",\"target_id\":\"%s\",\"payload\":{}}", id);
    }
    char *hit_id = native->hit_test(a11y, target_node.x + target_node.w / 2,
                                    target_node.y + target_node.h / 2);
    if (!hit_id || strcmp(hit_id, id))
        fprintf(stderr, "OWNED visibility mismatch target=%s hit=%s\n", id, hit_id ? hit_id : "(none)");
    free(hit_id);
    result = md_agent_handle_action_mcp(agent, (const uint8_t *)action, strlen(action));
    MD_CHECK(result); free(result);
    MD_CHECK(fixture_line(out, line, sizeof(line)) && !strcmp(line, "TARGET 1"));
    MD_CHECK(write(in, "COUNTS\n", 7) == 7);
    MD_CHECK(fixture_line(out, line, sizeof(line)) && !strcmp(line, "COUNTS 1 0"));
    MD_CHECK(md_agent_get_action_count(agent) == 1);
    printf("OWNED receipt: %s; successful actions=%u\n", line, md_agent_get_action_count(agent));
    md_agent_destroy(agent); md_input_destroy(input);
    a11y->vtable = native; md_a11y_destroy(a11y); CFRelease(app);
}

static MdStream *video_host, *video_client;
static MdDecoder *decoder;
static unsigned decoded;
static void decoded_frame(const MdDecodedFrame *frame, void *data) {
    (void)data;
    MD_CHECK(frame->width && frame->height);
    decoded++;
}
static void encoded(const MdEncodedPacket *pkt, void *data) {
    (void)data;
    MD_CHECK(md_stream_send(video_host, MD_PKT_VIDEO_FRAME, 1, pkt->data, pkt->size) == 0);
    MdPacketHeader header; uint8_t *payload = NULL;
    MD_CHECK(md_stream_recv(video_client, &header, &payload, 5000) == 0);
    MD_CHECK(header.type == MD_PKT_VIDEO_FRAME);
    MD_CHECK(md_decoder_submit(decoder, payload, header.payload_len, 0) == 0);
    MD_CHECK(md_decoder_poll(decoder, decoded_frame, NULL) >= 0);
    free(payload);
}

static void test_native_video(bool serial) {
    MdCaptureConfig capture_cfg = { .target_fps = 30, .max_width = 640, .max_height = 480 };
    MdCaptureCtx *capture = md_capture_create(&capture_cfg);
    MD_CHECK(capture && md_capture_start(capture) == 0);
    MdA11yCtx *a11y = md_a11y_create();
    MD_CHECK(a11y);
    MdAgentConfig cfg = { .a11y = a11y, .tree_format = MD_TREE_FORMAT_JSON };
    MdAgent *agent = md_agent_create(&cfg);
    MdStreamServer *server = md_stream_server_create("127.0.0.1", 0);
    video_client = md_stream_connect("127.0.0.1", md_stream_server_port(server), 3000);
    video_host = md_stream_server_accept(server, 3000);
    MdSession session;
    md_session_init(&session);
    MD_CHECK(md_session_request(&session, "L12-owned-loopback", MD_CAP_VIDEO | MD_CAP_AGENT, MD_TREE_FORMAT_JSON) == 0);
    MD_CHECK(md_session_accept(&session, "L12-hardware", MD_CAP_VIDEO | MD_CAP_AGENT) == 0);
    MdSessionAccept accept;
    MD_CHECK(md_session_prepare_binding(&session, &accept, md_now_ms()) == 0);
    char *proof = md_session_bind_to_json(&accept);
    MD_CHECK(md_stream_send(video_client, MD_PKT_SESSION_INFO, 0, (uint8_t *)proof, strlen(proof)) == 0);
    free(proof);
    MD_CHECK(md_stream_accept_binding(video_host, &session) == 0);
    MdPacketHeader ack; uint8_t *payload = NULL;
    MD_CHECK(md_stream_recv(video_client, &ack, &payload, 3000) == 0 && ack.type == MD_PKT_SESSION_INFO);
    free(payload);
    int64_t start = md_now_ms();
    MdAgentTreeJob *job = md_agent_tree_start(agent);
    MD_CHECK(job);
    char *tree = serial ? md_agent_tree_finish(job) : NULL;
    if (serial) job = NULL;
    decoder = md_decoder_create();
    MdEncoder *encoder = NULL;
    int64_t deadline = md_now_ms() + 10000;
    while (!decoded && md_now_ms() < deadline) {
        MdFrame frame;
        int ret = md_capture_get_frame(capture, &frame, 5000);
        MD_CHECK(ret == 0);
        if (ret) break;
        if (!encoder) {
            MdEncoderConfig ec = { .width = frame.width & ~1u, .height = frame.height & ~1u,
                                    .fps = 30, .bitrate = 1000000 };
            encoder = md_encoder_create(&ec);
            MD_CHECK(encoder);
        }
        MdPixFmt fmt = frame.format == MD_PIX_CAPTURE_NV12 ? MD_PIX_FMT_NV12 : MD_PIX_FMT_BGRA;
        MD_CHECK(md_encoder_submit(encoder, frame.data, frame.stride, fmt, frame.seq, encoded, NULL) == 0);
        md_capture_release_frame(capture, &frame);
    }
    MD_CHECK(decoded > 0);
    int64_t first = md_now_ms() - start;
    bool pending = job && md_agent_tree_take(job, &tree) == 0;
    printf("NATIVE authenticated loopback: serial=%d first-decoded=%lldms tree-pending=%d\n", serial, (long long)first, pending);
    if (job) {
        char *remaining = md_agent_tree_finish(job);
        if (!tree) tree = remaining; else free(remaining);
    }
    MD_CHECK(tree && strstr(tree, "Desktop") && strstr(tree, "L12 owned target"));
    printf("NATIVE tree-complete=%lldms bytes=%zu decoded=%u\n",
           (long long)(md_now_ms() - start), tree ? strlen(tree) : 0, decoded);
    if (!serial) MD_CHECK(pending);
    free(tree);
    md_encoder_destroy(encoder); md_decoder_destroy(decoder);
    md_capture_destroy(capture);
    md_stream_destroy(video_host); md_stream_destroy(video_client);
    md_stream_server_destroy(server);
    md_agent_destroy(agent); md_a11y_destroy(a11y);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "--fixture")) return fixture_main();
        const char *live = getenv("MD_A11Y_LIVE_TESTS");
        if (!live || strcmp(live, "1")) return 77;
        if (!AXIsProcessTrusted() || !CGPreflightScreenCaptureAccess()) return 1;
        bool serial = argc > 1 && !strcmp(argv[1], "--serial-tree");
        bool clicks_only = argc > 1 && !strcmp(argv[1], "--clicks-only");
        int input[2], output[2];
        MD_CHECK(pipe(input) == 0 && pipe(output) == 0);
        pid_t child = fork();
        if (child == 0) {
            dup2(input[0], STDIN_FILENO); dup2(output[1], STDOUT_FILENO);
            close(input[0]); close(input[1]); close(output[0]); close(output[1]);
            execl(argv[0], argv[0], "--fixture", (char *)NULL); _exit(127);
        }
        MD_CHECK(child > 0);
        close(input[0]); close(output[1]);
        char line[128];
        MD_CHECK(fixture_line(output[0], line, sizeof(line)) && !strcmp(line, "READY"));
        if (!serial) test_owned_clicks(child, input[1], output[0]);
        if (!clicks_only) test_native_video(serial);
        MD_CHECK(write(input[1], "QUIT\n", 5) == 5);
        MD_CHECK(fixture_line(output[0], line, sizeof(line)) && !strcmp(line, "BYE"));
        int status;
        MD_CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(input[1]); close(output[0]);
        return md_test_report();
    }
}
