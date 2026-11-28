#include "hosts-page.h"
#ifdef MD_HAVE_MANAGER_QR
#include "host-qr.h"
#endif
#include "launch.h"
#include "managed_store_controller.h"
#include "model.h"
#include "md_time.h"
#include "session.h"
#include "stream.h"
#include <jansson.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

#define HOSTS_DATA "md-manager-hosts"
enum { HOSTS, META, FAVORITES, NDOCS };
static const char *const coordinates[NDOCS] = {MD_STORE_HOSTS, MD_STORE_MANAGEMENT, MD_STORE_FAVORITES};

typedef struct { char revision[65]; json_t *winner, *draft; MdStoreResult status; MdStoreSync sync; gboolean review_ready; } Doc;
typedef struct { Doc doc[NDOCS]; json_t *recents; GHashTable *tags; gboolean store; } View;
typedef enum {
    CONFIGURE, START_SYNC, READ, RESOLVE, EDIT, RETRY, RECORD, IMPORT, EXPORT, CLOSE,
#ifdef MD_HAVE_MANAGER_QR
    SCAN,
#endif
} Action;
typedef struct { char *d, *plain, rev[65]; MdStoreEdit mode; json_t *members; gboolean remove; } Edit;
typedef struct {
    Action action;
    guint generation;
    char *directory, *key, **relays, *address, *route, *coordinate;
    GPtrArray *edits;
    MdRecentOutcome outcome;
    GFile *file;
    char *exchange;
    GError *error;
    View *view;
    MdIdentity identity;
    MdResolveResult resolution;
    char review_hosts[65], review_meta[65];
    gboolean action_ok;
    MdStoreResult result;
} Job;

typedef struct {
    GtkWidget *page;
    GtkBox *content;
    GtkEntry *address, *route, *tag_filter;
    GtkSearchEntry *search;
    GtkListBox *saved, *quick;
    GtkButton *review, *launch_cancel, *export_button, *import_button;
#ifdef MD_HAVE_MANAGER_QR
    GtkButton *scan_button;
#endif
    GtkLabel *error, *progress, *relay_diagnostic;
    AdwBanner *banner;
    GtkWidget *rotation;
    GtkLabel *rotation_hosts;
    char *previous_client, *previous_hosts;
    GtkApplication *application;
    GWeakRef weak_page;
    gint closed, read_queued;
    gboolean busy, refresh_pending, configure_pending, start_pending, close_queued;
    guint generation;
    char *directory, *key, **relays;
    View *view;
    MdManagerLaunch *launch;
    char launched[65];
    MdLaunchMessage phase;
    gboolean record_pending;
    MdRecentOutcome recent_outcome;
    char recent_key[65];
    /* Only the serialized task worker accesses these. */
    MdAccountStartup account;
    MdStoreController *store;
} Hosts;

static void start(Hosts *, Job *);
static void render(Hosts *);
static void present_review(Hosts *, const Job *);
static void present_import(Hosts *, const char *);
static void kick(Hosts *);
static gboolean member_set(json_t *, const char *, gboolean);

static void wipe(char *s) {
    if (!s) return;
    volatile char *p = s;
    for (size_t i = strlen(s); i; --i) *p++ = 0;
    free(s);
}
static void clear_doc(Doc *d) {
    md_managed_store_discard_plaintext(d->winner);
    md_managed_store_discard_plaintext(d->draft);
}
static void free_doc(gpointer data) { clear_doc(data); g_free(data); }
static void free_view(View *v) {
    if (!v) return;
    for (size_t i = 0; i < NDOCS; ++i) clear_doc(&v->doc[i]);
    g_clear_pointer(&v->tags, g_hash_table_unref);
    md_managed_store_discard_plaintext(v->recents);
    g_free(v);
}
static void free_edit(gpointer data) {
    Edit *e = data;
    g_free(e->d);
    wipe(e->plain);
    json_decref(e->members);
    g_free(e);
}
static void free_job(gpointer data) {
    Job *j = data;
    g_free(j->directory); g_free(j->key); g_strfreev(j->relays);
    g_free(j->address); g_free(j->route); g_free(j->coordinate);
    g_clear_object(&j->file); g_clear_error(&j->error);
    wipe(j->exchange);
    g_clear_pointer(&j->edits, g_ptr_array_unref);
    free_view(j->view);
    md_identity_clear(&j->identity);
    g_free(j);
}
static void clear_worker(Hosts *p) {
    md_store_controller_close(p->store);
    p->store = NULL;
    md_account_startup_clear(&p->account);
}
static void free_page(gpointer data) {
    Hosts *p = data;
    g_atomic_int_set(&p->closed, TRUE);
    md_manager_launch_free(p->launch);
    g_assert(p->store == NULL && p->account.accounts == NULL && p->account.store == NULL);
    free_view(p->view);
    g_weak_ref_clear(&p->weak_page);
    g_clear_object(&p->application);
    g_free(p->directory); g_free(p->key); g_strfreev(p->relays);
    g_free(p->previous_client); g_free(p->previous_hosts);
    g_free(p);
}
static gboolean refresh_on_main(gpointer page) {
    Hosts *p = g_object_get_data(G_OBJECT(page), HOSTS_DATA);
    g_atomic_int_set(&p->read_queued, 0);
    if (!g_atomic_int_get(&p->closed)) { p->refresh_pending = TRUE; kick(p); }
    return G_SOURCE_REMOVE;
}
static void changed(void *data) {
    Hosts *p = data;
    if (g_atomic_int_get(&p->closed) || !g_atomic_int_compare_and_exchange(&p->read_queued, 0, 1)) return;
    GObject *page = g_weak_ref_get(&p->weak_page);
    if (page) g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, refresh_on_main, page, g_object_unref);
}
static json_t *parse(char *plain) {
    json_t *result = plain ? json_loads(plain, 0, NULL) : NULL;
    wipe(plain);
    return result;
}
static void read_doc(Hosts *p, Doc *d, const char *coord) {
    char *winner = NULL, *draft = NULL;
    d->status = md_store_controller_read(p->store, coord, d->revision, &winner, &draft);
    d->winner = parse(winner);
    d->draft = parse(draft);
    d->sync = md_store_controller_sync(p->store, coord);
    d->review_ready = md_store_controller_review_ready(p->store, coord);
}
static View *snapshot(Hosts *p) {
    View *v = g_new0(View, 1);
    v->store = p->store != NULL;
    if (!v->store) return v;
    for (size_t i = 0; i < NDOCS; ++i) read_doc(p, &v->doc[i], coordinates[i]);
    v->tags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_doc);
    const char *uuid;
    json_t *tag;
    json_object_foreach(json_object_get(v->doc[META].winner, "tags"), uuid, tag) {
        if (json_is_true(json_object_get(tag, "deleted"))) continue;
        g_autofree char *coord = g_strconcat(MD_STORE_TAG_PREFIX, uuid, NULL);
        if (!md_store_controller_watch(p->store, coord)) continue;
        Doc *doc = g_new0(Doc, 1);
        read_doc(p, doc, coord);
        g_hash_table_insert(v->tags, g_strdup(uuid), doc);
    }
    v->recents = parse(md_store_controller_recents(p->store));
    return v;
}
static GBytes *read_file(GFile *file, gsize limit, GError **error) {
    if (!g_file_is_native(file)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Choose a local file");
        return NULL;
    }
    g_autoptr(GFileInputStream) stream = g_file_read(file, NULL, error);
    if (!stream) return NULL;
    guint8 *bytes = g_malloc(limit + 1);
    gsize n;
    if (!g_input_stream_read_all(G_INPUT_STREAM(stream), bytes, limit + 1, &n, NULL, error)) {
        g_free(bytes); return NULL;
    }
    if (n > limit) {
        g_free(bytes);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "The selected file is too large");
        return NULL;
    }
    bytes[n] = 0;
    return g_bytes_new_take(bytes, n);
}

static void work(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
    (void)cancellable;
    Hosts *p = g_object_get_data(source, HOSTS_DATA);
    Job *j = data;
    j->result = MD_STORE_INVALID;
    switch (j->action) {
    case CONFIGURE: {
        clear_worker(p);
        if (!j->key) break;
        p->account.store = md_credential_store_default();
        if (md_accounts_open_snapshot(j->directory, p->account.store, &p->account.accounts)) break;
        const MdAccount *account = md_accounts_role(p->account.accounts, MD_ACCOUNT_CLIENT);
        if (!account || strcmp(account->pubkey, j->key) ||
            md_accounts_restore_role(p->account.accounts, MD_ACCOUNT_CLIENT, &p->account.binding)) break;
        g_autofree char *dir = g_build_filename(g_get_user_state_dir(), "metadesk", NULL);
        if (!g_mkdir_with_parents(dir, 0700))
            p->store = md_store_controller_open(p->account.binding, dir, changed, p);
        break;
    }
    case START_SYNC: {
        if (!p->store) break;
        MdRelayPlan plan = {0};
        gboolean valid = TRUE;
        for (size_t i = 0; j->relays && j->relays[i] && valid; ++i)
            valid = md_relay_plan_add(&plan, j->relays[i], MD_RELAY_CONFIGURED);
        j->action_ok = valid && md_store_controller_start(p->store, &plan);
        md_relay_plan_clear(&plan);
        break;
    }
    case RESOLVE:
        j->resolution = md_identity_resolve(j->address, NULL, &j->identity);
        break;
    case EDIT: {
        gboolean ready = p->store != NULL;
        for (guint i = 0; i < j->edits->len && ready; ++i) {
            Edit *e = g_ptr_array_index(j->edits, i);
            if (e->members) ready = md_store_controller_watch(p->store, e->d);
        }
        uint64_t deadline = md_monotonic_ns() + 10000000000ULL;
        for (guint i = 0; i < j->edits->len && ready; ++i) {
            Edit *e = g_ptr_array_index(j->edits, i);
            if (e->members) ready = md_store_controller_wait(p->store, e->d, deadline) == MD_SYNC_READY;
        }
        if (!ready) { j->result = MD_STORE_NOT_READY; break; }
        gboolean partial = FALSE;
        for (guint i = 0; p->store && i < j->edits->len; ++i) {
            Edit *e = g_ptr_array_index(j->edits, i);
            if (e->members) {
                Doc doc = {0};
                read_doc(p, &doc, e->d);
                if (doc.status != MD_STORE_OK) { j->result = doc.status; clear_doc(&doc); break; }
                json_t *list = doc.winner ? json_incref(doc.winner) : json_array();
                gboolean ok = list != NULL;
                size_t k; json_t *key;
                json_array_foreach(e->members, k, key)
                    ok = ok && member_set(list, json_string_value(key), !e->remove);
                if (ok) e->plain = json_dumps(list, JSON_COMPACT);
                memcpy(e->rev, doc.revision, sizeof(e->rev));
                json_decref(list);
                clear_doc(&doc);
                if (!e->plain) { j->result = MD_STORE_IO; break; }
            }
            j->result = md_store_controller_edit(p->store, e->d, e->mode, e->rev, e->plain);
            if (j->result == MD_STORE_PENDING || j->result == MD_STORE_PARTIAL)
                j->result = md_store_controller_flush(p->store, e->d);
            if (j->result == MD_STORE_PARTIAL) partial = TRUE;
            else if (j->result != MD_STORE_OK && j->result != MD_STORE_UNCHANGED) break;
        }
        if (partial && (j->result == MD_STORE_OK || j->result == MD_STORE_UNCHANGED)) j->result = MD_STORE_PARTIAL;
        break;
    }
    case RETRY:
        if (p->store) j->result = md_store_controller_flush(p->store, j->coordinate);
        break;
    case RECORD:
        if (p->store) j->result = md_store_controller_record(p->store, j->key, j->outcome);
        break;
    case IMPORT: {
        g_autoptr(GBytes) bytes = read_file(j->file, MD_STORE_MAX_CONTENT, &j->error);
        gsize len;
        const char *s = bytes ? g_bytes_get_data(bytes, &len) : NULL;
        if (s && !memchr(s, 0, len)) j->result = md_managed_store_import(s, &j->exchange);
        break;
    }
    case EXPORT:
        if (g_file_is_native(j->file))
            j->action_ok = g_file_replace_contents(j->file, j->exchange, strlen(j->exchange), NULL, FALSE,
                G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, &j->error);
        break;
#ifdef MD_HAVE_MANAGER_QR
    case SCAN: {
        g_autoptr(GBytes) bytes = read_file(j->file, 16 * 1024 * 1024, &j->error);
        if (bytes) j->address = md_manager_qr_scan(bytes, &j->error);
        break;
    }
#endif
    case READ:
        break;
    case CLOSE:
        clear_worker(p);
        break;
    }
    if (!g_atomic_int_get(&p->closed)) j->view = snapshot(p);
    g_task_return_boolean(task, TRUE);
}

