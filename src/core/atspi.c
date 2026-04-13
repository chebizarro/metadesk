/*
 * metadesk — atspi.c
 * AT-SPI2 accessibility tree walking + serialization formats.
 *
 * Uses libatspi-2.0 to:
 *   1. Connect to the AT-SPI2 D-Bus registry
 *   2. Walk the accessibility tree from the desktop root
 *   3. Extract node attributes: id, role, label, states, bounds
 *   4. Build an MdAtspiNode tree matching our wire format
 *   5. Compute deltas by comparing current vs previous snapshot
 *
 * Serialization to JSON (§3.3.1), compact (§3.3.2), and delta
 * (§3.3.3) formats is also implemented here.
 *
 * OQ-1 investigation: AT-SPI2 supports both polling (walk full tree)
 * and event-based updates (register for state-change, children-changed,
 * etc.). Phase 1 uses polling; event registration will be added when
 * empirical data shows which apps need it.
 *
 * See spec §3.3 and §10 for tree formats and agent API.
 */
#include "atspi.h"

#include <atspi/atspi.h>

#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Maximum tree depth to prevent infinite recursion from cyclic trees */
#define MD_ATSPI_MAX_DEPTH 32

/* Maximum children per node to prevent OOM on enormous trees */
#define MD_ATSPI_MAX_CHILDREN 256

struct MdAtspiTree {
    int          connected;
    uint64_t     next_id;        /* monotonic ID counter for node IDs     */
    MdAtspiNode *last_snapshot;  /* previous tree for delta computation   */
};

/* ── Internal: convert AtspiAccessible to MdAtspiNode ────────── */

/* Generate a stable-ish node ID. In Phase 1 we use a monotonic counter
 * prefixed with "n". True stability requires tracking AT-SPI2 object
 * paths across walks, which is a Phase 2 enhancement. */
static char *make_node_id(MdAtspiTree *tree) {
    char buf[32];
    snprintf(buf, sizeof(buf), "n%lu", (unsigned long)tree->next_id++);
    return strdup(buf);
}

/* Extract state names from an AtspiStateSet. */
static void extract_states(AtspiStateSet *state_set, MdAtspiNode *node) {
    if (!state_set) return;

    GArray *states = atspi_state_set_get_states(state_set);
    if (!states || states->len == 0) {
        if (states) g_array_free(states, TRUE);
        return;
    }

    node->state_count = (int)states->len;
    node->states = calloc((size_t)node->state_count, sizeof(char *));
    if (!node->states) {
        node->state_count = 0;
        g_array_free(states, TRUE);
        return;
    }

    for (guint i = 0; i < states->len; i++) {
        AtspiStateType st = g_array_index(states, AtspiStateType, i);
        const char *name = NULL;

        /* Map AT-SPI2 state enum to string names */
        switch (st) {
        case ATSPI_STATE_ACTIVE:     name = "active";     break;
        case ATSPI_STATE_ENABLED:    name = "enabled";    break;
        case ATSPI_STATE_FOCUSED:    name = "focused";    break;
        case ATSPI_STATE_VISIBLE:    name = "visible";    break;
        case ATSPI_STATE_SHOWING:    name = "showing";    break;
        case ATSPI_STATE_SELECTED:   name = "selected";   break;
        case ATSPI_STATE_CHECKED:    name = "checked";    break;
        case ATSPI_STATE_EDITABLE:   name = "editable";   break;
        case ATSPI_STATE_EXPANDABLE: name = "expandable"; break;
        case ATSPI_STATE_EXPANDED:   name = "expanded";   break;
        case ATSPI_STATE_FOCUSABLE:  name = "focusable";  break;
        case ATSPI_STATE_SENSITIVE:  name = "sensitive";  break;
        case ATSPI_STATE_SELECTABLE: name = "selectable"; break;
        case ATSPI_STATE_MODAL:      name = "modal";      break;
        case ATSPI_STATE_MULTI_LINE: name = "multiline";  break;
        case ATSPI_STATE_SINGLE_LINE:name = "singleline"; break;
        case ATSPI_STATE_DEFUNCT:    name = "defunct";     break;
        default: break;
        }

        if (name)
            node->states[i] = strdup(name);
    }

    g_array_free(states, TRUE);
}

