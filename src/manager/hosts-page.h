#pragma once
#include <adwaita.h>

GtkWidget *md_manager_hosts_page_new(void);
void md_manager_hosts_configure(GtkWidget *, const char *directory, const char *client_key,
    const char *const *bootstrap);
void md_manager_hosts_stop(GtkWidget *);
