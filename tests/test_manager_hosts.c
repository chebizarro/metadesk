#include "hosts-page.h"
#include "launch.h"
#ifdef MD_HAVE_MANAGER_QR
#include "host-qr.h"
#endif
#include "managed_store.h"
#include "managed_store_controller.h"
#include "model.h"
#include "nostr.h"
#include <nostr/nip19/nip19.h>
#include <nostr-event.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

#define HOST "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
#define SAVED_KEY "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
#define OUTDATED "Saved host details may be out of date until sync completes."
#define NAME "desk@example.invalid"
#define TAG "86b09912-5e73-4bb0-a708-30c770882ace"
#define MEMBERS "[[\"p\",\"" HOST "\"]]"
static unsigned launches;
static gint sync_starts, unreadable_meta;
MdStoreResult __real_md_store_controller_read(MdStoreController *, const char *, char *, char **, char **);
MdStoreResult __wrap_md_store_controller_read(MdStoreController *store, const char *coordinate, char *revision,
    char **winner, char **draft) {
    if (g_atomic_int_get(&unreadable_meta) && !strcmp(coordinate, MD_STORE_MANAGEMENT)) {
        *revision = '\0'; *winner = NULL; *draft = NULL;
        return MD_STORE_SIGNER;
    }
    return __real_md_store_controller_read(store, coordinate, revision, winner, draft);
}
bool __real_md_store_controller_start(MdStoreController *, const MdRelayPlan *);
bool __wrap_md_store_controller_start(MdStoreController *store, const MdRelayPlan *plan) {
    g_assert_true(plan && plan->count);
    g_atomic_int_inc(&sync_starts);
    return __real_md_store_controller_start(store, plan);
}
static const char *name_key;
MdResolveResult __real_md_identity_resolve(const char *, const char *, MdIdentity *);
MdResolveResult __wrap_md_identity_resolve(const char *input, const char *pin, MdIdentity *out) {
    if (!strcmp(input, NAME)) {
        g_autofree char *json = g_strdup_printf("{\"names\":{\"desk\":\"%s\"}}", name_key);
        return md_identity_from_nip05_json(input, json, pin, out);
    }
    return __real_md_identity_resolve(input, pin, out);
}
MdManagerLaunch *__wrap_md_manager_launch_start(const char *client, const MdManagedLaunch *config,
    MdManagerLaunchStatus status, gpointer data, GError **error) {
    (void)client; (void)config; (void)status; (void)data; (void)error;
    ++launches;
    return NULL; /* A review fixture must never start a video client. */
}
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

