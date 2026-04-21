/*
 * metadesk — a11y.c
 * Platform-agnostic accessibility tree convenience API and serialization.
 *
 * Delegates tree walking/diffing to the backend vtable returned by
 * md_a11y_backend_create(). Serialization to JSON, compact, and delta
 * formats is implemented here (platform-independent).
 */
#include "a11y.h"

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Public convenience API ──────────────────────────────────── */

MdA11yCtx *md_a11y_create(void) {
    const MdA11yBackend *vtable = md_a11y_backend_create();
    if (!vtable) return NULL;

    MdA11yCtx *ctx = calloc(1, sizeof(MdA11yCtx));
    if (!ctx) return NULL;

    ctx->vtable = vtable;

    if (ctx->vtable->init(ctx) != 0) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

MdA11yNode *md_a11y_walk(MdA11yCtx *ctx) {
    if (!ctx || !ctx->vtable || !ctx->vtable->get_tree) return NULL;

    MdA11yNode *root = NULL;
    if (ctx->vtable->get_tree(ctx, &root) != 0)
        return NULL;
    return root;
}

MdA11yDelta *md_a11y_diff(MdA11yCtx *ctx, int *delta_count) {
    if (!ctx || !ctx->vtable || !ctx->vtable->get_diff || !delta_count)
        return NULL;

    *delta_count = 0;
    MdA11yDelta *deltas = NULL;
    if (ctx->vtable->get_diff(ctx, &deltas, delta_count) != 0)
        return NULL;
    return deltas;
}

int md_a11y_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb, void *userdata) {
    if (!ctx || !ctx->vtable || !ctx->vtable->subscribe_changes)
        return -1;
    return ctx->vtable->subscribe_changes(ctx, cb, userdata);
}

bool md_a11y_is_connected(const MdA11yCtx *ctx) {
    return ctx && ctx->backend_data; /* backends set backend_data on init */
}

void md_a11y_destroy(MdA11yCtx *ctx) {
    if (!ctx) return;
    if (ctx->vtable && ctx->vtable->destroy)
        ctx->vtable->destroy(ctx);
    free(ctx);
}

/* ── Memory management ───────────────────────────────────────── */

void md_a11y_node_free(MdA11yNode *node) {
    if (!node) return;
    free(node->id);
    free(node->label);
    free(node->role);
    for (int i = 0; i < node->state_count; i++)
        free(node->states[i]);
    free(node->states);
    for (int i = 0; i < node->child_count; i++)
        md_a11y_node_free(node->children[i]);
    free(node->children);
    free(node);
}

void md_a11y_delta_free(MdA11yDelta *deltas, int count) {
    if (!deltas) return;
    for (int i = 0; i < count; i++) {
        md_a11y_node_free(deltas[i].node);
        free(deltas[i].parent_id);
    }
    free(deltas);
}

/* ── JSON serialization (spec §3.3.1) ──────────────────────── */

