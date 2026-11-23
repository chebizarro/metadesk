#include "signer-status.h"
#include <string.h>
#ifdef MD_HAVE_GROTTO_CONTROL
#include <grotto-panels-control1-service.h>
#endif

#define CONTROL_NAME "org.nostr.Signer"
#define CONTROL_PATH "/org/nostr/signer/grotto"
#define CONTROL_INTERFACE "org.nostr.Grotto.Control1"

struct _MdSignerStatus {
    GObject parent_instance;
    GDBusProxy *proxy;
    GCancellable *cancel;
#ifdef MD_HAVE_GROTTO_CONTROL
    GrottoPanelsService *control;
    const char *remote;
#endif
    guint64 serial;
    char *connection;
    const char *key, *session, *signing;
};
G_DEFINE_TYPE(MdSignerStatus, md_signer_status, G_TYPE_OBJECT)

static void unknown(MdSignerStatus *self, const char *connection) {
    g_free(self->connection); self->connection = g_strdup(connection);
    self->key = "Unknown — key locking is not reported";
    self->session = "Unknown";
    self->signing = "Unknown";
    g_signal_emit_by_name(self, "changed");
}

typedef struct { MdSignerStatus *self; guint64 serial; char *owner; } Read;
static void status_done(GObject *source, GAsyncResult *result, gpointer data) {
    Read *read = data;
    MdSignerStatus *self = read->self;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    g_autofree char *owner = g_dbus_proxy_get_name_owner(self->proxy);
    if (read->serial == self->serial && !g_strcmp0(owner, read->owner)) {
        if (!reply) unknown(self, "Signer status unavailable — open the signer or refresh");
        else {
            g_autoptr(GVariant) values = NULL;
            g_variant_get(reply, "(@a{sv})", &values);
            gboolean enforced = FALSE, available = FALSE, locked = FALSE, paused = FALSE;
            const char *state = NULL;
            g_free(self->connection); self->connection = g_strdup("Connected to Grotto Control1");
            self->key = "Unknown — backend does not report enforced key locking";
            if (g_variant_lookup(values, "key_lock_enforced", "b", &enforced) && enforced &&
                g_variant_lookup(values, "key_state", "&s", &state)) {
                if (!strcmp(state, "locked")) self->key = "Locked";
                else if (!strcmp(state, "unlocked")) self->key = "Unlocked";
            }
            self->session = g_variant_lookup(values, "session_lock_available", "b", &available) && available &&
                g_variant_lookup(values, "session_locked", "b", &locked) ? (locked ? "Locked" : "Unlocked") : "Unknown";
            self->signing = g_variant_lookup(values, "paused", "b", &paused) ? (paused ? "Paused" : "Not paused") : "Unknown";
            g_signal_emit_by_name(self, "changed");
        }
    }
    g_free(read->owner); g_object_unref(self); g_free(read);
}

void md_signer_status_refresh(MdSignerStatus *self) {
    ++self->serial;
    if (!self->proxy) return;
    g_autofree char *owner = g_dbus_proxy_get_name_owner(self->proxy);
    if (!owner) { unknown(self, "Grotto is unavailable — no status reported"); return; }
    Read *read = g_new0(Read, 1);
    read->self = g_object_ref(self); read->serial = self->serial; read->owner = g_strdup(owner);
    g_dbus_connection_call(g_dbus_proxy_get_connection(self->proxy), owner, CONTROL_PATH,
        CONTROL_INTERFACE, "GetStatus", NULL, G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, self->cancel, status_done, read);
}

