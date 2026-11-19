/*
 * metadesk — mcp_bridge.c
 * MCP ↔ MdSession lifecycle bridge implementation.
 *
 * Creates and owns: MdSession, MdAgent, MdMcpServer, MdMcpStdio.
 * Registers tools, resources, and a11y change notifications.
 */
#include "mcp_bridge.h"
#define MD_LOG_TAG "mcp-bridge"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <signal.h>
#include <time.h>

struct MdMcpBridge {
    MdSession          session;
    MdAgent           *agent;
    MdA11ySnapshot    *snapshot;
    MdMcpServer       *server;
    MdMcpStdio        *stdio_ctx;
    MdMcpHttp         *http_ctx;

    /* Contexts for tool/resource handlers (must outlive server) */
    MdMcpToolCtx       tool_ctx;
    MdMcpResourceCtx   resource_ctx;

    /* Borrowed references */
    MdA11yCtx         *a11y;
    MdInput           *input;

    /* True while bridge_a11y_change_cb is registered on a11y with
     * this bridge as userdata; destroy must detach it first. */
    bool               a11y_subscribed;
    /* Serialize notifier installation with hints already arriving after subscribe. */
    pthread_mutex_t    notify_mu;
};

static void bridge_a11y_change_cb(const MdA11yDelta *deltas, int count, void *userdata)
{
    MdMcpBridge *b = userdata;
    pthread_mutex_lock(&b->notify_mu);
    md_mcp_a11y_change_cb(deltas, count, &b->resource_ctx);
    pthread_mutex_unlock(&b->notify_mu);
}

/* ── Lifecycle ───────────────────────────────────────────────── */

