#include "window.h"
#include "model.h"
#include "signer-status.h"
#include "md_test.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static gboolean expired(gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
/* Only a failure watchdog; completion is always a state transition. */
#define WAIT_UNTIL(condition) do { \
    gboolean deadline = FALSE; \
    guint watchdog = g_timeout_add_seconds(10, expired, &deadline); \
    while (!(condition) && !deadline) g_main_context_iteration(NULL, TRUE); \
    if (!deadline) g_source_remove(watchdog); \
    MD_CHECK(condition); \
} while (0)

typedef struct {
    GDBusConnection *bus;
    guint registration;
    guint reads;
    gboolean locked, paused, enforce, unknown, hold;
    GDBusMethodInvocation *held;
} FakeControl;
static GVariant *status_reply(FakeControl *fake) {
    GVariantBuilder values;
    g_variant_builder_init(&values, G_VARIANT_TYPE_VARDICT);
    if (!fake->unknown) {
        g_variant_builder_add(&values, "{sv}", "key_state", g_variant_new_string(fake->locked ? "locked" : "unlocked"));
        g_variant_builder_add(&values, "{sv}", "key_lock_enforced", g_variant_new_boolean(fake->enforce));
        g_variant_builder_add(&values, "{sv}", "session_lock_available", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&values, "{sv}", "session_locked", g_variant_new_boolean(fake->locked));
        g_variant_builder_add(&values, "{sv}", "paused", g_variant_new_boolean(fake->paused));
    }
    return g_variant_new("(a{sv})", &values);
}
static void call(GDBusConnection *bus, const char *sender, const char *path,
    const char *interface, const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer data) {
    (void)bus; (void)sender; (void)path; (void)interface; (void)args;
    FakeControl *fake = data;
    MD_CHECK(!strcmp(method, "GetStatus")); /* No signer-control mutations. */
    ++fake->reads;
    if (fake->hold) { fake->held = g_object_ref(inv); fake->hold = FALSE; }
    else g_dbus_method_invocation_return_value(inv, status_reply(fake));
}
static const GDBusInterfaceVTable vtable = {.method_call = call};
static const char xml[] = "<node><interface name='org.nostr.Grotto.Control1'>"
    "<method name='GetStatus'><arg type='a{sv}' direction='out'/></method>"
    "<signal name='StatusChanged'><arg type='t'/></signal></interface></node>";
