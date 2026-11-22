#include "window.h"

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
int main(int argc, char **argv) {
    g_autoptr(AdwApplication) app = adw_application_new("org.metadesk.Manager", G_APPLICATION_DEFAULT_FLAGS);
    const GActionEntry actions[] = {{.name = "close", .activate = close_window}};
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), app);
    const char *accels[] = {"<Primary>w", "<Primary>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.close", accels);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