static const char *store_error(MdStoreResult r) {
    switch (r) {
    case MD_STORE_OK: case MD_STORE_UNCHANGED: return "";
    case MD_STORE_PENDING: return "Saved as a pending edit. No storage relay has accepted it.";
    case MD_STORE_PARTIAL: return "Some storage relays have not accepted the edit.";
    case MD_STORE_CONFLICT: return "This document changed remotely. Review the conflict before editing again.";
    case MD_STORE_READ_ONLY: return "This document cannot be edited by this version of Metadesk.";
    case MD_STORE_NOT_READY: return "Wait for the address book to finish syncing.";
    case MD_STORE_SIGNER: return "The client signer could not read or sign this document.";
    case MD_STORE_LIMIT: return "The address book is too large to save.";
    default: return "The address book could not save this change.";
    }
}

static void completed(GObject *source, GAsyncResult *result, gpointer unused) {
    (void)unused;
    GTask *task = G_TASK(result);
    Hosts *p = g_object_get_data(source, HOSTS_DATA);
    Job *j = g_task_get_task_data(task);
    p->busy = FALSE;
    if (!g_atomic_int_get(&p->closed) && j->generation == p->generation) {
        free_view(p->view);
        p->view = g_steal_pointer(&j->view);
        if (j->action == CONFIGURE && p->view && p->view->store && p->relays && *p->relays) p->start_pending = TRUE;
        if (j->action == START_SYNC && !j->action_ok)
            gtk_label_set_text(p->error, "Could not open the address book relays. Cached data remains available.");
        if (j->action == EDIT || j->action == RETRY) gtk_label_set_text(p->error, store_error(j->result));
        render(p);
        if (j->action == RESOLVE) present_review(p, j);
        if (j->action == IMPORT) {
            if (j->exchange) present_import(p, j->exchange);
            else gtk_label_set_text(p->error, j->error ? j->error->message : "This file is not a supported metadesk-hosts/1 address book");
        }
        if (j->action == EXPORT) gtk_label_set_text(p->error, j->action_ok ? "" : j->error ? j->error->message : "Could not export hosts to this file");
#ifdef MD_HAVE_MANAGER_QR
        if (j->action == SCAN) {
            if (j->address) { gtk_editable_set_text(GTK_EDITABLE(p->address), j->address); gtk_widget_grab_focus(GTK_WIDGET(p->address)); }
            else gtk_label_set_text(p->error, j->error ? j->error->message : "No nprofile QR code found");
        }
#endif
    }
    kick(p);
    if (p->application) g_application_release(G_APPLICATION(p->application));
}

static void start(Hosts *p, Job *j) {
    if (p->busy || (g_atomic_int_get(&p->closed) && j->action != CLOSE)) { free_job(j); return; }
    j->generation = p->generation;
    p->busy = TRUE;
    if (j->action != CLOSE) gtk_widget_set_sensitive(GTK_WIDGET(p->review), FALSE);
    if (p->application) g_application_hold(G_APPLICATION(p->application));
    g_autoptr(GTask) task = g_task_new(p->page, NULL, completed, NULL);
    g_task_set_task_data(task, j, free_job);
    g_task_run_in_thread(task, work);
}

static void kick(Hosts *p) {
    if (p->busy) return;
    if (g_atomic_int_get(&p->closed)) {
        if (!p->close_queued) {
            p->close_queued = TRUE;
            Job *j = g_new0(Job, 1);
            j->action = CLOSE;
            start(p, j);
        }
        return;
    }
    if (p->configure_pending) {
        p->configure_pending = p->start_pending = p->refresh_pending = FALSE;
        Job *j = g_new0(Job, 1);
        j->action = CONFIGURE;
        j->directory = g_strdup(p->directory);
        j->key = g_strdup(p->key);
        start(p, j);
    } else if (p->start_pending) {
        p->start_pending = FALSE;
        Job *j = g_new0(Job, 1);
        j->action = START_SYNC;
        j->relays = g_strdupv(p->relays);
        start(p, j);
    } else if (p->record_pending) {
        p->record_pending = FALSE;
        Job *j = g_new0(Job, 1);
        j->action = RECORD;
        j->key = g_strdup(p->recent_key);
        j->outcome = p->recent_outcome;
        start(p, j);
    } else if (p->refresh_pending) {
        p->refresh_pending = FALSE;
        Job *j = g_new0(Job, 1);
        j->action = READ;
        start(p, j);
    }
}

static const char *string_field(json_t *o, const char *field) {
    return json_string_value(json_object_get(o, field));
}
static json_t *metadata(Hosts *p) {
    return p->view ? json_object_get(p->view->doc[META].winner, "hosts") : NULL;
}
static json_t *host_meta(Hosts *p, const char *key) { return json_object_get(metadata(p), key); }
static ssize_t member_index(json_t *list, const char *key) {
    size_t i;
    json_t *row;
    json_array_foreach(list, i, row) {
        const char *tag = json_string_value(json_array_get(row, 0));
        const char *pk = json_string_value(json_array_get(row, 1));
        if (tag && !strcmp(tag, "p") && pk && !strcmp(pk, key)) return (ssize_t)i;
    }
    return -1;
}
static gboolean member_set(json_t *list, const char *key, gboolean on) {
    ssize_t i = member_index(list, key);
    if (!on && i >= 0) return json_array_remove(list, (size_t)i) == 0;
    if (on && i < 0) return json_array_append_new(list, json_pack("[s,s]", "p", key)) == 0;
    return TRUE;
}
static const char *title_for(Hosts *p, const char *key, char fp[MD_KEY_FINGERPRINT_SIZE]) {
    const char *label = string_field(host_meta(p, key), "label");
    return label && *label ? label : md_key_fingerprint(key, fp) ? fp : "Unnamed Host";
}
static gboolean readable(const Doc *d) {
    return d->status == MD_STORE_OK || d->status == MD_STORE_PENDING || d->status == MD_STORE_PARTIAL;
}
static gboolean can_review_key(Hosts *p) {
    return p->key && p->relays && *p->relays && p->view && p->view->store &&
        readable(&p->view->doc[HOSTS]) && readable(&p->view->doc[META]);
}
static gboolean can_review(Hosts *p) {
    return can_review_key(p) &&
        p->view->doc[HOSTS].review_ready && p->view->doc[META].review_ready;
}
static gboolean retired_key(Hosts *p, const char *key) {
    const Doc *h = &p->view->doc[HOSTS], *m = &p->view->doc[META];
    json_t *record = host_meta(p, key);
    json_t *draft = json_object_get(json_object_get(m->draft, "hosts"), key);
    return (h->draft && (member_index(h->winner, key) >= 0) != (member_index(h->draft, key) >= 0)) ||
        (m->draft && ((record != NULL) != (draft != NULL) ||
            g_strcmp0(string_field(record, "nip05"), string_field(draft, "nip05"))));
}
static gboolean reviewable_key(Hosts *p, const char *key) {
    return !retired_key(p, key) &&
        (member_index(p->view->doc[HOSTS].winner, key) >= 0) == (host_meta(p, key) != NULL);
}
static gboolean writable(Hosts *p, size_t i) {
    return p->view && p->view->store && p->view->doc[i].sync == MD_SYNC_READY && p->view->doc[i].status == MD_STORE_OK;
}
static void append_document_edit(Job *j, const char *coord, const char *rev, MdStoreEdit mode, json_t *plain) {
    if (!j->edits) j->edits = g_ptr_array_new_with_free_func(free_edit);
    Edit *e = g_new0(Edit, 1);
    e->d = g_strdup(coord);
    g_strlcpy(e->rev, rev, sizeof(e->rev));
    e->mode = mode;
    e->plain = plain ? json_dumps(plain, JSON_COMPACT | JSON_ENCODE_ANY) : NULL;
    g_ptr_array_add(j->edits, e);
}
static void append_edit(Job *j, size_t doc, const char *rev, MdStoreEdit mode, json_t *plain) {
    append_document_edit(j, coordinates[doc], rev, mode, plain);
}
static gboolean append_tag_edit(Job *j, const char *uuid, const char *key, gboolean remove) {
    if (!j->edits) j->edits = g_ptr_array_new_with_free_func(free_edit);
    g_autofree char *coord = g_strconcat(MD_STORE_TAG_PREFIX, uuid, NULL);
    Edit *edit = NULL;
    for (guint i = 0; i < j->edits->len; ++i) {
        Edit *candidate = g_ptr_array_index(j->edits, i);
        if (!strcmp(candidate->d, coord) && candidate->remove == remove) { edit = candidate; break; }
    }
    if (!edit) {
        edit = g_new0(Edit, 1);
        edit->d = g_steal_pointer(&coord);
        edit->mode = MD_EDIT_STAGE;
        edit->remove = remove;
        edit->members = json_array();
        g_ptr_array_add(j->edits, edit);
    }
    return json_array_append_new(edit->members, json_string(key)) == 0;
}
static json_t *tag_labels(Hosts *p) {
    return p->view ? json_object_get(p->view->doc[META].winner, "tags") : NULL;
}
static json_t *copy_doc(Hosts *p, size_t i) {
    const Doc *d = &p->view->doc[i];
    return d->winner ? json_deep_copy(d->winner) :
        i == META ? json_pack("{s:i,s:{}}", "schema_version", 1, "hosts") : json_array();
}
static json_t *ensure_object(json_t *o, const char *key) {
    json_t *v = json_object_get(o, key);
    if (!v && json_object_set_new(o, key, json_object()) == 0) v = json_object_get(o, key);
    return v;
}
static const char *alias_pin(Hosts *p, const char *alias, gboolean *ambiguous) {
    const char *key, *pin = NULL;
    json_t *meta;
    *ambiguous = FALSE;
    if (alias) json_object_foreach(metadata(p), key, meta) {
        const char *stored = string_field(meta, "nip05");
        if (!stored || strcmp(stored, alias)) continue;
        if (pin) { *ambiguous = TRUE; break; }
        pin = key;
    }
    return pin;
}

