#ifndef MD_CLIPBOARD_H
#define MD_CLIPBOARD_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MD_CLIPBOARD_MAX_UTF8 65536u
#define MD_CLIPBOARD_SEND 1u
#define MD_CLIPBOARD_RECEIVE 2u
#define MD_CLIPBOARD_BOTH 3u
/* Directions are local to the endpoint. Signaling uses the client's perspective. */
uint8_t md_clipboard_reverse(uint8_t directions);
int md_clipboard_directions_parse(const char *text);
const char *md_clipboard_directions_name(uint8_t directions);
bool md_clipboard_text_valid(const uint8_t *text, size_t size);
typedef struct {
    int (*revision)(void *ctx, uint64_t *revision);
    int (*read)(void *ctx, char **text, size_t *size); /* 1=no text, -1=error/oversize */
    int (*write)(void *ctx, const uint8_t *text, size_t size);
} MdClipboardBackend;
typedef struct MdClipboard MdClipboard;
const MdClipboardBackend *md_clipboard_backend(void); /* NULL=unsupported */
const char *md_clipboard_unavailable_reason(void);
/* No backend access (even metadata) unless the explicit grant and direction allow it. */
MdClipboard *md_clipboard_create(uint32_t granted_caps, uint8_t directions);
MdClipboard *md_clipboard_create_with_backend(uint32_t granted_caps, uint8_t directions,
    const MdClipboardBackend *backend, void *ctx);
/* 1=owned UTF-8 update; 0=unchanged/no text/direction disabled; -1=error.
 * Native revision checks run at the existing stream/UI cadence, not a relay timer. */
int md_clipboard_poll(MdClipboard *c, char **text, size_t *size);
int md_clipboard_receive(MdClipboard *c, const uint8_t *text, size_t size);
void md_clipboard_destroy(MdClipboard *c);
#ifdef __cplusplus
}
#endif
#endif
