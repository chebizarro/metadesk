/*
 * metadesk — mcp_resources.c
 * MCP resource implementations: UI tree and session info.
 */
#include "mcp_resources.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

/* Build the MCP resource `contents` array: one text item carrying
 * `uri`, `mimeType`, and `text`. cJSON copies each string. */
static cJSON *resource_text_contents(const char *uri, const char *mime,
                                     const char *text)
{
    cJSON *contents = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uri", uri);
    cJSON_AddStringToObject(item, "mimeType", mime);
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(contents, item);
    return contents;
}

/* ── UI tree resource ───────────────────────────── */

static cJSON *read_ui_tree(void *userdata)
{
    MdMcpResourceCtx *ctx = (MdMcpResourceCtx *)userdata;

    /* Serialize the same lazy snapshot used by read tools. */
    char *tree_text = NULL;
    const char *mime = ctx->tree_format == MD_TREE_FORMAT_COMPACT
        ? "text/plain" : "application/json";

    if (ctx->snapshot) {
        const MdA11yNode *root = md_a11y_snapshot_acquire(ctx->snapshot);
        if (root) {
            uint64_t ts_ms = md_a11y_snapshot_time_ms(ctx->snapshot);
            switch (ctx->tree_format) {
            case MD_TREE_FORMAT_COMPACT:
                tree_text = md_a11y_to_compact_at(root, ts_ms);
                mime = "text/plain";
                break;
            case MD_TREE_FORMAT_JSON:
            default:
                tree_text = md_a11y_to_json_at(root, ts_ms);
                mime = "application/json";
                break;
            }
        }
        md_a11y_snapshot_release(ctx->snapshot);
    }

    cJSON *contents = resource_text_contents(
        MD_MCP_URI_UI_TREE, mime, tree_text ? tree_text :
        (ctx->tree_format == MD_TREE_FORMAT_COMPACT ? "" : "{\"root\":null}"));
    free(tree_text);
    return contents;
}

/* ── Session info resource ───────────────────────────────────── */

static cJSON *read_session_info(void *userdata)
{
    MdMcpResourceCtx *ctx = (MdMcpResourceCtx *)userdata;

    cJSON *info = cJSON_CreateObject();

    if (ctx->session) {
        if (ctx->session->session_id[0])
            cJSON_AddStringToObject(info, "session_id", ctx->session->session_id);
        if (ctx->session->peer_npub[0])
            cJSON_AddStringToObject(info, "peer_npub", ctx->session->peer_npub);

        /* State */
        const char *state_str = "unknown";
        switch (ctx->session->state) {
        case MD_SESSION_IDLE:          state_str = "idle"; break;
        case MD_SESSION_REQUESTING:    state_str = "requesting"; break;
        case MD_SESSION_NEGOTIATING:   state_str = "negotiating"; break;
        case MD_SESSION_ACTIVE:        state_str = "active"; break;
        case MD_SESSION_DISCONNECTING: state_str = "disconnecting"; break;
        }
        cJSON_AddStringToObject(info, "state", state_str);

        /* Effective capabilities: what the host offers, narrowed to the
         * requesting principal's grant (md-5km). */
        uint32_t effective = ctx->session->capabilities &
                             md_mcp_server_caller_caps(ctx->server);
        cJSON *caps = cJSON_CreateArray();
        if (effective & MD_CAP_VIDEO)
            cJSON_AddItemToArray(caps, cJSON_CreateString("video"));
        if (effective & MD_CAP_AGENT)
            cJSON_AddItemToArray(caps, cJSON_CreateString("agent"));
        if (effective & MD_CAP_INPUT)
            cJSON_AddItemToArray(caps, cJSON_CreateString("input"));
        cJSON_AddItemToObject(info, "capabilities", caps);

        cJSON_AddStringToObject(info, "tree_format",
                                ctx->tree_format == MD_TREE_FORMAT_COMPACT
                                    ? "compact" : "json");
        cJSON_AddNumberToObject(info, "keepalive_ms", ctx->session->keepalive_ms);
    }

    if (ctx->agent) {
        cJSON_AddNumberToObject(info, "action_count",
                                md_agent_get_action_count(ctx->agent));
    }

    /* Serialize info to a string, then wrap in MCP content array */
    char *info_str = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);

    cJSON *contents = resource_text_contents(
        MD_MCP_URI_SESSION_INFO, "application/json", info_str ? info_str : "{}");
    free(info_str);
    return contents;
}

/* ── Notifications ───────────────────────────────────────────── */

struct MdMcpNotifyCoalescer {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    MdMcpServer *server;
    uint32_t quiet_ms;
    bool stop, dirty;
    uint64_t first_ns, last_ns;
};

