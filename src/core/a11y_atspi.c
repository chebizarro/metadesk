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
#include "atspi.h"   /* md_atspi_extract_states prototype (test seam) */

#include <atspi/atspi.h>

#include <stdatomic.h>
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
    atomic_bool          connected;     /* event-driven, never takes tree lock   */
    DBusConnection      *bus;           /* borrowed from libatspi until exit     */
    uint64_t             next_id;        /* monotonic ID counter for node IDs     */
    GMutex               lock;           /* protects next_id + backend state      */
    AtspiEventListener  *listener;       /* push-change listener registered below */
    GThread             *event_thread;   /* runs the AT-SPI/GLib event loop       */
    GMainLoop           *event_loop;     /* owned; never libatspi's global loop   */
    gboolean             subscribed;     /* listener registered; the callback
                                          * itself lives on the MdA11yCtx         */
} AtspiState;

/* ── Internal: convert AtspiAccessible to MdA11yNode ─────────── */

static char *make_node_id(AtspiState *st) {
    char buf[32];
    snprintf(buf, sizeof(buf), "n%lu", (unsigned long)st->next_id++);
    return strdup(buf);
}

/* Map one AT-SPI state to its canonical wire name, or NULL if metadesk has
 * no vocabulary entry for it (e.g. HORIZONTAL, OPAQUE, MANAGES_DESCENDANTS). */
static const char *atspi_state_name(AtspiStateType st) {
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
    return name;
}

/* Fill node->states with the canonical names of the mapped states only.
 * The array is compacted: state_count is the number of mapped states and
 * there are never NULL holes, so consumers may index states[0..count) freely.
 * Unmapped AT-SPI states are dropped (they have no metadesk vocabulary). */
void md_atspi_extract_states(AtspiStateSet *state_set, MdA11yNode *node) {
    if (!state_set || !node) return;

    GArray *states = atspi_state_set_get_states(state_set);
    if (!states || states->len == 0) {
        if (states) g_array_free(states, TRUE);
        return;
    }

    char **names = calloc((size_t)states->len, sizeof(char *));
    if (!names) {
        g_array_free(states, TRUE);
        return;
    }

    int count = 0;
    for (guint i = 0; i < states->len; i++) {
        const char *name =
            atspi_state_name(g_array_index(states, AtspiStateType, i));
        if (!name) continue;
        char *dup = strdup(name);
        if (!dup) continue;
        names[count++] = dup;
    }
    g_array_free(states, TRUE);

    if (count == 0) {
        free(names);
        return;
    }

    node->states = names;
    node->state_count = count;
}

static void extract_states(AtspiStateSet *state_set, MdA11yNode *node) {
    md_atspi_extract_states(state_set, node);
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
    } else {
        node->role = strdup(md_a11y_role_name(MD_A11Y_ROLE_UNKNOWN));
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

    g_main_loop_run(st->event_loop);
    return NULL;
}

/* Another default-context dispatcher may run this before our thread has
 * entered run(). Keep the source pending until the owned loop is running;
 * quitting before run() would otherwise be lost. */