MdMcpBridge *md_mcp_bridge_create(const MdMcpBridgeConfig *config)
{
    /* a11y is required (see mcp_bridge.h); input stays optional */
    if (!config || !config->a11y) return NULL;

    MdMcpBridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    if (pthread_mutex_init(&b->notify_mu, NULL) != 0) {
        free(b);
        return NULL;
    }

    MdTreeFormat tree_format = config->tree_format == MD_MCP_TREE_FORMAT_JSON
        ? MD_TREE_FORMAT_JSON : MD_TREE_FORMAT_COMPACT;

    b->a11y = config->a11y;
    b->input = config->input;
    b->snapshot = md_a11y_snapshot_create(config->a11y);
    if (!b->snapshot) goto fail;

    /* 1. Initialize session */
    md_session_init(&b->session);
    b->session.capabilities = MD_CAP_AGENT;
    if (md_input_is_ready(config->input)) b->session.capabilities |= MD_CAP_INPUT;
    b->session.state = MD_SESSION_NEGOTIATING;

    /* Generate session ID */
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, b->session.session_id);

    /* 2. Create agent */
    MdAgentConfig agent_cfg = {
        .a11y = config->a11y,
        .input = config->input,
        /* Action results are always JSON, including full-tree fallbacks. */
        .tree_format = MD_TREE_FORMAT_JSON,
        .settle_ms = config->settle_ms > 0 ? config->settle_ms
                                            : MD_AGENT_DEFAULT_SETTLE_MS,
    };
    b->agent = md_agent_create(&agent_cfg);
    if (!b->agent)
        goto fail;

    /* 3. Create MCP server. The notification write_fn is wired once a
     * transport exists (steps 7/8) — the transport needs the server
     * first, so it cannot be supplied here. */
    MdMcpServerConfig srv_cfg = {
        .server_name = "metadesk",
        .server_version = MD_MCP_BRIDGE_VERSION,
    };
    b->server = md_mcp_server_create(&srv_cfg);
    if (!b->server)
        goto fail;

    /* 4. Register tools */
    b->tool_ctx.agent = b->agent;
    b->tool_ctx.input = config->input;
    b->tool_ctx.snapshot = b->snapshot;
    if (md_mcp_register_tools(b->server, &b->tool_ctx) != 0)
        goto fail;

    /* 5. Register resources */
    b->resource_ctx.snapshot = b->snapshot;
    b->resource_ctx.server = b->server;
    b->resource_ctx.session = &b->session;
    b->resource_ctx.agent = b->agent;
    b->resource_ctx.tree_format = tree_format;
    if (md_mcp_register_resources(b->server, &b->resource_ctx) != 0)
        goto fail;

    /* 6. Subscribe to a11y changes for notifications */
    if (md_a11y_subscribe_changes(config->a11y,
                                  bridge_a11y_change_cb, b) < 0) {
        MD_LOG_W("accessibility change subscription failed; refreshing on every read");
        md_a11y_snapshot_set_caching(b->snapshot, false);
    } else {
        b->a11y_subscribed = true;
        if (!config->notify_sync) {
            uint32_t quiet_ms = config->notify_coalesce_ms
                ? config->notify_coalesce_ms : MD_MCP_NOTIFY_COALESCE_MS;
            pthread_mutex_lock(&b->notify_mu);
            b->resource_ctx.notifier = md_mcp_notify_coalescer_create(b->server, quiet_ms);
            pthread_mutex_unlock(&b->notify_mu);
            if (!b->resource_ctx.notifier)
                MD_LOG_W("notification coalescer unavailable; notifying synchronously");
        }
    }

    /* 7. Create stdio transport if requested */
    if (config->stdio_in_fd >= 0) {
        b->stdio_ctx = md_mcp_stdio_create(b->server,
                                            config->stdio_in_fd,
                                            config->stdio_out_fd);
        if (!b->stdio_ctx)
            goto fail;
        /* stdio responses use per-request sinks; the server write_fn is
         * the notification channel writing to this transport. */
        md_mcp_server_set_write_fn(b->server, md_mcp_stdio_write, b->stdio_ctx);
    }

    /* 8. Create HTTP+SSE transport if requested */
    if (config->http_port > 0) {
        MdMcpHttpConfig http_cfg = {
            .server = b->server,
            .bind_addr = config->http_bind_addr,
            .port = config->http_port,
            .token_file = config->http_token_file,
            .public_urls = config->http_public_urls,
            .public_url_count = config->http_public_url_count,
            .allow_npubs = config->http_allow_npubs,
            .allow_npub_count = config->http_allow_npub_count,
            .allow_file = config->http_allow_file,
            .nip98_authorize = config->http_nip98_authorize,
            .nip98_authorize_userdata = config->http_nip98_authorize_userdata,
        };
        b->http_ctx = md_mcp_http_create(&http_cfg);
        if (!b->http_ctx)
            goto fail;
        /* HTTP responses use per-request sinks; the server write_fn is
         * the notification (SSE) channel. */
        md_mcp_server_set_write_fn(b->server, md_mcp_http_write, b->http_ctx);
    }

    /* 9. Activate session */
    md_session_activate(&b->session);

    return b;

fail:
    /* destroy also quiesces a partially constructed server before draining. */
    md_mcp_bridge_destroy(b);
    return NULL;
}

int md_mcp_bridge_run(MdMcpBridge *bridge)
{
    if (!bridge) return -1;

    if (bridge->stdio_ctx)
        return md_mcp_stdio_run(bridge->stdio_ctx);
    if (bridge->http_ctx)
        return md_mcp_http_run(bridge->http_ctx);

    /* No transport configured */
    return -1;
}

typedef struct {
    MdMcpBridge *bridge;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
    bool desktop_lost;
} McpLifecycle;

static void *bridge_signal_wait(void *userdata)
{
    McpLifecycle *life = userdata;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    int sig;
    if (sigwait(&signals, &sig) == 0)
        md_mcp_bridge_shutdown(life->bridge);
    return NULL;
}

static void *bridge_watch_desktop(void *userdata)
{
    McpLifecycle *life = userdata;
    pthread_mutex_lock(&life->mu);
    while (!life->done) {
        if (!md_a11y_is_connected(life->bridge->a11y)) {
            life->desktop_lost = true;
            pthread_mutex_unlock(&life->mu);
            MD_LOG_E("accessibility backend disconnected");
            md_mcp_bridge_shutdown(life->bridge);
            return NULL;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100 * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&life->cv, &life->mu, &deadline);
    }
    pthread_mutex_unlock(&life->mu);
    return NULL;
}

