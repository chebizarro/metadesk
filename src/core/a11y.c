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

static int has_state(const MdA11yNode *node, const char *state) {
    if (!node->states) return 0;
    for (int i = 0; i < node->state_count; i++) {
        if (node->states[i] && strcmp(node->states[i], state) == 0)
            return 1;
    }
    return 0;
}

/* Append a node in compact format. Returns chars written. */
static int compact_node(const MdA11yNode *node, int depth, char *buf, size_t buf_len) {
    if (!node || buf_len == 0) return 0;

    size_t written = 0;
    int n;

    /* Indentation */
    for (int i = 0; i < depth; i++) {
        n = snprintf(buf + written, buf_len - written, "  ");
        if (n < 0 || (size_t)n >= buf_len - written) return (int)written;
        written += (size_t)n;
    }

    /* ROLE[id] label *states* */
    const char *abbr = role_abbrev(node->role);
    const char *id = node->id ? node->id : "?";
    const char *label = node->label ? node->label : "";

    n = snprintf(buf + written, buf_len - written, "%s[%s] %s", abbr, id, label);
    if (n < 0 || (size_t)n >= buf_len - written) return (int)written;
    written += (size_t)n;

    /* State annotations */
    if (has_state(node, "focused")) {
        n = snprintf(buf + written, buf_len - written, " <focused>");
        if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
    }
    if (has_state(node, "enabled")) {
        n = snprintf(buf + written, buf_len - written, " *enabled*");
        if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;
    }

    n = snprintf(buf + written, buf_len - written, "\n");
    if (n > 0 && (size_t)n < buf_len - written) written += (size_t)n;

    /* Recurse children */
    for (int i = 0; i < node->child_count && node->children; i++) {
        int child_written = compact_node(node->children[i], depth + 1,
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
