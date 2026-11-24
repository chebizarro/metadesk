#include "window.h"
#include "model.h"
#include "signer-status.h"
#include "access-page.h"
#include "management_ipc.h"
#include "relay_plan.h"
#include <stdlib.h>
#include <string.h>

struct _MdManagerWindow {
    AdwApplicationWindow parent_instance;
    AdwNavigationSplitView *split;
    AdwNavigationPage *content_page;
    AdwToastOverlay *toasts;
    AdwBanner *banner;
    GtkStack *pages;
    GtkListBox *navigation, *account_list, *relay_list;
    GtkWidget *identity_form, *relay_form, *refresh;
    AdwActionRow *key_status, *session_status, *signing_status, *remote_status;
    AdwComboRow *client_role, *host_role, *backend;
    AdwEntryRow *public_key, *account_label, *relay_url;
    GtkButton *save_client, *save_host, *add_account, *add_relay;
    GtkLabel *account_error, *relay_error;
    MdSignerStatus *status;
    MdManagerAccess *access;
    char *directory;
    GPtrArray *accounts, *choices;
    char **relays;
    gboolean busy, closed;
};
G_DEFINE_TYPE(MdManagerWindow, md_manager_window, ADW_TYPE_APPLICATION_WINDOW)
enum { PROP_0, PROP_BUSY, N_PROPERTIES };
static GParamSpec *properties[N_PROPERTIES];

static const char *sections[] = {"Hosts", "This Computer", "Access", "Identity", "Relays", "Activity"};
static const char *icons[] = {"network-server-symbolic", "computer-symbolic", "changes-allow-symbolic",
                              "avatar-default-symbolic", "network-workgroup-symbolic", "document-open-recent-symbolic"};

static void update_banner(MdManagerWindow *self) {
    const char *message = md_signer_status_connection(self->status);
    if (g_str_has_prefix(message, "Connected")) {
        gboolean client = FALSE;
        for (guint i = 0; self->accounts && i < self->accounts->len; ++i)
            client |= ((MdManagerAccount *)g_ptr_array_index(self->accounts, i))->roles[MD_ACCOUNT_CLIENT];
        if (!client) message = "Assign a client account in Identity before managed connections become available";
        else if (!self->relays || !self->relays[0]) message = "Choose bootstrap relays in Relays — no default is selected";
        else message = "Use Access to manage a running local host; host startup and managed connections are not integrated yet";
    }
    adw_banner_set_title(self->banner, message);
}
static void status_changed(MdSignerStatus *status, gpointer data) {
    MdManagerWindow *self = data;
    if (self->closed) return;
    adw_action_row_set_subtitle(self->key_status, md_signer_status_key(status));
    adw_action_row_set_subtitle(self->session_status, md_signer_status_session(status));
    adw_action_row_set_subtitle(self->signing_status, md_signer_status_signing(status));
    const char *remote = md_signer_status_remote(status);
    adw_action_row_set_subtitle(self->remote_status, remote);
    gtk_widget_set_visible(GTK_WIDGET(self->remote_status), remote != NULL);
    update_banner(self);
}
static void set_busy(MdManagerWindow *self, gboolean busy) {
    self->busy = busy;
    gtk_widget_set_sensitive(self->identity_form, !busy);
    gtk_widget_set_sensitive(self->relay_form, !busy);
    gtk_widget_set_sensitive(self->refresh, !busy);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BUSY]);
}
static void navigate(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    if (!row) return;
    MdManagerWindow *self = data;
    const char *name = sections[gtk_list_box_row_get_index(row)];
    gtk_widget_set_visible(GTK_WIDGET(self->banner), strcmp(name,"Access") && strcmp(name,"Activity"));
    gtk_stack_set_visible_child_name(self->pages, name);
    adw_navigation_page_set_title(self->content_page, name);
    adw_navigation_split_view_set_show_content(self->split, TRUE);
}
static void clear_list(GtkListBox *list) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list)))) gtk_list_box_remove(list, child);
}
static GtkWidget *action_row(const char *title, const char *subtitle) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 2);
    return row;
}
static void populate_accounts(MdManagerWindow *self) {
    clear_list(self->account_list);
    g_clear_pointer(&self->choices, g_ptr_array_unref);
    self->choices = g_ptr_array_new();
    g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
    gtk_string_list_append(names, "Not assigned");
    guint selected[2] = {0, 0};
    for (guint i = 0; self->accounts && i < self->accounts->len; ++i) {
        MdManagerAccount *item = g_ptr_array_index(self->accounts, i);
        const char *label = *item->account.label ? item->account.label : "Unnamed Account";
        GtkWidget *row = adw_expander_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
        g_autofree char *detail = g_strdup_printf("%s · %s", md_accounts_backend_name(item->account.backend), item->fingerprint);
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), detail);
        GtkWidget *key = gtk_label_new(item->npub ? item->npub : "Public key could not be encoded");
        gtk_label_set_wrap(GTK_LABEL(key), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(key), PANGO_WRAP_CHAR);
        gtk_label_set_selectable(GTK_LABEL(key), TRUE);
        gtk_label_set_xalign(GTK_LABEL(key), 0);
        gtk_widget_set_margin_start(key, 12); gtk_widget_set_margin_end(key, 12);
        gtk_widget_set_margin_top(key, 12); gtk_widget_set_margin_bottom(key, 12);
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), key);
        gtk_list_box_append(self->account_list, row);
        if (item->account.backend == MD_SIGNER_DIRECT_KEY) continue;
        g_ptr_array_add(self->choices, item);
        g_autofree char *name = g_strdup_printf("%s · %s · %s", label,
            md_accounts_backend_name(item->account.backend), item->fingerprint);
        gtk_string_list_append(names, name);
        for (int role = 0; role < MD_ACCOUNT_ROLE_COUNT; ++role)
            if (item->roles[role]) selected[role] = self->choices->len;
    }
    if (!self->accounts || !self->accounts->len)
        gtk_list_box_append(self->account_list, action_row("No Accounts", "Add a public key from an external signer below"));
    adw_combo_row_set_model(self->client_role, G_LIST_MODEL(names));
    adw_combo_row_set_model(self->host_role, G_LIST_MODEL(names));
    adw_combo_row_set_selected(self->client_role, selected[MD_ACCOUNT_CLIENT]);
    adw_combo_row_set_selected(self->host_role, selected[MD_ACCOUNT_HOST]);
    gtk_widget_set_sensitive(GTK_WIDGET(self->save_client), self->accounts != NULL);
    gtk_widget_set_sensitive(GTK_WIDGET(self->save_host), self->accounts != NULL);
}

