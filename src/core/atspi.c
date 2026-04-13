/*
 * metadesk — atspi.c
 * AT-SPI2 tree walking + serialization formats.
 * Tree walking is stub (M1.6), but serialization is implemented.
 * See spec §3.3.
 */
#include "atspi.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct MdAtspiTree {
    int connected;
    MdAtspiNode *last_snapshot; /* for delta computation */
};

/* ── Tree context lifecycle ─────────────────────────────────── */

MdAtspiTree *md_atspi_create(void) {
    MdAtspiTree *tree = calloc(1, sizeof(MdAtspiTree));
    /* TODO: connect to AT-SPI2 bus (M1.6) */
    return tree;
}

MdAtspiNode *md_atspi_walk(MdAtspiTree *tree) {
    if (!tree) return NULL;
    /* TODO: walk accessibility tree (M1.6) */
    return NULL;
}

MdAtspiDelta *md_atspi_diff(MdAtspiTree *tree, int *delta_count) {
    if (!tree) return NULL;
    if (delta_count) *delta_count = 0;
    /* TODO: compute delta (M1.6) */
    return NULL;
}

/* ── JSON serialization (spec §3.3.1) ──────────────────────── */

static cJSON *node_to_json(const MdAtspiNode *node) {
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

char *md_atspi_to_json(const MdAtspiNode *root) {
    if (!root) return NULL;

    cJSON *doc = cJSON_CreateObject();
    if (!doc) return NULL;

    cJSON_AddNumberToObject(doc, "v", 1);
    cJSON_AddNumberToObject(doc, "ts", 0); /* TODO: real timestamp */
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
    return "UNK";
}

static int has_state(const MdAtspiNode *node, const char *state) {
    if (!node->states)
        return 0;
    for (int i = 0; i < node->state_count; i++) {
        if (node->states[i] && strcmp(node->states[i], state) == 0)
            return 1;
    }
    return 0;
}

/* Append a node in compact format. Returns chars written. */
static int compact_node(const MdAtspiNode *node, int depth, char *buf, size_t buf_len) {
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
        int child_written = compact_node(node->children[i], depth + 1, buf + written, buf_len - written);
        written += (size_t)child_written;
    }

    return (int)written;
}

char *md_atspi_to_compact(const MdAtspiNode *root) {
    if (!root) return NULL;

    /* Allocate a generous buffer; real implementation would use dynamic sizing */
    size_t buf_size = 64 * 1024;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    int written = snprintf(buf, buf_size, "v1 ts:0\n"); /* TODO: real timestamp */
    written += compact_node(root, 0, buf + written, buf_size - written);

    /* Trim to actual size */
    char *result = realloc(buf, written + 1);
    return result ? result : buf;
}

/* ── Delta serialization ───────────────────────────────────── */

static const char *op_str(MdAtspiOp op) {
    switch (op) {
        case MD_ATSPI_OP_ADD:    return "add";
        case MD_ATSPI_OP_REMOVE: return "remove";
        case MD_ATSPI_OP_UPDATE: return "update";
    }
    return "unknown";
}

char *md_atspi_delta_to_json(const MdAtspiDelta *deltas, int count) {
    if (!deltas || count <= 0) return NULL;

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "op", op_str(deltas[i].op));
        if (deltas[i].node) {
            cJSON *node_json = node_to_json(deltas[i].node);
            if (node_json) cJSON_AddItemToObject(entry, "node", node_json);
        }
        if (deltas[i].parent_id)
            cJSON_AddStringToObject(entry, "parent_id", deltas[i].parent_id);
        cJSON_AddItemToArray(arr, entry);
    }

    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return str;
}

/* ── Memory management ─────────────────────────────────────── */

void md_atspi_node_free(MdAtspiNode *node) {
    if (!node) return;
    free(node->id);
    free(node->label);
    free(node->role);
    for (int i = 0; i < node->state_count; i++)
        free(node->states[i]);
    free(node->states);
    for (int i = 0; i < node->child_count; i++)
        md_atspi_node_free(node->children[i]);
    free(node->children);
    free(node);
}

void md_atspi_delta_free(MdAtspiDelta *deltas, int count) {
    if (!deltas) return;
    for (int i = 0; i < count; i++) {
        md_atspi_node_free(deltas[i].node);
        free(deltas[i].parent_id);
    }
    free(deltas);
}

void md_atspi_destroy(MdAtspiTree *tree) {
    if (!tree) return;
    md_atspi_node_free(tree->last_snapshot);
    free(tree);
}
