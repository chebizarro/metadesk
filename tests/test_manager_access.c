#include "window.h"
#include "host-client.h"
#include "md_test.h"
#include <time.h>
#include "manager_fake_host.h"
#include "manager_fake_notifications.h"

static GtkWidget *find_button(GtkWidget *root,const char *text) {
    if (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)),text)) return root;
    for (GtkWidget *child=gtk_widget_get_first_child(root);child;child=gtk_widget_get_next_sibling(child)) {
        GtkWidget *found=find_button(child,text); if (found) return found;
    }
    return NULL;
}
static GtkCheckButton *find_check(GtkWidget *root,const char *text) {
    if (GTK_IS_CHECK_BUTTON(root) && !g_strcmp0(gtk_check_button_get_label(GTK_CHECK_BUTTON(root)),text)) return GTK_CHECK_BUTTON(root);
    for(GtkWidget *child=gtk_widget_get_first_child(root);child;child=gtk_widget_get_next_sibling(child)) {
        GtkCheckButton *found=find_check(child,text); if(found) return found;
    }
    return NULL;
}
static gboolean frame(GtkWidget *widget,GdkFrameClock *clock,gpointer data) {
    (void)widget; (void)clock; *(gboolean *)data=TRUE; return G_SOURCE_REMOVE;
}
static void painted(GdkFrameClock *clock,gpointer data) { (void)clock; *(gboolean *)data=TRUE; }
static int screenshot(GtkWindow *window,const char *name) {
    const char *directory=g_getenv("MD_MANAGER_SCREENSHOT_DIR"); if (!directory) return 0;
    GdkFrameClock *clock=gtk_widget_get_frame_clock(GTK_WIDGET(window)); gboolean complete=FALSE;
    gulong handler=g_signal_connect(clock,"after-paint",G_CALLBACK(painted),&complete);
    gtk_widget_queue_draw(GTK_WIDGET(window)); gdk_frame_clock_request_phase(clock,GDK_FRAME_CLOCK_PHASE_AFTER_PAINT);
    gboolean deadline=FALSE; guint watchdog=g_timeout_add_seconds(10,manager_timeout,&deadline);
    while (!complete && !deadline) g_main_context_iteration(NULL,TRUE);
    if (!deadline) g_source_remove(watchdog);
    g_signal_handler_disconnect(clock,handler);
    MD_CHECK(complete); if (!complete) return 1;
    int width=gtk_widget_get_width(GTK_WIDGET(window)),height=gtk_widget_get_height(GTK_WIDGET(window));
    g_autoptr(GdkPaintable) paintable=gtk_widget_paintable_new(GTK_WIDGET(window)); GtkSnapshot *snapshot=gtk_snapshot_new();
    gdk_paintable_snapshot(paintable,GDK_SNAPSHOT(snapshot),width,height);
    g_autoptr(GskRenderNode) node=gtk_snapshot_free_to_node(snapshot); MD_CHECK(node); if(!node) return 1;
    graphene_rect_t bounds=GRAPHENE_RECT_INIT(0,0,width,height);
    g_autoptr(GdkTexture) texture=gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)),node,&bounds);
    g_autofree char *path=g_build_filename(directory,name,NULL); MD_CHECK(gdk_texture_save_to_png(texture,path)); return 0;
}
static void page(GtkWindow *window,const char *name) {
    GtkStack *pages=GTK_STACK(gtk_widget_get_template_child(GTK_WIDGET(window),MD_TYPE_MANAGER_WINDOW,"pages"));
    AdwNavigationPage *content=ADW_NAVIGATION_PAGE(gtk_widget_get_template_child(GTK_WIDGET(window),MD_TYPE_MANAGER_WINDOW,"content_page"));
    AdwNavigationSplitView *split=ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(GTK_WIDGET(window),MD_TYPE_MANAGER_WINDOW,"split"));
    GtkListBox *navigation=GTK_LIST_BOX(gtk_widget_get_template_child(GTK_WIDGET(window),MD_TYPE_MANAGER_WINDOW,"navigation"));
    gtk_list_box_select_row(navigation,gtk_list_box_get_row_at_index(navigation,!strcmp(name,"Access")?2:5));
    gtk_stack_set_visible_child_name(pages,name); adw_navigation_page_set_title(content,name); adw_navigation_split_view_set_show_content(split,TRUE);
}
static gboolean dialog_has_focus(GtkWindow *window,AdwDialog *dialog) {
    GtkWidget *focus=gtk_root_get_focus(GTK_ROOT(window));
    return focus && gtk_widget_is_ancestor(focus,GTK_WIDGET(dialog));
}
static gboolean presented(GtkWindow *window,GtkWidget *button) {
    if (!button || !gtk_widget_get_mapped(button)) return FALSE;
    graphene_point_t center=GRAPHENE_POINT_INIT(gtk_widget_get_width(button)/2.f,gtk_widget_get_height(button)/2.f),point;
    if (!gtk_widget_compute_point(button,GTK_WIDGET(window),&center,&point)) return FALSE;
    GtkWidget *picked=gtk_widget_pick(GTK_WIDGET(window),point.x,point.y,GTK_PICK_DEFAULT);
    return picked && (picked==button || gtk_widget_is_ancestor(picked,button));
}
static gboolean metadata_busy(GtkWindow *window) { gboolean busy; g_object_get(window,"busy",&busy,NULL); return busy; }
static void click(GtkWidget *button) { MD_CHECK(button && gtk_widget_get_sensitive(button)); if(button) g_signal_emit_by_name(button,"clicked"); }

