#pragma once
#include <adwaita.h>

/* Returns only a valid NIP-19 nprofile from an explicitly chosen image. */
char *md_manager_qr_scan(GBytes *image, GError **error);
GdkTexture *md_manager_qr_texture(const char *nprofile);
void md_manager_qr_present(GtkWidget *parent, const char *host_key);
