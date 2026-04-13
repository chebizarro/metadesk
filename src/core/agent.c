/*
 * metadesk — agent.c
 * Agent action handler: the host-side orchestrator for agent interactions.
 *
 * Flow per action:
 *   1. Parse MD_PKT_ACTION JSON → MdAction
 *   2. If action has target_id, resolve to screen coordinates by
 *      walking the AT-SPI2 tree and finding the matching node
 *   3. Inject the action via md_input_execute_action()
 *   4. Wait settle_ms for the UI to update
 *   5. Compute AT-SPI2 tree delta
 *   6. Send MD_PKT_UI_TREE_DELTA (or full tree if delta is too large)
 *
 * Target resolution: AT-SPI2 nodes are identified by their node ID
 * (e.g. "n42"). The agent handler searches the current tree snapshot
 * to find the node's bounding box center, then uses those coordinates
 * for mouse-based actions (click, dbl_click, right_click).
 */
#include "agent.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Maximum delta JSON size before falling back to full tree (64 KB) */
#define MD_AGENT_MAX_DELTA_SIZE (64 * 1024)

struct MdAgent {
    MdAtspiTree  *atspi;
    MdInput      *input;
    MdTreeFormat  tree_format;
    uint32_t      settle_ms;
    uint32_t      action_count;

    /* Cached last tree for target resolution */
    MdAtspiNode  *cached_tree;
};

/* ── Target resolution ───────────────────────────────────────── */

/* Recursively search for a node with matching ID. */
static const MdAtspiNode *find_node_by_id(const MdAtspiNode *node,
                                           const char *target_id) {
    if (!node || !target_id) return NULL;

    if (node->id && strcmp(node->id, target_id) == 0)
        return node;

    for (int i = 0; i < node->child_count && node->children; i++) {
        const MdAtspiNode *found = find_node_by_id(node->children[i], target_id);
        if (found) return found;
    }

    return NULL;
}

/* Resolve a target_id to the center of the node's bounding box.
 * Returns 0 on success, -1 if node not found. */
static int resolve_target(MdAgent *agent, const char *target_id,
                          int *x_out, int *y_out) {
    if (!target_id || target_id[0] == '\0')
        return -1;

    /* Ensure we have a cached tree */
    if (!agent->cached_tree && agent->atspi) {
        agent->cached_tree = md_atspi_walk(agent->atspi);
    }

    if (!agent->cached_tree)
        return -1;

    const MdAtspiNode *node = find_node_by_id(agent->cached_tree, target_id);
    if (!node)
        return -1;

    /* Return center of bounding box */
    *x_out = node->x + node->w / 2;
    *y_out = node->y + node->h / 2;
    return 0;
}

/* ── Sleep helper ────────────────────────────────────────────── */

