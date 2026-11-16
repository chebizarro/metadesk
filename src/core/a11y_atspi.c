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
#include "log.h"
#define MD_LOG_TAG "a11y_atspi"

/* Maximum tree depth to prevent infinite recursion from cyclic trees */
#define MD_ATSPI_MAX_DEPTH 32

/* Maximum children per node to prevent OOM on enormous trees */
#define MD_ATSPI_MAX_CHILDREN 256

#define MD_ATSPI_ROLE_ASSERT(native, role) _Static_assert(role >= 0 && role < MD_A11Y_ROLE_COUNT, "invalid AT-SPI role");
MD_A11Y_ROLE_ALIASES(MD_ATSPI_ROLE_ASSERT)
#undef MD_ATSPI_ROLE_ASSERT

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    int                  connected;
    uint64_t             next_id;        /* monotonic ID counter for node IDs     */
    GMutex               lock;           /* protects next_id + backend state      */
    AtspiEventListener  *listener;       /* push-change listener registered below */
    GThread             *event_thread;   /* runs the AT-SPI/GLib event loop       */
    gboolean             subscribed;     /* listener registered; the callback
                                          * itself lives on the MdA11yCtx         */

    /* Event-thread handshake, all under `lock`, signalled on `loop_cond`:
     * the thread sets event_loop_running right before atspi_event_main()
     * (or thread_exited if it saw stop_requested first) and thread_exited
     * when it returns. Teardown waits for one of them before deciding how
     * to stop the loop — see atspi_teardown_subscription. */
    GCond                loop_cond;
    gboolean             stop_requested;
    gboolean             event_loop_running;
    gboolean             thread_exited;
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
        case ATSPI_STATE_ACTIVE:     name = md_a11y_state_name(MD_A11Y_STATE_ACTIVE);     break;
        case ATSPI_STATE_ENABLED:    name = md_a11y_state_name(MD_A11Y_STATE_ENABLED);    break;
        case ATSPI_STATE_FOCUSED:    name = md_a11y_state_name(MD_A11Y_STATE_FOCUSED);    break;
        case ATSPI_STATE_VISIBLE:    name = md_a11y_state_name(MD_A11Y_STATE_VISIBLE);    break;
        case ATSPI_STATE_SHOWING:    name = md_a11y_state_name(MD_A11Y_STATE_SHOWING);    break;
        case ATSPI_STATE_SELECTED:   name = md_a11y_state_name(MD_A11Y_STATE_SELECTED);   break;
        case ATSPI_STATE_CHECKED:    name = md_a11y_state_name(MD_A11Y_STATE_CHECKED);    break;
        case ATSPI_STATE_EDITABLE:   name = md_a11y_state_name(MD_A11Y_STATE_EDITABLE);   break;
        case ATSPI_STATE_EXPANDABLE: name = md_a11y_state_name(MD_A11Y_STATE_EXPANDABLE); break;
        case ATSPI_STATE_EXPANDED:   name = md_a11y_state_name(MD_A11Y_STATE_EXPANDED);   break;
        case ATSPI_STATE_FOCUSABLE:  name = md_a11y_state_name(MD_A11Y_STATE_FOCUSABLE);  break;
        case ATSPI_STATE_SENSITIVE:  name = md_a11y_state_name(MD_A11Y_STATE_SENSITIVE);   break;
        case ATSPI_STATE_SELECTABLE: name = md_a11y_state_name(MD_A11Y_STATE_SELECTABLE); break;
        case ATSPI_STATE_MODAL:      name = md_a11y_state_name(MD_A11Y_STATE_MODAL);      break;
        case ATSPI_STATE_MULTI_LINE: name = md_a11y_state_name(MD_A11Y_STATE_MULTILINE);  break;
        case ATSPI_STATE_SINGLE_LINE:name = md_a11y_state_name(MD_A11Y_STATE_SINGLELINE); break;
        case ATSPI_STATE_DEFUNCT:    name = md_a11y_state_name(MD_A11Y_STATE_DEFUNCT);     break;
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

    /* Prefer the toolkit-provided accessible id over DFS position —
     * it survives across walks where the DFS counter does not. */
    {
        gchar *acc_id = atspi_accessible_get_accessible_id(acc, &error);
        if (error) { g_error_free(error); error = NULL; }
        if (acc_id) {
            if (*acc_id) {
                free(node->id);
                node->id = g_strdup_printf("a:%s", acc_id);
            }
            g_free(acc_id);
        }
    }

    /* Role */
    gchar *role_name = atspi_accessible_get_role_name(acc, &error);
    if (role_name) {
        node->role = strdup(md_a11y_role_name(md_a11y_role_lookup(role_name)));
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

/* ── Change subscriptions ────────────────────────────────────── */

static const char *const atspi_change_events[] = {
    "object:state-changed",
    "object:children-changed",
    "window:activate",
};

static gpointer atspi_event_loop_thread(gpointer data) {
    AtspiState *st = data;

    g_mutex_lock(&st->lock);
    if (st->stop_requested) {
        /* Teardown won the race: honour it without ever entering the loop. */
        st->thread_exited = TRUE;
        g_cond_broadcast(&st->loop_cond);
        g_mutex_unlock(&st->lock);
        return NULL;
    }
    st->event_loop_running = TRUE;
    g_cond_broadcast(&st->loop_cond);
    g_mutex_unlock(&st->lock);

    atspi_event_main();

    g_mutex_lock(&st->lock);
    st->event_loop_running = FALSE;
    st->thread_exited = TRUE;
    g_cond_broadcast(&st->loop_cond);
    g_mutex_unlock(&st->lock);
    return NULL;
}

/* Dispatched by the GMainLoop that atspi_event_main() is running, so the
 * loop provably exists and g_main_loop_quit() cannot be lost. */
static gboolean atspi_quit_from_loop(gpointer data) {
    (void)data;
    atspi_event_quit();
    return G_SOURCE_REMOVE;
}

static void atspi_change_event_cb(AtspiEvent *event, void *user_data) {
    MdA11yCtx *ctx = user_data;
    AtspiState *st = ctx ? ctx->backend_data : NULL;

    if (st) md_a11y_change_dispatch(ctx);

    if (event)
        g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static int atspi_register_change_events(AtspiState *st) {
    for (size_t i = 0; i < sizeof(atspi_change_events) / sizeof(atspi_change_events[0]); i++) {
        GError *error = NULL;
        if (!atspi_event_listener_register(st->listener, atspi_change_events[i], &error)) {
            if (error) {
                MD_LOG_E("failed to register %s: %s", atspi_change_events[i], error->message);
                g_error_free(error);
            } else {
                MD_LOG_E("failed to register %s", atspi_change_events[i]);
            }

            for (size_t j = 0; j < i; j++) {
                GError *dereg_error = NULL;
                atspi_event_listener_deregister(st->listener,
                                                atspi_change_events[j],
                                                &dereg_error);
                if (dereg_error) g_error_free(dereg_error);
            }
            return -1;
        }
    }
    return 0;
}

static void atspi_deregister_change_events(AtspiEventListener *listener) {
    if (!listener) return;

    for (size_t i = 0; i < sizeof(atspi_change_events) / sizeof(atspi_change_events[0]); i++) {
        GError *error = NULL;
        atspi_event_listener_deregister(listener, atspi_change_events[i], &error);
        if (error) g_error_free(error);
    }
}

/* Single teardown path shared by unsubscribe, the subscribe failure path
 * and destroy: deregister every change event, stop and join the GLib event
 * loop thread, drop the listener. The user callback is not touched here —
 * a11y.c has already cleared and drained it before calling us.
 *
 * Dispatch runs on the GLib main loop that atspi_event_main() spins on the
 * backend-owned event thread, so joining that thread is what guarantees
 * no listener callback is still executing when this returns.
 *
 * Stopping that loop safely: atspi_event_quit() is a no-op (g_main_loop_quit
 * on NULL) until atspi_event_main() has created its GMainLoop, so a quit
 * issued to a thread that was spawned but has not entered the loop yet
 * would be lost and the join would hang forever — reachable when
 * md_mcp_bridge_create fails right after subscribing. Hence the handshake:
 * stop_requested is set under the lock; the thread checks it under the same
 * lock before committing to the loop, so it either bails (thread_exited) or
 * publishes event_loop_running. We wait for one of the two, and in the
 * running case request the quit via a high-priority idle source on the
 * default GMainContext — the context atspi_event_main() runs — so the quit
 * executes *inside* the loop, where it cannot be early.
 *
 * If this is reached from the event thread itself (unsubscribe inside the
 * callback — documented as unsupported) we are inside the running loop, so
 * a direct quit is safe and the self-join is skipped. Idempotent. */
static void atspi_teardown_subscription(AtspiState *st) {
    if (!st) return;

    g_mutex_lock(&st->lock);
    AtspiEventListener *listener = st->listener;
    GThread *event_thread = st->event_thread;
    st->listener = NULL;
    st->event_thread = NULL;
    st->subscribed = FALSE;
    st->stop_requested = TRUE;

    gboolean on_event_thread = event_thread && event_thread == g_thread_self();
    gboolean quit_via_loop = FALSE;
    if (event_thread && !on_event_thread) {
        /* Wait for the thread to commit one way or the other. */
        while (!st->event_loop_running && !st->thread_exited)
            g_cond_wait(&st->loop_cond, &st->lock);
        quit_via_loop = st->event_loop_running;
    }
    g_mutex_unlock(&st->lock);

    atspi_deregister_change_events(listener);

    if (on_event_thread)
        atspi_event_quit();
    else if (quit_via_loop)
        g_idle_add_full(G_PRIORITY_HIGH, atspi_quit_from_loop, NULL, NULL);

    if (event_thread) {
        if (on_event_thread)
            g_thread_unref(event_thread);
        else
            g_thread_join(event_thread);
    }

    if (listener)
        g_object_unref(listener);
}

/* ── Vtable implementation ───────────────────────────────────── */

static int atspi_init_backend(MdA11yCtx *ctx) {
    AtspiState *st = calloc(1, sizeof(AtspiState));
    if (!st) return -1;

    int ret = atspi_init();
    if (ret < 0) {
        MD_LOG_E("failed to initialize AT-SPI2");
        free(st);
        return -1;
    }

    g_mutex_init(&st->lock);
    g_cond_init(&st->loop_cond);
    st->connected = 1;
    ctx->backend_data = st;
    return 0;
}

static int atspi_get_tree_unlocked(MdA11yCtx *ctx, MdA11yNode **out_root) {
    AtspiState *st = ctx->backend_data;
    if (!st || !st->connected || !out_root) return -1;

    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop) {
        MD_LOG_E("failed to get desktop");
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

    root->id = strdup("desktop");
    root->role = strdup(md_a11y_role_name(MD_A11Y_ROLE_DESKTOP));
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

static int atspi_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    AtspiState *st = ctx ? ctx->backend_data : NULL;
    if (!st) return -1;

    g_mutex_lock(&st->lock);
    int ret = atspi_get_tree_unlocked(ctx, out_root);
    g_mutex_unlock(&st->lock);
    return ret;
}

static int atspi_subscribe_changes(MdA11yCtx *ctx, MdA11yChangeCb cb,
                                   void *userdata) {
    AtspiState *st = ctx ? ctx->backend_data : NULL;
    if (!st || !st->connected || !cb) return -1;
    (void)userdata; /* callback + userdata are stored on ctx by a11y.c */

    g_mutex_lock(&st->lock);
    if (st->subscribed) {
        /* Listener already registered; a11y.c swapped the callback. */
        g_mutex_unlock(&st->lock);
        return 0;
    }
    g_mutex_unlock(&st->lock);

    AtspiEventListener *listener = atspi_event_listener_new(
        atspi_change_event_cb, ctx, NULL);
    if (!listener) {
        MD_LOG_E("failed to create event listener");
        return -1;
    }

    g_mutex_lock(&st->lock);
    st->listener = listener;
    g_mutex_unlock(&st->lock);

    if (atspi_register_change_events(st) != 0) {
        g_mutex_lock(&st->lock);
        st->listener = NULL;
        g_mutex_unlock(&st->lock);
        g_object_unref(listener);
        return -1;
    }

    g_mutex_lock(&st->lock);
    st->subscribed = TRUE;
    g_mutex_unlock(&st->lock);

    /* Reset the thread handshake before spawning (a previous subscription
     * may have left stop_requested/thread_exited set). */
    g_mutex_lock(&st->lock);
    st->stop_requested = FALSE;
    st->event_loop_running = FALSE;
    st->thread_exited = FALSE;
    g_mutex_unlock(&st->lock);

    /* g_thread_new() aborts on failure; try_new makes the failure path real. */
    GError *thread_error = NULL;
    GThread *event_thread = g_thread_try_new("md-atspi-events",
                                             atspi_event_loop_thread, st,
                                             &thread_error);
    if (!event_thread) {
        MD_LOG_E("failed to start AT-SPI event thread: %s",
                 thread_error ? thread_error->message : "unknown error");
        if (thread_error) g_error_free(thread_error);
        atspi_teardown_subscription(st);
        return -1;
    }
    g_mutex_lock(&st->lock);
    st->event_thread = event_thread;
    g_mutex_unlock(&st->lock);

    return 0;
}

static void atspi_unsubscribe_changes(MdA11yCtx *ctx) {
    AtspiState *st = ctx ? ctx->backend_data : NULL;
    if (!st) return;
    atspi_teardown_subscription(st);
}

static void atspi_destroy_backend(MdA11yCtx *ctx) {
    AtspiState *st = ctx->backend_data;
    if (!st) return;

    atspi_teardown_subscription(st);

    atspi_exit();

    g_cond_clear(&st->loop_cond);
    g_mutex_clear(&st->lock);
    free(st);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdA11yBackend atspi_backend = {
    .init              = atspi_init_backend,
    .get_tree          = atspi_get_tree,
    .subscribe_changes   = atspi_subscribe_changes,
    .unsubscribe_changes = atspi_unsubscribe_changes,
    .destroy             = atspi_destroy_backend,
};

const MdA11yBackend *md_a11y_backend_create(void) {
    return &atspi_backend;
}