static cJSON *node_to_json(const MdA11yNode *node) {
    if (!node) return NULL;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    if (node->id)    cJSON_AddStringToObject(obj, "id", node->id);
    if (node->role)  cJSON_AddStringToObject(obj, "role", node->role);
    if (node->label) cJSON_AddStringToObject(obj, "label", node->label);

    /* States array */
    if (node->state_count > 0 && node->states) {
        cJSON *states = cJSON_CreateArray();
        for (int i = 0; i < node->state_count; i++) {
            if (node->states[i])
                cJSON_AddItemToArray(states, cJSON_CreateString(node->states[i]));
        }
        cJSON_AddItemToObject(obj, "state", states);
    }

    /* Bounds */
    cJSON *bounds = cJSON_CreateObject();
    cJSON_AddNumberToObject(bounds, "x", node->x);
    cJSON_AddNumberToObject(bounds, "y", node->y);
    cJSON_AddNumberToObject(bounds, "w", node->w);
    cJSON_AddNumberToObject(bounds, "h", node->h);
    cJSON_AddItemToObject(obj, "bounds", bounds);

    /* Children */
    if (node->child_count > 0 && node->children) {
        cJSON *children = cJSON_CreateArray();
        for (int i = 0; i < node->child_count; i++) {
            cJSON *child = node_to_json(node->children[i]);
            if (child) cJSON_AddItemToArray(children, child);
        }
        cJSON_AddItemToObject(obj, "children", children);
    } else {
        cJSON_AddItemToObject(obj, "children", cJSON_CreateArray());
    }

    return obj;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

char *md_a11y_to_json(const MdA11yNode *root) {
    if (!root) return NULL;

    cJSON *doc = cJSON_CreateObject();
    if (!doc) return NULL;

    cJSON_AddNumberToObject(doc, "v", 1);
    cJSON_AddNumberToObject(doc, "ts", (double)now_ms());
    cJSON_AddItemToObject(doc, "root", node_to_json(root));

    char *str = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    return str;
}

/* ── Compact format (spec §3.3.2) ──────────────────────────── */

/*
 * Compact interactable list — token-efficient format for agent clients.
 *
 * Only interactable elements are emitted (buttons, text entries, menus,
 * checkboxes, etc.). Container nodes (windows, dialogs, apps) provide
 * structural context. Decorative/structural nodes (panels, separators,
 * scroll bars, images, labels) are skipped but their children are still
 * walked — an interactable nested under a panel still appears.
 *
 * Format per spec §3.3.2:
 *   WIN[1] gedit - untitled
 *     BTN[42] Save *enabled*
 *     TXT[44] <focused> 'Hello world...'
 *     MNU[45] File
 */

/* Role abbreviation map */
static const char *role_abbrev(const char *role) {
    if (!role) return "???";
    if (strcmp(role, "frame") == 0 || strcmp(role, "window") == 0) return "WIN";
    if (strcmp(role, "application") == 0) return "APP";
    if (strcmp(role, "desktop frame") == 0 || strcmp(role, "desktop") == 0) return "DSK";
    if (strcmp(role, "push button") == 0 || strcmp(role, "button") == 0) return "BTN";
    if (strcmp(role, "text") == 0 || strcmp(role, "entry") == 0) return "TXT";
    if (strcmp(role, "menu") == 0 || strcmp(role, "menu bar") == 0) return "MNU";
    if (strcmp(role, "menu item") == 0) return "MNI";
    if (strcmp(role, "check box") == 0) return "CHK";
    if (strcmp(role, "radio button") == 0) return "RAD";
    if (strcmp(role, "combo box") == 0) return "CMB";
    if (strcmp(role, "list") == 0) return "LST";
    if (strcmp(role, "list item") == 0) return "LI";
    if (strcmp(role, "tab") == 0) return "TAB";
    if (strcmp(role, "panel") == 0 || strcmp(role, "filler") == 0) return "PNL";
    if (strcmp(role, "label") == 0) return "LBL";
    if (strcmp(role, "scroll bar") == 0) return "SCR";
    if (strcmp(role, "separator") == 0) return "SEP";
    if (strcmp(role, "tool bar") == 0) return "TBR";
    if (strcmp(role, "tree") == 0) return "TRE";
    if (strcmp(role, "table") == 0) return "TBL";
    if (strcmp(role, "image") == 0) return "IMG";
    if (strcmp(role, "link") == 0) return "LNK";
    if (strcmp(role, "status bar") == 0) return "STS";
    if (strcmp(role, "dialog") == 0) return "DLG";
    if (strcmp(role, "page tab") == 0) return "PTB";
    if (strcmp(role, "page tab list") == 0) return "PTL";
    if (strcmp(role, "split pane") == 0) return "SPL";
    if (strcmp(role, "toggle button") == 0) return "TGL";
    if (strcmp(role, "slider") == 0) return "SLD";
    if (strcmp(role, "progress bar") == 0) return "PRG";
    if (strcmp(role, "spin button") == 0) return "SPN";
    return "UNK";
}

/*
 * Interactable classification. Returns true if the node should be
 * emitted in compact output. Container roles (WIN, APP, DSK, DLG)
 * are always emitted for structural context. Leaf interactable roles
 * (BTN, TXT, MNU, CHK, etc.) are the primary targets. Decorative
 * roles (PNL, SCR, SEP, IMG, LBL, STS) are skipped — but their
 * children are still walked.
 */
static bool is_interactable(const char *role) {
    if (!role) return false;
    /* Containers — always show for context */
    if (strcmp(role, "frame") == 0 || strcmp(role, "window") == 0) return true;
    if (strcmp(role, "application") == 0) return true;
    if (strcmp(role, "desktop frame") == 0 || strcmp(role, "desktop") == 0) return true;
    if (strcmp(role, "dialog") == 0) return true;
    /* Interactive controls — the primary targets */
    if (strcmp(role, "push button") == 0 || strcmp(role, "button") == 0) return true;
    if (strcmp(role, "toggle button") == 0) return true;
    if (strcmp(role, "text") == 0 || strcmp(role, "entry") == 0) return true;
    if (strcmp(role, "menu") == 0 || strcmp(role, "menu bar") == 0) return true;
    if (strcmp(role, "menu item") == 0) return true;
    if (strcmp(role, "check box") == 0) return true;
    if (strcmp(role, "radio button") == 0) return true;
    if (strcmp(role, "combo box") == 0) return true;
    if (strcmp(role, "list item") == 0) return true;
    if (strcmp(role, "tab") == 0) return true;
    if (strcmp(role, "page tab") == 0) return true;
    if (strcmp(role, "link") == 0) return true;
    if (strcmp(role, "slider") == 0) return true;
    if (strcmp(role, "spin button") == 0) return true;
    if (strcmp(role, "tree") == 0) return true;
    if (strcmp(role, "table") == 0) return true;
    if (strcmp(role, "list") == 0) return true;
    /* Everything else is decorative / structural — skip it */
    return false;
}

/* Returns true if the role is a text input (content should be quoted) */
static bool is_text_role(const char *role) {
    if (!role) return false;
    return strcmp(role, "text") == 0 || strcmp(role, "entry") == 0;
}

static int has_state(const MdA11yNode *node, const char *state) {
    if (!node->states) return 0;
    for (int i = 0; i < node->state_count; i++) {
        if (node->states[i] && strcmp(node->states[i], state) == 0)
            return 1;
    }
    return 0;
}

/* Append state annotations. Returns chars written. */
static int compact_states(const MdA11yNode *node, char *buf, size_t buf_len) {
    size_t written = 0;
    int n;

    /* State annotations in priority order */
    static const struct { const char *state; const char *fmt; } state_map[] = {
        { "focused",  " <focused>" },
        { "enabled",  " *enabled*" },
        { "disabled", " *disabled*" },
        { "checked",  " *checked*" },
        { "selected", " *selected*" },
        { "pressed",  " *pressed*" },
        { "expanded", " *expanded*" },
    };

    for (size_t i = 0; i < sizeof(state_map) / sizeof(state_map[0]); i++) {
        if (has_state(node, state_map[i].state)) {
            n = snprintf(buf + written, buf_len - written, "%s", state_map[i].fmt);
            if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
        }
    }
    return (int)written;
}

/*
 * Append a node in compact format. Applies interactable filtering:
 * non-interactable nodes are skipped but their children are still
 * walked (so interactive elements nested under panels still appear).
 *
 * Returns chars written.
 */
static int compact_node(const MdA11yNode *node, int depth, char *buf, size_t buf_len) {
    if (!node || buf_len == 0) return 0;

    size_t written = 0;
    int n;
    bool emit = is_interactable(node->role);

    if (emit) {
        /* Indentation */
        for (int i = 0; i < depth; i++) {
            n = snprintf(buf + written, buf_len - written, "  ");
            if (n < 0 || (size_t)n >= buf_len - written) return (int)written;
            written += (size_t)n;
        }

        /* ROLE[id] */
        const char *abbr = role_abbrev(node->role);
        const char *id = node->id ? node->id : "?";
        n = snprintf(buf + written, buf_len - written, "%s[%s]", abbr, id);
        if (n < 0 || (size_t)n >= buf_len - written) return (int)written;
        written += (size_t)n;

        /* For text entries: <focused> before quoted content (spec §3.3.2) */
        if (is_text_role(node->role)) {
            if (has_state(node, "focused")) {
                n = snprintf(buf + written, buf_len - written, " <focused>");
                if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
            }
            /* Quote text content with single quotes */
            const char *label = node->label ? node->label : "";
            n = snprintf(buf + written, buf_len - written, " '%s'", label);
            if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
            /* Remaining states (exclude focused — already emitted) */
            if (has_state(node, "enabled")) {
                n = snprintf(buf + written, buf_len - written, " *enabled*");
                if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
            }
            if (has_state(node, "disabled")) {
                n = snprintf(buf + written, buf_len - written, " *disabled*");
                if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
            }
        } else {
            /* Non-text: label then states */
            const char *label = node->label ? node->label : "";
            if (label[0]) {
                n = snprintf(buf + written, buf_len - written, " %s", label);
                if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
            }
            written += (size_t)compact_states(node, buf + written, buf_len - written);
        }

        n = snprintf(buf + written, buf_len - written, "\n");
        if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
    }

    /* Always recurse children — interactables under skipped nodes still show.
     * When a node is skipped, children inherit the parent's depth. */
    int child_depth = emit ? depth + 1 : depth;
    for (int i = 0; i < node->child_count && node->children; i++) {
        int child_written = compact_node(node->children[i], child_depth,
                                         buf + written, buf_len - written);
        written += (size_t)child_written;
    }

    return (int)written;
}

char *md_a11y_to_compact(const MdA11yNode *root) {
    if (!root) return NULL;

    /* Allocate a generous buffer; real implementation would use dynamic sizing */
    size_t buf_size = 256 * 1024;  /* 256 KB — enough for deep trees */
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    int written = snprintf(buf, buf_size, "v1 ts:%lu\n", (unsigned long)now_ms());
    if (written < 0) { free(buf); return NULL; }
    written += compact_node(root, 0, buf + written, buf_size - (size_t)written);

    /* Trim to actual size */
    char *result = realloc(buf, (size_t)written + 1);
    return result ? result : buf;
}

/* ── Delta serialization (spec §3.3.3) ─────────────────────── */

static const char *op_str(MdA11yOp op) {
    switch (op) {
    case MD_A11Y_OP_ADD:    return "add";
    case MD_A11Y_OP_REMOVE: return "remove";
    case MD_A11Y_OP_UPDATE: return "update";
    }
    return "unknown";
}

char *md_a11y_delta_to_json(const MdA11yDelta *deltas, int count) {
    if (!deltas || count <= 0) return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "op", op_str(deltas[i].op));
        if (deltas[i].node) {
            cJSON *nj = node_to_json(deltas[i].node);
            if (nj) cJSON_AddItemToObject(entry, "node", nj);
        }
        if (deltas[i].parent_id)
            cJSON_AddStringToObject(entry, "parent_id", deltas[i].parent_id);
        cJSON_AddItemToArray(arr, entry);
    }

    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return str;
}

