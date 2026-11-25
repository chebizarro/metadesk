#include "window.h"
#include "identity_resolver.h"
#include "nostr.h"

static void activate(GtkApplication *app, gpointer data) {
    (void)data;
    GtkWindow *window = gtk_application_get_active_window(app);
    if (!window) window = md_manager_window_new(app, NULL);
    gtk_window_present(window);
}
static void close_window(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)action; (void)parameter;
    GtkWindow *window = gtk_application_get_active_window(data);
    if (window) gtk_window_close(window);
}
static void review_request(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)action;
    GtkWindow *window=gtk_application_get_active_window(data);
    if (!window) { activate(GTK_APPLICATION(data),NULL); window=gtk_application_get_active_window(data); }
    const char *generation, *id; g_variant_get(parameter,"(&s&s)",&generation,&id);
    if (window) md_manager_window_review(MD_MANAGER_WINDOW(window),generation,id);
}
static int command_line(GApplication *app,GApplicationCommandLine *line,gpointer data) {
    (void)data; activate(GTK_APPLICATION(app),NULL);
    const char *path=NULL;
    if (g_variant_dict_lookup(g_application_command_line_get_options_dict(line),"management-socket","&s",&path)) {
        GtkWindow *window=gtk_application_get_active_window(GTK_APPLICATION(app));
        md_manager_window_connect_host(MD_MANAGER_WINDOW(window),path);
    }
    return 0;
}
int main(int argc, char **argv) {
    md_identity_scrub_environment();
    g_autoptr(AdwApplication) app = adw_application_new("org.metadesk.Manager", G_APPLICATION_HANDLES_COMMAND_LINE);
    const GActionEntry actions[] = {{.name = "close", .activate = close_window},
        {.name="review-request", .activate=review_request, .parameter_type="(ss)"}};
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), app);
    const char *accels[] = {"<Primary>w", "<Primary>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.close", accels);
    g_application_add_main_option(G_APPLICATION(app),"management-socket",0,0,G_OPTION_ARG_STRING,"Connect to a running local host’s private management socket","PATH");
    g_signal_connect(app,"command-line",G_CALLBACK(command_line),NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_clear_object(&app);
    md_nostr_shutdown();
    return status;
}