typedef enum { LOAD, ADD_ACCOUNT, SAVE_ROLE, SAVE_RELAYS, PROBE_RELAY } JobKind;
typedef struct {
    JobKind kind;
    MdSignerType backend;
    MdAccountRole role;
    char *key, *label;
    char **relays;
    guint relay_index;
} Job;
typedef struct { GPtrArray *accounts; char **relays; GError *account_error, *relay_error; } Snapshot;
static void job_free(gpointer data) {
    Job *job = data;
    g_free(job->key); g_free(job->label); g_strfreev(job->relays); g_free(job);
}
static void snapshot_free(gpointer data) {
    Snapshot *snapshot = data;
    g_clear_pointer(&snapshot->accounts, g_ptr_array_unref); g_strfreev(snapshot->relays);
    g_clear_error(&snapshot->account_error); g_clear_error(&snapshot->relay_error); g_free(snapshot);
}
static void work(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    (void)cancel;
    MdManagerWindow *self = source;
    Job *job = task_data;
    g_autoptr(GError) error = NULL;
    if (job->kind == PROBE_RELAY) {
        MdRelayTarget target = {job->key, MD_RELAY_CONFIGURED};
        g_task_return_int(task, md_relay_probe_auth(&target));
        return;
    }
    if (job->kind == ADD_ACCOUNT)
        md_manager_account_add(self->directory, job->backend, job->key, job->label, &error);
    else if (job->kind == SAVE_ROLE) {
        MdSigner *signer = NULL;
        MdCredentialStore *store = md_credential_store_default();
        int rc = job->key ? md_accounts_connect_signer(job->backend, job->key, store, &signer) : 0;
        if (rc) g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Signer verification failed (%d). Check the signer, selected key, pairing, and enabled build adapters.", rc);
        else md_manager_role_save(self->directory, job->role, job->backend, job->key, signer, &error);
        md_signer_destroy(signer); md_credential_store_destroy(store);
    } else if (job->kind == SAVE_RELAYS)
        md_manager_relays_save(self->directory, (const char *const *)job->relays, &error);
    if (error) { g_task_return_error(task, g_steal_pointer(&error)); return; }
    Snapshot *snapshot = g_new0(Snapshot, 1);
    snapshot->accounts = md_manager_accounts_load(self->directory, &snapshot->account_error);
    snapshot->relays = md_manager_relays_load(self->directory, &snapshot->relay_error);
    g_task_return_pointer(task, snapshot, snapshot_free);
}
static void populate_relays(MdManagerWindow *self);
static void work_done(GObject *source, GAsyncResult *result, gpointer data) {
    MdManagerWindow *self = MD_MANAGER_WINDOW(source);
    GTask *task = G_TASK(result);
    Job *job = g_task_get_task_data(task);
    g_autoptr(GError) error = NULL;
    if (job->kind == PROBE_RELAY) {
        MdRelayAuth auth = g_task_propagate_int(task, &error);
        if (!self->closed) {
            const char *status = error ? "Could not check relay authentication" :
                auth == MD_RELAY_AUTH_REQUIRED ? "This relay requires Nostr authentication" :
                auth == MD_RELAY_AUTH_NOT_ADVERTISED ? "This relay does not advertise required authentication" :
                "Could not read the relay information document";
            GtkListBoxRow *row = gtk_list_box_get_row_at_index(self->relay_list, job->relay_index);
            if (row) {
                g_autofree char *text = g_str_has_prefix(job->key, "ws://") ?
                    g_strdup_printf("Unencrypted connection — %s", status) : g_strdup(status);
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), text);
            }
            set_busy(self, FALSE);
        }
        g_application_release(G_APPLICATION(data));
        g_object_unref(data);
        return;
    }
    Snapshot *snapshot = g_task_propagate_pointer(task, &error);
    if (!self->closed) {
        GtkLabel *error_label = job->kind == SAVE_RELAYS ? self->relay_error : self->account_error;
        if (error) {
            g_autofree char *message = g_strdup_printf("Not saved: %s", error->message);
            gtk_label_set_text(error_label, message);
        } else {
            g_clear_pointer(&self->choices, g_ptr_array_unref);
            g_clear_pointer(&self->accounts, g_ptr_array_unref); g_strfreev(self->relays);
            self->accounts = g_steal_pointer(&snapshot->accounts);
            self->relays = g_steal_pointer(&snapshot->relays);
            gtk_label_set_text(self->account_error, snapshot->account_error ? snapshot->account_error->message : "");
            gtk_label_set_text(self->relay_error, snapshot->relay_error ? snapshot->relay_error->message : "");
            populate_accounts(self); populate_relays(self); update_banner(self);
            if (job->kind != LOAD) {
                const char *message = job->kind == ADD_ACCOUNT ? "Public account metadata saved" :
                    job->kind == SAVE_ROLE ? "Role saved — restart the host or client to apply" : "Relay settings saved locally";
                adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
                if (job->kind == ADD_ACCOUNT) {
                    gtk_editable_set_text(GTK_EDITABLE(self->public_key), "");
                    gtk_editable_set_text(GTK_EDITABLE(self->account_label), "");
                } else if (job->kind == SAVE_RELAYS) gtk_editable_set_text(GTK_EDITABLE(self->relay_url), "");
            }
        }
        set_busy(self, FALSE);
        if (error) gtk_widget_grab_focus(GTK_WIDGET(error_label));
    }
    if (snapshot) snapshot_free(snapshot);
    g_application_release(G_APPLICATION(data));
    g_object_unref(data);
}
static void start_job(MdManagerWindow *self, Job *job) {
    if (self->busy || self->closed) { job_free(job); return; }
    set_busy(self, TRUE);
    GApplication *app = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
    g_application_hold(app);
    GTask *task = g_task_new(self, NULL, work_done, g_object_ref(app));
    g_task_set_task_data(task, job, job_free);
    g_task_run_in_thread(task, work); g_object_unref(task);
}
static void refresh_clicked(GtkButton *button, gpointer data) {
    (void)button;
    MdManagerWindow *self = data;
    md_signer_status_refresh(self->status);
    start_job(self, g_new0(Job, 1));
}
static void add_account_clicked(GtkButton *button, gpointer data) {
    (void)button;
    MdManagerWindow *self = data;
    static const MdSignerType backends[] = {MD_SIGNER_NIP55L, MD_SIGNER_NIP5F, MD_SIGNER_NIP46};
    Job *job = g_new0(Job, 1); job->kind = ADD_ACCOUNT;
    job->backend = backends[adw_combo_row_get_selected(self->backend)];
    job->key = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->public_key)));
    job->label = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->account_label)));
    start_job(self, job);
}
static void save_role_clicked(GtkButton *button, gpointer data) {
    MdManagerWindow *self = data;
    Job *job = g_new0(Job, 1); job->kind = SAVE_ROLE;
    job->role = button == self->save_client ? MD_ACCOUNT_CLIENT : MD_ACCOUNT_HOST;
    guint selected = adw_combo_row_get_selected(job->role == MD_ACCOUNT_CLIENT ? self->client_role : self->host_role);
    if (selected) {
        if (!self->choices || selected > self->choices->len) { job_free(job); return; }
        MdManagerAccount *item = g_ptr_array_index(self->choices, selected - 1);
        job->backend = item->account.backend; job->key = g_strdup(item->account.pubkey);
    }
    start_job(self, job);
}
static void remove_relay_clicked(GtkButton *button, gpointer data) {
    MdManagerWindow *self = data;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "relay-index"));
    Job *job = g_new0(Job, 1); job->kind = SAVE_RELAYS;
    guint count = g_strv_length(self->relays);
    job->relays = g_new0(char *, count);
    for (guint i = 0, j = 0; i < count; ++i)
        if (i != index) job->relays[j++] = g_strdup(self->relays[i]);
    start_job(self, job);
}
static void probe_relay_clicked(GtkButton *button, gpointer data) {
    MdManagerWindow *self = data;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "relay-index"));
    if (self->busy || !self->relays || index >= g_strv_length(self->relays)) return;
    Job *job = g_new0(Job, 1);
    job->kind = PROBE_RELAY;
    job->relay_index = index;
    job->key = g_strdup(self->relays[index]);
    start_job(self, job);
}