static void commit_doc(MdManagedStore *store, const char *coord, const char *plain) {
    const char *route[] = {"ws://127.0.0.1:1"};
    g_assert_cmpint(md_managed_store_backfill(store, coord, true), ==, MD_STORE_OK);
    g_assert_cmpint(md_managed_store_stage(store, coord, "", plain, route, 1, time(NULL)), ==, MD_STORE_PENDING);
    char *wire = NULL;
    g_assert_cmpint(md_managed_store_outgoing(store, coord, route[0], &wire), ==, MD_STORE_PENDING);
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_signed(event, wire, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
    char *id = nostr_event_get_id(event);
    g_assert_cmpint(md_managed_store_record_result(store, coord, route[0], id, MD_STORE_RELAY_ACKNOWLEDGED), ==, MD_STORE_OK);
    free(id); free(wire); nostr_event_free(event);
}

static void seed(void) {
    MdAccountStartup startup = {0};
    g_assert_cmpint(md_account_startup_open(MD_ACCOUNT_CLIENT, &startup), ==, 0);
    g_assert_nonnull(startup.binding);
    g_autofree char *state = g_build_filename(g_get_user_state_dir(), "metadesk", NULL);
    g_assert_cmpint(g_mkdir_with_parents(state, 0700), ==, 0);
    MdManagedStore *store = md_managed_store_open(startup.binding, state);
    g_assert_nonnull(store);
    const char *coords[] = {MD_STORE_HOSTS, MD_STORE_MANAGEMENT, MD_STORE_FAVORITES, MD_STORE_TAG_PREFIX TAG};
    const char *plain[] = {"[[\"p\",\"" HOST "\"],[\"p\",\"" SAVED_KEY "\"]]",
        "{\"schema_version\":1,\"tags\":{\"" TAG "\":{\"label\":\"Lab\"}},"
        "\"hosts\":{\"" HOST "\":{\"label\":\"Cached Test Host\",\"notes\":\"Cached note\",\"nip05\":\"" NAME "\"},"
        "\"" SAVED_KEY "\":{\"label\":\"Cached Key Only\",\"requested_caps\":[\"video\",\"input\"]}}}", MEMBERS, MEMBERS};
    for (size_t i = 0; i < G_N_ELEMENTS(coords); ++i) commit_doc(store, coords[i], plain[i]);
    g_assert_cmpint(md_managed_store_record(store, HOST, time(NULL), MD_RECENT_CONNECTED), ==, MD_STORE_OK);
    md_managed_store_close(store);
    md_account_startup_clear(&startup);
}

static void seed_retired(const char *directory, const char *key) {
    MdAccountStartup startup = {.store = md_credential_store_default()};
    g_assert_cmpint(md_accounts_open_snapshot(directory, startup.store, &startup.accounts), ==, 0);
    g_assert_cmpint(md_accounts_restore_role(startup.accounts, MD_ACCOUNT_CLIENT, &startup.binding), ==, 0);
    g_autofree char *state = g_build_filename(g_get_user_state_dir(), "metadesk", NULL);
    MdManagedStore *store = md_managed_store_open(startup.binding, state);
    g_assert_nonnull(store);
    g_autofree char *hosts = g_strdup_printf("[[\"p\",\"%s\"]]", key);
    g_autofree char *meta = g_strdup_printf("{\"schema_version\":1,\"hosts\":{\"%s\":{\"label\":\"Pending removal\"}}}", key);
    commit_doc(store, MD_STORE_HOSTS, hosts);
    commit_doc(store, MD_STORE_MANAGEMENT, meta);
    char revision[65];
    g_assert_cmpint(md_managed_store_read(store, MD_STORE_HOSTS, revision, NULL, NULL), ==, MD_STORE_OK);
    const char *route[] = {g_getenv("MD_FIXTURE_RELAY")};
    g_assert_cmpint(md_managed_store_stage(store, MD_STORE_HOSTS, revision, "[]", route, 1, time(NULL)+1), ==, MD_STORE_PENDING);
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

static void type_text(const char *text) {
    g_autofree char *tool = g_find_program_in_path("xdotool");
    g_assert_nonnull(tool);
    char *argv[] = {tool, "type", "--clearmodifiers", "--", (char *)text, NULL};
    int status;
    g_assert_true(g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL));
    g_assert_true(g_spawn_check_wait_status(status, NULL));
}

static GtkWidget *find_edit(GtkWidget *root) {
    if (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_widget_get_tooltip_text(root), "Edit Saved Host")) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *edit = find_edit(child);
        if (edit) return edit;
    }
    return NULL;
}