/* Recursively walk an AtspiAccessible and build an MdAtspiNode tree. */
static MdAtspiNode *walk_accessible(MdAtspiTree *tree, AtspiAccessible *acc,
                                    int depth) {
    if (!acc || depth > MD_ATSPI_MAX_DEPTH)
        return NULL;

    GError *error = NULL;

    MdAtspiNode *node = calloc(1, sizeof(MdAtspiNode));
    if (!node) return NULL;

    /* Node ID */
    node->id = make_node_id(tree);

    /* Role */
    gchar *role_name = atspi_accessible_get_role_name(acc, &error);
    if (role_name) {
        node->role = strdup(role_name);
        g_free(role_name);
    }
    if (error) { g_error_free(error); error = NULL; }

    /* Label (accessible name) */
    gchar *name = atspi_accessible_get_name(acc, &error);
    if (name && name[0] != '\0') {
        node->label = strdup(name);
    }
    if (name) g_free(name);
    if (error) { g_error_free(error); error = NULL; }

    /* States */
    AtspiStateSet *state_set = atspi_accessible_get_state_set(acc);
    if (state_set) {
        extract_states(state_set, node);
        g_object_unref(state_set);
    }

    /* Bounds (component interface) */
    AtspiComponent *comp = atspi_accessible_get_component_iface(acc);
    if (comp) {
        AtspiRect *extent = atspi_component_get_extents(
            comp, ATSPI_COORD_TYPE_SCREEN, &error);
        if (extent) {
            node->x = extent->x;
            node->y = extent->y;
            node->w = extent->width;
            node->h = extent->height;
            g_free(extent);
        }
        if (error) { g_error_free(error); error = NULL; }
        g_object_unref(comp);
    }

    /* Children */
    int child_count = atspi_accessible_get_child_count(acc, &error);
    if (error) { g_error_free(error); error = NULL; child_count = 0; }

    if (child_count > MD_ATSPI_MAX_CHILDREN)
        child_count = MD_ATSPI_MAX_CHILDREN;

    if (child_count > 0) {
        node->children = calloc((size_t)child_count, sizeof(MdAtspiNode *));
        if (node->children) {
            int actual = 0;
            for (int i = 0; i < child_count; i++) {
                AtspiAccessible *child = atspi_accessible_get_child_at_index(
                    acc, i, &error);
                if (error) { g_error_free(error); error = NULL; }
                if (!child) continue;

                /* Skip defunct children */
                AtspiStateSet *child_states = atspi_accessible_get_state_set(child);
                bool defunct = false;
                if (child_states) {
                    defunct = atspi_state_set_contains(child_states, ATSPI_STATE_DEFUNCT);
                    g_object_unref(child_states);
                }

                if (!defunct) {
                    MdAtspiNode *child_node = walk_accessible(tree, child, depth + 1);
                    if (child_node)
                        node->children[actual++] = child_node;
                }
                g_object_unref(child);
            }
            node->child_count = actual;
        }
    }

    return node;
}

/* ── Tree context lifecycle ─────────────────────────────────── */

MdAtspiTree *md_atspi_create(void) {
    MdAtspiTree *tree = calloc(1, sizeof(MdAtspiTree));
    if (!tree) return NULL;

    /* Initialize AT-SPI2 client library.
     * atspi_init() connects to the D-Bus accessibility bus.
     * Returns 0 on success, 1 if already initialized. */
    int ret = atspi_init();
    if (ret < 0) {
        fprintf(stderr, "atspi: failed to initialize AT-SPI2\n");
        free(tree);
        return NULL;
    }

    tree->connected = 1;
    return tree;
}