static void owner_changed(GObject *object, GParamSpec *pspec, gpointer data) {
    (void)object; (void)pspec;
    MdSignerStatus *self = data;
    unknown(self, "Checking signer status…");
    md_signer_status_refresh(self);
}
static void status_signal(GDBusProxy *proxy, const char *sender, const char *name, GVariant *args, gpointer data) {
    (void)proxy; (void)sender; (void)args;
    if (!strcmp(name, "StatusChanged")) md_signer_status_refresh(data);
}
#ifdef MD_HAVE_GROTTO_CONTROL
static void remote_status_changed(MdSignerStatus *self) {
    const char *remote = NULL;
    if (grotto_panels_service_is_available(self->control)) {
        GListModel *model = grotto_panels_service_get_model(self->control, GROTTO_PANELS_STATUS);
        g_autoptr(GrottoPanelsItem) item = model ? g_list_model_get_item(model, 0) : NULL;
        if (item) {
            const char *state = grotto_panels_item_get_metadata(item)->state;
            static const struct { const char *state, *label; } states[] = {
                {"DISABLED", "Disabled"}, {"STARTING", "Starting"},
                {"LISTENING", "Listening"}, {"DEGRADED", "Degraded"},
                {"OFFLINE", "Offline"}, {"STOPPING", "Stopping"}, {"ERROR", "Error"},
            };
            for (guint i = 0; i < G_N_ELEMENTS(states); ++i)
                if (!g_strcmp0(state, states[i].state)) { remote = states[i].label; break; }
        }
    }
    if (self->remote != remote) {
        self->remote = remote;
        g_signal_emit_by_name(self, "changed");
    }
}
static void control_availability_changed(GrottoPanelsService *service, MdSignerStatus *self) {
    (void)service;
    remote_status_changed(self);
}
static void control_status_changed(GListModel *model, guint position, guint removed,
                                   guint added, MdSignerStatus *self) {
    (void)model; (void)position; (void)removed; (void)added;
    remote_status_changed(self);
}
#endif

static void proxy_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    MdSignerStatus *self = data;
    self->proxy = g_dbus_proxy_new_for_bus_finish(result, NULL);
    if (!self->proxy) unknown(self, "Session bus unavailable — no signer status reported");
    else {
        g_signal_connect_object(self->proxy, "notify::g-name-owner", G_CALLBACK(owner_changed), self, 0);
        g_signal_connect_object(self->proxy, "g-signal", G_CALLBACK(status_signal), self, 0);
#ifdef MD_HAVE_GROTTO_CONTROL
        /* The SDK normalizes remote signer status, but not the lock fields.
         * Keep read-only GetStatus for those fields and older daemons. */
        self->control = grotto_panels_control1_service_new_manager(self->proxy);
        g_signal_connect_object(self->control, "availability-changed",
                                G_CALLBACK(control_availability_changed), self, 0);
        GListModel *model = grotto_panels_service_get_model(self->control, GROTTO_PANELS_STATUS);
        g_signal_connect_object(model, "items-changed",
                                G_CALLBACK(control_status_changed), self, 0);
#endif
        md_signer_status_refresh(self);
    }
    g_object_unref(self);
}
static void md_signer_status_finalize(GObject *object) {
    MdSignerStatus *self = MD_SIGNER_STATUS(object);
    g_cancellable_cancel(self->cancel);
#ifdef MD_HAVE_GROTTO_CONTROL
    g_clear_object(&self->control);
#endif
    g_clear_object(&self->cancel); g_clear_object(&self->proxy); g_free(self->connection);
    G_OBJECT_CLASS(md_signer_status_parent_class)->finalize(object);
}
static void md_signer_status_class_init(MdSignerStatusClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = md_signer_status_finalize;
    g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void md_signer_status_init(MdSignerStatus *self) {
    self->cancel = g_cancellable_new();
    unknown(self, "Checking signer status…");
}
MdSignerStatus *md_signer_status_new(void) {
    MdSignerStatus *self = g_object_new(MD_TYPE_SIGNER_STATUS, NULL);
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
        NULL, CONTROL_NAME, CONTROL_PATH, CONTROL_INTERFACE, self->cancel, proxy_done, g_object_ref(self));
    return self;
}
const char *md_signer_status_connection(MdSignerStatus *self) { return self->connection; }
const char *md_signer_status_key(MdSignerStatus *self) { return self->key; }
const char *md_signer_status_session(MdSignerStatus *self) { return self->session; }
const char *md_signer_status_signing(MdSignerStatus *self) { return self->signing; }
const char *md_signer_status_remote(MdSignerStatus *self) {
#ifdef MD_HAVE_GROTTO_CONTROL
    return self->remote;
#else
    (void)self;
    return NULL;
#endif
}
