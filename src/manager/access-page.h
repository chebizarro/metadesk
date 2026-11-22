#pragma once
#include "host-client.h"
#include <adwaita.h>
#define MD_TYPE_MANAGER_ACCESS (md_manager_access_get_type())
G_DECLARE_FINAL_TYPE(MdManagerAccess, md_manager_access, MD, MANAGER_ACCESS, GObject)
MdManagerAccess *md_manager_access_new(GtkWindow *window, GtkStack *pages);
void md_manager_access_connect(MdManagerAccess *self, const char *path);
void md_manager_access_stop(MdManagerAccess *self);
/* Exact-request review only; never a notification approval action. */
void md_manager_access_review(MdManagerAccess *self, const char *generation, const char *id);
MdManagerHost *md_manager_access_host(MdManagerAccess *self);
