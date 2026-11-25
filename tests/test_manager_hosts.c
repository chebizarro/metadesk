#include "hosts-page.h"
#include "host-qr.h"
#include "managed_store.h"
#include "model.h"
#include "nostr.h"
#include <nostr/nip19/nip19.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

#define HOST "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define TAG "86b09912-5e73-4bb0-a708-30c770882ace"
#define MEMBERS "[[\"p\",\"" HOST "\"]]"
static gboolean expired(gpointer data) { *(gboolean *)data = TRUE; return G_SOURCE_REMOVE; }
static gboolean focus_inside(GtkWindow *window, GtkWidget *container) {
    GtkWidget *focus = gtk_window_get_focus(window);
    return focus && (focus == container || gtk_widget_is_ancestor(focus, container));
}
#define WAIT_UNTIL(predicate) do { \
    gboolean deadline = FALSE; \
    guint id = g_timeout_add_seconds(8, expired, &deadline); \
    while (!(predicate) && !deadline) g_main_context_iteration(NULL, TRUE); \
    if (!deadline) g_source_remove(id); \
    g_assert_true(predicate); \
} while (0)

static void seed(void) {
    MdAccountStartup startup = {0};
    g_assert_cmpint(md_account_startup_open(MD_ACCOUNT_CLIENT, &startup), ==, 0);
    g_assert_nonnull(startup.binding);
    g_autofree char *state = g_build_filename(g_get_user_state_dir(), "metadesk", NULL);
    g_assert_cmpint(g_mkdir_with_parents(state, 0700), ==, 0);
    MdManagedStore *store = md_managed_store_open(startup.binding, state);
    g_assert_nonnull(store);
    const char *coords[] = {MD_STORE_HOSTS, MD_STORE_MANAGEMENT, MD_STORE_FAVORITES, MD_STORE_TAG_PREFIX TAG};
    const char *plain[] = {MEMBERS,
        "{\"schema_version\":1,\"tags\":{\"" TAG "\":{\"label\":\"Lab\"}},"
        "\"hosts\":{\"" HOST "\":{\"label\":\"Cached Test Host\",\"notes\":\"Cached note\"}}}", MEMBERS, MEMBERS};
    const char *route[] = {"ws://127.0.0.1:1"};
    for (size_t i = 0; i < G_N_ELEMENTS(coords); ++i) {
        g_assert_cmpint(md_managed_store_backfill(store, coords[i], true), ==, MD_STORE_OK);
        g_assert_cmpint(md_managed_store_stage(store, coords[i], "", plain[i], route, 1, time(NULL)), ==, MD_STORE_PENDING);
        char *wire = NULL;
        g_assert_cmpint(md_managed_store_outgoing(store, coords[i], route[0], &wire), ==, MD_STORE_PENDING);
        NostrEvent *event = nostr_event_new();
        g_assert_cmpint(nostr_event_deserialize(event, wire), ==, 0);
        char *id = nostr_event_get_id(event);
        g_assert_cmpint(md_managed_store_record_result(store, coords[i], route[0], id, MD_STORE_RELAY_ACKNOWLEDGED), ==, MD_STORE_OK);
        free(id); free(wire); nostr_event_free(event);
    }
    g_assert_cmpint(md_managed_store_record(store, HOST, time(NULL), MD_RECENT_CONNECTED), ==, MD_STORE_OK);
    md_managed_store_close(store);
    md_account_startup_clear(&startup);
}

static GtkWidget *find(GtkWidget *root, const char *name, const char *text) {
    if (name && !strcmp(gtk_widget_get_name(root), name)) return root;
    if (text && ((ADW_IS_BANNER(root) && !g_strcmp0(adw_banner_get_title(ADW_BANNER(root)), text)) ||
                 (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text)) ||
                 (GTK_IS_LABEL(root) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text)) ||
                 (ADW_IS_ACTION_ROW(root) && !g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(root)), text)))) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *match = find(child, name, text);
        if (match) return match;
    }
    return NULL;
}

static void keyboard(const char *keys, gboolean focus_window) {
    g_autofree char *tool = g_find_program_in_path("xdotool");
    g_assert_nonnull(tool);
    g_autofree char *cmd = focus_window ?
        g_strdup_printf("%s search --onlyvisible --pid %ld windowfocus key --clearmodifiers %s", tool, (long)getpid(), keys) :
        g_strdup_printf("%s key --clearmodifiers %s", tool, keys);
    g_autoptr(GError) err = NULL;
    int status;
    g_assert_true(g_spawn_command_line_sync(cmd, NULL, NULL, &status, &err));
    g_assert_no_error(err);
    g_assert_true(g_spawn_check_wait_status(status, &err));
}