static AdwAlertDialog *dialog(GtkWindow *window) {
    return ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
}
static void submit(GtkWidget *address, GtkWidget *button, const char *value) {
    g_assert_true(gtk_widget_is_sensitive(button));
    gtk_editable_set_text(GTK_EDITABLE(address), value);
    g_signal_emit_by_name(button, "clicked");
}
static void review_saved_details(GtkWindow *window, GtkWidget *address, GtkWidget *button, gboolean outdated) {
    submit(address, button, SAVED_KEY);
    WAIT_UNTIL(dialog(window) != NULL);
    AdwAlertDialog *review = dialog(window);
    g_assert_nonnull(strstr(adw_alert_dialog_get_body(review), "Requested access: Video and input"));
    g_assert_cmpint(find(GTK_WIDGET(review), NULL, OUTDATED) != NULL, ==, outdated);
    WAIT_UNTIL(focus_inside(window, GTK_WIDGET(review)));
    keyboard("Escape", TRUE);
    WAIT_UNTIL(!dialog(window));
}
static void review_without_sync(GtkWindow *window, GtkWidget *page, GtkWidget *address, GtkWidget *button) {
    submit(address, button, NAME);
    g_assert_nonnull(find(page, NULL, "Wait for the address book to sync before reviewing a NIP-05 name."));
    submit(address, button, HOST); /* saved name; raw spelling doesn't bypass the barrier */
    g_assert_nonnull(find(page, NULL, "Wait for the address book to sync before reviewing a NIP-05 name."));
    g_assert_null(dialog(window));
    unsigned before = launches;
    submit(address, button, g_getenv("MD_FIXTURE_CLIENT"));
    WAIT_UNTIL(dialog(window) != NULL);
    AdwAlertDialog *review = dialog(window);
    g_assert_nonnull(strstr(adw_alert_dialog_get_body(review), "Fingerprint:"));
    g_assert_true(adw_alert_dialog_get_response_enabled(review, "connect"));
    g_assert_false(adw_alert_dialog_get_response_enabled(review, "save"));
    g_assert_nonnull(find(GTK_WIDGET(review), NULL, "Saving requires a synced, writable address book."));
    g_assert_null(find(GTK_WIDGET(review), NULL, OUTDATED));
    g_assert_cmpuint(launches, ==, before);
    WAIT_UNTIL(focus_inside(window, GTK_WIDGET(review)));
    keyboard("Escape", TRUE);
    WAIT_UNTIL(!dialog(window));
    review_saved_details(window, address, button, TRUE);
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
    name_key = g_getenv("MD_FIXTURE_CLIENT");
    /* Fresh empty-list page opens the real cache without even asking the
     * controller to start a network plan. */
    GtkWidget *empty = g_object_ref_sink(md_manager_hosts_page_new());
    md_manager_hosts_configure(empty, directory, name_key, NULL);
    WAIT_UNTIL(find(empty, NULL, "Cached Test Host"));
    g_assert_cmpint(g_atomic_int_get(&sync_starts), ==, 0);
    g_assert_nonnull(find(empty, NULL, "Offline — choose relays to sync and connect"));
    GtkWidget *empty_connect = find(empty, NULL, "_Review and Connect…");
    GtkWidget *empty_address = find(empty, "host-identity", NULL);
    g_assert_nonnull(empty_connect);
    g_assert_nonnull(empty_address);
    gtk_editable_set_text(GTK_EDITABLE(empty_address), name_key);
    g_assert_false(gtk_widget_is_sensitive(empty_connect));
    g_object_add_weak_pointer(G_OBJECT(empty), (gpointer *)&empty);
    md_manager_hosts_stop(empty);
    g_object_unref(empty);
    WAIT_UNTIL(empty == NULL);

    const char *gated[] = {g_getenv("MD_FIXTURE_CATCHING_UP"), NULL};
    g_assert_nonnull(gated[0]);
    md_manager_hosts_configure(page, directory, g_getenv("MD_FIXTURE_CLIENT"), gated);
    GtkWidget *connect = find(page, NULL, "_Review and Connect…");
    GtkWidget *address = find(page, "host-identity", NULL);
    g_assert_nonnull(connect);
    g_assert_nonnull(address);
    WAIT_UNTIL(gtk_widget_get_sensitive(connect) && find(page, NULL,
        "Syncing address book…\nWait for sync to connect by name, or use the host’s public key."));
    g_assert_cmpint(g_atomic_int_get(&sync_starts), ==, 1);
    review_without_sync(window, page, address, connect);

    md_manager_hosts_configure(page, directory, g_getenv("MD_FIXTURE_CLIENT"), NULL);
    WAIT_UNTIL(find(page, NULL, "Cached Test Host"));
    g_assert_cmpint(gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(window))), ==, 360);
    g_assert_nonnull(find(page, NULL, "Offline — choose relays to sync and connect"));
    g_assert_cmpint(g_atomic_int_get(&sync_starts), ==, 1);
    gtk_editable_set_text(GTK_EDITABLE(address), name_key);
    g_assert_false(gtk_widget_is_sensitive(connect));
    g_assert_null(dialog(window));
    g_assert_cmpuint(launches, ==, 0);
    gtk_editable_set_text(GTK_EDITABLE(address), "");
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
    WAIT_UNTIL(gtk_widget_get_sensitive(connect) && find(page, NULL,
        "Offline — showing cached hosts\nWait for sync to connect by name, or use the host’s public key."));
    review_without_sync(window, page, address, connect);
    gtk_editable_set_text(GTK_EDITABLE(address), "");
    row = find(page, NULL, "Cached Test Host");
    g_assert_true(gtk_widget_get_width(row) <= 360);

    /* One successful EOSE enables review without authorizing writes on an
     * incomplete fetch set. */
    const char *bootstrap[] = {g_getenv("MD_FIXTURE_RELAY"), unreachable[0], NULL};
    g_assert_nonnull(bootstrap[0]);
    md_manager_hosts_configure(page, directory, g_getenv("MD_FIXTURE_CLIENT"), bootstrap);
    WAIT_UNTIL(gtk_widget_get_sensitive(connect));
    g_assert_true(gtk_widget_grab_focus(address));
    type_text("127.0.0.1");
    keyboard("Return", FALSE);
    WAIT_UNTIL(find(page, NULL, md_resolve_error(MD_RESOLVE_INVALID)) != NULL);
    g_assert_null(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    keyboard("ctrl+a", FALSE);
    type_text(g_getenv("MD_FIXTURE_CLIENT"));
    keyboard("Return", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) != NULL);
    AdwAlertDialog *review = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    g_assert_cmpstr(adw_alert_dialog_get_heading(review), ==, "Review Host Key");
    const char *body = adw_alert_dialog_get_body(review);
    g_assert_nonnull(strstr(body, "c604 7f94 41ed 7d6d"));
    g_assert_nonnull(strstr(body, "NIP-05: Not provided"));
    g_assert_nonnull(strstr(body, "Requested access: View only"));
    g_assert_true(adw_alert_dialog_has_response(review, "cancel"));
    g_assert_false(adw_alert_dialog_get_response_enabled(review, "save"));
    g_assert_cmpstr(adw_alert_dialog_get_default_response(review), ==, "cancel");
    /* The dialog's requested focus can be set before the sheet is open.
     * Wait for the window to actually hand keyboard focus into the dialog. */
    WAIT_UNTIL(focus_inside(window, GTK_WIDGET(review)));
    g_assert_cmpint(gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(window))), ==, 360);
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(review)), <=, 360);
    keyboard("Escape", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL);
    /* Restore the full write barrier and save a first-contact key by keyboard. */
    const char *online[] = {bootstrap[0], NULL};
    md_manager_hosts_configure(page, directory, g_getenv("MD_FIXTURE_CLIENT"), online);
    GtkWidget *banner = find(page, "hosts-sync-status", NULL);
    g_assert_true(ADW_IS_BANNER(banner));
    WAIT_UNTIL(gtk_widget_get_sensitive(connect) && !adw_banner_get_revealed(ADW_BANNER(banner)));
    review_saved_details(window, address, connect, FALSE);
    submit(address, connect, HOST);
    WAIT_UNTIL(dialog(window));
    g_assert_cmpstr(adw_alert_dialog_get_heading(dialog(window)), ==, "Host Key Changed");
    g_assert_false(adw_alert_dialog_has_response(dialog(window), "connect"));
    g_assert_false(adw_alert_dialog_get_response_enabled(dialog(window), "replace"));
    WAIT_UNTIL(focus_inside(window, GTK_WIDGET(dialog(window))));
    keyboard("Escape", FALSE);
    WAIT_UNTIL(!dialog(window));
    gtk_editable_set_text(GTK_EDITABLE(address), name_key);
    g_assert_true(gtk_widget_grab_focus(address));
    keyboard("Return", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) != NULL);
    review = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
    g_assert_true(adw_alert_dialog_has_response(review, "save"));
    WAIT_UNTIL(focus_inside(window, GTK_WIDGET(review)));
    keyboard("alt+s", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL &&
               gtk_widget_get_sensitive(connect) && find(page, NULL, "c604 7f94 41ed 7d6d") != NULL);
    GtkWidget *saved = find(page, NULL, "c604 7f94 41ed 7d6d");
    GtkWidget *edit = find_edit(saved);
    g_assert_nonnull(edit);
    g_assert_true(gtk_widget_grab_focus(edit));
    g_assert_true(gtk_widget_has_focus(edit));
    keyboard("space", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) != NULL);
    AdwDialog *editor = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
    g_assert_cmpstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(editor)), ==, "Edit Saved Host");
    WAIT_UNTIL(focus_inside(window, GTK_WIDGET(editor)));
    g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(editor)), <=, 360);
    type_text("Keyboard-edited host");
    keyboard("alt+s", FALSE);
    WAIT_UNTIL(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)) == NULL &&
               find(page, NULL, "Keyboard-edited host") != NULL);