static void launch_progress(MdLaunchMessage msg, gpointer data) {
    Hosts *p = data;
    if (msg >= MD_LAUNCH_RELAY_AUTH) {
        const char *detail = msg == MD_LAUNCH_RELAY_AUTH ? "A signaling relay requires authentication" :
            msg == MD_LAUNCH_RELAY_CLOSED ? "A signaling relay closed the subscription" :
            "A signaling relay rejected an event";
        gtk_label_set_text(p->relay_diagnostic, detail);
        return;
    }
    if (msg == MD_LAUNCH_RESOLVE || msg == MD_LAUNCH_CONNECTED || msg == MD_LAUNCH_CANCELLED || msg == MD_LAUNCH_ENDED)
        gtk_label_set_text(p->relay_diagnostic, "");
    const char *message = msg == MD_LAUNCH_ENDED ? "Video session ended" : "Connection failed";
    static const char *const messages[] = {NULL, "Checking host identity…", "Connecting to signer…",
        "Connecting to relays…", "Checking FIPS route…", "Waiting for approval…", "Connecting…",
        "Binding session…", "Opening video…", "Connected in video window", NULL, "Connection cancelled"};
    if (msg > 0 && msg < (int)G_N_ELEMENTS(messages) && messages[msg]) message = messages[msg];
    guint phase = (guint)p->phase;
    if (msg == MD_LAUNCH_FAILED && phase > 0 && phase < MD_LAUNCH_CONNECTED && messages[phase]) {
        static const char *const failures[] = {NULL, "Host identity could not be verified", "The client signer is unavailable",
            "The signaling relays could not be reached", "The FIPS route is unavailable", "No approval was received or the request was declined",
            "The stream could not be opened", "The host did not confirm the session", "Video could not be opened"};
        message = failures[phase];
    }
    gtk_label_set_text(p->progress, message);
    gtk_button_set_label(p->launch_cancel, msg == MD_LAUNCH_CONNECTED ? "_Disconnect" : "_Cancel Connection");
    gtk_widget_set_visible(GTK_WIDGET(p->launch_cancel), msg < MD_LAUNCH_FAILED);
    if (msg > 0 && msg < MD_LAUNCH_CONNECTED) p->phase = msg;
    if (msg == MD_LAUNCH_CONNECTED || msg == MD_LAUNCH_FAILED) {
        static const MdRecentOutcome outcomes[] = {MD_RECENT_RESOLVE, MD_RECENT_RESOLVE, MD_RECENT_SIGNER,
            MD_RECENT_RELAY, MD_RECENT_FIPS, MD_RECENT_NEGOTIATION, MD_RECENT_FIPS, MD_RECENT_BIND, MD_RECENT_MEDIA};
        p->recent_outcome = msg == MD_LAUNCH_CONNECTED ? MD_RECENT_CONNECTED : outcomes[MIN(phase, G_N_ELEMENTS(outcomes) - 1)];
        memcpy(p->recent_key, p->launched, sizeof(p->recent_key));
        p->record_pending = TRUE;
        kick(p);
    }
}

static char *nprofile(const char *key, char **hints, size_t count) {
    char *safe[MD_RELAY_TARGET_MAX];
    size_t n = 0;
    for (size_t i = 0; i < count && n < MD_RELAY_TARGET_MAX; ++i)
        if (hints[i] && strlen(hints[i]) <= 255) safe[n++] = hints[i];
    NostrProfilePointer ptr = {.public_key = (char *)key, .relays = safe, .relays_count = n};
    char *out = NULL;
    if (nostr_nip19_encode_nprofile(&ptr, &out)) return NULL;
    return out;
}

static size_t review_hints(const MdIdentity *id, json_t *record, char *hints[MD_RELAY_TARGET_MAX]) {
    size_t n = MIN(id->hint_count, MD_RELAY_TARGET_MAX), i;
    for (i = 0; i < n; ++i) hints[i] = id->hints[i];
    json_t *item;
    json_array_foreach(json_object_get(record, "relay_hints"), i, item) {
        if (n == MD_RELAY_TARGET_MAX) break;
        const char *hint = json_string_value(item);
        if (!hint || !*hint) continue;
        gboolean duplicate = FALSE;
        for (size_t k = 0; k < n; ++k) if (!strcmp(hints[k], hint)) duplicate = TRUE;
        if (!duplicate) hints[n++] = (char *)hint;
    }
    return n;
}

typedef struct {
    GtkWidget *page;
    guint generation;
    char key[65], old[65], host_revision[65], meta_revision[65];
    char *alias, *route, *address, *fingerprint;
    json_t *hints;
    GtkEditable *confirmation;
} Review;

static void free_review(gpointer data) {
    Review *r = data;
    g_object_unref(r->page);
    g_free(r->alias); g_free(r->route); g_free(r->address); g_free(r->fingerprint);
    json_decref(r->hints);
    g_free(r);
}
static uint8_t requested_access(Hosts *p, const char *key) {
    uint8_t caps = MD_CAP_VIDEO;
    if (member_index(p->view->doc[HOSTS].winner, key) < 0) return caps;
    size_t i; json_t *item;
    json_array_foreach(json_object_get(host_meta(p, key), "requested_caps"), i, item) {
        const char *s = json_string_value(item);
        if (!g_strcmp0(s, "input")) caps |= MD_CAP_INPUT;
        else if (!g_strcmp0(s, "agent")) caps |= MD_CAP_AGENT;
    }
    return caps;
}

static void start_launch(Hosts *p, Review *r) {
    if (!can_review_key(p)) return;
    MdManagedLaunch *c = g_new0(MdManagedLaunch, 1);
    g_strlcpy(c->address, r->address, sizeof(c->address));
    memcpy(c->pin, r->key, sizeof(c->pin));
    g_strlcpy(c->signer, p->key, sizeof(c->signer));
    g_strlcpy(c->route, r->route, sizeof(c->route));
    json_t *meta = host_meta(p, r->key);
    json_int_t port = json_integer_value(json_object_get(meta, "port"));
    c->port = port > 0 && port <= UINT16_MAX ? (uint16_t)port : MD_STREAM_PORT;
    c->capabilities = requested_access(p, r->key);
    for (size_t i = 0; i < MD_RELAY_TARGET_MAX && p->relays && p->relays[i]; ++i) {
        g_strlcpy(c->relays[i], p->relays[i], sizeof(c->relays[i]));
        ++c->relay_count;
    }
    g_autoptr(GError) error = NULL;
    md_manager_launch_free(p->launch);
    g_autofree char *self = g_file_read_link("/proc/self/exe", NULL);
    g_autofree char *directory = self ? g_path_get_dirname(self) : NULL;
    g_autofree char *sibling = directory ? g_build_filename(directory, "metadesk-client", NULL) : NULL;
    const char *client = sibling && g_file_test(sibling, G_FILE_TEST_IS_EXECUTABLE) ? sibling : "metadesk-client";
    p->launch = md_manager_launch_start(client, c, launch_progress, p, &error);
    g_free(c);
    if (!p->launch) { gtk_label_set_text(p->error, error ? error->message : "Could not start the video client"); return; }
    memcpy(p->launched, r->key, sizeof(p->launched));
    p->phase = MD_LAUNCH_RESOLVE;
    launch_progress(MD_LAUNCH_RESOLVE, p);
}

static gboolean prepare_membership_replacement(Job *j, const char *coord, const Doc *d,
    const char *old_key, const char *new_key) {
    if (!d || d->status != MD_STORE_OK || d->sync != MD_SYNC_READY) return FALSE;
    if (member_index(d->winner, old_key) < 0) return TRUE;
    json_t *list = json_deep_copy(d->winner);
    gboolean good = list && member_set(list, old_key, FALSE) && member_set(list, new_key, TRUE);
    if (good) append_document_edit(j, coord, d->revision, MD_EDIT_STAGE, list);
    md_managed_store_discard_plaintext(list);
    return good;
}

static void review_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
    (void)dialog;
    Review *r = data;
    Hosts *p = g_object_get_data(G_OBJECT(r->page), HOSTS_DATA);
    if (!strcmp(response, "cancel") || g_atomic_int_get(&p->closed)) return;
    if (p->busy || r->generation != p->generation || !can_review_key(p) ||
        (r->alias && !can_review(p)) || !reviewable_key(p, r->key) ||
        (*r->old && !reviewable_key(p, r->old)) ||
        strcmp(r->meta_revision, p->view->doc[META].revision) || strcmp(r->host_revision, p->view->doc[HOSTS].revision)) {
        gtk_label_set_text(p->error, "Saved keys or relay state changed. Review the host again.");
        return;
    }
    if (!strcmp(response, "connect") && !*r->old) { start_launch(p, r); return; }
    gboolean replace = !strcmp(response, "replace");
    if (replace && (!r->confirmation || strcmp(gtk_editable_get_text(r->confirmation), r->fingerprint))) return;
    if ((!replace && strcmp(response, "save")) || (*r->old && !replace) ||
        !can_review(p) || !writable(p, HOSTS) || !writable(p, META)) return;
    json_t *doc = copy_doc(p, META), *hosts = copy_doc(p, HOSTS);
    json_t *meta = ensure_object(doc, "hosts");
    json_t *record = json_object_get(meta, r->key);
    if (!record) record = ensure_object(meta, r->key);
    gboolean ok = record && (!r->alias || json_object_set_new(record, "nip05", json_string(r->alias)) == 0) &&
        (!json_array_size(r->hints) || json_object_set(record, "relay_hints", r->hints) == 0) &&
        member_set(hosts, r->key, TRUE);
    if (ok && replace) {
        json_t *old = json_object_get(meta, r->old);
        const char *fields[] = {"label", "notes"};
        for (size_t i = 0; i < G_N_ELEMENTS(fields) && ok; ++i) {
            const char *text = string_field(old, fields[i]);
            if (text && !json_object_get(record, fields[i]))
                ok = json_object_set_new(record, fields[i], json_string(text)) == 0;
        }
        ok = ok && json_object_del(meta, r->old) == 0 && member_set(hosts, r->old, FALSE);
    }
    if (ok) {
        Job *j = g_new0(Job, 1);
        j->action = EDIT;
        append_edit(j, META, r->meta_revision, MD_EDIT_STAGE, doc);
        append_edit(j, HOSTS, r->host_revision, MD_EDIT_STAGE, hosts);
        if (replace) {
            ok = prepare_membership_replacement(j, MD_STORE_FAVORITES, &p->view->doc[FAVORITES], r->old, r->key);
            const char *uuid; json_t *tag;
            json_object_foreach(tag_labels(p), uuid, tag) {
                if (!ok) break;
                if (json_is_true(json_object_get(tag, "deleted"))) continue;
                Doc *d = p->view->tags ? g_hash_table_lookup(p->view->tags, uuid) : NULL;
                g_autofree char *coord = g_strconcat(MD_STORE_TAG_PREFIX, uuid, NULL);
                ok = prepare_membership_replacement(j, coord, d, r->old, r->key);
            }
        }
        if (ok) start(p, j);
        else {
            free_job(j);
            gtk_label_set_text(p->error, "Could not verify all favorites and tags. Sync the address book before replacing this key.");
        }
    } else gtk_label_set_text(p->error, "Could not prepare the saved host change");
    json_decref(doc); json_decref(hosts);
}

