#include "signer-status.h"
#include <string.h>

#define CONTROL_NAME "org.nostr.Signer"
#define CONTROL_PATH "/org/nostr/signer/grotto"
#define CONTROL_INTERFACE "org.nostr.Grotto.Control1"

struct _MdSignerStatus {
    GObject parent_instance;
    GDBusProxy *proxy;
    GCancellable *cancel;
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
static void proxy_done(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source;
    MdSignerStatus *self = data;
    self->proxy = g_dbus_proxy_new_for_bus_finish(result, NULL);
    if (!self->proxy) unknown(self, "Session bus unavailable — no signer status reported");
    else {
        g_signal_connect_object(self->proxy, "notify::g-name-owner", G_CALLBACK(owner_changed), self, 0);
        g_signal_connect_object(self->proxy, "g-signal", G_CALLBACK(status_signal), self, 0);
        md_signer_status_refresh(self);
    }
    g_object_unref(self);
}
static void md_signer_status_finalize(GObject *object) {
    MdSignerStatus *self = MD_SIGNER_STATUS(object);
    g_cancellable_cancel(self->cancel);
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
