#pragma once
#include <gio/gio.h>

/* Per-user frontend preferences, separate from the platform account store. */
char *md_manager_config_directory(void);
char *md_manager_relay_normalize(const char *text, GError **error);
char **md_manager_relays_load(const char *directory, GError **error);
gboolean md_manager_relays_save(const char *directory, const char *const *relays,
    GError **error);