static GtkWidget *label(const char *text) {
    GtkWidget *w = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(w), 0);
    gtk_label_set_wrap(GTK_LABEL(w), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(w), PANGO_WRAP_WORD_CHAR);
    return w;
}
static void confirmation_changed(GtkEditable *entry, gpointer data) {
    AdwAlertDialog *dialog = data;
    Review *r = g_object_get_data(G_OBJECT(dialog), "review");
    adw_alert_dialog_set_response_enabled(dialog, "replace", !strcmp(gtk_editable_get_text(entry), r->fingerprint));
}

static void present_review(Hosts *p, const Job *j) {
    if (!can_review_key(p)) return; /* render() already explains why review is unavailable. */
    if (j->resolution != MD_RESOLVE_OK) {
        gtk_label_set_text(p->error, md_resolve_error(j->resolution));
        return;
    }
    if (j->identity.hint_source == MD_HINT_NIP05 && !can_review(p)) {
        gtk_label_set_text(p->error, "Wait for the address book to sync before reviewing a NIP-05 name.");
        return;
    }
    if (strcmp(j->review_hosts, p->view->doc[HOSTS].revision) ||
        strcmp(j->review_meta, p->view->doc[META].revision)) {
        gtk_label_set_text(p->error, "Saved keys changed. Review the host again.");
        return;
    }
    if (!reviewable_key(p, j->identity.pubkey)) {
        gtk_label_set_text(p->error, "Reconcile pending or incomplete changes to this host before connecting.");
        return;
    }
    Review *r = g_new0(Review, 1);
    r->page = g_object_ref(p->page);
    r->generation = p->generation;
    g_strlcpy(r->meta_revision, p->view->doc[META].revision, sizeof(r->meta_revision));
    g_strlcpy(r->host_revision, p->view->doc[HOSTS].revision, sizeof(r->host_revision));
    memcpy(r->key, j->identity.pubkey, sizeof(r->key));
    r->alias = j->identity.hint_source == MD_HINT_NIP05 ? md_identity_normalize_nip05(j->address) : NULL;
    if (j->identity.hint_source == MD_HINT_NIP05 && !r->alias) {
        gtk_label_set_text(p->error, "The NIP-05 name could not be verified");
        free_review(r);
        return;
    }
    r->route = g_strdup(j->route ? j->route : "");
    r->hints = json_array();
    const char *stored = string_field(host_meta(p, r->key), "nip05");
    gboolean ambiguous;
    const char *pin = alias_pin(p, r->alias, &ambiguous);
    if ((pin && !reviewable_key(p, pin)) || (j->key && g_strcmp0(pin, j->key))) {
        gtk_label_set_text(p->error, "The saved host changed. Reconcile the address book before reviewing it again.");
        free_review(r);
        return;
    }
    if (ambiguous || (stored && *stored && g_strcmp0(stored, r->alias))) {
        gtk_label_set_text(p->error, ambiguous ? "This name has more than one saved key. Resolve the address book conflict first." :
            "This key has a saved NIP-05 name. Review the saved name before connecting.");
        free_review(r);
        return;
    }
    MdResolveResult pin_result = md_identity_check_pin(&j->identity, pin);
    if (pin_result == MD_RESOLVE_PIN_MISMATCH) memcpy(r->old, pin, sizeof(r->old));
    else if (pin_result != MD_RESOLVE_OK) {
        gtk_label_set_text(p->error, "The saved host pin is invalid");
        free_review(r);
        return;
    }
    char fp[MD_KEY_FINGERPRINT_SIZE], old_fp[MD_KEY_FINGERPRINT_SIZE];
    md_key_fingerprint(r->key, fp);
    r->fingerprint = g_strdup(fp);
    char *hints[MD_RELAY_TARGET_MAX];
    size_t hint_count = review_hints(&j->identity, host_meta(p, r->key), hints);
    for (size_t i = 0; i < hint_count; ++i) json_array_append_new(r->hints, json_string(hints[i]));
    r->address = nprofile(r->key, hints, hint_count);
    if (!r->address || strlen(r->address) > MD_MANAGED_ADDRESS_MAX) {
        g_clear_pointer(&r->address, g_free);
        r->address = g_strdup(r->key);
    }
    g_autofree char *npub = md_identity_npub(&j->identity);
    g_autoptr(GDateTime) verified = g_date_time_new_now_local();
    g_autofree char *when = g_date_time_format(verified, "%c");
    g_autofree char *nip = r->alias ? g_strdup_printf("NIP-05: %s (verified %s)", r->alias, when) :
        g_strdup("NIP-05: Not provided");
    uint8_t caps = requested_access(p, r->key);
    const char *access = (caps & MD_CAP_INPUT) ? ((caps & MD_CAP_AGENT) ? "Video, input and accessibility" : "Video and input") :
        ((caps & MD_CAP_AGENT) ? "Video and accessibility" : "View only");
    gboolean changed = *r->old != 0;
    if (changed) md_key_fingerprint(r->old, old_fp);
    g_autofree char *body = changed ? g_strdup_printf("%s\n\nSaved fingerprint: %s\nNew fingerprint: %s\n\nThe connection is blocked. Verify the new fingerprint with the host before replacing the saved key.", nip, old_fp, fp) :
        g_strdup_printf("%s\n\nFingerprint: %s\nRequested access: %s\n\nCheck the fingerprint with the host before trusting this key.", nip, fp, access);
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(changed ? "Host Key Changed" : "Review Host Key", body));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *key = label(npub ? npub : r->key);
    gtk_label_set_selectable(GTK_LABEL(key), TRUE);
    gtk_box_append(GTK_BOX(box), key);
    adw_alert_dialog_set_extra_child(dialog, box);
    adw_alert_dialog_add_response(dialog, "cancel", changed ? "_Keep Old Key" : "_Cancel");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    adw_alert_dialog_set_default_response(dialog, "cancel");
    if (changed) {
        GtkWidget *entry = gtk_entry_new();
        GtkWidget *prompt = gtk_label_new_with_mnemonic("_Type the new fingerprint");
        gtk_label_set_mnemonic_widget(GTK_LABEL(prompt), entry);
        gtk_box_append(GTK_BOX(box), prompt);
        gtk_box_append(GTK_BOX(box), entry);
        r->confirmation = GTK_EDITABLE(entry);
        adw_alert_dialog_add_response(dialog, "replace", "_Replace Key");
        adw_alert_dialog_set_response_appearance(dialog, "replace", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_enabled(dialog, "replace", FALSE);
        g_signal_connect(entry, "changed", G_CALLBACK(confirmation_changed), dialog);
    } else {
        gboolean can_save = can_review(p) && writable(p, HOSTS) && writable(p, META);
        adw_alert_dialog_add_response(dialog, "save", "_Save Host");
        adw_alert_dialog_set_response_enabled(dialog, "save", can_save);
        if (!r->alias && host_meta(p, r->key) &&
            (p->view->doc[HOSTS].sync != MD_SYNC_READY || p->view->doc[META].sync != MD_SYNC_READY))
            gtk_box_append(GTK_BOX(box), label("Saved host details may be out of date until sync completes."));
        if (!can_save) gtk_box_append(GTK_BOX(box), label("Saving requires a synced, writable address book."));
        adw_alert_dialog_add_response(dialog, "connect", "_Connect");
        adw_alert_dialog_set_response_appearance(dialog, "connect", ADW_RESPONSE_SUGGESTED);
    }
    g_object_set_data_full(G_OBJECT(dialog), "review", r, free_review);
    g_signal_connect(dialog, "response", G_CALLBACK(review_response), r);
    adw_dialog_set_focus(ADW_DIALOG(dialog), changed ? GTK_WIDGET(r->confirmation) : key);
    adw_dialog_present(ADW_DIALOG(dialog), p->page);
}