static void populate_relays(MdManagerWindow *self) {
    clear_list(self->relay_list);
    for (guint i = 0; self->relays && self->relays[i]; ++i) {
        GtkWidget *row = action_row(self->relays[i], g_str_has_prefix(self->relays[i], "ws://") ?
                                    "Unencrypted connection — saved locally" : "Saved locally — not connected");
        GtkWidget *probe = gtk_button_new_from_icon_name("view-refresh-symbolic");
        gtk_widget_set_tooltip_text(probe, "Check Relay Authentication");
        gtk_accessible_update_property(GTK_ACCESSIBLE(probe), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Check Relay Authentication", -1);
        gtk_widget_set_valign(probe, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(probe), "relay-index", GUINT_TO_POINTER(i));
        g_signal_connect_object(probe, "clicked", G_CALLBACK(probe_relay_clicked), self, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), probe);
        GtkWidget *button = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_tooltip_text(button, "Remove Relay");
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(button), "relay-index", GUINT_TO_POINTER(i));
        g_signal_connect_object(button, "clicked", G_CALLBACK(remove_relay_clicked), self, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
        gtk_list_box_append(self->relay_list, row);
    }
    if (!self->relays || !self->relays[0])
        gtk_list_box_append(self->relay_list, action_row("No Relays Configured", "Add a bootstrap relay to enable discovery"));
    gtk_widget_set_sensitive(GTK_WIDGET(self->add_relay), self->relays != NULL);
}
static void add_relay_clicked(GtkButton *button, gpointer data) {
    (void)button;
    MdManagerWindow *self = data;
    if (!self->relays) return;
    Job *job = g_new0(Job, 1); job->kind = SAVE_RELAYS;
    guint count = g_strv_length(self->relays);
    job->relays = g_new0(char *, count + 2);
    for (guint i = 0; i < count; ++i) job->relays[i] = g_strdup(self->relays[i]);
    job->relays[count] = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->relay_url)));
    start_job(self, job);
}
static void add_placeholder(MdManagerWindow *self, guint section, const char *title, const char *description) {
    GtkWidget *status = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), icons[section]);
    adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
    adw_status_page_set_description(ADW_STATUS_PAGE(status), description);
    gtk_stack_add_named(self->pages, status, sections[section]);
}
static gboolean close_requested(GtkWindow *window, gpointer data) {
    (void)data;
    MD_MANAGER_WINDOW(window)->closed = TRUE;
    md_manager_access_stop(MD_MANAGER_WINDOW(window)->access);
    return FALSE;
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec) {
    if (id == PROP_BUSY) g_value_set_boolean(value, MD_MANAGER_WINDOW(object)->busy);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void dispose(GObject *object) {
    MdManagerWindow *self = MD_MANAGER_WINDOW(object);
    self->closed = TRUE;
    g_clear_object(&self->access);
    g_clear_object(&self->status);
    gtk_widget_dispose_template(GTK_WIDGET(self), MD_TYPE_MANAGER_WINDOW);
    G_OBJECT_CLASS(md_manager_window_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
    MdManagerWindow *self = MD_MANAGER_WINDOW(object);
    g_free(self->directory); g_clear_pointer(&self->choices, g_ptr_array_unref);
    g_clear_pointer(&self->accounts, g_ptr_array_unref); g_strfreev(self->relays);
    G_OBJECT_CLASS(md_manager_window_parent_class)->finalize(object);
}
static void md_manager_window_class_init(MdManagerWindowClass *klass) {
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->dispose = dispose; object->finalize = finalize; object->get_property = get_property;
    properties[PROP_BUSY] = g_param_spec_boolean("busy", NULL, NULL, FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object, N_PROPERTIES, properties);
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_template_from_resource(widget, "/org/metadesk/Manager/window.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget, MdManagerWindow, name)
    BIND(split); BIND(content_page); BIND(toasts); BIND(banner); BIND(pages); BIND(navigation);
    BIND(account_list); BIND(relay_list); BIND(identity_form); BIND(relay_form); BIND(refresh);
    BIND(key_status); BIND(session_status); BIND(signing_status); BIND(remote_status); BIND(client_role); BIND(host_role);
    BIND(backend); BIND(public_key); BIND(account_label); BIND(relay_url); BIND(save_client);
    BIND(save_host); BIND(add_account); BIND(add_relay); BIND(account_error); BIND(relay_error);
#undef BIND
}
static void md_manager_window_init(MdManagerWindow *self) {
    gtk_widget_init_template(GTK_WIDGET(self));
    for (guint i = 0; i < G_N_ELEMENTS(sections); ++i) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sections[i]);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(icons[i]));
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        gtk_list_box_append(self->navigation, row);
    }
    add_placeholder(self, 0, "Host Connections Are Coming", "Saved hosts, favorites, search, and managed client launch are planned in md-vb18. No connection is started here.");
    add_placeholder(self, 1, "Host Controls Are Coming", "Host lifecycle controls are planned in md-37rb. Use Access to connect to a running host and Activity to end its current session.");
    self->access = md_manager_access_new(GTK_WINDOW(self), self->pages);
    g_signal_connect(self->navigation, "row-activated", G_CALLBACK(navigate), self);
    g_signal_connect(self->refresh, "clicked", G_CALLBACK(refresh_clicked), self);
    g_signal_connect(self->add_account, "clicked", G_CALLBACK(add_account_clicked), self);
    g_signal_connect(self->save_client, "clicked", G_CALLBACK(save_role_clicked), self);
    g_signal_connect(self->save_host, "clicked", G_CALLBACK(save_role_clicked), self);
    g_signal_connect(self->add_relay, "clicked", G_CALLBACK(add_relay_clicked), self);
    g_signal_connect(self, "close-request", G_CALLBACK(close_requested), NULL);
    self->status = md_signer_status_new();
    g_signal_connect_object(self->status, "changed", G_CALLBACK(status_changed), self, 0);
    status_changed(self->status, self);
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->navigation, 0);
    gtk_list_box_select_row(self->navigation, first);
    gtk_stack_set_visible_child_name(self->pages, "Hosts");
}
GtkWindow *md_manager_window_new(GtkApplication *app, const char *directory) {
    MdManagerWindow *self = g_object_new(MD_TYPE_MANAGER_WINDOW, "application", app, NULL);
    self->directory = directory ? g_strdup(directory) : md_manager_config_directory();
    start_job(self, g_new0(Job, 1));
    return GTK_WINDOW(self);
}

MdManagerAccess *md_manager_window_access(MdManagerWindow *self) { return self->access; }
void md_manager_window_review(MdManagerWindow *self,const char *generation,const char *id) {
    GtkListBoxRow *row=gtk_list_box_get_row_at_index(self->navigation,2);
    gtk_list_box_select_row(self->navigation,row); navigate(self->navigation,row,self);
    md_manager_access_review(self->access,generation,id);
}
void md_manager_window_connect_host(MdManagerWindow *self,const char *path) {
    GtkListBoxRow *row=gtk_list_box_get_row_at_index(self->navigation,2);
    gtk_list_box_select_row(self->navigation,row); navigate(self->navigation,row,self);
    md_manager_access_connect(self->access,path);
}
