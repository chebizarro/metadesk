#include "access-page.h"
#include "model.h"
#include "nostr.h"
#include <string.h>
#include <time.h>

struct _MdManagerAccess {
    GObject parent_instance;
    GtkWindow *window;
    GtkStack *pages;
    MdManagerHost *host;
    AdwEntryRow *endpoint;
    AdwBanner *connection, *activity_connection;
    GtkListBox *requests, *clients, *sessions;
    GHashTable *notifications;
    guint expiry;
    gboolean stopped;
    GWeakRef dialog;
};
G_DEFINE_TYPE(MdManagerAccess, md_manager_access, G_TYPE_OBJECT)
static void render(MdManagerAccess *self);
static void sync_dialog(MdManagerAccess *self);
static GtkWidget *row(const char *title,const char *subtitle) {
    GtkWidget *w=adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(w),FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(w),title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(w),subtitle);
    adw_action_row_set_title_lines(ADW_ACTION_ROW(w),2);
    return w;
}
static void clear(GtkListBox *list) {
    GtkWidget *child;
    while ((child=gtk_widget_get_first_child(GTK_WIDGET(list)))) gtk_list_box_remove(list,child);
}
static void withdraw(MdManagerAccess *self,const char *id) {
    GApplication *app=G_APPLICATION(gtk_window_get_application(self->window));
    if (g_application_get_is_registered(app)) g_application_withdraw_notification(app,id);
}
void md_manager_access_stop(MdManagerAccess *self) {
    if (self->stopped) return;
    self->stopped=TRUE;
    if (self->expiry) { g_source_remove(self->expiry); self->expiry=0; }
    GHashTableIter iter; gpointer id;
    g_hash_table_iter_init(&iter,self->notifications);
    while (g_hash_table_iter_next(&iter,&id,NULL)) withdraw(self,id);
    g_hash_table_remove_all(self->notifications);
    md_manager_host_stop(self->host);
}
static void dispose(GObject *object) {
    MdManagerAccess *self=MD_MANAGER_ACCESS(object);
    md_manager_access_stop(self); g_clear_object(&self->host);
    G_OBJECT_CLASS(md_manager_access_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
    g_weak_ref_clear(&MD_MANAGER_ACCESS(object)->dialog);
    g_hash_table_unref(MD_MANAGER_ACCESS(object)->notifications);
    G_OBJECT_CLASS(md_manager_access_parent_class)->finalize(object);
}
static void md_manager_access_class_init(MdManagerAccessClass *klass) {
    G_OBJECT_CLASS(klass)->dispose=dispose; G_OBJECT_CLASS(klass)->finalize=finalize;
}
static void md_manager_access_init(MdManagerAccess *self) {
    g_weak_ref_init(&self->dialog,NULL);
    self->host=md_manager_host_new();
    self->notifications=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
}
MdManagerHost *md_manager_access_host(MdManagerAccess *self) { return self->host; }
static gboolean expire(gpointer data) {
    MdManagerAccess *self=data; self->expiry=0; render(self); return G_SOURCE_REMOVE;
}
static void notifications(MdManagerAccess *self,const cJSON *snapshot) {
    GApplication *app=G_APPLICATION(gtk_window_get_application(self->window));
    GHashTable *live=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
    const cJSON *q; gint64 now=(gint64)time(NULL), next=G_MAXINT64;
    cJSON_ArrayForEach(q,cJSON_GetObjectItemCaseSensitive(snapshot,"requests")) {
        gint64 deadline=(gint64)md_manager_json_uint(q,"expires_at");
        if (deadline<=now || strcmp(md_manager_json_string(q,"state"),"pending")) continue;
        next=MIN(next,deadline);
        g_autofree char *id=g_strdup_printf("request-%s-%s",md_manager_json_string(snapshot,"generation"),md_manager_json_string(q,"request_id"));
        g_hash_table_add(live,g_strdup(id));
        if (g_hash_table_contains(self->notifications,id)) continue;
        g_autoptr(GNotification) notification=g_notification_new("Desktop Access Requested");
        g_autofree char *caps=md_manager_caps((guint32)md_manager_json_uint(q,"requested"));
        g_autofree char *body=g_strdup_printf("%.8s…%.8s requests %s. Review the exact request in Access.",md_manager_json_string(q,"peer"),md_manager_json_string(q,"peer")+56,caps);
        g_notification_set_body(notification,body);
        g_notification_set_default_action_and_target(notification,"app.review-request","(ss)",
            md_manager_json_string(snapshot,"generation"),md_manager_json_string(q,"request_id"));
        if (g_application_get_is_registered(app)) g_application_send_notification(app,id,notification);
    }
    GHashTableIter iter; gpointer id;
    g_hash_table_iter_init(&iter,self->notifications);
    while (g_hash_table_iter_next(&iter,&id,NULL)) if (!g_hash_table_contains(live,id)) withdraw(self,id);
    g_hash_table_unref(self->notifications); self->notifications=live;
    if (self->expiry) { g_source_remove(self->expiry); self->expiry=0; }
    /* An authenticated request's exact expiry, not a relay completion timer. */
    if (next!=G_MAXINT64) self->expiry=g_timeout_add((guint)MIN((next-now)*1000, G_MAXUINT),expire,self);
}

typedef struct {
    MdManagerHost *host;
    cJSON *command;
    GtkCheckButton *checks[3];
    gboolean policy;
    GtkCheckButton *remember;
    gboolean valid;
} Decision;
static void decision_free(gpointer data,GClosure *closure) {
    (void)closure; Decision *d=data;
    g_object_unref(d->host); cJSON_Delete(d->command); g_free(d);
}
static guint32 selected(Decision *d) {
    guint32 caps=0;
    for (guint i=0;i<3;++i) if (gtk_check_button_get_active(d->checks[i])) caps|=1u<<i;
    return caps;
}
static void selection_changed(GtkCheckButton *button,gpointer data) {
    (void)button; AdwAlertDialog *dialog=data;
    Decision *d=g_object_get_data(G_OBJECT(dialog),"decision");
    adw_alert_dialog_set_response_enabled(dialog,"once",d->valid && selected(d)!=0);

}
static void response(AdwAlertDialog *dialog,const char *name,gpointer data) {
    (void)dialog; Decision *d=data;
    if (!strcmp(name,"cancel") || !d->valid) return;
    if (d->policy) {
        if (!strcmp(name,"revoke")) cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(d->command,"method"),"access.remove");
        else cJSON_AddNumberToObject(d->command,"caps",selected(d));
    } else if (strcmp(name,"kick")) {
        gboolean approve=strcmp(name,"deny")!=0;
        cJSON_AddStringToObject(d->command,"decision",approve?"approve":"deny");
        if (approve) {
            cJSON_AddNumberToObject(d->command,"caps",selected(d));
            cJSON_AddBoolToObject(d->command,"remember",d->remember && gtk_check_button_get_active(d->remember));
        }
    }
    md_manager_host_command(d->host,d->command);
}
static GtkWidget *label(const char *text) {
    GtkWidget *w=gtk_label_new(text); gtk_label_set_wrap(GTK_LABEL(w),TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(w),PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(w),TRUE); gtk_label_set_xalign(GTK_LABEL(w),0); return w;
}
static void review(MdManagerAccess *self,const cJSON *item,gboolean policy,gboolean kick) {
    const cJSON *snapshot=md_manager_host_snapshot(self->host);
    if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self->window))) return;
    if (!snapshot || !md_manager_host_ready(self->host) || md_manager_host_busy(self->host)) return;
    AdwDialog *dialog=adw_alert_dialog_new(kick?"End This Session?":policy?"Client Access":"Review Access Request",NULL);
    AdwAlertDialog *alert=ADW_ALERT_DIALOG(dialog);
    Decision *d=g_new0(Decision,1); d->host=g_object_ref(self->host); d->policy=policy; d->valid=TRUE;
    d->command=md_manager_host_mutation(snapshot,kick?"session.kick":policy?"access.set":"requests.decide");
    cJSON_AddStringToObject(d->command,kick?"session_id":policy?"peer":"request_id",md_manager_json_string(item,kick?"session_id":policy?"peer":"request_id"));
    if (policy) cJSON_AddNumberToObject(d->command,"policy_version",(double)md_manager_json_uint(md_manager_host_policy(self->host),"policy_version"));
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,12);
    g_autofree char *npub=md_manager_npub(md_manager_json_string(item,"peer"));
    gtk_box_append(GTK_BOX(box),label(npub?npub:md_manager_json_string(item,"peer")));
    gboolean editable=TRUE;
    if (kick) gtk_box_append(GTK_BOX(box),label("This ends the current session, not the remembered grant. A new request can still be authorized."));
    else {
        guint32 requested=(guint32)md_manager_json_uint(item,"requested");
        guint32 supported=cJSON_GetObjectItemCaseSensitive(snapshot,"supported")?(guint32)md_manager_json_uint(snapshot,"supported"):7u;
        if (!policy) {
            g_autofree char *caps=md_manager_caps(requested);
            g_autoptr(GDateTime) deadline=g_date_time_new_from_unix_local((gint64)md_manager_json_uint(item,"expires_at"));
            g_autofree char *expires=deadline?g_date_time_format(deadline,"%F %T %Z"):g_strdup("Unknown");
            g_autofree char *detail=g_strdup_printf("Request %s\nRequested: %s\nExpires: %s",
                md_manager_json_string(item,"request_id"),caps,expires);
            gtk_box_append(GTK_BOX(box),label(detail));
        }
        gtk_box_append(GTK_BOX(box),label(policy?"Save replaces the exact grant and ends any current session. Revoke removes the remembered client and ends its session. No permissions are implied by another permission.":
            "Select exact permissions. Accessibility actions need both the tree and input permissions. Remember saves only this selection in the host’s private allowlist."));
        static const char *names[]={"_Video","_Accessibility tree","_Input"};
        guint32 current=0;
        if (policy) {
            editable=md_nostr_parse_caps(md_manager_json_string(item,"caps"),&current)==0;
            if (!editable) gtk_box_append(GTK_BOX(box),label("This manager cannot edit unknown permissions. Revoke Access is still available."));
        }
        for (guint i=0;i<3;++i) {
            d->checks[i]=GTK_CHECK_BUTTON(gtk_check_button_new_with_mnemonic(names[i]));
            gtk_widget_set_sensitive(GTK_WIDGET(d->checks[i]),policy || (requested & supported & (1u<<i)));
            gtk_check_button_set_active(d->checks[i],policy && (current&(1u<<i)));
            gtk_box_append(GTK_BOX(box),GTK_WIDGET(d->checks[i]));
        }
        if (!policy) {
            d->remember=GTK_CHECK_BUTTON(gtk_check_button_new_with_mnemonic("_Remember this exact grant"));
            gtk_box_append(GTK_BOX(box),GTK_WIDGET(d->remember));
        }
        if (!policy && (requested&~7u)) gtk_box_append(GTK_BOX(box),label("Unknown permissions are shown but cannot be granted by this manager version"));
    }
    adw_alert_dialog_set_extra_child(alert,box);
    adw_alert_dialog_add_response(alert,"cancel","_Cancel");
    adw_alert_dialog_set_close_response(alert,"cancel"); adw_alert_dialog_set_default_response(alert,"cancel");
    if (kick) { adw_alert_dialog_add_response(alert,"kick","_End Session"); adw_alert_dialog_set_response_appearance(alert,"kick",ADW_RESPONSE_DESTRUCTIVE); }
    else if (policy) {
        adw_alert_dialog_add_responses(alert,"revoke","_Revoke Access","save","_Save",NULL);
        adw_alert_dialog_set_response_appearance(alert,"revoke",ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_enabled(alert,"save",editable);
    } else {
        adw_alert_dialog_add_responses(alert,"deny","_Deny","once","_Approve",NULL);
        adw_alert_dialog_set_response_enabled(alert,"once",FALSE);

        for (guint i=0;i<3;++i) g_signal_connect_object(d->checks[i],"toggled",G_CALLBACK(selection_changed),alert,0);
    }
    g_object_set_data(G_OBJECT(alert),"decision",d);
    g_weak_ref_set(&self->dialog,alert);
    g_signal_connect_data(alert,"response",G_CALLBACK(response),d,decision_free,0);
    adw_dialog_present(dialog,GTK_WIDGET(self->window));
}
static void sync_dialog(MdManagerAccess *self) {
    g_autoptr(AdwAlertDialog) dialog=g_weak_ref_get(&self->dialog);
    if (!dialog) return;
    Decision *d=g_object_get_data(G_OBJECT(dialog),"decision");
    const cJSON *snapshot=md_manager_host_snapshot(self->host);
    gboolean current=md_manager_host_ready(self->host) &&
        !strcmp(md_manager_json_string(d->command,"generation"),md_manager_json_string(snapshot,"generation")) &&
        md_manager_json_uint(d->command,"revision")==md_manager_json_uint(snapshot,"revision");
    const char *id=md_manager_json_string(d->command,"request_id");
    if (*id) {
        gboolean pending=FALSE; const cJSON *q;
        cJSON_ArrayForEach(q,cJSON_GetObjectItemCaseSensitive(snapshot,"requests"))
            if (!strcmp(id,md_manager_json_string(q,"request_id")) && !strcmp(md_manager_json_string(q,"state"),"pending") &&
                md_manager_json_uint(q,"expires_at")>(guint64)time(NULL)) pending=TRUE;
        current &= pending;
    }
    if (d->policy) current &= md_manager_json_uint(d->command,"policy_version")==md_manager_json_uint(md_manager_host_policy(self->host),"policy_version");
    if (current) return;
    d->valid=FALSE;
    const char *responses[]={"once","deny","save","revoke","kick"};
    for (guint i=0;i<G_N_ELEMENTS(responses);++i)
        if (adw_alert_dialog_has_response(dialog,responses[i])) adw_alert_dialog_set_response_enabled(dialog,responses[i],FALSE);
    adw_alert_dialog_set_body(dialog,"This review is no longer current. Close it and review the latest host state.");
}
static void review_clicked(GtkButton *button,gpointer data) {
    MdManagerAccess *self=data;
    const cJSON *item=g_object_get_data(G_OBJECT(button),"item");
    review(self,item,GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),"policy")),GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),"kick")));
}
static void add_item(MdManagerAccess *self,GtkListBox *list,const cJSON *item,const char *detail,gboolean policy,gboolean kick,gboolean enabled) {
    const char *peer=md_manager_json_string(item,"peer");
    g_autofree char *title=g_strdup_printf("%.8s…%.8s",peer,peer+56);
    GtkWidget *w=row(title,detail), *button=gtk_button_new_with_mnemonic(kick?"_End Session…":policy?"_Edit Access…":"_Review…");
    gtk_widget_set_valign(button,GTK_ALIGN_CENTER); gtk_widget_set_sensitive(button,enabled);
    g_object_set_data_full(G_OBJECT(button),"item",cJSON_Duplicate(item,TRUE),(GDestroyNotify)cJSON_Delete);
    g_object_set_data(G_OBJECT(button),"policy",GINT_TO_POINTER(policy)); g_object_set_data(G_OBJECT(button),"kick",GINT_TO_POINTER(kick));
    g_signal_connect_object(button,"clicked",G_CALLBACK(review_clicked),self,0);
    adw_action_row_add_suffix(ADW_ACTION_ROW(w),button); gtk_list_box_append(list,w);
}
static void render(MdManagerAccess *self) {
    if (self->stopped) return;
    const cJSON *snapshot=md_manager_host_snapshot(self->host), *policy=md_manager_host_policy(self->host), *q;
    const char *status=md_manager_host_status(self->host);
    adw_banner_set_title(self->connection,status); adw_banner_set_title(self->activity_connection,status);
    gboolean enabled=md_manager_host_ready(self->host)&&!md_manager_host_busy(self->host);
    sync_dialog(self);
    notifications(self,snapshot); clear(self->requests); clear(self->clients); clear(self->sessions);
    cJSON_ArrayForEach(q,cJSON_GetObjectItemCaseSensitive(snapshot,"requests")) {
        if (md_manager_json_uint(q,"expires_at")<=(guint64)time(NULL)) continue;
        g_autofree char *caps=md_manager_caps((guint32)md_manager_json_uint(q,"requested"));
        g_autofree char *detail=g_strdup_printf("%s · %s",md_manager_json_string(q,"state"),caps);
        add_item(self,self->requests,q,detail,FALSE,FALSE,enabled&&!strcmp(md_manager_json_string(q,"state"),"pending"));
    }
    if (!gtk_widget_get_first_child(GTK_WIDGET(self->requests))) gtk_list_box_append(self->requests,row(snapshot?"No Pending Requests":"Requests Unavailable",snapshot?"Only authenticated, unexpired requests appear here":"Connect to a running host to review requests"));
    cJSON_ArrayForEach(q,cJSON_GetObjectItemCaseSensitive(policy,"clients")) {
        gboolean legacy=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(q,"review_required"));
        guint32 bits;
        gboolean known=md_nostr_parse_caps(md_manager_json_string(q,"caps"),&bits)==0;
        g_autofree char *detail=g_strdup_printf("%s%s",md_manager_json_string(q,"caps"),legacy?" · Legacy policy — migrate with the host CLI":!known?" · Unknown permissions — update the manager to edit":"");
        add_item(self,self->clients,q,detail,TRUE,FALSE,enabled&&!legacy);
    }
    if (!gtk_widget_get_first_child(GTK_WIDGET(self->clients))) {
        gboolean available=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(policy,"policy_available"));
        gtk_list_box_append(self->clients,row(available?"No Remembered Clients":"Private Policy Unavailable",available?"Use Approve and Remember for an incoming request":"The host must load its private policy before access can be edited"));
    }
    const cJSON *session=cJSON_GetObjectItemCaseSensitive(snapshot,"session");
    if (cJSON_IsObject(session)) {
        gboolean revoked=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(session,"revoked"));
        g_autofree char *caps=md_manager_caps((guint32)md_manager_json_uint(session,"granted"));
        g_autofree char *detail=g_strdup_printf("%s · %s",revoked?"Ending — waiting for host cleanup":*md_manager_json_string(session,"session_id")?"Reserved or active":"Reserved — awaiting binding",caps);
        add_item(self,self->sessions,session,detail,FALSE,TRUE,enabled&&!revoked&&*md_manager_json_string(session,"session_id"));
    } else gtk_list_box_append(self->sessions,row(snapshot?"No Active Session":"Sessions Unavailable",snapshot?"A reservation appears here before binding; this is not a connection history":"Connect to the local host in Access"));
}
void md_manager_access_review(MdManagerAccess *self,const char *generation,const char *id) {
    const cJSON *snapshot=md_manager_host_snapshot(self->host), *q;
    gtk_stack_set_visible_child_name(self->pages,"Access"); gtk_window_present(self->window);
    if (strcmp(generation,md_manager_json_string(snapshot,"generation"))) return;
    cJSON_ArrayForEach(q,cJSON_GetObjectItemCaseSensitive(snapshot,"requests"))
        if (!strcmp(id,md_manager_json_string(q,"request_id")) && !strcmp(md_manager_json_string(q,"state"),"pending") &&
            md_manager_json_uint(q,"expires_at")>(guint64)time(NULL)) { review(self,q,FALSE,FALSE); return; }
}
void md_manager_access_connect(MdManagerAccess *self,const char *path) {
    self->stopped=FALSE; gtk_editable_set_text(GTK_EDITABLE(self->endpoint),path);
    md_manager_host_connect(self->host,path);
}
static void connect_clicked(GtkButton *button,gpointer data) {
    (void)button; MdManagerAccess *self=data;
    g_autofree char *path=g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->endpoint)));
    md_manager_access_connect(self,path);
}
static void refresh_clicked(GtkButton *button,gpointer data) { (void)button; md_manager_host_refresh_access(MD_MANAGER_ACCESS(data)->host); }
static GtkWidget *page(MdManagerAccess *self,const char *name,AdwBanner **banner,GtkWidget **box) {
    GtkWidget *outer=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    *banner=ADW_BANNER(adw_banner_new("Host not connected")); adw_banner_set_revealed(*banner,TRUE);
    gtk_box_append(GTK_BOX(outer),GTK_WIDGET(*banner));
    GtkWidget *scroll=gtk_scrolled_window_new(), *clamp=adw_clamp_new(); *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,24);
    gtk_widget_set_margin_start(*box,18); gtk_widget_set_margin_end(*box,18); gtk_widget_set_margin_top(*box,24); gtk_widget_set_margin_bottom(*box,24);
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp),680); adw_clamp_set_child(ADW_CLAMP(clamp),*box);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),clamp); gtk_widget_set_vexpand(scroll,TRUE);
    gtk_box_append(GTK_BOX(outer),scroll); gtk_stack_add_named(self->pages,outer,name); return outer;
}
static void group(GtkWidget *box,const char *title,const char *description,GtkListBox **list) {
    GtkWidget *g=adw_preferences_group_new(); adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g),title);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),description);
    *list=GTK_LIST_BOX(gtk_list_box_new()); gtk_list_box_set_selection_mode(*list,GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(*list),"boxed-list"); adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),GTK_WIDGET(*list));
    gtk_box_append(GTK_BOX(box),g);
}
MdManagerAccess *md_manager_access_new(GtkWindow *window,GtkStack *pages) {
    MdManagerAccess *self=g_object_new(MD_TYPE_MANAGER_ACCESS,NULL); self->window=window; self->pages=pages;
    GtkWidget *box; page(self,"Access",&self->connection,&box);
    GtkWidget *g=adw_preferences_group_new(); adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g),"Local Host");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),"Same-user, credential-verified management only. Connecting never enables attended access or starts a host.");
    self->endpoint=ADW_ENTRY_ROW(adw_entry_row_new()); adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->endpoint),"Management _socket path"); adw_preferences_row_set_use_underline(ADW_PREFERENCES_ROW(self->endpoint),TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),GTK_WIDGET(self->endpoint));
    GtkWidget *button=gtk_button_new_with_mnemonic("_Connect to Host"); gtk_widget_set_halign(button,GTK_ALIGN_END);
    g_signal_connect_object(button,"clicked",G_CALLBACK(connect_clicked),self,0); adw_preferences_group_add(ADW_PREFERENCES_GROUP(g),button); gtk_box_append(GTK_BOX(box),g);
    group(box,"Pending Requests","Review the full public key and exact permissions. Notification activation only opens this request.",&self->requests);
    group(box,"Remembered Clients","Private host policy. Edits replace exact permissions and end the client’s current session. Refresh to reconcile external policy changes.",&self->clients);
    button=gtk_button_new_with_mnemonic("_Refresh Private Policy"); gtk_widget_set_halign(button,GTK_ALIGN_END);
    g_signal_connect_object(button,"clicked",G_CALLBACK(refresh_clicked),self,0); gtk_box_append(GTK_BOX(box),button);
    page(self,"Activity",&self->activity_connection,&box);
    group(box,"Current Session","Live host state, not an audit log. End Session revokes the reservation; cleanup is complete only when it disappears.",&self->sessions);
    g_signal_connect_object(self->host,"changed",G_CALLBACK(render),self,G_CONNECT_SWAPPED);
    render(self); return self;
}