static void resolve(Hosts *p, const char *address, const char *route) {
    if (p->busy || !can_review_key(p)) return;
    g_autofree char *value = g_strstrip(g_strdup(address));
    g_autofree char *hint = g_strstrip(g_strdup(route ? route : ""));
    if (!*value || strlen(value) > MD_MANAGED_ADDRESS_MAX) {
        gtk_label_set_text(p->error, "Enter an npub, nprofile, public key or NIP-05 name"); return;
    }
    if (*hint) {
        g_autoptr(GInetAddress) ip = g_inet_address_new_from_string(hint);
        if (!ip) { gtk_label_set_text(p->error, "Enter an IPv4 or IPv6 address as the route hint"); return; }
    }
    g_autofree char *saved_pin = NULL;
    g_autofree char *alias = md_identity_normalize_nip05(value);
    if (!alias) {
        if (strchr(value, '@')) { gtk_label_set_text(p->error, md_resolve_error(MD_RESOLVE_INVALID)); return; }
        /* Non-name identity parsing is local. Look up the saved record before
         * the worker does any network resolution or relay validation. */
        MdIdentity id = {0};
        MdResolveResult result = md_identity_resolve(value, NULL, &id);
        if (result != MD_RESOLVE_OK || !reviewable_key(p, id.pubkey)) {
            gtk_label_set_text(p->error, result != MD_RESOLVE_OK ? md_resolve_error(result) :
                "Reconcile pending or incomplete changes to this host before connecting.");
            md_identity_clear(&id);
            return;
        }
        json_t *record = host_meta(p, id.pubkey);
        const char *stored = string_field(record, "nip05");
        if (stored && *stored) {
            alias = md_identity_normalize_nip05(stored);
            if (!alias) {
                gtk_label_set_text(p->error, "The saved NIP-05 name is invalid.");
                md_identity_clear(&id);
                return;
            }
            saved_pin = g_strdup(id.pubkey);
        } else if (record) {
            char *hints[MD_RELAY_TARGET_MAX];
            size_t n = review_hints(&id, record, hints);
            g_autofree char *profile = nprofile(id.pubkey, hints, n);
            if (profile && strlen(profile) <= MD_MANAGED_ADDRESS_MAX) {
                g_free(value);
                value = g_steal_pointer(&profile);
            }
        }
        md_identity_clear(&id);
    }
    if (alias && !can_review(p)) {
        gtk_label_set_text(p->error, "Wait for the address book to sync before reviewing a NIP-05 name.");
        return;
    }
    gtk_label_set_text(p->error, "");
    Job *j = g_new0(Job, 1);
    j->action = RESOLVE;
    memcpy(j->review_hosts, p->view->doc[HOSTS].revision, sizeof(j->review_hosts));
    memcpy(j->review_meta, p->view->doc[META].revision, sizeof(j->review_meta));
    j->key = g_steal_pointer(&saved_pin);
    if (alias) { g_free(value); value = g_steal_pointer(&alias); }
    j->address = g_steal_pointer(&value);
    j->route = g_steal_pointer(&hint);
    start(p, j);
}
static gboolean reject_secret(Hosts *p, GtkEditable *entry) {
    const char *s = gtk_editable_get_text(entry);
    while (g_ascii_isspace(*s)) ++s;
    if (!g_ascii_strncasecmp(s, "nostr:", 6)) s += 6;
    if (g_ascii_strncasecmp(s, "nsec1", 5) && g_ascii_strncasecmp(s, "ncryptsec1", 10)) return FALSE;
    gtk_editable_set_text(entry, "");
    gtk_label_set_text(p->error, "Private keys are not host addresses. Use a public host identity.");
    return TRUE;
}
static void address_changed(GtkEditable *entry, gpointer data) {
    reject_secret(data, entry);
}
static void review_entry(GtkWidget *widget, gpointer data) {
    (void)widget;
    Hosts *p = data;
    if (reject_secret(p, GTK_EDITABLE(p->address)) || reject_secret(p, GTK_EDITABLE(p->route))) return;
    resolve(p, gtk_editable_get_text(GTK_EDITABLE(p->address)), gtk_editable_get_text(GTK_EDITABLE(p->route)));
}
static void review_saved(GtkWidget *button, gpointer data) {
    Hosts *p = data;
    const char *key = g_object_get_data(G_OBJECT(button), "host-key");
    resolve(p, key, "");
}

static void connect_control(GtkButton *button, gpointer data) {
    (void)button;
    Hosts *p = data;
    if (!md_manager_launch_cancel(p->launch) && !md_manager_launch_disconnect(p->launch))
        gtk_widget_set_visible(GTK_WIDGET(p->launch_cancel), FALSE);
}

static Doc *document(Hosts *p, const char *coord) {
    if (!p->view) return NULL;
    for (size_t i = 0; i < NDOCS; ++i) if (!strcmp(coord, coordinates[i])) return &p->view->doc[i];
    if (p->view->tags && g_str_has_prefix(coord, MD_STORE_TAG_PREFIX))
        return g_hash_table_lookup(p->view->tags, coord + sizeof(MD_STORE_TAG_PREFIX) - 1);
    return NULL;
}

static void retry_document(GtkWidget *button, gpointer data) {
    Hosts *p = data;
    if (p->busy) return;
    Job *j = g_new0(Job, 1);
    j->action = RETRY;
    j->coordinate = g_strdup(g_object_get_data(G_OBJECT(button), "coordinate"));
    start(p, j);
}
static void conflict_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
    Hosts *p = data;
    if (!strcmp(response, "cancel") || p->busy || !p->view || g_atomic_int_get(&p->closed)) return;
    const char *coord = g_object_get_data(G_OBJECT(dialog), "coordinate");
    Doc *d = document(p, coord);
    const char *rev = g_object_get_data(G_OBJECT(dialog), "revision");
    if (!d || GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "generation")) != p->generation ||
        strcmp(rev, d->revision) || d->status != MD_STORE_CONFLICT) return;
    Job *j = g_new0(Job, 1);
    j->action = EDIT;
    append_document_edit(j, coord, rev, !strcmp(response, "remote") ? MD_EDIT_KEEP_REMOTE : MD_EDIT_REAPPLY, d->draft);
    start(p, j);
}
static void review_conflict(GtkWidget *button, gpointer data) {
    Hosts *p = data;
    if (!p->view) return;
    const char *coord = g_object_get_data(G_OBJECT(button), "coordinate");
    const Doc *d = document(p, coord);
    if (!d || d->status != MD_STORE_CONFLICT) return;
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Review Address Book Conflict",
        "A newer document arrived from the relays. Keep the remote version or reapply the local draft after reviewing both versions."));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 170);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    json_t *values[] = {d->winner, d->draft};
    const char *names[] = {"Remote version", "Local draft"};
    for (size_t k = 0; k < G_N_ELEMENTS(values); ++k) {
        gtk_box_append(GTK_BOX(box), label(names[k]));
        char *text = values[k] ? json_dumps(values[k], JSON_INDENT(2) | JSON_ENCODE_ANY) : NULL;
        GtkWidget *value = label(text ? text : "Unavailable");
        gtk_label_set_selectable(GTK_LABEL(value), TRUE);
        gtk_box_append(GTK_BOX(box), value);
        wipe(text);
    }
    adw_alert_dialog_set_extra_child(dialog, scroll);
    adw_alert_dialog_add_responses(dialog, "cancel", "_Cancel", "remote", "_Keep Remote", "reapply", "_Reapply Draft", NULL);
    adw_alert_dialog_set_close_response(dialog, "cancel");
    adw_alert_dialog_set_response_enabled(dialog, "reapply", d->draft != NULL);
    g_object_set_data_full(G_OBJECT(dialog), "coordinate", g_strdup(coord), g_free);
    g_object_set_data(G_OBJECT(dialog), "generation", GUINT_TO_POINTER(p->generation));
    g_object_set_data_full(G_OBJECT(dialog), "revision", g_strdup(d->revision), g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(conflict_response), p);
    adw_dialog_present(ADW_DIALOG(dialog), p->page);
}

static char *last_connected(Hosts *p, const char *pk) {
    json_t *recent;
    size_t i;
    if (p->view) json_array_foreach(p->view->recents, i, recent) {
        if (g_strcmp0(pk, string_field(recent, "pubkey")) || json_integer_value(json_object_get(recent, "outcome")) != MD_RECENT_CONNECTED) continue;
        g_autoptr(GDateTime) ts = g_date_time_new_from_unix_local(json_integer_value(json_object_get(recent, "at")));
        if (!ts) continue;
        g_autofree char *date = g_date_time_format(ts, "%x %X");
        return g_strdup_printf("Last connected on this device: %s", date);
    }
    return NULL;
}