static gboolean atspi_quit_from_loop(gpointer data) {
    GMainLoop *loop = data;
    if (!g_main_loop_is_running(loop)) return G_SOURCE_CONTINUE;
    g_main_loop_quit(loop);
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

typedef struct {
    AtspiState *st;
    AtspiEventListener *listener;
    gboolean add;
    GMutex lock;
    GCond done_cond;
    gboolean done;
    int result;
} AtspiListenerOp;

static gboolean atspi_listener_op(gpointer data) {
    AtspiListenerOp *op = data;
    if (op->add)
        op->result = atspi_register_change_events(op->st);
    else
        atspi_deregister_change_events(op->listener);
    g_mutex_lock(&op->lock);
    op->done = TRUE;
    g_cond_signal(&op->done_cond);
    g_mutex_unlock(&op->lock);
    return G_SOURCE_REMOVE;
}

/* libatspi's listener lists are not synchronized. Now that dispatch runs
 * from init, mutate those lists on the dispatch context, not concurrently
 * from the subscribing/unsubscribing thread. Never wait with st->lock held. */
static int atspi_update_listener(AtspiState *st, AtspiEventListener *listener,
                                  gboolean add) {
    AtspiListenerOp op = { .st = st, .listener = listener, .add = add };
    g_mutex_init(&op.lock);
    g_cond_init(&op.done_cond);
    g_main_context_invoke_full(g_main_loop_get_context(st->event_loop),
                               G_PRIORITY_DEFAULT, atspi_listener_op, &op, NULL);
    g_mutex_lock(&op.lock);
    while (!op.done) g_cond_wait(&op.done_cond, &op.lock);
    g_mutex_unlock(&op.lock);
    g_cond_clear(&op.done_cond);
    g_mutex_clear(&op.lock);
    return op.result;
}

/* Disconnect dispatch outlives subscriptions. Own both loop and quit source
 * so a competing default-context dispatcher cannot issue a premature quit
 * or leave a stale source targeting the next backend instance. */
static void atspi_stop_event_thread(AtspiState *st) {
    if (!st) return;

    g_mutex_lock(&st->lock);
    GThread *event_thread = st->event_thread;
    st->event_thread = NULL;
    g_mutex_unlock(&st->lock);
    if (!event_thread) return;

    GSource *quit = g_idle_source_new();
    g_source_set_priority(quit, G_PRIORITY_HIGH);
    g_source_set_callback(quit, atspi_quit_from_loop,
                          g_main_loop_ref(st->event_loop),
                          (GDestroyNotify)g_main_loop_unref);
    g_source_attach(quit, g_main_loop_get_context(st->event_loop));
    g_thread_join(event_thread);
    g_source_destroy(quit);
    g_source_unref(quit);
}

static void atspi_teardown_subscription(AtspiState *st) {
    g_mutex_lock(&st->lock);
    AtspiEventListener *listener = st->listener;
    st->listener = NULL;
    st->subscribed = FALSE;
    g_mutex_unlock(&st->lock);
    if (listener) {
        atspi_update_listener(st, listener, FALSE);
        g_object_unref(listener);
    }
}

/* libatspi dispatches this local signal on its GLib event context. Do not
 * take the tree lock here: a synchronous backend call may be holding it. */
static DBusHandlerResult atspi_bus_filter(DBusConnection *bus,
                                          DBusMessage *message, void *data) {
    (void)bus;
    AtspiState *st = data;
    if (dbus_message_is_signal(message, DBUS_INTERFACE_LOCAL, "Disconnected"))
        atomic_store(&st->connected, false);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ── Vtable implementation ───────────────────────────────────── */

static int atspi_init_backend(MdA11yCtx *ctx) {
    AtspiState *st = calloc(1, sizeof(AtspiState));
    if (!st) return -1;

    int ret = atspi_init();
    if (ret != 0 && ret != 1) {
        MD_LOG_E("failed to initialize AT-SPI2 (code %d)", ret);
        free(st);
        return -1;
    }

    g_mutex_init(&st->lock);
    st->event_loop = g_main_loop_new(NULL, FALSE);
    atomic_init(&st->connected, true);
    st->bus = atspi_get_a11y_bus();
    if (!st->bus) goto fail;
    /* Let the host supervise cleanup rather than libdbus exiting abruptly. */
    dbus_connection_set_exit_on_disconnect(st->bus, FALSE);
    if (!dbus_connection_add_filter(st->bus, atspi_bus_filter, st, NULL))
        goto fail;
    if (!dbus_connection_get_is_connected(st->bus))
        atomic_store(&st->connected, false);

    /* Liveness must work before subscribe and after unsubscribe as well. */
    GError *error = NULL;
    st->event_thread = g_thread_try_new("md-atspi-events",
                                        atspi_event_loop_thread, st, &error);
    if (!st->event_thread) {
        MD_LOG_E("failed to start AT-SPI event thread: %s",
                 error ? error->message : "unknown error");
        if (error) g_error_free(error);
        dbus_connection_remove_filter(st->bus, atspi_bus_filter, st);
        goto fail;
    }
    ctx->backend_data = st;
    return 0;

fail:
    atspi_exit();
    g_main_loop_unref(st->event_loop);
    g_mutex_clear(&st->lock);
    free(st);
    return -1;
}

static int atspi_get_tree_unlocked(MdA11yCtx *ctx, MdA11yNode **out_root) {
    AtspiState *st = ctx->backend_data;
    if (!st || !atomic_load(&st->connected) || !out_root) return -1;

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
    if (!st || !atomic_load(&st->connected) || !cb) return -1;
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

    if (atspi_update_listener(st, listener, TRUE) != 0) {
        g_mutex_lock(&st->lock);
        st->listener = NULL;
        g_mutex_unlock(&st->lock);
        g_object_unref(listener);
        return -1;
    }

    g_mutex_lock(&st->lock);
    st->subscribed = TRUE;
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
    atspi_stop_event_thread(st);
    /* Joining dispatch before removing the filter keeps its userdata alive. */
    dbus_connection_remove_filter(st->bus, atspi_bus_filter, st);
    atspi_exit();

    g_main_loop_unref(st->event_loop);
    g_mutex_clear(&st->lock);
    free(st);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static bool atspi_is_connected(const MdA11yCtx *ctx) {
    const AtspiState *st = ctx->backend_data;
    return st && atomic_load(&st->connected);
}

static const MdA11yBackend atspi_backend = {
    .is_connected      = atspi_is_connected,
    .init              = atspi_init_backend,
    .get_tree          = atspi_get_tree,
    .subscribe_changes   = atspi_subscribe_changes,
    .unsubscribe_changes = atspi_unsubscribe_changes,
    .destroy             = atspi_destroy_backend,
};

const MdA11yBackend *md_a11y_backend_create(void) {
    return &atspi_backend;
}
