/*
 * metadesk — a11y_atspi.c
 * Linux accessibility backend: AT-SPI2 via D-Bus.
 *
 * Uses libatspi-2.0 to:
 *   1. Connect to the AT-SPI2 D-Bus registry
 *   2. Walk the accessibility tree from the desktop root
 *   3. Extract node attributes: id, role, label, states, bounds
 *   4. Build an MdA11yNode tree
 *   5. Compute deltas by comparing current vs previous snapshot
 *
 * Serialization is handled by the platform-agnostic a11y.c.
 *
 * See spec §3.3 and §10 for tree formats and agent API.
 */
#include "a11y.h"

#include <atspi/atspi.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Maximum tree depth to prevent infinite recursion from cyclic trees */
#define MD_ATSPI_MAX_DEPTH 32

/* Maximum children per node to prevent OOM on enormous trees */
#define MD_ATSPI_MAX_CHILDREN 256

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    int          connected;
    uint64_t     next_id;        /* monotonic ID counter for node IDs     */
    MdA11yNode  *last_snapshot;  /* previous tree for delta computation   */
} AtspiState;

/* ── Internal: convert AtspiAccessible to MdA11yNode ─────────── */

static char *make_node_id(AtspiState *st) {
    char buf[32];
    snprintf(buf, sizeof(buf), "n%lu", (unsigned long)st->next_id++);
    return strdup(buf);
}

static void extract_states(AtspiStateSet *state_set, MdA11yNode *node) {
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
        case ATSPI_STATE_SENSITIVE:  name = "sensitive";   break;
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

static MdA11yNode *walk_accessible(AtspiState *st, AtspiAccessible *acc,
                                   int depth) {
    if (!acc || depth > MD_ATSPI_MAX_DEPTH)
        return NULL;

    GError *error = NULL;

    MdA11yNode *node = calloc(1, sizeof(MdA11yNode));
    if (!node) return NULL;

    node->id = make_node_id(st);

    /* Role */
    gchar *role_name = atspi_accessible_get_role_name(acc, &error);
    if (role_name) {
        node->role = strdup(role_name);
        g_free(role_name);
    }
    if (error) { g_error_free(error); error = NULL; }

    /* Label (accessible name) */
    gchar *name = atspi_accessible_get_name(acc, &error);
    if (name && name[0] != '\0')
        node->label = strdup(name);
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
        node->children = calloc((size_t)child_count, sizeof(MdA11yNode *));
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
                    MdA11yNode *child_node = walk_accessible(st, child, depth + 1);
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

/* ── Delta computation ───────────────────────────────────────── */

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

static const FlatEntry *find_by_id(const FlatEntry *entries, int count,
                                   const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < count; i++) {
        if (entries[i].id && strcmp(entries[i].id, id) == 0)
            return &entries[i];
    }
    return NULL;
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

/* ── Vtable implementation ───────────────────────────────────── */

static int atspi_init_backend(MdA11yCtx *ctx) {
    AtspiState *st = calloc(1, sizeof(AtspiState));
    if (!st) return -1;

    int ret = atspi_init();
    if (ret < 0) {
        fprintf(stderr, "a11y_atspi: failed to initialize AT-SPI2\n");
        free(st);
        return -1;
    }

    st->connected = 1;
    ctx->backend_data = st;
    return 0;
}

static int atspi_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    AtspiState *st = ctx->backend_data;
    if (!st || !st->connected || !out_root) return -1;

    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop) {
        fprintf(stderr, "a11y_atspi: failed to get desktop\n");
        return -1;
    }

    GError *error = NULL;
    int app_count = atspi_accessible_get_child_count(desktop, &error);
    if (error) { g_error_free(error); error = NULL; }

    MdA11yNode *root = calloc(1, sizeof(MdA11yNode));
    if (!root) {
        g_object_unref(desktop);
        return -1;
    }

    st->next_id = 0;

    root->id = make_node_id(st);
    root->role = strdup("desktop");
    root->label = strdup("Desktop");

    if (app_count > 0) {
        if (app_count > MD_ATSPI_MAX_CHILDREN)
            app_count = MD_ATSPI_MAX_CHILDREN;

        root->children = calloc((size_t)app_count, sizeof(MdA11yNode *));
        if (root->children) {
            int actual = 0;
            for (int i = 0; i < app_count; i++) {
                AtspiAccessible *app = atspi_accessible_get_child_at_index(
                    desktop, i, &error);
                if (error) { g_error_free(error); error = NULL; }
                if (!app) continue;

                MdA11yNode *app_node = walk_accessible(st, app, 1);
                if (app_node)
                    root->children[actual++] = app_node;
                g_object_unref(app);
            }
            root->child_count = actual;
        }
    }

    g_object_unref(desktop);
    *out_root = root;
    return 0;
}

static int atspi_get_diff(MdA11yCtx *ctx, MdA11yDelta **out_deltas,
                          int *out_count) {
    AtspiState *st = ctx->backend_data;
    if (!st || !out_deltas || !out_count) return -1;

    *out_deltas = NULL;
    *out_count = 0;

    MdA11yNode *current = NULL;
    if (atspi_get_tree(ctx, &current) != 0 || !current)
        return -1;

    MdA11yNode *prev = st->last_snapshot;

    /* No previous snapshot — everything is new. Return empty delta;
     * caller should send a full tree instead. */
    if (!prev) {
        st->last_snapshot = current;
        return 0;
    }

    /* Flatten both trees */
    FlatEntry *prev_flat = NULL, *curr_flat = NULL;
    int prev_count = 0, curr_count = 0;
    int prev_cap = 0, curr_cap = 0;

    flatten_tree(prev, &prev_flat, &prev_count, &prev_cap);
    flatten_tree(current, &curr_flat, &curr_count, &curr_cap);

    int max_deltas = prev_count + curr_count;
    MdA11yDelta *deltas = calloc((size_t)max_deltas, sizeof(MdA11yDelta));
    if (!deltas) {
        free(prev_flat);
        free(curr_flat);
        md_a11y_node_free(current);
        return -1;
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
    md_a11y_node_free(st->last_snapshot);
    st->last_snapshot = current;

    if (dc == 0) {
        free(deltas);
        *out_deltas = NULL;
        *out_count = 0;
        return 0;
    }

    *out_deltas = deltas;
    *out_count = dc;
    return 0;
}

static int atspi_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                   void *userdata) {
    (void)ctx; (void)cb; (void)userdata;
    /* Phase 2: register for AT-SPI2 events (state-change, children-changed).
     * For now, return -1 to indicate polling-only mode. */
    return -1;
}

static void atspi_destroy_backend(MdA11yCtx *ctx) {
    AtspiState *st = ctx->backend_data;
    if (!st) return;

    md_a11y_node_free(st->last_snapshot);
    atspi_exit();

    free(st);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdA11yBackend atspi_backend = {
    .init              = atspi_init_backend,
    .get_tree          = atspi_get_tree,
    .get_diff          = atspi_get_diff,
    .subscribe_changes = atspi_subscribe_changes,
    .destroy           = atspi_destroy_backend,
};

const MdA11yBackend *md_a11y_backend_create(void) {
    return &atspi_backend;
}