static uint64_t notify_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void *notify_worker(void *userdata)
{
    MdMcpNotifyCoalescer *n = userdata;
    pthread_mutex_lock(&n->mu);
    unsigned failures = 0;
    uint64_t retry_ns = 0;
    while (!n->stop) {
        while (!n->dirty && !n->stop)
            pthread_cond_wait(&n->cv, &n->mu);
        if (n->stop) break;
        uint64_t deadline = n->last_ns + (uint64_t)n->quiet_ms * 1000000;
        uint64_t cap = n->first_ns + 200000000ULL;
        if (deadline > cap) deadline = cap;
        if (retry_ns) deadline = retry_ns;
        uint64_t now = notify_now_ns();
        if (now < deadline) {
#ifdef __APPLE__
            uint64_t remaining = deadline - now;
            struct timespec wait = { (time_t)(remaining / 1000000000ULL),
                                    (long)(remaining % 1000000000ULL) };
            pthread_cond_timedwait_relative_np(&n->cv, &n->mu, &wait);
#else
            struct timespec wait = { (time_t)(deadline / 1000000000ULL),
                                    (long)(deadline % 1000000000ULL) };
            pthread_cond_timedwait(&n->cv, &n->mu, &wait);
#endif
            continue; /* recompute after every hint or spurious wake */
        }
        n->dirty = false;
        pthread_mutex_unlock(&n->mu);
        int rc = md_mcp_notify_tree_changed(n->server);
        pthread_mutex_lock(&n->mu);
        if (rc < 0 && !n->stop && ++failures < 3) {
            n->dirty = true;
            retry_ns = notify_now_ns() + 200000000ULL;
        } else {
            /* Success starts a fresh burst; the third failure drops this
             * burst, including hints accumulated during its failed writes. */
            if (rc < 0) n->dirty = false;
            failures = 0;
            retry_ns = 0;
        }
    }
    pthread_mutex_unlock(&n->mu);
    return NULL;
}

MdMcpNotifyCoalescer *md_mcp_notify_coalescer_create(MdMcpServer *server,
                                                  uint32_t quiet_ms)
{
    if (!server || !quiet_ms) return NULL;
    MdMcpNotifyCoalescer *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->server = server;
    n->quiet_ms = quiet_ms > 200 ? 200 : quiet_ms;
    if (pthread_mutex_init(&n->mu, NULL) != 0) goto fail;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) goto fail_mu;
#ifndef __APPLE__
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        goto fail_mu;
    }
#endif
    int rc = pthread_cond_init(&n->cv, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) goto fail_mu;
    if (pthread_create(&n->thread, NULL, notify_worker, n) != 0) {
        pthread_cond_destroy(&n->cv);
        goto fail_mu;
    }
    return n;
fail_mu:
    pthread_mutex_destroy(&n->mu);
fail:
    free(n);
    return NULL;
}

void md_mcp_notify_coalescer_destroy(MdMcpNotifyCoalescer *n)
{
    if (!n) return;
    pthread_mutex_lock(&n->mu);
    n->stop = true;
    pthread_cond_signal(&n->cv);
    pthread_mutex_unlock(&n->mu);
    pthread_join(n->thread, NULL);
    pthread_cond_destroy(&n->cv);
    pthread_mutex_destroy(&n->mu);
    free(n);
}

static void notify_dirty(MdMcpNotifyCoalescer *n)
{
    pthread_mutex_lock(&n->mu);
    uint64_t now = notify_now_ns();
    if (!n->dirty) {
        n->first_ns = now;
        n->dirty = true;
        pthread_cond_signal(&n->cv);
    }
    n->last_ns = now;
    pthread_mutex_unlock(&n->mu);
}

int md_mcp_notify_tree_changed(MdMcpServer *server)
{
    if (!server) return -1;
    return md_mcp_server_notify_resource_updated(server, MD_MCP_URI_UI_TREE);
}

void md_mcp_a11y_change_cb(const MdA11yDelta *deltas, int count,
                           void *userdata)
{
    (void)deltas;
    (void)count;
    MdMcpResourceCtx *ctx = userdata;
    if (!ctx) return;
    md_a11y_snapshot_invalidate(ctx->snapshot);
    if (ctx->notifier)
        notify_dirty(ctx->notifier);
    else
        md_mcp_notify_tree_changed(ctx->server);
}

/* ── Registration ────────────────────────────────────────────── */

int md_mcp_register_resources(MdMcpServer *server, MdMcpResourceCtx *res_ctx)
{
    if (!server || !res_ctx) return -1;

    MdMcpResource ui_tree = {
        .uri = MD_MCP_URI_UI_TREE,
        .name = "UI Tree",
        .description = "Current accessibility tree of the remote desktop. "
                       "Returns the full tree in the negotiated format "
                       "(JSON or compact).",
        .mime_type = res_ctx->tree_format == MD_TREE_FORMAT_COMPACT
                         ? "text/plain" : "application/json",
        .read_handler = read_ui_tree,
        .userdata = res_ctx,
    };
    if (md_mcp_server_register_resource(server, &ui_tree) != 0)
        return -1;

    MdMcpResource session_info = {
        .uri = MD_MCP_URI_SESSION_INFO,
        .name = "Session Info",
        .description = "Current session metadata: state, capabilities, "
                       "peer identity, and action count.",
        .mime_type = "application/json",
        .read_handler = read_session_info,
        .userdata = res_ctx,
    };
    if (md_mcp_server_register_resource(server, &session_info) != 0)
        return -1;

    return 0;
}