static void sleep_ms(uint32_t ms) {
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/* ── Serialize tree in the negotiated format ─────────────────── */

static char *serialize_tree(const MdAtspiNode *root, MdTreeFormat fmt) {
    if (!root) return NULL;

    switch (fmt) {
    case MD_TREE_FORMAT_JSON:
        return md_atspi_to_json(root);
    case MD_TREE_FORMAT_COMPACT:
        return md_atspi_to_compact(root);
    }
    return md_atspi_to_json(root); /* default fallback */
}

/* ── Public API ──────────────────────────────────────────────── */

MdAgent *md_agent_create(const MdAgentConfig *cfg) {
    if (!cfg) return NULL;

    MdAgent *agent = calloc(1, sizeof(MdAgent));
    if (!agent) return NULL;

    agent->atspi       = cfg->atspi;
    agent->input       = cfg->input;
    agent->tree_format = cfg->tree_format;
    agent->settle_ms   = cfg->settle_ms > 0 ? cfg->settle_ms
                                             : MD_AGENT_DEFAULT_SETTLE_MS;

    return agent;
}

int md_agent_handle_action(MdAgent *agent, MdStream *stream,
                           uint32_t *seq,
                           const uint8_t *payload, uint32_t payload_len) {
    if (!agent || !stream || !seq || !payload || payload_len == 0)
        return -1;

    /* 1. Parse the action JSON */
    MdAction action;
    memset(&action, 0, sizeof(action));

    int ret = md_action_parse(&action, (const char *)payload, payload_len);
    if (ret < 0) {
        fprintf(stderr, "agent: failed to parse action JSON\n");
        return -1;
    }

    fprintf(stderr, "agent: action=%s target=%s\n",
            md_action_type_str(action.type),
            action.target_id[0] ? action.target_id : "(none)");

    /* 2. Resolve target_id to screen coordinates if needed */
    if (action.target_id[0] != '\0') {
        int tx, ty;
        if (resolve_target(agent, action.target_id, &tx, &ty) == 0) {
            /* For mouse-based actions, use resolved coordinates.
             * The action's region[0]/region[1] are used for click coords. */
            switch (action.type) {
            case MD_ACTION_CLICK:
            case MD_ACTION_DBL_CLICK:
            case MD_ACTION_RIGHT_CLICK:
                action.region[0] = tx;
                action.region[1] = ty;
                break;
            case MD_ACTION_FOCUS:
                /* Focus by clicking the element center */
                action.region[0] = tx;
                action.region[1] = ty;
                action.type = MD_ACTION_CLICK;
                break;
            default:
                break;
            }
        } else {
            fprintf(stderr, "agent: could not resolve target '%s'\n",
                    action.target_id);
            /* Continue anyway — some actions (key_combo, type) don't need coords */
        }
    }

    /* 3. Inject the action */
    if (agent->input) {
        ret = md_input_execute_action(agent->input, &action);
        if (ret < 0) {
            fprintf(stderr, "agent: action injection failed\n");
            /* Don't return error — still send delta */
        }
    }

    md_action_cleanup(&action);
    agent->action_count++;

    /* 4. Wait for UI to settle */
    sleep_ms(agent->settle_ms);

    /* 5. Invalidate cached tree (UI changed) */
    if (agent->cached_tree) {
        md_atspi_node_free(agent->cached_tree);
        agent->cached_tree = NULL;
    }

    /* 6. Compute and send delta */
    if (agent->atspi) {
        int delta_count = 0;
        MdAtspiDelta *deltas = md_atspi_diff(agent->atspi, &delta_count);

        if (deltas && delta_count > 0) {
            /* Serialize delta */
            char *delta_json = md_atspi_delta_to_json(deltas, delta_count);
            if (delta_json) {
                size_t len = strlen(delta_json);

                if (len < MD_AGENT_MAX_DELTA_SIZE) {
                    /* Send as delta */
                    md_stream_send(stream, MD_PKT_UI_TREE_DELTA, (*seq)++,
                                   (const uint8_t *)delta_json, (uint32_t)len);
                } else {
                    /* Delta too large — send full tree instead */
                    free(delta_json);
                    md_agent_send_tree(agent, stream, seq);
                    md_atspi_delta_free(deltas, delta_count);
                    return 0;
                }
                free(delta_json);
            }
            md_atspi_delta_free(deltas, delta_count);
        } else {
            /* No delta available or no changes detected.
             * Send a full tree on the first interaction. */
            md_agent_send_tree(agent, stream, seq);
        }
    }

    return 0;
}

int md_agent_send_tree(MdAgent *agent, MdStream *stream, uint32_t *seq) {
    if (!agent || !stream || !seq || !agent->atspi)
        return -1;

    /* Walk the full tree */
    MdAtspiNode *root = md_atspi_walk(agent->atspi);
    if (!root)
        return -1;

    /* Update cached tree */
    if (agent->cached_tree)
        md_atspi_node_free(agent->cached_tree);
    agent->cached_tree = NULL; /* walk returns a new tree; we don't cache it
                                  since the caller's tree is separate */

    /* Serialize in negotiated format */
    char *data = serialize_tree(root, agent->tree_format);
    md_atspi_node_free(root);

    if (!data)
        return -1;

    /* Send as MD_PKT_UI_TREE */
    size_t len = strlen(data);
    int ret = md_stream_send(stream, MD_PKT_UI_TREE, (*seq)++,
                             (const uint8_t *)data, (uint32_t)len);
    free(data);
    return ret;
}

int md_agent_send_delta(MdAgent *agent, MdStream *stream, uint32_t *seq) {
    if (!agent || !stream || !seq || !agent->atspi)
        return -1;

    int delta_count = 0;
    MdAtspiDelta *deltas = md_atspi_diff(agent->atspi, &delta_count);

    if (!deltas || delta_count == 0)
        return 0; /* no changes */

    char *json = md_atspi_delta_to_json(deltas, delta_count);
    md_atspi_delta_free(deltas, delta_count);

    if (!json)
        return -1;

    size_t len = strlen(json);
    int ret = md_stream_send(stream, MD_PKT_UI_TREE_DELTA, (*seq)++,
                             (const uint8_t *)json, (uint32_t)len);
    free(json);
    return ret;
}

uint32_t md_agent_get_action_count(const MdAgent *agent) {
    return agent ? agent->action_count : 0;
}

void md_agent_destroy(MdAgent *agent) {
    if (!agent) return;

    if (agent->cached_tree)
        md_atspi_node_free(agent->cached_tree);

    free(agent);
}