#ifdef MD_HAVE_MANAGER_QR
    g_assert_nonnull(find(page, NULL, "Scan QR _Image…"));
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
#else
    g_assert_null(find(page, NULL, "Scan QR _Image…"));
#endif
    const char *second = g_getenv("MD_FIXTURE_SECOND_DIRECTORY");
    g_assert_true(g_file_set_contents(g_getenv("MD_FIXTURE_SIGNER_ROLE"), "host", -1, NULL));
    seed_retired(second, name_key);
    gint previous_starts = g_atomic_int_get(&sync_starts);
    md_manager_hosts_configure(page, second, HOST, online);
    GtkWidget *rotation = find(page, "client-key-rotation", NULL);
    g_assert_nonnull(rotation);
    g_assert_true(gtk_widget_get_visible(rotation));
    GtkWidget *guidance = gtk_expander_get_child(GTK_EXPANDER(rotation));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(guidance)), "The new client key does not inherit access"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(guidance)), "Cached Test Host"));
    WAIT_UNTIL(find(page, NULL, "Pending removal") && gtk_widget_get_sensitive(connect) &&
        !adw_banner_get_revealed(ADW_BANNER(banner)));
    g_assert_cmpint(g_atomic_int_get(&sync_starts), ==, previous_starts+1);
    submit(address, connect, name_key);
    g_assert_nonnull(find(page, NULL, "Reconcile pending or incomplete changes to this host before connecting."));
    g_assert_null(dialog(window));
    /* A signer refusal arriving during a review invalidates the metadata.
     * Do not let the refreshed projection treat an unknown pin as absent. */
    g_atomic_int_set(&unreadable_meta, TRUE);
    submit(address, connect, HOST);
    WAIT_UNTIL(find(page, NULL, "The saved hosts or their details could not be read. Host review is unavailable."));
    g_assert_false(gtk_widget_is_sensitive(connect));
    g_assert_null(find(page, NULL, "Wait for the address book to sync before reviewing a NIP-05 name."));
    g_assert_null(dialog(window));
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