static GtkWidget *button(const char *text, GCallback click, Hosts *p) {
    GtkWidget *w = gtk_button_new_with_mnemonic(text);
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    g_signal_connect(w, "clicked", click, p);
    return w;
}
static void clear_list(GtkListBox *list) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list)))) gtk_list_box_remove(list, child);
}
static void edit_host(GtkWidget *, gpointer);
static void add_host_row(Hosts *p, GtkListBox *list, const char *key, const char *qualified) {
    char fp[MD_KEY_FINGERPRINT_SIZE];
    const char *title = title_for(p, key, fp);
    const char *alias = string_field(host_meta(p, key), "nip05");
    const char *notes = string_field(host_meta(p, key), "notes");
    g_autofree char *recent = last_connected(p, key);
    GString *desc = g_string_new(qualified ? qualified : (alias ? alias : ""));
    if (list == p->saved && p->view->tags) {
        const char *uuid; json_t *tag;
        json_object_foreach(tag_labels(p), uuid, tag) {
            Doc *d = g_hash_table_lookup(p->view->tags, uuid);
            const char *name = string_field(tag, "label");
            if (name && d && member_index(d->winner, key) >= 0)
                g_string_append_printf(desc, "%s%s", desc->len ? " · " : "", name);
        }
    }
    if (notes && *notes && list == p->saved) g_string_append_printf(desc, "%s%s", desc->len ? "\n" : "", notes);
    if (recent) g_string_append_printf(desc, "%s%s", desc->len ? "\n" : "", recent);
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_title_lines(row, 2);
    adw_action_row_set_subtitle_lines(row, 3);
    adw_action_row_set_subtitle(row, desc->str);
    g_string_free(desc, TRUE);
    GtkWidget *connect = button("_Connect…", G_CALLBACK(review_saved), p);
    g_object_set_data_full(G_OBJECT(connect), "host-key", g_strdup(key), g_free);
    gtk_widget_set_sensitive(connect, can_review_key(p) && reviewable_key(p, key) &&
        (!(alias && *alias) || can_review(p)));
    adw_action_row_add_suffix(row, connect);
    adw_action_row_set_activatable_widget(row, connect);
    if (list == p->saved) {
        GtkWidget *edit = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_set_valign(edit, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(edit, "Edit Saved Host");
        gtk_accessible_update_property(GTK_ACCESSIBLE(edit), GTK_ACCESSIBLE_PROPERTY_LABEL, "Edit Saved Host", -1);
        g_object_set_data_full(G_OBJECT(edit), "host-key", g_strdup(key), g_free);
        g_signal_connect(edit, "clicked", G_CALLBACK(edit_host), p);
        gtk_widget_set_sensitive(edit, writable(p, META) || writable(p, FAVORITES));
        adw_action_row_add_suffix(row, edit);
    }
    gtk_list_box_append(list, GTK_WIDGET(row));
}

static gboolean tag_matches(Hosts *p, const char *key, const char *filter) {
    if (!*filter) return TRUE;
    const char *uuid; json_t *tag;
    json_object_foreach(tag_labels(p), uuid, tag) {
        Doc *d = p->view->tags ? g_hash_table_lookup(p->view->tags, uuid) : NULL;
        const char *name = string_field(tag, "label");
        if (!d || !name || member_index(d->winner, key) < 0) continue;
        g_autofree char *fold = g_utf8_casefold(name, -1);
        if (strstr(fold, filter)) return TRUE;
    }
    return FALSE;
}
static gboolean matches(Hosts *p, const char *key, const char *query) {
    if (!*query) return TRUE;
    json_t *meta = host_meta(p, key);
    char fp[MD_KEY_FINGERPRINT_SIZE];
    const char *fields[] = {title_for(p, key, fp), key, string_field(meta, "nip05"), string_field(meta, "notes")};
    for (size_t i = 0; i < G_N_ELEMENTS(fields); ++i) if (fields[i]) {
        g_autofree char *fold = g_utf8_casefold(fields[i], -1);
        if (strstr(fold, query)) return TRUE;
    }
    return FALSE;
}
static void document_action(Hosts *p, const char *coord, const Doc *d) {
    GtkWidget *action = NULL;
    if (d->status == MD_STORE_CONFLICT)
        action = button("_Review Address Book Conflict…", G_CALLBACK(review_conflict), p);
    else if (d->status == MD_STORE_PENDING || d->status == MD_STORE_PARTIAL)
        action = button("_Retry Pending Edit", G_CALLBACK(retry_document), p);
    if (action) {
        g_object_set_data_full(G_OBJECT(action), "coordinate", g_strdup(coord), g_free);
        gtk_widget_set_sensitive(action, !p->busy && d->sync == MD_SYNC_READY);
        gtk_list_box_append(p->saved, action);
    } else if (d->status == MD_STORE_READ_ONLY)
        gtk_list_box_append(p->saved, label("An address book document is read-only and cannot be overwritten"));
}
static void render(Hosts *p) {
    gtk_widget_set_sensitive(GTK_WIDGET(p->review), !p->busy && can_review_key(p));
    gtk_widget_set_sensitive(GTK_WIDGET(p->export_button), !p->busy && p->view && p->view->store && readable(&p->view->doc[HOSTS]) && readable(&p->view->doc[META]));
    gtk_widget_set_sensitive(GTK_WIDGET(p->import_button), !p->busy && writable(p, META) && writable(p, HOSTS));
#ifdef MD_HAVE_MANAGER_QR
    gtk_widget_set_sensitive(GTK_WIDGET(p->scan_button), !p->busy);
#endif
    const char *status = !p->key ? "Assign a client account in Identity" :
        !p->view || !p->view->store ? "Address book unavailable — check the client signer" :
        !p->relays || !*p->relays ? "Offline — choose relays to sync and connect" : NULL;
    if (!status && (!readable(&p->view->doc[HOSTS]) || !readable(&p->view->doc[META])))
        status = "The saved hosts or their details could not be read. Host review is unavailable.";
    gboolean sync_status = FALSE;
    if (!status) for (size_t i = 0; i < NDOCS; ++i) {
        Doc *d = &p->view->doc[i];
        switch (d->status) {
        case MD_STORE_SIGNER: status = "Some documents are unavailable — check the client signer"; break;
        case MD_STORE_READ_ONLY: status = "Some address book data is read-only"; break;
        case MD_STORE_CONFLICT: status = "Address book changes need review"; break;
        case MD_STORE_IO: case MD_STORE_INVALID: status = "Part of the address book could not be read"; break;
        default: break;
        }
        if (status) break;
        switch (d->sync) {
        case MD_SYNC_READY: break;
        case MD_SYNC_OFFLINE: status = "Offline — showing cached hosts"; break;
        case MD_SYNC_REFUSED: status = "An address book relay refused the subscription"; break;
        case MD_SYNC_AUTH_PENDING: status = "Waiting for relay authentication…"; break;
        case MD_SYNC_AUTH_REFUSED: status = "An address book relay refused authentication"; break;
        case MD_SYNC_REPLAY_NEEDED: status = "Address book catch-up was incomplete. Refresh to try again."; break;
        default: status = "Syncing address book…"; break;
        }
        if (status) { sync_status = TRUE; break; }
    }
    g_autofree char *guidance = sync_status && can_review_key(p) && !can_review(p) ?
        g_strconcat(status, "\nWait for sync to connect by name, or use the host’s public key.", NULL) : NULL;
    adw_banner_set_title(p->banner, guidance ? guidance : status ? status : "");
    adw_banner_set_revealed(p->banner, status != NULL);
    clear_list(p->saved);
    clear_list(p->quick);
    if (!p->view || !p->view->store) return;
    for (size_t i = 0; i < NDOCS; ++i) document_action(p, coordinates[i], &p->view->doc[i]);
    if (p->view->tags) {
        GHashTableIter iter;
        gpointer key, doc;
        g_hash_table_iter_init(&iter, p->view->tags);
        while (g_hash_table_iter_next(&iter, &key, &doc)) {
            g_autofree char *coord = g_strconcat(MD_STORE_TAG_PREFIX, key, NULL);
            document_action(p, coord, doc);
        }
    }
    json_t *hosts = p->view->doc[HOSTS].winner, *entry;
    g_autofree char *query = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(p->search)), -1);
    g_autofree char *tag_filter = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(p->tag_filter)), -1);
    size_t i;
    g_autoptr(GHashTable) shown = g_hash_table_new(g_str_hash, g_str_equal);
    json_array_foreach(hosts, i, entry) {
        const char *tag = json_string_value(json_array_get(entry, 0));
        const char *key = json_string_value(json_array_get(entry, 1));
        if (g_strcmp0(tag, "p") || !key) continue;
        if (matches(p, key, query) && tag_matches(p, key, tag_filter)) add_host_row(p, p->saved, key, NULL);
        if (member_index(p->view->doc[FAVORITES].winner, key) >= 0) {
            add_host_row(p, p->quick, key, "Favorite");
            g_hash_table_add(shown, (gpointer)key);
        }
    }
    guint recent_count = 0;
    json_array_foreach(p->view->recents, i, entry) {
        const char *key = string_field(entry, "pubkey");
        if (!key || g_hash_table_contains(shown, key)) continue;
        add_host_row(p, p->quick, key, "Recent");
        g_hash_table_add(shown, (gpointer)key);
        if (++recent_count == 10) break;
    }
}

static void search_changed(GtkWidget *entry, gpointer data) { (void)entry; render(data); }

typedef struct {
    GtkEditable *name, *tag;
    GtkTextBuffer *notes;
    GtkCheckButton *favorite, *remove_tag;
} HostEdit;
static void edit_response(AdwAlertDialog *dialog, const char *reply, gpointer data) {
    Hosts *p = data;
    if (strcmp(reply, "save") || p->busy || !p->view || g_atomic_int_get(&p->closed)) return;
    if (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "generation")) != p->generation ||
        g_strcmp0(g_object_get_data(G_OBJECT(dialog), "meta-rev"), p->view->doc[META].revision) ||
        g_strcmp0(g_object_get_data(G_OBJECT(dialog), "favorite-rev"), p->view->doc[FAVORITES].revision)) {
        gtk_label_set_text(p->error, "The address book changed. Open the host editor again."); return;
    }
    const char *key = g_object_get_data(G_OBJECT(dialog), "key");
    HostEdit *form = g_object_get_data(G_OBJECT(dialog), "edit");
    Job *j = g_new0(Job, 1);
    j->action = EDIT;
    const char *name = gtk_editable_get_text(form->name);
    GtkTextIter begin, end;
    gtk_text_buffer_get_bounds(form->notes, &begin, &end);
    g_autofree char *notes = gtk_text_buffer_get_text(form->notes, &begin, &end, TRUE);
    if (strlen(name) > 128 || strlen(notes) > 2048) {
        gtk_label_set_text(p->error, "Use a shorter label or note (maximum 128 and 2,048 bytes)");
        free_job(j); return;
    }
    g_autofree char *tag_name = g_strstrip(g_strdup(gtk_editable_get_text(form->tag)));
    gboolean remove_tag = gtk_check_button_get_active(form->remove_tag);
    if (strlen(tag_name) > 64) {
        gtk_label_set_text(p->error, "Use a shorter tag label (maximum 64 bytes)");
        free_job(j); return;
    }
    if (writable(p, META)) {
        json_t *doc = copy_doc(p, META);
        json_t *host = ensure_object(ensure_object(doc, "hosts"), key);
        gboolean ok = host && !json_object_set_new(host, "label", json_string(name)) && !json_object_set_new(host, "notes", json_string(notes));
        g_autofree char *uuid = NULL;
        if (*tag_name) {
            const char *id; json_t *tag;
            json_object_foreach(tag_labels(p), id, tag) {
                if (json_is_true(json_object_get(tag, "deleted")) || g_strcmp0(string_field(tag, "label"), tag_name)) continue;
                if (uuid) { ok = FALSE; break; }
                uuid = g_strdup(id);
            }
            if (ok && !uuid && !remove_tag) {
                uuid = g_uuid_string_random();
                ok = json_object_set_new(ensure_object(doc, "tags"), uuid, json_pack("{s:s}", "label", tag_name)) == 0;
            } else if (!uuid && remove_tag) ok = FALSE;
        }
        if (ok) {
            append_edit(j, META, p->view->doc[META].revision, MD_EDIT_STAGE, doc);
            if (uuid) ok = append_tag_edit(j, uuid, key, remove_tag);
        }
        json_decref(doc);
        if (!ok) { free_job(j); gtk_label_set_text(p->error, "The tag could not be found or is ambiguous"); return; }
    }
    if (writable(p, FAVORITES)) {
        json_t *favorites = copy_doc(p, FAVORITES);
        if (member_set(favorites, key, gtk_check_button_get_active(form->favorite)))
            append_edit(j, FAVORITES, p->view->doc[FAVORITES].revision, MD_EDIT_STAGE, favorites);
        json_decref(favorites);
    }
    if (j->edits) start(p, j); else free_job(j);
}
static void edit_host(GtkWidget *button, gpointer data) {
    Hosts *p = data;
    const char *key = g_object_get_data(G_OBJECT(button), "host-key");
    if (!p->view || p->busy) return;
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Edit Saved Host", NULL));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    HostEdit *form = g_new0(HostEdit, 1);
    json_t *meta = host_meta(p, key);
    GtkWidget *name = gtk_entry_new(), *prompt = gtk_label_new_with_mnemonic("_Label");
    gtk_label_set_mnemonic_widget(GTK_LABEL(prompt), name);
    gtk_label_set_xalign(GTK_LABEL(prompt), 0);
    form->name = GTK_EDITABLE(name);
    gtk_editable_set_text(form->name, string_field(meta, "label") ? string_field(meta, "label") : "");
    gtk_widget_set_sensitive(name, writable(p, META));
    gtk_box_append(GTK_BOX(box), prompt);
    gtk_box_append(GTK_BOX(box), name);
    GtkWidget *notes = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(notes), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(notes, -1, 72);
    form->notes = gtk_text_view_get_buffer(GTK_TEXT_VIEW(notes));
    gtk_text_buffer_set_text(form->notes, string_field(meta, "notes") ? string_field(meta, "notes") : "", -1);
    gtk_widget_set_sensitive(notes, writable(p, META));
    GtkWidget *np = gtk_label_new_with_mnemonic("_Notes");
    gtk_label_set_mnemonic_widget(GTK_LABEL(np), notes);
    gtk_label_set_xalign(GTK_LABEL(np), 0);
    gtk_box_append(GTK_BOX(box), np);
    gtk_box_append(GTK_BOX(box), notes);
    form->favorite = GTK_CHECK_BUTTON(gtk_check_button_new_with_mnemonic("_Favorite"));
    gtk_check_button_set_active(form->favorite, member_index(p->view->doc[FAVORITES].winner, key) >= 0);
    gtk_widget_set_sensitive(GTK_WIDGET(form->favorite), writable(p, FAVORITES));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(form->favorite));
    GtkWidget *tag = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(tag), "New or existing tag label");
    GtkWidget *tp = gtk_label_new_with_mnemonic("_Tag (optional)");
    gtk_label_set_xalign(GTK_LABEL(tp), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(tp), tag);
    form->tag = GTK_EDITABLE(tag);
    form->remove_tag = GTK_CHECK_BUTTON(gtk_check_button_new_with_mnemonic("_Remove this tag from the host"));
    gtk_widget_set_sensitive(tag, writable(p, META));
    gtk_widget_set_sensitive(GTK_WIDGET(form->remove_tag), writable(p, META));
    gtk_box_append(GTK_BOX(box), tp);
    gtk_box_append(GTK_BOX(box), tag);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(form->remove_tag));
    adw_alert_dialog_set_extra_child(dialog, box);
    adw_alert_dialog_add_responses(dialog, "cancel", "_Cancel", "save", "_Save", NULL);
    adw_alert_dialog_set_close_response(dialog, "cancel");
    adw_alert_dialog_set_response_appearance(dialog, "save", ADW_RESPONSE_SUGGESTED);
    g_object_set_data_full(G_OBJECT(dialog), "edit", form, g_free);
    g_object_set_data_full(G_OBJECT(dialog), "key", g_strdup(key), g_free);
    g_object_set_data_full(G_OBJECT(dialog), "meta-rev", g_strdup(p->view->doc[META].revision), g_free);
    g_object_set_data_full(G_OBJECT(dialog), "favorite-rev", g_strdup(p->view->doc[FAVORITES].revision), g_free);
    g_object_set_data(G_OBJECT(dialog), "generation", GUINT_TO_POINTER(p->generation));
    g_signal_connect(dialog, "response", G_CALLBACK(edit_response), p);
    adw_dialog_present(ADW_DIALOG(dialog), p->page);
}