int main(void) {
    g_assert_cmpstr(g_getenv("GTK_A11Y"), ==, "test");
    adw_init();
    seed();
    g_autoptr(GtkApplication) app = gtk_application_new("org.metadesk.HostsFixture", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(app), NULL, NULL));
    GtkWindow *window = GTK_WINDOW(adw_application_window_new(app));
    GtkWidget *page = md_manager_hosts_page_new();
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), page);
    gtk_window_set_default_size(window, 360, 540);
    gtk_window_present(window);
    g_autofree char *directory = md_manager_config_directory();
    md_manager_hosts_configure(page, directory, g_getenv("MD_FIXTURE_CLIENT"), NULL);
    WAIT_UNTIL(find(page, NULL, "Cached Test Host") != NULL);
    g_assert_cmpint(gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(window))), ==, 360);
    g_assert_nonnull(find(page, NULL, "Offline — choose bootstrap relays in Relays to connect"));
    GtkWidget *connect = find(page, NULL, "_Review and Connect…");
    g_assert_nonnull(connect);
    g_assert_false(gtk_widget_get_sensitive(connect));
    GtkWidget *address = find(page, "host-identity", NULL);
    g_assert_nonnull(address);
    g_assert_true(gtk_widget_grab_focus(address));
    GtkWidget *before = gtk_window_get_focus(window);
    keyboard("Tab", TRUE);
    WAIT_UNTIL(gtk_window_get_focus(window) != before);
    keyboard("shift+Tab", FALSE);
    WAIT_UNTIL(gtk_window_get_focus(window) == before);
    GtkWidget *row = find(page, NULL, "Cached Test Host");
    g_assert_true(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), "Favorite") != NULL);
    g_assert_true(strstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), "Last connected on this device") != NULL);

    const char *const secrets[] = {"NSEC1fixture", " nostr:Nsec1fixture", "NCRYPTSEC1fixture"};
    for (size_t i = 0; i < G_N_ELEMENTS(secrets); ++i) {
        gtk_editable_set_text(GTK_EDITABLE(address), secrets[i]);
        g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(address)), ==, "");
        g_assert_nonnull(find(page, NULL, "Private keys are not host addresses. Use a public host identity."));
    }
    const char *unreachable[] = {g_getenv("MD_FIXTURE_UNREACHABLE"), NULL};
    g_assert_nonnull(unreachable[0]);
    md_manager_hosts_configure(page, directory, g_getenv("MD_FIXTURE_CLIENT"), unreachable);
    WAIT_UNTIL(find(page, NULL, "Cached Test Host") != NULL && find(page, NULL, "Offline — showing cached hosts") != NULL);
    g_assert_false(gtk_widget_get_sensitive(connect));
    row = find(page, NULL, "Cached Test Host");
    g_assert_true(gtk_widget_get_width(row) <= 360);

    /* One successful EOSE enables review without authorizing writes on an
     * incomplete fetch set. */
    const char *bootstrap[] = {g_getenv("MD_FIXTURE_RELAY"), unreachable[0], NULL};
    g_assert_nonnull(bootstrap[0]);
    md_manager_hosts_configure(page, directory, g_getenv("MD_FIXTURE_CLIENT"), bootstrap);
    WAIT_UNTIL(gtk_widget_get_sensitive(connect));
    gtk_editable_set_text(GTK_EDITABLE(address), "127.0.0.1");
    g_assert_true(gtk_widget_grab_focus(address));
    keyboard("Return", FALSE);
    WAIT_UNTIL(find(page, NULL, md_resolve_error(MD_RESOLVE_INVALID)) != NULL);
    g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    gtk_editable_set_text(GTK_EDITABLE(address), g_getenv("MD_FIXTURE_CLIENT"));
    keyboard("Return", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) != NULL);
    AdwAlertDialog *review = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    g_assert_cmpstr(adw_alert_dialog_get_heading(review), ==, "Review Host Key");
    const char *body = adw_alert_dialog_get_body(review);
    g_assert_nonnull(strstr(body, "c604 7f94 41ed 7d6d"));
    g_assert_nonnull(strstr(body, "NIP-05: Not provided"));
    g_assert_nonnull(strstr(body, "Requested access: View only"));
    g_assert_true(adw_alert_dialog_has_response(review, "cancel"));
    g_assert_false(adw_alert_dialog_has_response(review, "save"));
    g_assert_cmpstr(adw_alert_dialog_get_default_response(review), ==, "cancel");
    /* The dialog's requested focus can be set before the sheet is open.
     * Wait for the window to actually hand keyboard focus into the dialog. */
    WAIT_UNTIL(focus_inside(window, GTK_WIDGET(review)));
    g_assert_cmpint(gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(window))), ==, 360);
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(review)), <=, 360);
    keyboard("Escape", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
    /* The same QR generator/scanner used by the page must round-trip. */
    NostrProfilePointer ptr = {.public_key = HOST};
    char *profile = NULL;
    g_assert_cmpint(nostr_nip19_encode_nprofile(&ptr, &profile), ==, 0);
    g_autoptr(GdkTexture) texture = md_manager_qr_texture(profile);
    g_assert_nonnull(texture);
    g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(texture);
    g_autofree char *decoded = md_manager_qr_scan(png, NULL);
    g_assert_cmpstr(decoded, ==, profile);
    free(profile);
    g_object_add_weak_pointer(G_OBJECT(page), (gpointer *)&page);
    md_manager_hosts_stop(page);
    /* The final task owns the page until the worker closes the live bridge,
     * even when the window is destroyed before that task returns. */
    gtk_window_destroy(window);
    g_assert_nonnull(page);
    WAIT_UNTIL(page == NULL);
    md_nostr_shutdown();
    return 0;
}