int main(int argc,char **argv) {
    adw_init();
    /* Reduced motion makes fixture renders complete rather than capturing an
     * intermediate fade. Normal test runs retain toolkit animations. */
    if (g_getenv("MD_MANAGER_SCREENSHOT_DIR"))
        g_object_set(gtk_settings_get_default(),"gtk-enable-animations",FALSE,NULL);
    ManagerFakeNotifications notifications; notifications_start(&notifications);
    g_autoptr(AdwApplication) app=adw_application_new("org.metadesk.Manager.AccessTest",G_APPLICATION_NON_UNIQUE);
    MD_CHECK(g_application_register(G_APPLICATION(app),NULL,NULL));
    g_autofree char *directory=g_dir_make_tmp("md-w16-manager-XXXXXX",NULL);
    GtkWindow *window=md_manager_window_new(GTK_APPLICATION(app),directory); gtk_window_present(window);
    MdManagerAccess *access=md_manager_window_access(MD_MANAGER_WINDOW(window)); MdManagerHost *host=md_manager_access_host(access);
    ManagerFakeHost fake={0}; gboolean live=argc>2;
    if(!live) fake_start(&fake);
    md_manager_window_connect_host(MD_MANAGER_WINDOW(window),live?argv[1]:fake.path);
    MANAGER_WAIT(md_manager_host_ready(host)&&md_manager_host_policy(host)&&!md_manager_host_busy(host));
    MANAGER_WAIT(notifications.added>0 || (live && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(md_manager_host_snapshot(host),"requests"))==0));
    const char *action=live?argv[2]:"approve";
    const cJSON *snapshot=md_manager_host_snapshot(host);
    if(live && argc==6) {
        MD_CHECK(!strcmp(argv[3],md_manager_json_string(snapshot,"generation")));
        MD_CHECK(g_ascii_strtoull(argv[4],NULL,10)==md_manager_json_uint(snapshot,"revision"));
    }
    page(window,!strcmp(action,"kick")?"Activity":"Access");
    gboolean seen=FALSE; gtk_widget_add_tick_callback(GTK_WIDGET(window),frame,&seen,NULL); MANAGER_WAIT(seen);
    g_autofree char *live_page=g_strdup_printf("manager-live-%s.png",action);
    screenshot(window,live?live_page:"manager-access-inbox.png");
    GtkWidget *button=find_button(GTK_WIDGET(window),!strcmp(action,"kick")?"_End Session…":(!strcmp(action,"revoke")||!strcmp(action,"set"))?"_Edit Access…":"_Review…");
    click(button);
    AdwDialog *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); MD_CHECK(dialog && ADW_IS_ALERT_DIALOG(dialog));
    if (!dialog) return md_test_report();
    MANAGER_WAIT(dialog_has_focus(window,dialog));
    AdwAlertDialog *alert=ADW_ALERT_DIALOG(dialog);
    const char *response_name="once";
    if(!strcmp(action,"kick")) response_name="kick";
    else if(!strcmp(action,"revoke")) response_name="revoke";
    else if(!strcmp(action,"set")) {
        response_name="save";
        GtkCheckButton *agent=find_check(adw_alert_dialog_get_extra_child(alert),"_Accessibility tree");
        MD_CHECK(agent); gtk_check_button_set_active(agent,TRUE);
    }
    else {
        GtkWidget *extra=adw_alert_dialog_get_extra_child(alert);
        GtkCheckButton *video=find_check(extra,"_Video"), *input=find_check(extra,"_Input");
        MD_CHECK(video && input);
        MD_CHECK(!gtk_check_button_get_active(video));


        MD_CHECK(!adw_alert_dialog_get_response_enabled(alert,"once"));
        if (!live) {
            GtkCheckButton *monitor=find_check(extra,"_Monitor selection"), *clipboard=find_check(extra,"_Clipboard");
            MD_CHECK(monitor && clipboard);
            MD_CHECK(!gtk_check_button_get_active(monitor) && !gtk_check_button_get_active(clipboard));
            MD_CHECK(gtk_widget_get_sensitive(GTK_WIDGET(monitor)) && gtk_widget_get_sensitive(GTK_WIDGET(clipboard)));
            gtk_check_button_set_active(monitor,TRUE); gtk_check_button_set_active(clipboard,TRUE);
        }
        MD_CHECK(!gtk_widget_get_sensitive(GTK_WIDGET(input))); // --no-input or fixture supported mask
        if(live && argc==6) MD_CHECK(!strcmp(argv[5],md_manager_json_string(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(snapshot,"requests"),0),"request_id")));
        if(!strcmp(action,"deny")) response_name="deny";
        else gtk_check_button_set_active(video,TRUE);
        if(!strcmp(action,"remember")) gtk_check_button_set_active(find_check(extra,"_Remember this exact grant"),TRUE);
    }
    seen=FALSE; gtk_widget_add_tick_callback(GTK_WIDGET(dialog),frame,&seen,NULL); MANAGER_WAIT(seen);
    const char *response_label=adw_alert_dialog_get_response_label(alert,response_name);
    GtkWidget *response_button=find_button(GTK_WIDGET(alert),response_label);
    MANAGER_WAIT(presented(window,response_button));
    if (!strcmp(action,"approve") || !strcmp(action,"remember") || !strcmp(action,"deny")) {
        GtkWidget *extra=adw_alert_dialog_get_extra_child(alert);
        MD_CHECK(gtk_widget_grab_focus(GTK_WIDGET(find_check(extra,"_Video"))));
        MD_CHECK(gtk_widget_child_focus(extra,GTK_DIR_TAB_FORWARD));
    }
    g_autofree char *live_review=g_strdup_printf("manager-live-%s-review.png",action);
    screenshot(window,live?live_review:"manager-request-review.png");
    click(response_button);
    if(!live) {
        MANAGER_WAIT(fake.commands>=2);
        MD_CHECK(!strcmp(md_manager_json_string(fake.last,"method"),"requests.decide"));
        MD_CHECK(!strcmp(md_manager_json_string(fake.last,"request_id"),"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
        MD_CHECK(md_manager_json_uint(fake.last,"caps")==25);
        MD_CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(fake.last,"remember")));
    }
    MANAGER_WAIT(!md_manager_host_busy(host));
    MD_CHECK(md_manager_host_ready(host)); MD_CHECK(!g_str_has_prefix(md_manager_host_status(host),"Not completed"));
    if(live) {
        char *json=cJSON_PrintUnformatted(md_manager_host_snapshot(host)); puts(json); free(json);
    } else {
        // Server cancellation removes the exact inbox row, and stale notification
        // review must not open a new dialog or decide a different request.
        fake_push(&fake,fake_snapshot(2,FALSE));
        MANAGER_WAIT(md_manager_json_uint(md_manager_host_snapshot(host),"revision")==2);
        MANAGER_WAIT(adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))==NULL);
        md_manager_access_review(access,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        MD_CHECK(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
        MANAGER_WAIT(notifications.removed==1);
        MD_CHECK(notifications.added==1 && g_hash_table_size(notifications.live)==0);
        cJSON *expiring=fake_snapshot(3,TRUE);
        cJSON *request=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(expiring,"requests"),0);
        cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(request,"expires_at"),(double)time(NULL)+2);
        fake_push(&fake,expiring);
        MANAGER_WAIT(notifications.added==2);
        md_manager_access_review(access,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        AdwDialog *expired_review=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
        MD_CHECK(expired_review); GtkWidget *cancel=find_button(GTK_WIDGET(expired_review),"_Cancel");
        MANAGER_WAIT(presented(window,cancel));
        GtkCheckButton *video=find_check(adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(expired_review)),"_Video");
        gtk_check_button_set_active(video,TRUE);
        MD_CHECK(adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(expired_review),"once"));
        MANAGER_WAIT(notifications.removed==2);
        MD_CHECK(!adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(expired_review),"once"));
        MD_CHECK(!adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(expired_review),"deny"));
        gtk_check_button_set_active(video,FALSE); gtk_check_button_set_active(video,TRUE);
        MD_CHECK(!adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(expired_review),"once"));
        click(cancel);
        MANAGER_WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
        md_manager_access_review(access,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        MD_CHECK(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
        page(window,"Access");
        gtk_window_set_default_size(window,360,700);
        MANAGER_WAIT(gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(window)))==360);
        seen=FALSE; gtk_widget_add_tick_callback(GTK_WIDGET(window),frame,&seen,NULL); MANAGER_WAIT(seen);
        screenshot(window,"manager-access-narrow.png");
        click(find_button(GTK_WIDGET(window),"_Edit Access…"));
        dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); MD_CHECK(dialog);
        if(dialog) {
            MANAGER_WAIT(dialog_has_focus(window,dialog));
            GtkWidget *extra=adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(dialog));
            MD_CHECK(gtk_check_button_get_active(find_check(extra,"_Monitor selection")));
            MD_CHECK(gtk_check_button_get_active(find_check(extra,"_Clipboard")));
            gtk_check_button_set_active(find_check(extra,"_Input"),TRUE);
            GtkWidget *save=find_button(GTK_WIDGET(dialog),"_Save");
            MANAGER_WAIT(presented(window,save));
            guint before_save=fake.commands; click(save); MANAGER_WAIT(fake.commands>before_save);
            MD_CHECK(!strcmp(md_manager_json_string(fake.last,"method"),"access.set"));
            MD_CHECK(md_manager_json_uint(fake.last,"caps")==MD_HOST_ACCESS_CAPS);
            MD_CHECK(md_manager_json_uint(fake.last,"policy_version")==8);
            MANAGER_WAIT(!md_manager_host_busy(host));
            MANAGER_WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
            click(find_button(GTK_WIDGET(window),"_Edit Access…"));
            dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); MD_CHECK(dialog);
            MANAGER_WAIT(dialog_has_focus(window,dialog));
            GtkWidget *revoke=find_button(GTK_WIDGET(dialog),"_Revoke Access");
            MANAGER_WAIT(presented(window,revoke));
            click(revoke);
        }
        guint commands=fake.commands; MANAGER_WAIT(fake.commands>commands);
        MD_CHECK(!strcmp(md_manager_json_string(fake.last,"method"),"access.remove"));
        MD_CHECK(md_manager_json_uint(fake.last,"policy_version")==8);
        MANAGER_WAIT(!md_manager_host_busy(host));
        MANAGER_WAIT(!adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)));
        fake.unknown_caps=TRUE; md_manager_host_refresh_access(host);
        MANAGER_WAIT(!md_manager_host_busy(host));
        click(find_button(GTK_WIDGET(window),"_Edit Access…"));
        dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)); MD_CHECK(dialog);
        GtkWidget *cancel_unknown=find_button(GTK_WIDGET(dialog),"_Cancel");
        MANAGER_WAIT(presented(window,cancel_unknown));
        MD_CHECK(!adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(dialog),"save"));
        MD_CHECK(adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(dialog),"revoke"));
        gtk_check_button_set_active(find_check(adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(dialog)),"_Video"),TRUE);
        MD_CHECK(!adw_alert_dialog_get_response_enabled(ADW_ALERT_DIALOG(dialog),"save"));
        click(cancel_unknown);
    }
    MANAGER_WAIT(!metadata_busy(window));
    gtk_window_close(window);
    MANAGER_WAIT(gtk_application_get_windows(GTK_APPLICATION(app))==NULL);
    MANAGER_WAIT(g_hash_table_size(notifications.live)==0);
    if(!live) fake_stop(&fake);
    notifications_stop(&notifications);
    // Existing account worker owns the application until completion.
    while(g_main_context_pending(NULL)) g_main_context_iteration(NULL,FALSE);
    g_autofree char *lock=g_build_filename(directory,".management.lock",NULL); MD_CHECK(!g_unlink(lock)); MD_CHECK(!g_rmdir(directory));
    if(!live) puts("PASS: production Access controls, no implicit grants, exact approve, stale review refusal, private revoke and narrow render");
    return md_test_report();
}
