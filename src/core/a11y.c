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
#include <stdarg.h>
#include <time.h>

/* ── Public convenience API ──────────────────────────────────── */

MdA11yCtx *md_a11y_create(void) {
    const MdA11yBackend *vtable = md_a11y_backend_create();
    if (!vtable) return NULL;

    MdA11yCtx *ctx = calloc(1, sizeof(MdA11yCtx));
    if (!ctx) return NULL;

    ctx->vtable = vtable;
    pthread_mutex_init(&ctx->snapshot_mu, NULL);

    if (ctx->vtable->init(ctx) != 0) {
        pthread_mutex_destroy(&ctx->snapshot_mu);
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

/* ── Shared diff engine ──────────────────────────────────────
 * Computes deltas between the previous snapshot and a fresh tree from
 * vtable->get_tree. Operates only on platform-neutral MdA11yNode, so
 * it lives here, not in the backends. */

typedef struct {
    const char        *id;
    const MdA11yNode  *node;
} FlatEntry;

static void flatten_tree(const MdA11yNode *node, FlatEntry **entries,
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

/* Order FlatEntry by id (NULLs first) so the diff can bsearch instead of
 * linear-scanning — the two lookup loops below were O(n²) per change event on
 * a whole-desktop tree. */
static int flat_cmp(const void *a, const void *b) {
    const FlatEntry *ea = a, *eb = b;
    if (!ea->id) return eb->id ? -1 : 0;
    if (!eb->id) return 1;
    return strcmp(ea->id, eb->id);
}

/* Look up an id in a FlatEntry array that has been sorted with flat_cmp. */
static const FlatEntry *find_by_id(const FlatEntry *entries, int count,
                                   const char *id) {
    if (!id) return NULL;
    FlatEntry key = { .id = id, .node = NULL };
    return bsearch(&key, entries, (size_t)count, sizeof(FlatEntry), flat_cmp);
}

static bool nodes_differ(const MdA11yNode *a, const MdA11yNode *b) {
    if (!a || !b) return true;
    if ((a->role == NULL) != (b->role == NULL)) return true;
    if (a->role && b->role && strcmp(a->role, b->role) != 0) return true;
    if ((a->label == NULL) != (b->label == NULL)) return true;
    if (a->label && b->label && strcmp(a->label, b->label) != 0) return true;
    if (a->x != b->x || a->y != b->y || a->w != b->w || a->h != b->h)
        return true;
    if (a->state_count != b->state_count) return true;
    /* Compare state *contents*, not just the count: a toggle from
     * ["focused"] to ["selected"] keeps the same count but is a real change.
     * Backends emit states in a stable order, so an element-wise compare is
     * sufficient (and mirrors the role/label comparisons above). */
    for (int i = 0; i < a->state_count; i++) {
        const char *sa = a->states ? a->states[i] : NULL;
        const char *sb = b->states ? b->states[i] : NULL;
        if ((sa == NULL) != (sb == NULL)) return true;
        if (sa && sb && strcmp(sa, sb) != 0) return true;
    }
    return false;
}

static MdA11yNode *clone_node_shallow(const MdA11yNode *src) {
    if (!src) return NULL;

    MdA11yNode *dst = calloc(1, sizeof(MdA11yNode));
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

MdA11yDelta *md_a11y_diff(MdA11yCtx *ctx, int *delta_count) {
    if (!ctx || !ctx->vtable || !ctx->vtable->get_tree || !delta_count)
        return NULL;

    *delta_count = 0;

    pthread_mutex_lock(&ctx->snapshot_mu);

    MdA11yNode *current = NULL;
    if (ctx->vtable->get_tree(ctx, &current) != 0 || !current) {
        pthread_mutex_unlock(&ctx->snapshot_mu);
        return NULL;
    }

    MdA11yNode *prev = ctx->last_snapshot;

    /* No previous snapshot — caller should send a full tree instead. */
    if (!prev) {
        ctx->last_snapshot = current;
        pthread_mutex_unlock(&ctx->snapshot_mu);
        return NULL;
    }

    /* Flatten both trees */
    FlatEntry *prev_flat = NULL, *curr_flat = NULL;
    int prev_count = 0, curr_count = 0;
    int prev_cap = 0, curr_cap = 0;

    flatten_tree(prev, &prev_flat, &prev_count, &prev_cap);
    flatten_tree(current, &curr_flat, &curr_count, &curr_cap);

    /* Sort both so find_by_id() can bsearch (O(n log n) total, not O(n²)). */
    if (prev_flat) qsort(prev_flat, (size_t)prev_count, sizeof(FlatEntry), flat_cmp);
    if (curr_flat) qsort(curr_flat, (size_t)curr_count, sizeof(FlatEntry), flat_cmp);

    int max_deltas = prev_count + curr_count;
    MdA11yDelta *deltas = calloc((size_t)(max_deltas > 0 ? max_deltas : 1),
                                 sizeof(MdA11yDelta));
    if (!deltas) {
        free(prev_flat);
        free(curr_flat);
        md_a11y_node_free(current);
        pthread_mutex_unlock(&ctx->snapshot_mu);
        return NULL;
    }

    int dc = 0;

    /* Removed nodes */
    for (int i = 0; i < prev_count; i++) {
        if (!find_by_id(curr_flat, curr_count, prev_flat[i].id)) {
            deltas[dc].op = MD_A11Y_OP_REMOVE;
            deltas[dc].node = clone_node_shallow(prev_flat[i].node);
            dc++;
        }
    }

    /* Added and updated nodes */
    for (int i = 0; i < curr_count; i++) {
        const FlatEntry *prev_entry = find_by_id(prev_flat, prev_count,
                                                  curr_flat[i].id);
        if (!prev_entry) {
            deltas[dc].op = MD_A11Y_OP_ADD;
            deltas[dc].node = clone_node_shallow(curr_flat[i].node);
            dc++;
        } else if (nodes_differ(prev_entry->node, curr_flat[i].node)) {
            deltas[dc].op = MD_A11Y_OP_UPDATE;
            deltas[dc].node = clone_node_shallow(curr_flat[i].node);
            dc++;
        }
    }

    free(prev_flat);
    free(curr_flat);

    /* Replace snapshot */
    md_a11y_node_free(ctx->last_snapshot);
    ctx->last_snapshot = current;

    pthread_mutex_unlock(&ctx->snapshot_mu);

    if (dc == 0) {
        free(deltas);
        return NULL;
    }

    *delta_count = dc;
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
    md_a11y_node_free(ctx->last_snapshot);
    pthread_mutex_destroy(&ctx->snapshot_mu);
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

/* Wall-clock milliseconds. This value is only ever serialized into the tree
 * document's "ts" field, which is meant to be a real timestamp a consumer can
 * interpret across processes and restarts — so it uses CLOCK_REALTIME, not the
 * monotonic clock (whose epoch is arbitrary and would make "ts" meaningless). */
static uint64_t now_wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

char *md_a11y_to_json(const MdA11yNode *root) {
    if (!root) return NULL;

    cJSON *doc = cJSON_CreateObject();
    if (!doc) return NULL;

    cJSON_AddNumberToObject(doc, "v", 1);
    cJSON_AddNumberToObject(doc, "ts", (double)now_wall_ms());
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
/*
 * One role vocabulary, one table. role_abbrev() and is_interactable() were
 * two parallel strcmp cascades over the same set of roles that had to be kept
 * in sync by hand; they now read a single source of truth. `alt` is an
 * accepted synonym for the same role (or NULL).
 */
typedef struct {
    const char *role;
    const char *alt;
    const char *abbrev;
    bool        interactable;
} RoleInfo;

static const RoleInfo role_table[] = {
    /* Containers — always shown for structural context */
    { "frame",         "window",  "WIN", true  },
    { "application",   NULL,      "APP", true  },
    { "desktop frame", "desktop", "DSK", true  },
    { "dialog",        NULL,      "DLG", true  },
    /* Interactive controls — the primary targets */
    { "push button",   "button",  "BTN", true  },
    { "toggle button", NULL,      "TGL", true  },
    { "text",          "entry",   "TXT", true  },
    { "menu",          "menu bar","MNU", true  },
    { "menu item",     NULL,      "MNI", true  },
    { "check box",     NULL,      "CHK", true  },
    { "radio button",  NULL,      "RAD", true  },
    { "combo box",     NULL,      "CMB", true  },
    { "list",          NULL,      "LST", true  },
    { "list item",     NULL,      "LI",  true  },
    { "tab",           NULL,      "TAB", true  },
    { "page tab",      NULL,      "PTB", true  },
    { "link",          NULL,      "LNK", true  },
    { "slider",        NULL,      "SLD", true  },
    { "spin button",   NULL,      "SPN", true  },
    { "tree",          NULL,      "TRE", true  },
    { "table",         NULL,      "TBL", true  },
    /* Decorative / structural — skipped in compact output (children walked) */
    { "panel",         "filler",  "PNL", false },
    { "label",         NULL,      "LBL", false },
    { "scroll bar",    NULL,      "SCR", false },
    { "separator",     NULL,      "SEP", false },
    { "tool bar",      NULL,      "TBR", false },
    { "image",         NULL,      "IMG", false },
    { "status bar",    NULL,      "STS", false },
    { "page tab list", NULL,      "PTL", false },
    { "split pane",    NULL,      "SPL", false },
    { "progress bar",  NULL,      "PRG", false },
};

static const RoleInfo *role_lookup(const char *role) {
    if (!role) return NULL;
    for (size_t i = 0; i < sizeof(role_table) / sizeof(role_table[0]); i++) {
        if (strcmp(role, role_table[i].role) == 0 ||
            (role_table[i].alt && strcmp(role, role_table[i].alt) == 0))
            return &role_table[i];
    }
    return NULL;
}

static const char *role_abbrev(const char *role) {
    if (!role) return "???";
    const RoleInfo *ri = role_lookup(role);
    return ri ? ri->abbrev : "UNK";
}

/*
 * Interactable classification: true if the node should be emitted in compact
 * output. Containers (WIN/APP/DSK/DLG) show for structure; leaf controls
 * (BTN/TXT/MNU/CHK/...) are the targets; decorative roles are skipped (their
 * children are still walked).
 */
static bool is_interactable(const char *role) {
    const RoleInfo *ri = role_lookup(role);
    return ri && ri->interactable;
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

/* ── Growable string builder ──────────────────────────────────
 * The compact serializer used to write into a fixed 256 KB buffer and
 * silently truncate anything larger (a self-labelled placeholder). It now
 * appends into this buffer, which grows as needed. */
typedef struct { char *buf; size_t len; size_t cap; bool oom; } StrBuf;

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->oom) return;
    if (sb->buf && sb->len + extra + 1 <= sb->cap) return;
    size_t newcap = sb->cap ? sb->cap : 4096;
    while (newcap < sb->len + extra + 1) newcap *= 2;
    char *nb = realloc(sb->buf, newcap);
    if (!nb) { sb->oom = true; return; }
    sb->buf = nb;
    sb->cap = newcap;
}

/* Append a literal string (never treated as a format — labels may contain %). */
static void sb_append(StrBuf *sb, const char *s) {
    if (sb->oom || !s) return;
    size_t n = strlen(s);
    sb_reserve(sb, n);
    if (sb->oom) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    if (sb->oom) return;
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { sb->oom = true; return; }
    sb_reserve(sb, (size_t)need);
    if (sb->oom) return;
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)need;
}

/* Append state annotations in priority order. */
static void compact_states(const MdA11yNode *node, StrBuf *sb) {
    static const struct { const char *state; const char *fmt; } state_map[] = {
        { "focused",  " <focused>" },
        { "enabled",  " *enabled*" },
        { "disabled", " *disabled*" },
        { "checked",  " *checked*" },
        { "selected", " *selected*" },
        { "pressed",  " *pressed*" },
        { "expanded", " *expanded*" },
    };
    for (size_t i = 0; i < sizeof(state_map) / sizeof(state_map[0]); i++)
        if (has_state(node, state_map[i].state))
            sb_append(sb, state_map[i].fmt);
}

/*
 * Append a node in compact format. Applies interactable filtering:
 * non-interactable nodes are skipped but their children are still walked
 * (so interactive elements nested under panels still appear).
 */
static void compact_node(const MdA11yNode *node, int depth, StrBuf *sb) {
    if (!node) return;
    bool emit = is_interactable(node->role);

    if (emit) {
        for (int i = 0; i < depth; i++) sb_append(sb, "  ");

        /* ROLE[id] */
        sb_appendf(sb, "%s[%s]", role_abbrev(node->role),
                   node->id ? node->id : "?");

        if (is_text_role(node->role)) {
            /* text entry: <focused> before quoted content (spec §3.3.2) */
            if (has_state(node, "focused")) sb_append(sb, " <focused>");
            sb_appendf(sb, " '%s'", node->label ? node->label : "");
            if (has_state(node, "enabled"))  sb_append(sb, " *enabled*");
            if (has_state(node, "disabled")) sb_append(sb, " *disabled*");
        } else {
            const char *label = node->label ? node->label : "";
            if (label[0]) sb_appendf(sb, " %s", label);
            compact_states(node, sb);
        }

        sb_append(sb, "\n");
    }

    int child_depth = emit ? depth + 1 : depth;
    for (int i = 0; i < node->child_count && node->children; i++)
        compact_node(node->children[i], child_depth, sb);
}

char *md_a11y_to_compact(const MdA11yNode *root) {
    if (!root) return NULL;

    StrBuf sb = { 0 };
    sb_appendf(&sb, "v1 ts:%lu\n", (unsigned long)now_wall_ms());
    compact_node(root, 0, &sb);

    if (sb.oom) { free(sb.buf); return NULL; }
    return sb.buf;  /* NUL-terminated; caller frees */
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
    cJSON_AddNumberToObject(doc, "ts", (double)now_wall_ms());

    char *result = cJSON_PrintUnformatted(doc);
    cJSON_Delete(deltas);
    cJSON_Delete(doc);
    return result;
}