/* ── Delta application (spec §3.3.3, in-place patching) ────── */

/*
 * Find a node by "id" field in a JSON tree (recursive DFS).
 * Returns the cJSON object with matching id, or NULL.
 */
static cJSON *find_node_by_id(cJSON *node, const char *id) {
    if (!node || !id) return NULL;

    cJSON *node_id = cJSON_GetObjectItem(node, "id");
    if (node_id && cJSON_IsString(node_id) &&
        strcmp(node_id->valuestring, id) == 0) {
        return node;
    }

    cJSON *children = cJSON_GetObjectItem(node, "children");
    if (children && cJSON_IsArray(children)) {
        int count = cJSON_GetArraySize(children);
        for (int i = 0; i < count; i++) {
            cJSON *found = find_node_by_id(cJSON_GetArrayItem(children, i), id);
            if (found) return found;
        }
    }

    return NULL;
}

/*
 * Remove a child node by id from a parent's children array.
 * Searches recursively — removes from wherever it's found.
 * Returns true if found and removed.
 */
static bool remove_node_by_id(cJSON *node, const char *id) {
    if (!node || !id) return false;

    cJSON *children = cJSON_GetObjectItem(node, "children");
    if (!children || !cJSON_IsArray(children)) return false;

    int count = cJSON_GetArraySize(children);
    for (int i = 0; i < count; i++) {
        cJSON *child = cJSON_GetArrayItem(children, i);
        cJSON *child_id = cJSON_GetObjectItem(child, "id");
        if (child_id && cJSON_IsString(child_id) &&
            strcmp(child_id->valuestring, id) == 0) {
            cJSON_DeleteItemFromArray(children, i);
            return true;
        }
        /* Recurse into subtree */
        if (remove_node_by_id(child, id))
            return true;
    }

    return false;
}