MdAtspiNode *md_atspi_walk(MdAtspiTree *tree) {
    if (!tree || !tree->connected)
        return NULL;

    /* Get the desktop object (root of the accessibility tree).
     * Desktop 0 is the primary desktop on standard Linux setups. */
    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop) {
        fprintf(stderr, "atspi: failed to get desktop\n");
        return NULL;
    }

    GError *error = NULL;
    int app_count = atspi_accessible_get_child_count(desktop, &error);
    if (error) { g_error_free(error); error = NULL; }

    /* Build a virtual root node that contains all applications */
    MdAtspiNode *root = calloc(1, sizeof(MdAtspiNode));
    if (!root) {
        g_object_unref(desktop);
        return NULL;
    }

    tree->next_id = 0; /* reset ID counter for each walk */

    root->id = make_node_id(tree);
    root->role = strdup("desktop");
    root->label = strdup("Desktop");

    if (app_count > 0) {
        if (app_count > MD_ATSPI_MAX_CHILDREN)
            app_count = MD_ATSPI_MAX_CHILDREN;

        root->children = calloc((size_t)app_count, sizeof(MdAtspiNode *));
        if (root->children) {
            int actual = 0;
            for (int i = 0; i < app_count; i++) {
                AtspiAccessible *app = atspi_accessible_get_child_at_index(
                    desktop, i, &error);
                if (error) { g_error_free(error); error = NULL; }
                if (!app) continue;

                MdAtspiNode *app_node = walk_accessible(tree, app, 1);
                if (app_node)
                    root->children[actual++] = app_node;
                g_object_unref(app);
            }
            root->child_count = actual;
        }
    }

    g_object_unref(desktop);

    /* Store snapshot for delta computation */
    /* (For delta, we'd deep-copy; for now just track the pointer.
     *  The caller owns the returned tree, so we only use last_snapshot
     *  for comparison if the caller explicitly provides it.) */

    return root;
}

/* ── Delta computation ─────────────────────────────────────── */

/* Simple delta: compare two trees by node ID and detect adds/removes/updates.
 * This is a basic implementation — Phase 2 will use hash-based diffing. */

/* Flatten a tree into an array of (id, node) pairs for O(n) comparison. */
typedef struct {
    const char    *id;
    const MdAtspiNode *node;
} FlatEntry;

static void flatten_tree(const MdAtspiNode *node, FlatEntry **entries,
                         int *count, int *capacity) {
    if (!node) return;

    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? 64 : *capacity * 2;
        *entries = realloc(*entries, (size_t)*capacity * sizeof(FlatEntry));
        if (!*entries) { *count = 0; return; }
    }

    (*entries)[*count].id = node->id;
    (*entries)[*count].node = node;
    (*count)++;

    for (int i = 0; i < node->child_count && node->children; i++)
        flatten_tree(node->children[i], entries, count, capacity);
}

static const FlatEntry *find_by_id(const FlatEntry *entries, int count,
                                    const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < count; i++) {
        if (entries[i].id && strcmp(entries[i].id, id) == 0)
            return &entries[i];
    }
    return NULL;
}

/* Check if two nodes differ in label, role, or states. */
static bool nodes_differ(const MdAtspiNode *a, const MdAtspiNode *b) {
    if (!a || !b) return true;

    /* Compare role */
    if ((a->role == NULL) != (b->role == NULL)) return true;
    if (a->role && b->role && strcmp(a->role, b->role) != 0) return true;

    /* Compare label */
    if ((a->label == NULL) != (b->label == NULL)) return true;
    if (a->label && b->label && strcmp(a->label, b->label) != 0) return true;

    /* Compare bounds */
    if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h)
        return true;

    /* Compare state count (simple check) */
    if (a->state_count != b->state_count) return true;

    return false;
}