typedef struct { GtkWidget *page; Action action; guint generation; } FilePick;
static void file_pick_free(FilePick *pick) { g_object_unref(pick->page); g_free(pick); }

static char *export_text(Hosts *p) {
    if (!p->view || !p->view->store) return NULL;
    json_t *all = json_array();
    gboolean ok = all != NULL;
    size_t i;
    json_t *entry;
    json_array_foreach(p->view->doc[HOSTS].winner, i, entry) {
        const char *tag = json_string_value(json_array_get(entry, 0));
        const char *key = json_string_value(json_array_get(entry, 1));
        if (g_strcmp0(tag, "p") || !key) continue;
        json_t *meta = host_meta(p, key);
        json_t *record = meta ? json_deep_copy(meta) : json_object();
        json_t *tag_ids = json_array();
        const char *uuid; json_t *tag_record;
        ok = record && tag_ids && json_object_set_new(record, "pubkey", json_string(key)) == 0;
        json_object_foreach(tag_labels(p), uuid, tag_record) {
            if (json_is_true(json_object_get(tag_record, "deleted"))) continue;
            Doc *doc = p->view->tags ? g_hash_table_lookup(p->view->tags, uuid) : NULL;
            if (!doc || !readable(doc)) { ok = FALSE; break; }
            if (member_index(doc->winner, key) >= 0 && json_array_append_new(tag_ids, json_string(uuid))) { ok = FALSE; break; }
        }
        if (ok && json_array_size(tag_ids)) ok = json_object_set(record, "tags", tag_ids) == 0;
        if (ok) ok = json_array_append(all, record) == 0;
        json_decref(tag_ids); json_decref(record);
        if (!ok) break;
    }
    char *items = ok ? json_dumps(all, JSON_COMPACT) : NULL, *text = NULL;
    if (items) md_managed_store_export(items, &text);
    wipe(items);
    json_decref(all);
    return text;
}

static void file_chosen(GObject *source, GAsyncResult *result, gpointer data) {
    FilePick *pick = data;
    Hosts *p = g_object_get_data(G_OBJECT(pick->page), HOSTS_DATA);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFile) file = pick->action == EXPORT ? gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error) :
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!g_atomic_int_get(&p->closed) && pick->generation == p->generation) {
        if (file && !p->busy) {
            Job *j = g_new0(Job, 1);
            j->action = pick->action;
            j->file = g_steal_pointer(&file);
            if (j->action == EXPORT) j->exchange = export_text(p);
            if (j->action == EXPORT && !j->exchange) {
                gtk_label_set_text(p->error, "Could not prepare the address book for export");
                free_job(j);
            } else start(p, j);
        } else if (!file && error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
                   !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) gtk_label_set_text(p->error, error->message);
    }
    file_pick_free(pick);
}
static void choose_file(Hosts *p, Action action) {
    GtkRoot *root = gtk_widget_get_root(p->page);
    if (!root || !GTK_IS_WINDOW(root)) return;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
    const char *title = action == EXPORT ? "Export Saved Hosts" : "Import Hosts";
#ifdef MD_HAVE_MANAGER_QR
    if (action == SCAN) title = "Open QR Code Image";
#endif
    gtk_file_dialog_set_title(dialog, title);
    FilePick *pick = g_new0(FilePick, 1);
    pick->page = g_object_ref(p->page);
    pick->action = action;
    pick->generation = p->generation;
    if (action == EXPORT) {
        gtk_file_dialog_set_initial_name(dialog, "metadesk-hosts.json");
        gtk_file_dialog_save(dialog, GTK_WINDOW(root), NULL, file_chosen, pick);
    } else gtk_file_dialog_open(dialog, GTK_WINDOW(root), NULL, file_chosen, pick);
}
static void import_clicked(GtkButton *button, gpointer data) { (void)button; choose_file(data, IMPORT); }
#ifdef MD_HAVE_MANAGER_QR
static void scan_clicked(GtkButton *button, gpointer data) { (void)button; choose_file(data, SCAN); }
#endif
static void export_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
    (void)dialog;
    if (!strcmp(response, "export")) choose_file(data, EXPORT);
}
static void export_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Hosts *p = data;
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Export Saved Hosts?",
        "The file is not encrypted. Anyone who can read it can see the host keys, labels and notes. It does not contain credentials or access grants."));
    adw_alert_dialog_add_responses(dialog, "cancel", "_Cancel", "export", "_Export…", NULL);
    adw_alert_dialog_set_close_response(dialog, "cancel");
    g_signal_connect(dialog, "response", G_CALLBACK(export_response), p);
    adw_dialog_present(ADW_DIALOG(dialog), p->page);
}

typedef struct {
    GtkWidget *page;
    guint generation;
    char meta_revision[65], hosts_revision[65];
    GPtrArray *checks;
} ImportReview;
static void free_import(gpointer data) {
    ImportReview *r = data;
    g_object_unref(r->page);
    g_ptr_array_unref(r->checks);
    g_free(r);
}
static void release_json(gpointer data) { json_decref(data); }
static void select_all(GtkButton *button, gpointer data) {
    (void)button;
    ImportReview *r = data;
    for (guint i = 0; i < r->checks->len; ++i)
        gtk_check_button_set_active(g_ptr_array_index(r->checks, i), TRUE);
}
static void import_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
    (void)dialog;
    ImportReview *r = data;
    Hosts *p = g_object_get_data(G_OBJECT(r->page), HOSTS_DATA);
    if (strcmp(response, "import") || p->busy || g_atomic_int_get(&p->closed)) return;
    if (r->generation != p->generation || !writable(p, HOSTS) || !writable(p, META) ||
        strcmp(r->hosts_revision, p->view->doc[HOSTS].revision) || strcmp(r->meta_revision, p->view->doc[META].revision)) {
        gtk_label_set_text(p->error, "The address book changed. Review the import again."); return;
    }
    json_t *book = copy_doc(p, HOSTS), *doc = copy_doc(p, META);
    json_t *meta = ensure_object(doc, "hosts");
    gboolean ok = book && meta;
    Job *tags = g_new0(Job, 1);
    guint count = 0;
    g_autoptr(GHashTable) aliases = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < r->checks->len && ok; ++i) {
        GtkCheckButton *check = g_ptr_array_index(r->checks, i);
        if (!gtk_check_button_get_active(check)) continue;
        json_t *candidate = g_object_get_data(G_OBJECT(check), "candidate");
        const char *key = string_field(candidate, "pubkey");
        const char *alias = string_field(candidate, "nip05");
        gboolean ambiguous;
        const char *pin = alias_pin(p, alias, &ambiguous);
        if (ambiguous || (pin && strcmp(key, pin)) || (alias && g_hash_table_contains(aliases, alias))) { ok = FALSE; break; }
        if (alias) g_hash_table_add(aliases, g_strdup(alias));
        json_t *record = json_deep_copy(candidate);
        if (!record) { ok = FALSE; break; }
        size_t n; json_t *tag_id;
        json_array_foreach(json_object_get(record, "tags"), n, tag_id) {
            const char *uuid = json_string_value(tag_id);
            json_t *labels = ensure_object(doc, "tags");
            json_t *tag = json_object_get(labels, uuid);
            if (!tag && json_object_set_new(labels, uuid, json_pack("{s:s}", "label", uuid))) { ok = FALSE; break; }
            if (json_is_true(json_object_get(tag, "deleted")) || !append_tag_edit(tags, uuid, key, FALSE)) { ok = FALSE; break; }
        }
        json_object_del(record, "pubkey");
        json_object_del(record, "tags");
        ok = ok && member_set(book, key, TRUE) && json_object_set(meta, key, record) == 0;
        json_decref(record);
        ++count;
    }
    if (!ok) gtk_label_set_text(p->error, "Import blocked: a selected host has a conflicting NIP-05 pin or tag");
    else if (count) {
        Job *j = g_new0(Job, 1);
        j->action = EDIT;
        append_edit(j, META, r->meta_revision, MD_EDIT_STAGE, doc);
        append_edit(j, HOSTS, r->hosts_revision, MD_EDIT_STAGE, book);
        while (tags->edits && tags->edits->len) g_ptr_array_add(j->edits, g_ptr_array_steal_index(tags->edits, 0));
        start(p, j);
    }
    free_job(tags);
    json_decref(book); json_decref(doc);
}

