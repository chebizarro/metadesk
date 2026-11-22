#pragma once
#include <adwaita.h>
#include "access-page.h"
#define MD_TYPE_MANAGER_WINDOW (md_manager_window_get_type())
G_DECLARE_FINAL_TYPE(MdManagerWindow, md_manager_window, MD, MANAGER_WINDOW, AdwApplicationWindow)
GtkWindow *md_manager_window_new(GtkApplication *app, const char *directory);
MdManagerAccess *md_manager_window_access(MdManagerWindow *self);
void md_manager_window_review(MdManagerWindow *self,const char *generation,const char *id);
void md_manager_window_connect_host(MdManagerWindow *self,const char *path);