/* Deep copy a single node (without children) for delta entries. */
static MdAtspiNode *clone_node_shallow(const MdAtspiNode *src) {
    if (!src) return NULL;

    MdAtspiNode *dst = calloc(1, sizeof(MdAtspiNode));
    if (!dst) return NULL;

    if (src->id)    dst->id    = strdup(src->id);
    if (src->role)  dst->role  = strdup(src->role);
    if (src->label) dst->label = strdup(src->label);
    dst->x = src->x;
    dst->y = src->y;
    dst->w = src->w;
    dst->h = src->h;

    if (src->state_count > 0 && src->states) {
        dst->states = calloc((size_t)src->state_count, sizeof(char *));
        if (dst->states) {
            dst->state_count = src->state_count;
            for (int i = 0; i < src->state_count; i++) {
                if (src->states[i])
                    dst->states[i] = strdup(src->states[i]);
            }
        }
    }

    return dst;
}

MdAtspiDelta *md_atspi_diff(MdAtspiTree *tree, int *delta_count) {
    if (!tree || !delta_count) return NULL;
    *delta_count = 0;

    MdAtspiNode *current = md_atspi_walk(tree);
    if (!current) return NULL;

    MdAtspiNode *prev = tree->last_snapshot;

    /* If no previous snapshot, everything is an "add" — but that's
     * equivalent to sending a full tree, which is handled separately.
     * Return empty delta set. */
    if (!prev) {
        tree->last_snapshot = current;
        return NULL;
    }

    /* Flatten both trees */
    FlatEntry *prev_flat = NULL, *curr_flat = NULL;
    int prev_count = 0, curr_count = 0;
    int prev_cap = 0, curr_cap = 0;

    flatten_tree(prev, &prev_flat, &prev_count, &prev_cap);
    flatten_tree(current, &curr_flat, &curr_count, &curr_cap);

    /* Allocate delta array (worst case: all adds + all removes) */
    int max_deltas = prev_count + curr_count;
    MdAtspiDelta *deltas = calloc((size_t)max_deltas, sizeof(MdAtspiDelta));
    if (!deltas) {
        free(prev_flat);
        free(curr_flat);
        md_atspi_node_free(current);
        return NULL;
    }

    int dc = 0;

    /* Find removed nodes (in prev but not in current) */
    for (int i = 0; i < prev_count; i++) {
        if (!find_by_id(curr_flat, curr_count, prev_flat[i].id)) {
            deltas[dc].op = MD_ATSPI_OP_REMOVE;
            deltas[dc].node = clone_node_shallow(prev_flat[i].node);
            dc++;
        }
    }

    /* Find added and updated nodes */
    for (int i = 0; i < curr_count; i++) {
        const FlatEntry *prev_entry = find_by_id(prev_flat, prev_count,
                                                  curr_flat[i].id);
        if (!prev_entry) {
            /* New node */
            deltas[dc].op = MD_ATSPI_OP_ADD;
            deltas[dc].node = clone_node_shallow(curr_flat[i].node);
            dc++;
        } else if (nodes_differ(prev_entry->node, curr_flat[i].node)) {
            /* Changed node */
            deltas[dc].op = MD_ATSPI_OP_UPDATE;
            deltas[dc].node = clone_node_shallow(curr_flat[i].node);
            dc++;
        }
    }

    free(prev_flat);
    free(curr_flat);

    /* Replace snapshot */
    md_atspi_node_free(tree->last_snapshot);
    tree->last_snapshot = current;

    *delta_count = dc;

    /* Trim allocation */
    if (dc == 0) {
        free(deltas);
        return NULL;
    }

    return deltas;
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

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

char *md_atspi_to_json(const MdAtspiNode *root) {
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

    /* AT-SPI2 cleanup — call atspi_exit() to release D-Bus resources.
     * Returns the number of leaked events (should be 0). */
    atspi_exit();

    free(tree);
}