static void present_import(Hosts *p, const char *text) {
    if (!writable(p, HOSTS) || !writable(p, META)) return;
    json_t *candidates = json_loads(text, JSON_REJECT_DUPLICATES, NULL);
    ImportReview *r = g_new0(ImportReview, 1);
    r->page = g_object_ref(p->page);
    r->generation = p->generation;
    memcpy(r->hosts_revision, p->view->doc[HOSTS].revision, 65);
    memcpy(r->meta_revision, p->view->doc[META].revision, 65);
    r->checks = g_ptr_array_new();
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Review Imported Host Keys",
        "Select only keys whose fingerprints you recognize. NIP-05 names in the file have not been verified. Existing hosts are not changed."));
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 180);
    GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), rows);
    gtk_box_append(GTK_BOX(outer), scroll);
    size_t i;
    json_t *candidate;
    json_array_foreach(candidates, i, candidate) {
        const char *key = string_field(candidate, "pubkey");
        char fp[MD_KEY_FINGERPRINT_SIZE];
        if (!md_key_fingerprint(key, fp) || member_index(p->view->doc[HOSTS].winner, key) >= 0) continue;
        const char *name = string_field(candidate, "label");
        const char *alias = string_field(candidate, "nip05");
        GtkWidget *check = gtk_check_button_new_with_label(fp);
        gtk_box_append(GTK_BOX(rows), check);
        if (name && *name) gtk_box_append(GTK_BOX(rows), label(name));
        if (alias && *alias) gtk_box_append(GTK_BOX(rows), label(alias));
        g_autofree char *npub = md_manager_npub(key);
        GtkWidget *identity = label(npub ? npub : key);
        gtk_label_set_selectable(GTK_LABEL(identity), TRUE);
        gtk_box_append(GTK_BOX(rows), identity);
        g_autoptr(GString) access = g_string_new("Requested access: ");
        json_t *caps = json_object_get(candidate, "requested_caps");
        size_t index;
        json_t *cap;
        json_array_foreach(caps, index, cap) {
            if (index) g_string_append(access, ", ");
            g_string_append(access, json_string_value(cap));
        }
        if (!json_array_size(caps)) g_string_append(access, "video only");
        gtk_box_append(GTK_BOX(rows), label(access->str));
        g_object_set_data_full(G_OBJECT(check), "candidate", json_incref(candidate), release_json);
        g_ptr_array_add(r->checks, check);
    }
    json_decref(candidates);
    if (!r->checks->len) {
        gtk_label_set_text(p->error, "This file contains no new hosts");
        g_object_ref_sink(dialog);
        g_object_unref(dialog);
        free_import(r);
        return;
    }
    GtkWidget *all = gtk_button_new_with_mnemonic("Select _All Reviewed Keys");
    g_signal_connect(all, "clicked", G_CALLBACK(select_all), r);
    gtk_box_append(GTK_BOX(outer), all);
    adw_alert_dialog_set_extra_child(dialog, outer);
    adw_alert_dialog_add_responses(dialog, "cancel", "_Cancel", "import", "_Import Selected", NULL);
    adw_alert_dialog_set_close_response(dialog, "cancel");
    adw_alert_dialog_set_response_appearance(dialog, "import", ADW_RESPONSE_SUGGESTED);
    g_object_set_data_full(G_OBJECT(dialog), "import-review", r, free_import);
    g_signal_connect(dialog, "response", G_CALLBACK(import_response), r);
    adw_dialog_present(ADW_DIALOG(dialog), p->page);
}

static GtkEntry *entry(Hosts *p, const char *title, const char *placeholder, int max) {
    GtkWidget *input = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(input), placeholder);
    gtk_entry_set_max_length(GTK_ENTRY(input), max);
    gtk_editable_set_width_chars(GTK_EDITABLE(input), 12);
    GtkWidget *prompt = gtk_label_new_with_mnemonic(title);
    gtk_label_set_xalign(GTK_LABEL(prompt), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(prompt), input);
    gtk_box_append(p->content, prompt);
    gtk_box_append(p->content, input);
    return GTK_ENTRY(input);
}
static GtkListBox *section(Hosts *p, const char *title) {
    GtkWidget *heading = label(title);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(p->content, heading);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_box_append(p->content, list);
    return GTK_LIST_BOX(list);
}
static void refresh_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    Hosts *p = data;
    if (p->busy) return;
    if (!p->view || !p->view->store) p->configure_pending = TRUE;
    else p->start_pending = TRUE;
    kick(p);
}

GtkWidget *md_manager_hosts_page_new(void) {
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_name(scroll, "hosts-page");
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 680);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    Hosts *p = g_new0(Hosts, 1);
    p->page = scroll;
    g_weak_ref_init(&p->weak_page, scroll);
    g_object_set_data_full(G_OBJECT(scroll), HOSTS_DATA, p, free_page);
    p->content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
    GtkWidget *content = GTK_WIDGET(p->content);
    gtk_widget_set_margin_start(content, 12); gtk_widget_set_margin_end(content, 12);
    gtk_widget_set_margin_top(content, 12); gtk_widget_set_margin_bottom(content, 12);
    adw_clamp_set_child(ADW_CLAMP(clamp), content);
    p->banner = ADW_BANNER(adw_banner_new(""));
    gtk_widget_set_name(GTK_WIDGET(p->banner), "hosts-sync-status");
    gtk_box_append(p->content, GTK_WIDGET(p->banner));
    p->rotation = gtk_expander_new_with_mnemonic("Hosts to _Request Access From Again");
    gtk_widget_set_name(p->rotation, "client-key-rotation");
    p->rotation_hosts = GTK_LABEL(label(""));
    gtk_label_set_selectable(p->rotation_hosts, TRUE);
    gtk_expander_set_child(GTK_EXPANDER(p->rotation), GTK_WIDGET(p->rotation_hosts));
    gtk_widget_set_visible(p->rotation, FALSE);
    gtk_box_append(p->content, p->rotation);
    p->address = entry(p, "_Host identity", "npub, nprofile, or name@example.org", MD_MANAGED_ADDRESS_MAX);
    gtk_widget_set_name(GTK_WIDGET(p->address), "host-identity");
    p->route = entry(p, "_IP address (optional route)", "IPv4 or IPv6", 45);
    p->review = GTK_BUTTON(button("_Review and Connect…", G_CALLBACK(review_entry), p));
    gtk_box_append(p->content, GTK_WIDGET(p->review));
    g_signal_connect(p->address, "activate", G_CALLBACK(review_entry), p);
    g_signal_connect(p->route, "activate", G_CALLBACK(review_entry), p);
#ifdef MD_HAVE_MANAGER_QR
    p->scan_button = GTK_BUTTON(button("Scan QR _Image…", G_CALLBACK(scan_clicked), p));
    gtk_box_append(p->content, GTK_WIDGET(p->scan_button));
#endif
    p->error = GTK_LABEL(label(""));
    g_signal_connect(p->address, "changed", G_CALLBACK(address_changed), p);
    g_signal_connect(p->route, "changed", G_CALLBACK(address_changed), p);
    gtk_widget_add_css_class(GTK_WIDGET(p->error), "error");
    gtk_box_append(p->content, GTK_WIDGET(p->error));
    p->progress = GTK_LABEL(label(""));
    gtk_box_append(p->content, GTK_WIDGET(p->progress));
    p->relay_diagnostic = GTK_LABEL(label(""));
    gtk_box_append(p->content, GTK_WIDGET(p->relay_diagnostic));
    p->launch_cancel = GTK_BUTTON(button("_Cancel Connection", G_CALLBACK(connect_control), p));
    gtk_widget_set_visible(GTK_WIDGET(p->launch_cancel), FALSE);
    gtk_box_append(p->content, GTK_WIDGET(p->launch_cancel));
    p->quick = section(p, "Quick Connect");
    p->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(p->search, "Search Saved Hosts");
    g_signal_connect(p->search, "search-changed", G_CALLBACK(search_changed), p);
    gtk_box_append(p->content, GTK_WIDGET(p->search));
    p->tag_filter = entry(p, "Filter by _tag", "Tag label", 64);
    g_signal_connect(p->tag_filter, "changed", G_CALLBACK(search_changed), p);
    p->saved = section(p, "Saved Hosts");
    p->import_button = GTK_BUTTON(button("_Import Hosts…", G_CALLBACK(import_clicked), p));
    gtk_box_append(p->content, GTK_WIDGET(p->import_button));
    p->export_button = GTK_BUTTON(button("_Export Hosts…", G_CALLBACK(export_clicked), p));
    gtk_box_append(p->content, GTK_WIDGET(p->export_button));
    gtk_box_append(p->content, button("Re_fresh Address Book", G_CALLBACK(refresh_clicked), p));
    render(p);
    return scroll;
}

static void show_rotation_guidance(Hosts *p, const char *next_key) {
    if (p->key && p->view && p->view->store && readable(&p->view->doc[HOSTS])) {
        GString *hosts = g_string_new("");
        size_t i; json_t *record;
        json_array_foreach(p->view->doc[HOSTS].winner, i, record) {
            if (g_strcmp0(json_string_value(json_array_get(record, 0)), "p")) continue;
            const char *key = json_string_value(json_array_get(record, 1));
            g_autofree char *npub = md_manager_npub(key);
            if (!npub) continue;
            char fp[MD_KEY_FINGERPRINT_SIZE];
            g_string_append_printf(hosts, "\n\n%s\n%s", title_for(p, key, fp), npub);
        }
        g_free(p->previous_client); p->previous_client = g_strdup(p->key);
        g_free(p->previous_hosts); p->previous_hosts = g_string_free(hosts, FALSE);
    }
    gboolean show = next_key && p->previous_client && strcmp(next_key, p->previous_client) &&
        p->previous_hosts && *p->previous_hosts;
    g_autofree char *text = show ? g_strconcat(
        "The new client key does not inherit access from the old key. Request access again from these saved hosts:",
        p->previous_hosts, NULL) : NULL;
    gtk_label_set_text(p->rotation_hosts, text ? text : "");
    gtk_widget_set_visible(p->rotation, show);
}

void md_manager_hosts_configure(GtkWidget *page, const char *directory, const char *key,
    const char *const *relays) {
    Hosts *p = g_object_get_data(G_OBJECT(page), HOSTS_DATA);
    if (g_atomic_int_get(&p->closed)) return;
    const char *empty[] = {NULL};
    if (!relays) relays = empty;
    if (!g_strcmp0(p->directory, directory) && !g_strcmp0(p->key, key) &&
        p->relays && g_strv_equal((const char *const *)p->relays, relays)) return;
    GtkRoot *root = gtk_widget_get_root(page);
    if (!p->application && root && GTK_IS_WINDOW(root)) {
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(root));
        if (app) p->application = g_object_ref(app);
    }
    if (g_strcmp0(p->key, key)) {
        show_rotation_guidance(p, key);
        md_manager_launch_free(p->launch);
        p->launch = NULL;
        p->record_pending = FALSE;
        gtk_label_set_text(p->progress, "");
        gtk_widget_set_visible(GTK_WIDGET(p->launch_cancel), FALSE);
    }
    g_free(p->directory); p->directory = g_strdup(directory);
    g_free(p->key); p->key = g_strdup(key);
    g_strfreev(p->relays); p->relays = g_strdupv((char **)relays);
    ++p->generation;
    g_clear_pointer(&p->view, free_view);
    p->configure_pending = TRUE;
    render(p);
    kick(p);
}

void md_manager_hosts_stop(GtkWidget *page) {
    Hosts *p = g_object_get_data(G_OBJECT(page), HOSTS_DATA);
    g_atomic_int_set(&p->closed, TRUE);
    g_clear_pointer(&p->launch, md_manager_launch_free);
    kick(p);
}