/*
 * Update fields of an existing node with values from delta node.
 * Only updates fields that are present in the delta (partial update).
 */
static void update_node_fields(cJSON *target, const cJSON *source) {
    if (!target || !source) return;

    /* Update scalar fields if present in source */
    static const char *fields[] = {"role", "label"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        cJSON *src_field = cJSON_GetObjectItem(source, fields[i]);
        if (src_field && cJSON_IsString(src_field)) {
            cJSON_DeleteItemFromObject(target, fields[i]);
            cJSON_AddStringToObject(target, fields[i], src_field->valuestring);
        }
    }

    /* Update state array if present */
    cJSON *src_state = cJSON_GetObjectItem(source, "state");
    if (src_state && cJSON_IsArray(src_state)) {
        cJSON_DeleteItemFromObject(target, "state");
        cJSON_AddItemToObject(target, "state", cJSON_Duplicate(src_state, true));
    }

    /* Update bounds if present */
    cJSON *src_bounds = cJSON_GetObjectItem(source, "bounds");
    if (src_bounds && cJSON_IsObject(src_bounds)) {
        cJSON_DeleteItemFromObject(target, "bounds");
        cJSON_AddItemToObject(target, "bounds",
                              cJSON_Duplicate(src_bounds, true));
    }

    /* Update children if present (full replacement) */
    cJSON *src_children = cJSON_GetObjectItem(source, "children");
    if (src_children && cJSON_IsArray(src_children)) {
        cJSON_DeleteItemFromObject(target, "children");
        cJSON_AddItemToObject(target, "children",
                              cJSON_Duplicate(src_children, true));
    }
}

