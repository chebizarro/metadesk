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

/* libatspi is a process-wide runtime, not a per-MdA11yCtx connection.
 * In 2.52, atspi_exit disposes app caches but retains the app-name table;
 * reinitialization or deferred dispatch then resolves disposed NULL caches.
 * Keep library caches alive across backend instances and exit once,
 * at process shutdown, after all instance listeners/filters have detached. */
static gboolean atspi_runtime_initialized;
static gboolean atspi_runtime_exit_registered;

/* ── Internal: convert AtspiAccessible to MdA11yNode ─────────── */

static char *atspi_element_id(AtspiAccessible *acc) {
    AtspiObject *object = ATSPI_OBJECT(acc);
    if (!object || !object->app || !object->app->bus_name || !object->path)
        return NULL;
    return g_strdup_printf("atspi:%s:%s", object->app->bus_name, object->path);
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

    node->id = atspi_element_id(acc);

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
    GSourceFunc callback;
    gpointer data;
    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    bool done;
    atomic_bool published;
} AtspiContextOp;

static gboolean atspi_context_op(gpointer data) {
    AtspiContextOp *op = data;
    /* Explicit publication across the GLib source boundary, before touching
     * the request's mutex or borrowed stack payload. */
    if (!atomic_load_explicit(&op->published, memory_order_acquire)) abort();
    pthread_mutex_lock(&op->lock);
    GSourceFunc callback = op->callback;
    gpointer callback_data = op->data;
    pthread_mutex_unlock(&op->lock);
    callback(callback_data);
    pthread_mutex_lock(&op->lock);
    op->done = true;
    pthread_cond_signal(&op->done_cond);
    pthread_mutex_unlock(&op->lock);
    return G_SOURCE_REMOVE;
}

/* The owned dispatcher exists before every handoff. Publish both the request
 * and its completion through one pthread mutex, including stack-owned data.
 * Shutdown must enqueue: invoke_full can run inline and ignore priority. */
static void atspi_invoke_sync_priority(GSourceFunc callback, gpointer data,
                                        int priority) {
    GMainContext *context = g_main_context_default();
    bool owner = g_main_context_is_owner(context);
    if (owner && priority != G_PRIORITY_LOW) {
        callback(data);
        return;
    }
    AtspiContextOp op = { .callback = callback, .data = data };
    pthread_mutex_init(&op.lock, NULL);
    pthread_cond_init(&op.done_cond, NULL);
    atomic_init(&op.published, false);
    pthread_mutex_lock(&op.lock);
    GSource *source = g_idle_source_new();
    g_source_set_priority(source, priority);
    g_source_set_callback(source, atspi_context_op, &op, NULL);
    atomic_store_explicit(&op.published, true, memory_order_release);
    g_source_attach(source, context);
    g_source_unref(source);
    while (!op.done) {
        if (owner) {
            /* exit() can be called from a context callback. Drain the queued
             * low-priority operation without waiting on our own ownership. */
            pthread_mutex_unlock(&op.lock);
            g_main_context_iteration(context, TRUE);
            pthread_mutex_lock(&op.lock);
        } else {
            pthread_cond_wait(&op.done_cond, &op.lock);
        }
    }
    pthread_mutex_unlock(&op.lock);
    pthread_cond_destroy(&op.done_cond);
    pthread_mutex_destroy(&op.lock);
}

static void atspi_invoke_sync(GSourceFunc callback, gpointer data) {
    atspi_invoke_sync_priority(callback, data, G_PRIORITY_DEFAULT);
}

typedef struct {
    AtspiState *st;
    AtspiEventListener *listener;
    gboolean add;
    int result;
} AtspiListenerOp;

static gboolean atspi_listener_op(gpointer data) {
    AtspiListenerOp *op = data;
    if (op->add)
        op->result = atspi_register_change_events(op->st);
    else
        atspi_deregister_change_events(op->listener);
    return G_SOURCE_REMOVE;
}