static void name_call(FakeControl *fake, const char *method) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(fake->bus, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", method,
        !strcmp(method, "RequestName") ? g_variant_new("(su)", "org.nostr.Signer", 4u) :
                                        g_variant_new("(s)", "org.nostr.Signer"),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    MD_CHECK(reply && !error);
}
static void changed(FakeControl *fake) {
    MD_CHECK(g_dbus_connection_emit_signal(fake->bus, NULL, "/org/nostr/signer/grotto",
        "org.nostr.Grotto.Control1", "StatusChanged", g_variant_new("(t)", (guint64)fake->reads), NULL));
}
static GObject *child(GtkWindow *window, const char *name) {
    GObject *object = gtk_widget_get_template_child(GTK_WIDGET(window), MD_TYPE_MANAGER_WINDOW, name);
    MD_CHECK(object != NULL); return object;
}
static gboolean busy(GtkWindow *window) {
    gboolean value = TRUE; g_object_get(window, "busy", &value, NULL); return value;
}
static gboolean keyboard(const char *keys) {
    g_autofree char *tool = g_find_program_in_path("xdotool");
    if (!tool) { g_print("SKIP native keyboard injection: xdotool is unavailable\n"); return FALSE; }
    g_autofree char *command = g_strdup_printf(
        "%s search --onlyvisible --pid %ld windowfocus key --clearmodifiers %s", tool, (long)getpid(), keys);
    int status = -1;
    g_autoptr(GError) error = NULL;
    MD_CHECK(g_spawn_command_line_sync(command, NULL, NULL, &status, &error));
    MD_CHECK(!error && g_spawn_check_wait_status(status, NULL));
    return !error && status == 0;
}
static gboolean frame_seen(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget; (void)clock; *(gboolean *)data = TRUE; return G_SOURCE_REMOVE;
}
static void page_shown(AdwNavigationPage *page, gpointer data) {
    (void)page; *(gboolean *)data = TRUE;
}
static int surface_width(GtkWindow *window) {
    return gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(window)));
}
static void check_frame_width(GtkWindow *window, GtkWidget *form) {
    gboolean seen = FALSE;
    gtk_widget_add_tick_callback(GTK_WIDGET(window), frame_seen, &seen, NULL);
    WAIT_UNTIL(seen);
    MD_CHECK(surface_width(window) == 360);
    MD_CHECK(gtk_widget_get_width(form) > 0 &&
             gtk_widget_get_width(form) <= gtk_widget_get_width(GTK_WIDGET(window)));
}
static void save_render(GtkWindow *window, const char *filename) {
    const char *directory = g_getenv("MD_MANAGER_SCREENSHOT_DIR");
    if (!directory) return;
    gboolean seen = FALSE;
    gtk_widget_add_tick_callback(GTK_WIDGET(window), frame_seen, &seen, NULL);
    WAIT_UNTIL(seen);
    GtkWidget *widget = GTK_WIDGET(window);
    int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
    g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
    MD_CHECK(node != NULL);
    if (!node) return;
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
    g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(
        gtk_native_get_renderer(GTK_NATIVE(window)), node, &bounds);
    g_autofree char *path = g_build_filename(directory, filename, NULL);
    MD_CHECK(gdk_texture_save_to_png(texture, path));
}
static void activate_button(GtkWindow *window, const char *name) {
    g_signal_emit_by_name(child(window, name), "clicked");
    WAIT_UNTIL(!busy(window));
}
static void cleanup(const char *directory) {
    GDir *dir = g_dir_open(directory, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            g_autofree char *path = g_build_filename(directory, name, NULL); MD_CHECK(!g_remove(path));
        }
        g_dir_close(dir);
    }
    MD_CHECK(!g_rmdir(directory));
}
int main(void) {
    adw_init();
    g_autoptr(GError) error = NULL;
    g_autoptr(AdwApplication) app = adw_application_new("org.metadesk.Manager.Test", G_APPLICATION_NON_UNIQUE);
    MD_CHECK(g_application_register(G_APPLICATION(app), NULL, &error));
    g_autofree char *directory = g_dir_make_tmp("md-w11-ui-XXXXXX", &error);
    MD_CHECK(directory && !error);
    if (!directory) return md_test_report();
    g_autoptr(MdSignerStatus) status = md_signer_status_new();
    WAIT_UNTIL(g_str_has_prefix(md_signer_status_connection(status), "Grotto is unavailable"));

    FakeControl fake = {.locked = TRUE, .paused = TRUE, .enforce = TRUE};
    fake.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, &error);
    fake.registration = g_dbus_connection_register_object(fake.bus, "/org/nostr/signer/grotto",
        info->interfaces[0], &vtable, &fake, NULL, &error);
    MD_CHECK(fake.registration && !error);
    name_call(&fake, "RequestName");
    WAIT_UNTIL(!strcmp(md_signer_status_key(status), "Locked"));
    MD_CHECK(!strcmp(md_signer_status_session(status), "Locked"));
    MD_CHECK(!strcmp(md_signer_status_signing(status), "Paused"));
    fake.locked = FALSE; fake.paused = FALSE; changed(&fake);
    WAIT_UNTIL(!strcmp(md_signer_status_key(status), "Unlocked"));
    MD_CHECK(!strcmp(md_signer_status_session(status), "Unlocked"));
    MD_CHECK(!strcmp(md_signer_status_signing(status), "Not paused"));
    fake.enforce = FALSE; changed(&fake);
    WAIT_UNTIL(g_str_has_prefix(md_signer_status_key(status), "Unknown"));
    fake.unknown = TRUE; changed(&fake);
    WAIT_UNTIL(!strcmp(md_signer_status_session(status), "Unknown"));
    MD_CHECK(!strcmp(md_signer_status_signing(status), "Unknown"));

    /* A superseded response cannot resurrect stale lock state. */
    fake.unknown = FALSE; fake.enforce = TRUE; fake.hold = TRUE;
    md_signer_status_refresh(status);
    WAIT_UNTIL(fake.held != NULL);
    fake.locked = TRUE; md_signer_status_refresh(status);
    WAIT_UNTIL(!strcmp(md_signer_status_key(status), "Locked"));
    fake.locked = FALSE;
    g_dbus_method_invocation_return_value(fake.held, status_reply(&fake));
    g_clear_object(&fake.held);
    /* This read is a bus-order barrier, not a guessed elapsed delay. */
    guint reads = fake.reads; md_signer_status_refresh(status);
    WAIT_UNTIL(fake.reads > reads && !strcmp(md_signer_status_key(status), "Unlocked"));

    GtkWindow *window = md_manager_window_new(GTK_APPLICATION(app), directory);
    g_object_ref(window);
    gboolean content_shown = FALSE;
    g_signal_connect(child(window, "content_page"), "shown", G_CALLBACK(page_shown), &content_shown);
    gtk_window_set_default_size(window, 360, 600);
    gtk_window_present(window);
    WAIT_UNTIL(!busy(window) && gtk_widget_get_width(GTK_WIDGET(window)) > 0);
    AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(child(window, "split"));
    WAIT_UNTIL(adw_navigation_split_view_get_collapsed(split));
    g_print("Narrow layout: window=%d, split=%d, surface=%d\n",
        gtk_widget_get_width(GTK_WIDGET(window)), gtk_widget_get_width(GTK_WIDGET(split)),
        gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(window))));
    MD_CHECK(surface_width(window) == 360);
    GtkListBox *navigation = GTK_LIST_BOX(child(window, "navigation"));
    GtkStack *pages = GTK_STACK(child(window, "pages"));
    MD_CHECK(gtk_widget_grab_focus(GTK_WIDGET(gtk_list_box_get_row_at_index(navigation, 0))));
    if (keyboard("Down Down Down Return")) {
        WAIT_UNTIL(!strcmp(gtk_stack_get_visible_child_name(pages), "Identity"));
        MD_CHECK(adw_navigation_split_view_get_show_content(split));
        WAIT_UNTIL(content_shown);
    }
    const char *names[] = {"Hosts", "This Computer", "Access", "Identity", "Relays", "Activity"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(navigation, i);
        MD_CHECK(row != NULL);
        g_signal_emit_by_name(navigation, "row-activated", row);
        MD_CHECK(!strcmp(gtk_stack_get_visible_child_name(pages), names[i]));
        MD_CHECK(adw_navigation_split_view_get_show_content(split));
        MD_CHECK(surface_width(window) == 360);
    }
    MD_CHECK(!gtk_list_box_get_row_at_index(navigation, 6));
    g_signal_emit_by_name(navigation, "row-activated", gtk_list_box_get_row_at_index(navigation, 3));
    WAIT_UNTIL(content_shown);
    gtk_editable_set_text(GTK_EDITABLE(child(window, "public_key")), "not a key");
    activate_button(window, "add_account");
    MD_CHECK(*gtk_label_get_text(GTK_LABEL(child(window, "account_error"))));
    const char *key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    gtk_editable_set_text(GTK_EDITABLE(child(window, "public_key")), key);
    gtk_editable_set_text(GTK_EDITABLE(child(window, "account_label")), "Smoke Account");
    activate_button(window, "add_account");
    MD_CHECK(!*gtk_label_get_text(GTK_LABEL(child(window, "account_error"))));
    AdwComboRow *role = ADW_COMBO_ROW(child(window, "client_role"));
    MD_CHECK(g_list_model_get_n_items(adw_combo_row_get_model(role)) == 2);
    MD_CHECK(adw_combo_row_get_selected(role) == 0); /* Adding never implicitly binds. */
    adw_combo_row_set_selected(role, 1);
    activate_button(window, "save_client");
    MD_CHECK(*gtk_label_get_text(GTK_LABEL(child(window, "account_error"))));
    g_autoptr(GPtrArray) unbound = md_manager_accounts_load(directory, &error);
    MD_CHECK(unbound && unbound->len == 1 &&
        !((MdManagerAccount *)g_ptr_array_index(unbound, 0))->roles[MD_ACCOUNT_CLIENT]);
    adw_combo_row_set_selected(role, 0);
    activate_button(window, "save_client"); /* Explicit no-role selection persists. */
    check_frame_width(window, GTK_WIDGET(child(window, "identity_form")));
    save_render(window, "fixture-identity-360.png");
    g_signal_emit_by_name(navigation, "row-activated", gtk_list_box_get_row_at_index(navigation, 4));
    gtk_editable_set_text(GTK_EDITABLE(child(window, "relay_url")), "https://invalid.example");
    activate_button(window, "add_relay");
    MD_CHECK(*gtk_label_get_text(GTK_LABEL(child(window, "relay_error"))));
    gtk_editable_set_text(GTK_EDITABLE(child(window, "relay_url")), "wss://Operator.Example:443");
    activate_button(window, "add_relay");
    MD_CHECK(!*gtk_label_get_text(GTK_LABEL(child(window, "relay_error"))));
    g_auto(GStrv) saved = md_manager_relays_load(directory, &error);
    MD_CHECK(saved && saved[0] && !strcmp(saved[0], "wss://operator.example/"));
    GtkListBox *relay_list = GTK_LIST_BOX(child(window, "relay_list"));
    AdwActionRow *relay = ADW_ACTION_ROW(gtk_list_box_get_row_at_index(relay_list, 0));
    MD_CHECK(!strcmp(adw_action_row_get_subtitle(relay), "Saved locally — not connected"));
    check_frame_width(window, GTK_WIDGET(child(window, "relay_form")));
    save_render(window, "fixture-relays-360.png");
    if (g_getenv("MD_MANAGER_SCREENSHOT_DIR")) {
        gtk_window_set_default_size(window, 1000, 900);
        WAIT_UNTIL(surface_width(window) == 1000 && !adw_navigation_split_view_get_collapsed(split));
        g_signal_emit_by_name(navigation, "row-activated", gtk_list_box_get_row_at_index(navigation, 3));
        save_render(window, "fixture-identity-wide.png");
        g_signal_emit_by_name(navigation, "row-activated", gtk_list_box_get_row_at_index(navigation, 4));
    }

    /* Traverse with the same GTK focus-navigation path used by Tab/Shift+Tab. */
    MD_CHECK(gtk_widget_grab_focus(GTK_WIDGET(child(window, "relay_url"))));
    GtkWidget *before = gtk_window_get_focus(window);
    MD_CHECK(gtk_widget_child_focus(GTK_WIDGET(window), GTK_DIR_TAB_FORWARD));
    MD_CHECK(gtk_window_get_focus(window) != before);
    MD_CHECK(gtk_widget_child_focus(GTK_WIDGET(window), GTK_DIR_TAB_BACKWARD));
    before = gtk_window_get_focus(window);
    if (keyboard("Tab")) {
        WAIT_UNTIL(gtk_window_get_focus(window) != before);
        if (keyboard("shift+Tab")) WAIT_UNTIL(gtk_window_get_focus(window) == before);
    }
    name_call(&fake, "ReleaseName");
    WAIT_UNTIL(g_str_has_prefix(md_signer_status_connection(status), "Grotto is unavailable"));
    WAIT_UNTIL(g_str_has_prefix(adw_action_row_get_subtitle(ADW_ACTION_ROW(child(window, "key_status"))), "Unknown"));

    gtk_window_close(window); g_object_unref(window);
    /* Close while a job is in flight: completion must not access disposed UI. */
    window = md_manager_window_new(GTK_APPLICATION(app), directory);
    g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);
    gtk_window_present(window);
    gtk_window_close(window);
    WAIT_UNTIL(window == NULL);
    g_dbus_connection_unregister_object(fake.bus, fake.registration);
    g_object_unref(fake.bus);
    cleanup(directory);
    return md_test_report();
}
