#pragma once
#include <adwaita.h>
#define MD_TYPE_MANAGER_WINDOW (md_manager_window_get_type())
G_DECLARE_FINAL_TYPE(MdManagerWindow, md_manager_window, MD, MANAGER_WINDOW, AdwApplicationWindow)
GtkWindow *md_manager_window_new(GtkApplication *app, const char *directory);