static int atspi_update_listener(AtspiState *st, AtspiEventListener *listener,
                                  gboolean add) {
    AtspiListenerOp op = { .st = st, .listener = listener, .add = add };
    atspi_invoke_sync(atspi_listener_op, &op);
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

typedef struct { AtspiState *st; int result; } AtspiInitOp;

static void atspi_runtime_shutdown(void);

static gboolean atspi_init_on_context(gpointer data) {
    AtspiInitOp *op = data;
    AtspiState *st = op->st;
    op->result = atspi_runtime_initialized ? 1 : atspi_init();
    if (op->result != 0 && op->result != 1) return G_SOURCE_REMOVE;
    atspi_runtime_initialized = TRUE;
    if (!atspi_runtime_exit_registered) {
        if (atexit(atspi_runtime_shutdown) != 0) { op->result = -1; return G_SOURCE_REMOVE; }
        atspi_runtime_exit_registered = TRUE;
    }

    st->bus = atspi_get_a11y_bus();
    if (!st->bus) goto fail;
    /* Install the disconnect policy/filter before this context can dispatch
     * a disconnect, not after returning to the initializing caller. */
    dbus_connection_set_exit_on_disconnect(st->bus, FALSE);
    if (!dbus_connection_add_filter(st->bus, atspi_bus_filter, st, NULL))
        goto fail;
    if (!dbus_connection_get_is_connected(st->bus))
        atomic_store(&st->connected, false);
    return G_SOURCE_REMOVE;

fail:
    op->result = -1;
    return G_SOURCE_REMOVE;
}

static gboolean atspi_detach_on_context(gpointer data) {
    AtspiState *st = data;
    if (st->bus) dbus_connection_remove_filter(st->bus, atspi_bus_filter, st);
    st->bus = NULL;
    return G_SOURCE_REMOVE;
}

static gboolean atspi_runtime_exit_on_context(gpointer data) {
    AtspiState *runtime = data;
    if (atspi_runtime_initialized) atspi_exit();
    atspi_runtime_initialized = FALSE;
    /* No dispatch turn may run after disposing the global caches. The exit
     * operation runs below deferred-message priority, draining queued work
     * first, and quits in this same context operation, not a later idle. */
    g_main_loop_quit(runtime->event_loop);
    return G_SOURCE_REMOVE;
}

static void atspi_runtime_shutdown(void) {
    if (!atspi_runtime_initialized) return;
    /* Backend instances have detached and joined their dispatchers. Own a
     * dispatcher for the final context handoff, even when no caller has a
     * default main loop. Do not leave a thread alive between instances. */
    AtspiState runtime = {0};
    g_mutex_init(&runtime.lock);
    runtime.event_loop = g_main_loop_new(NULL, FALSE);
    runtime.event_thread = g_thread_new("md-atspi-exit", atspi_event_loop_thread, &runtime);
    atspi_invoke_sync_priority(atspi_runtime_exit_on_context, &runtime, G_PRIORITY_LOW);
    atspi_stop_event_thread(&runtime);
    g_main_loop_unref(runtime.event_loop);
    g_mutex_clear(&runtime.lock);
}

/* ── Vtable implementation ───────────────────────────────────── */

static int atspi_init_backend(MdA11yCtx *ctx) {
    AtspiState *st = calloc(1, sizeof(*st));
    if (!st) return -1;
    g_mutex_init(&st->lock);
    st->event_loop = g_main_loop_new(NULL, FALSE);
    atomic_init(&st->connected, true);
    st->event_thread = g_thread_try_new("md-atspi-events", atspi_event_loop_thread, st, NULL);
    if (!st->event_thread) goto fail;
    AtspiInitOp op = { .st = st };
    atspi_invoke_sync(atspi_init_on_context, &op);
    if (op.result != 0 && op.result != 1) {
        MD_LOG_E("failed to initialize AT-SPI2 (code %d)", op.result);
        goto fail;
    }
    ctx->backend_data = st;
    return 0;
fail:
    atspi_stop_event_thread(st);
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

typedef struct { MdA11yCtx *ctx; MdA11yNode **out_root; int result; } AtspiTreeOp;

static gboolean atspi_get_tree_on_context(gpointer data) {
    AtspiTreeOp *op = data;
    AtspiState *st = op->ctx->backend_data;
    g_mutex_lock(&st->lock);
    op->result = atspi_get_tree_unlocked(op->ctx, op->out_root);
    g_mutex_unlock(&st->lock);
    return G_SOURCE_REMOVE;
}

/* libatspi is single-threaded. Its async requests arm their notify only
 * after dbus_connection_send_with_reply() returns (get_application's
 * GetApplicationBusAddress from atspi_get_desktop, for example), and its
 * synchronous calls dispatch the connection from the calling thread. Walking
 * on a caller thread while another thread dispatches the default context lets
 * a reply complete before the notify is armed; libdbus then never runs it,
 * stranding libatspi's DBusPendingCall reference and reply (md-x16). Walk on
 * the context so no dispatch can interleave with libatspi's calls. */
static int atspi_get_tree(MdA11yCtx *ctx, MdA11yNode **out_root) {
    AtspiState *st = ctx ? ctx->backend_data : NULL;
    if (!st) return -1;

    AtspiTreeOp op = { .ctx = ctx, .out_root = out_root, .result = -1 };
    atspi_invoke_sync(atspi_get_tree_on_context, &op);
    return op.result;
}

typedef struct { int x, y; char *id; } AtspiHitOp;

static gboolean atspi_hit_on_context(gpointer data) {
    AtspiHitOp *op = data;
    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop) return G_SOURCE_REMOVE;
    AtspiComponent *component = atspi_accessible_get_component_iface(desktop);
    if (component) {
        GError *error = NULL;
        AtspiAccessible *hit = atspi_component_get_accessible_at_point(
            component, op->x, op->y, ATSPI_COORD_TYPE_SCREEN, &error);
        if (hit && !error) op->id = atspi_element_id(hit);
        if (hit) g_object_unref(hit);
        if (error) g_error_free(error);
        g_object_unref(component);
    }
    g_object_unref(desktop);
    /* A desktop without a global Component hit test is unsupported, not
     * permission to search apps in arbitrary order and click an occluder. */
    return G_SOURCE_REMOVE;
}

static char *atspi_hit_test(MdA11yCtx *ctx, int x, int y) {
    AtspiState *st = ctx ? ctx->backend_data : NULL;
    if (!st || !atomic_load(&st->connected)) return NULL;
    AtspiHitOp op = { .x = x, .y = y };
    atspi_invoke_sync(atspi_hit_on_context, &op);
    return op.id;
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
    /* Drain removal on the shared dispatcher before freeing filter userdata.
     * atspi_exit is process-wide and must not run on instance teardown. */
    atspi_invoke_sync(atspi_detach_on_context, st);
    atspi_stop_event_thread(st);
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
    .hit_test          = atspi_hit_test,
    .init              = atspi_init_backend,
    .get_tree          = atspi_get_tree,
    .subscribe_changes   = atspi_subscribe_changes,
    .unsubscribe_changes = atspi_unsubscribe_changes,
    .destroy             = atspi_destroy_backend,
};

const MdA11yBackend *md_a11y_backend_create(void) {
    return &atspi_backend;
}