int md_mcp_bridge_run_supervised(MdMcpBridge *bridge)
{
    if (!bridge) return -1;
    McpLifecycle life = { .bridge = bridge };
    if (pthread_mutex_init(&life.mu, NULL) != 0) return -1;
    if (pthread_cond_init(&life.cv, NULL) != 0) {
        pthread_mutex_destroy(&life.mu);
        return -1;
    }
    int rc = -1;
    pthread_t signal_thread, desktop_thread;
    bool signal_started = pthread_create(&signal_thread, NULL, bridge_signal_wait, &life) == 0;
    bool desktop_started = signal_started &&
        pthread_create(&desktop_thread, NULL, bridge_watch_desktop, &life) == 0;
    if (desktop_started) {
        rc = md_mcp_bridge_run(bridge);
    } else {
        MD_LOG_E("failed to start lifecycle threads");
        md_mcp_bridge_shutdown(bridge);
    }
    pthread_mutex_lock(&life.mu);
    life.done = true;
    pthread_cond_signal(&life.cv);
    pthread_mutex_unlock(&life.mu);
    if (desktop_started)
        pthread_join(desktop_thread, NULL);
    /* Also catch a disconnect coincident with transport EOF. */
    if (life.desktop_lost || !md_a11y_is_connected(bridge->a11y))
        rc = -1;
    if (signal_started) {
        /* Wake a still-waiting sigwait on normal EOF. Bridge stays alive
         * until both lifecycle threads are joined. */
        pthread_kill(signal_thread, SIGTERM);
        pthread_join(signal_thread, NULL);
    }
    pthread_cond_destroy(&life.cv);
    pthread_mutex_destroy(&life.mu);
    return rc;
}

void md_mcp_bridge_shutdown(MdMcpBridge *bridge)
{
    if (!bridge) return;
    if (bridge->stdio_ctx)
        md_mcp_stdio_shutdown(bridge->stdio_ctx);
    if (bridge->http_ctx)
        md_mcp_http_shutdown(bridge->http_ctx);
}

MdSessionState md_mcp_bridge_get_state(const MdMcpBridge *bridge)
{
    return bridge ? bridge->session.state : MD_SESSION_IDLE;
}

MdMcpServer *md_mcp_bridge_get_server(const MdMcpBridge *bridge)
{
    return bridge ? bridge->server : NULL;
}

void md_mcp_bridge_destroy(MdMcpBridge *bridge)
{
    if (!bridge) return;

    /* Stop new notifications before draining the callback. Transport work
     * is bounded by one write timeout (the entire SSE broadcast), plus any
     * in-flight dispatch using the serialized output. This does not impose
     * a deadline on tool execution itself. Keep callback dependencies alive. */
    md_mcp_server_quiesce(bridge->server);
    if (bridge->a11y_subscribed) {
        md_a11y_unsubscribe_changes(bridge->a11y);
        bridge->a11y_subscribed = false;
    }

    md_mcp_notify_coalescer_destroy(bridge->resource_ctx.notifier);
    bridge->resource_ctx.notifier = NULL;

    md_session_disconnect(&bridge->session);

    if (bridge->stdio_ctx)
        md_mcp_stdio_destroy(bridge->stdio_ctx);
    if (bridge->http_ctx)
        md_mcp_http_destroy(bridge->http_ctx);

    /* Free heap-allocated per-tool handler contexts before destroying server */
    md_mcp_tools_cleanup(&bridge->tool_ctx);

    if (bridge->server)
        md_mcp_server_destroy(bridge->server);
    if (bridge->agent)
        md_agent_destroy(bridge->agent);
    md_a11y_snapshot_destroy(bridge->snapshot);

    pthread_mutex_destroy(&bridge->notify_mu);
    free(bridge);
}
