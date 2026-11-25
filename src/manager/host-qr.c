#include "host-qr.h"
#include "identity_resolver.h"
#include <stdlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <nostr/nip19/nip19.h>
#include <qrencode.h>
#include <zbar.h>
#include <string.h>

GdkTexture *md_manager_qr_texture(const char *profile) {
    QRcode *code = profile ? QRcode_encodeString(profile, 0, QR_ECLEVEL_M, QR_MODE_8, 1) : NULL;
    if (!code) return NULL;
    const int quiet = 4;
    const int scale = 4;
    int side = (code->width + 2 * quiet) * scale;
    gsize stride = 3u * (gsize)side;
    gsize size = stride * (gsize)side;
    guchar *pixels = g_malloc(size);
    memset(pixels, 255, size);
    for (int y = 0; y < code->width; ++y)
        for (int x = 0; x < code->width; ++x)
            if (code->data[y * code->width + x] & 1)
                for (int row = 0; row < scale; ++row)
                    memset(pixels + (gsize)((y + quiet) * scale + row) * stride + (x + quiet) * scale * 3,
                           0, scale * 3);
    QRcode_free(code);
    g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, size);
    return gdk_memory_texture_new(side, side, GDK_MEMORY_R8G8B8, bytes, stride);
}

char *md_manager_qr_scan(GBytes *bytes, GError **error) {
    g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(bytes);
    g_autoptr(GdkPixbuf) rgb = gdk_pixbuf_new_from_stream_at_scale(stream, 2048, 2048, TRUE, NULL, error);
    if (!rgb) return NULL;
    int w = gdk_pixbuf_get_width(rgb), h = gdk_pixbuf_get_height(rgb), nc = gdk_pixbuf_get_n_channels(rgb);
    gsize len = (gsize)w * h;
    g_autofree guchar *gray = g_malloc(len);
    const guchar *pixels = gdk_pixbuf_read_pixels(rgb);
    int stride = gdk_pixbuf_get_rowstride(rgb);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const guchar *p = pixels + (gsize)y * stride + (gsize)x * nc;
        gray[(gsize)y * w + x] = (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
    }
    zbar_image_scanner_t *scanner = zbar_image_scanner_create();
    zbar_image_t *image = zbar_image_create();
    char *profile = NULL;
    if (scanner && image) {
        zbar_image_scanner_set_config(scanner, ZBAR_NONE, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);
        zbar_image_set_format(image, zbar_fourcc('Y', '8', '0', '0'));
        zbar_image_set_size(image, (unsigned)w, (unsigned)h);
        zbar_image_set_data(image, gray, len, NULL);
        if (zbar_scan_image(scanner, image) > 0) {
            for (const zbar_symbol_t *s = zbar_image_first_symbol(image); s; s = zbar_symbol_next(s)) {
                const char *value = zbar_symbol_get_data(s);
                unsigned n = zbar_symbol_get_data_length(s);
                if (!value || n > 8192 || strlen(value) != n) continue;
                if (g_str_has_prefix(value, "nostr:")) value += 6;
                NostrProfilePointer *decoded = NULL;
                if (!g_str_has_prefix(value, "nprofile1") || nostr_nip19_decode_nprofile(value, &decoded) != 0 || !decoded) continue;
                nostr_profile_pointer_free(decoded);
                if (profile && strcmp(profile, value)) { g_clear_pointer(&profile, g_free); break; }
                if (!profile) profile = g_strdup(value);
            }
        }
    }
    if (image) zbar_image_destroy(image);
    if (scanner) zbar_image_scanner_destroy(scanner);
    if (!profile) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The image must contain one nprofile QR code");
    return profile;
}

void md_manager_qr_present(GtkWidget *parent, const char *host_key) {
    char fp[MD_KEY_FINGERPRINT_SIZE];
    if (!md_key_fingerprint(host_key, fp)) return;
    NostrProfilePointer ptr = {.public_key = (char *)host_key};
    char *raw = NULL;
    if (nostr_nip19_encode_nprofile(&ptr, &raw) || !raw) return;
    g_autofree char *profile = g_strdup(raw);
    free(raw);
    g_autoptr(GdkTexture) qr = md_manager_qr_texture(profile);
    if (!qr) return;
    g_autofree char *body = g_strdup_printf("Scan this code in Metadesk on another device.\nFingerprint: %s", fp);
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Host QR Code", body));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *image = gtk_picture_new_for_paintable(GDK_PAINTABLE(qr));
    gtk_picture_set_alternative_text(GTK_PICTURE(image), "QR code for this computer’s public host identity");
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), image);
    GtkWidget *label = gtk_label_new(profile);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_box_append(GTK_BOX(box), label);
    adw_alert_dialog_set_extra_child(dialog, box);
    adw_alert_dialog_add_response(dialog, "close", "_Close");
    adw_alert_dialog_set_close_response(dialog, "close");
    adw_alert_dialog_set_default_response(dialog, "close");
    adw_dialog_present(ADW_DIALOG(dialog), parent);
}
