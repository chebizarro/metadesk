/*
 * Private e2e host for `metadesk-host mcp-proxy` (md-ah9.14 / md-ah9.3).
 *
 * The production MCP HTTP server and the production host-allowlist NIP-98
 * source (src/host/mcp_nostr_acl.c: saved host role -> live kind-30000 REQ)
 * on loopback, without the accessibility/input backends a real desktop host
 * needs. Tools, gated like the real ones (md-5km): "echo" and "notify"
 * (broadcasts one SSE notification) need agent; "act" stands in for an
 * action tool and needs agent+input. The allowlist change feed is wired
 * as in the host (md-ht7), so grant changes reach the SSE streams.
 *
 * Usage: mcp_proxy_host_fixture RELAY_URL... [--allow-npub NPUB]
 * Prints "PORT <n>" on stdout once listening; exits when stdin closes.
 */
#include "host_mcp.h"
#include "mcp_http.h"
#include "mcp_server.h"
#include "session.h"
#include "log.h"
#include "md_time.h"

#include <cjson/cJSON.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static MdMcpHttp *g_http;

static cJSON *text_result(const char *text) {
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    return content;
}

static cJSON *echo_tool(const cJSON *args, bool *is_error, char **msg, void *ud) {
    (void)is_error; (void)msg; (void)ud;
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
    return text_result(cJSON_IsString(text) ? text->valuestring : "");
}

/* The proxy reports "notification stream open" when the GET's 200 headers
 * arrive; the server marks the client ready right after writing them. This
 * bounded wait for {"text": "<n>"} (default 1) ready streams covers that
 * sub-millisecond hand-off for the test only. */
static cJSON *notify_tool(const cJSON *args, bool *is_error, char **msg, void *ud) {
    (void)ud;
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
    int want = cJSON_IsString(text) ? atoi(text->valuestring) : 1;
    int64_t deadline = md_now_ms() + 5000;
    while (md_mcp_http_sse_client_count(g_http) < (want > 0 ? want : 1) && md_now_ms() < deadline)
        sched_yield();
    static const char note[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
        "\"params\":{\"level\":\"info\",\"data\":\"fixture-notify\"}}";
    if (md_mcp_http_write(note, sizeof(note) - 1, g_http) != 0) {
        *is_error = true;
        *msg = strdup("broadcast failed");
        return NULL;
    }
    return text_result("notified");
}

static cJSON *act_tool(const cJSON *args, bool *is_error, char **msg, void *ud) {
    (void)args; (void)is_error; (void)msg; (void)ud;
    return text_result("acted");
}

static void *serve(void *arg) {
    md_mcp_http_run(arg);
    return NULL;
}

static cJSON *schema(void) {
    return cJSON_Parse("{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}");
}

int main(int argc, char **argv) {
    int relay_count = 1;
    while (relay_count + 1 < argc && strcmp(argv[relay_count + 1], "--allow-npub")) relay_count++;
    const char *npub = NULL;
    if (relay_count + 1 < argc) {
        if (relay_count + 3 != argc) return 2;
        npub = argv[argc - 1];
    }
    if (argc < 2 || !strcmp(argv[1], "--allow-npub")) return 2;
    sigset_t term;
    sigemptyset(&term);
    sigaddset(&term, SIGINT);
    sigaddset(&term, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &term, NULL);
    signal(SIGPIPE, SIG_IGN);

    HostMcpAcl *acl = host_mcp_acl_start((const char *const *)&argv[1], relay_count);
    if (!acl) return 1;
    MdMcpServerConfig scfg = { .server_name = "mcp-proxy-fixture", .server_version = "0" };
    MdMcpServer *server = md_mcp_server_create(&scfg);
    MdMcpTool echo = { .name = "echo", .description = "echo text",
                       .input_schema = schema(), .handler = echo_tool,
                       .required_caps = MD_CAP_AGENT };
    MdMcpTool notify = { .name = "notify", .description = "broadcast one notification",
                         .input_schema = schema(), .handler = notify_tool,
                         .required_caps = MD_CAP_AGENT };
    MdMcpTool act = { .name = "act", .description = "stand-in action tool",
                      .input_schema = schema(), .handler = act_tool,
                      .required_caps = MD_CAP_AGENT | MD_CAP_INPUT };
    const char *npubs[] = { npub };
    MdMcpHttpConfig hcfg = {
        .server = server, .ephemeral_port = true,
        .allow_npubs = npubs, .allow_npub_count = npub ? 1 : 0,
        .nip98_grant = host_mcp_acl_grant, .nip98_grant_userdata = acl,
        .nip98_grant_watch = host_mcp_acl_watch,
    };
    if (!server || md_mcp_server_register_tool(server, &echo) ||
        md_mcp_server_register_tool(server, &notify) ||
        md_mcp_server_register_tool(server, &act) ||
        !(g_http = md_mcp_http_create(&hcfg)) ||
        md_mcp_server_set_write_fn(server, md_mcp_http_write, g_http)) {
        fprintf(stderr, "fixture: setup failed\n");
        return 1;
    }
    pthread_t tid;
    pthread_create(&tid, NULL, serve, g_http);
    printf("PORT %u\n", md_mcp_http_port(g_http));
    fflush(stdout);

    char buf[64];
    while (fread(buf, 1, sizeof(buf), stdin) > 0) {}

    md_mcp_http_shutdown(g_http);
    /* Wake the accept loop promptly (as tests/test_mcp_http.c does). */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 addr = { .sin6_family = AF_INET6,
        .sin6_port = htons(md_mcp_http_port(g_http)), .sin6_addr = in6addr_loopback };
    if (fd >= 0) { connect(fd, (struct sockaddr *)&addr, sizeof(addr)); close(fd); }
    pthread_join(tid, NULL);
    md_mcp_http_destroy(g_http);
    md_mcp_server_destroy(server);
    host_mcp_acl_stop(acl);
    return 0;
}