char *md_a11y_tree_patch(const char *tree_json, const char *delta_json) {
    if (!tree_json || !delta_json) return NULL;

    /* Parse tree document */
    cJSON *doc = cJSON_Parse(tree_json);
    if (!doc) return NULL;

    cJSON *root = cJSON_GetObjectItem(doc, "root");
    if (!root) {
        cJSON_Delete(doc);
        return NULL;
    }

    /* Parse delta array */
    cJSON *deltas = cJSON_Parse(delta_json);
    if (!deltas || !cJSON_IsArray(deltas)) {
        cJSON_Delete(deltas);
        cJSON_Delete(doc);
        return NULL;
    }

    /* Apply each delta operation */
    int delta_count = cJSON_GetArraySize(deltas);
    for (int i = 0; i < delta_count; i++) {
        cJSON *delta = cJSON_GetArrayItem(deltas, i);
        if (!delta) continue;

        cJSON *op = cJSON_GetObjectItem(delta, "op");
        if (!op || !cJSON_IsString(op)) continue;

        cJSON *delta_node = cJSON_GetObjectItem(delta, "node");
        const char *op_s = op->valuestring;

        if (strcmp(op_s, "add") == 0) {
            /* Add: insert node under parent_id */
            cJSON *parent_id_j = cJSON_GetObjectItem(delta, "parent_id");
            if (!parent_id_j || !cJSON_IsString(parent_id_j)) continue;
            if (!delta_node) continue;

            cJSON *parent = find_node_by_id(root, parent_id_j->valuestring);
            if (!parent) continue;

            cJSON *children = cJSON_GetObjectItem(parent, "children");
            if (!children) {
                children = cJSON_CreateArray();
                cJSON_AddItemToObject(parent, "children", children);
            }
            cJSON_AddItemToArray(children, cJSON_Duplicate(delta_node, true));

        } else if (strcmp(op_s, "remove") == 0) {
            /* Remove: delete node by id */
            if (!delta_node) continue;
            cJSON *del_id = cJSON_GetObjectItem(delta_node, "id");
            if (!del_id || !cJSON_IsString(del_id)) continue;

            remove_node_by_id(root, del_id->valuestring);

        } else if (strcmp(op_s, "update") == 0) {
            /* Update: modify existing node's fields */
            if (!delta_node) continue;
            cJSON *upd_id = cJSON_GetObjectItem(delta_node, "id");
            if (!upd_id || !cJSON_IsString(upd_id)) continue;

            cJSON *target = find_node_by_id(root, upd_id->valuestring);
            if (!target) continue;

            update_node_fields(target, delta_node);
        }
    }

    /* Update timestamp */
    cJSON_DeleteItemFromObject(doc, "ts");
    cJSON_AddNumberToObject(doc, "ts", (double)now_ms());

    char *result = cJSON_PrintUnformatted(doc);
    cJSON_Delete(deltas);
    cJSON_Delete(doc);
    return result;
}
