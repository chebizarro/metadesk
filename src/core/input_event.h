/*
 * metadesk — input_event.h
 * Wire format for raw human input forwarded from the video client to the
 * host (MD_PKT_INPUT). Deliberately tiny and endian-explicit so the client
 * (macOS) and host (Linux/Windows) agree regardless of struct padding.
 *
 * Payload layout (little-endian), 8-byte fixed header optionally followed
 * by `text_len` bytes of UTF-8 (key name for KEY, typed text for TEXT):
 *
 *   offset 0  u8   kind       (MdInputEventKind)
 *   offset 1  u8   button     (1=left 2=middle 3=right; 0 otherwise)
 *   offset 2  i16  x          MOVE/BUTTON: remote x;  WHEEL: dx
 *   offset 4  i16  y          MOVE/BUTTON: remote y;  WHEEL: dy
 *   offset 6  u16  text_len   trailing UTF-8 bytes (KEY name / TEXT)
 */
#ifndef MD_INPUT_EVENT_H
#define MD_INPUT_EVENT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MD_INPUT_EVENT_HDR_LEN 8u
#define MD_INPUT_EVENT_MAX_TEXT 256u

typedef enum {
    MD_INPUT_EV_MOVE   = 1,  /* absolute mouse move to (x,y)            */
    MD_INPUT_EV_BUTTON = 2,  /* click (press+release) at (x,y), button  */
    MD_INPUT_EV_WHEEL  = 3,  /* scroll by (x=dx, y=dy)                  */
    MD_INPUT_EV_KEY    = 4,  /* single key by name (e.g. "Return")      */
    MD_INPUT_EV_TEXT   = 5,  /* type UTF-8 text                         */
} MdInputEventKind;

/* Serialize an input event into buf (must hold >= 8 + text_len bytes).
 * Returns total bytes written, or 0 on overflow. */
static inline size_t md_input_event_pack(uint8_t *buf, size_t cap,
                                         uint8_t kind, uint8_t button,
                                         int16_t x, int16_t y,
                                         const char *text, uint16_t text_len)
{
    if (text_len > MD_INPUT_EVENT_MAX_TEXT)
        return 0;
    if (cap < (size_t)MD_INPUT_EVENT_HDR_LEN + text_len)
        return 0;
    buf[0] = kind;
    buf[1] = button;
    buf[2] = (uint8_t)(x & 0xFF);
    buf[3] = (uint8_t)((x >> 8) & 0xFF);
    buf[4] = (uint8_t)(y & 0xFF);
    buf[5] = (uint8_t)((y >> 8) & 0xFF);
    buf[6] = (uint8_t)(text_len & 0xFF);
    buf[7] = (uint8_t)((text_len >> 8) & 0xFF);
    if (text_len && text)
        memcpy(buf + MD_INPUT_EVENT_HDR_LEN, text, text_len);
    return (size_t)MD_INPUT_EVENT_HDR_LEN + text_len;
}

/* Parse an input event header from buf. Returns 0 on success and sets the
 * out params; *text points into buf (not NUL-terminated). -1 if too short. */
static inline int md_input_event_unpack(const uint8_t *buf, size_t len,
                                        uint8_t *kind, uint8_t *button,
                                        int16_t *x, int16_t *y,
                                        const char **text, uint16_t *text_len)
{
    if (len < MD_INPUT_EVENT_HDR_LEN)
        return -1;
    uint16_t tl = (uint16_t)(buf[6] | (buf[7] << 8));
    if ((size_t)MD_INPUT_EVENT_HDR_LEN + tl > len || tl > MD_INPUT_EVENT_MAX_TEXT)
        return -1;
    *kind     = buf[0];
    *button   = buf[1];
    *x        = (int16_t)(buf[2] | (buf[3] << 8));
    *y        = (int16_t)(buf[4] | (buf[5] << 8));
    *text_len = tl;
    *text     = tl ? (const char *)(buf + MD_INPUT_EVENT_HDR_LEN) : NULL;
    return 0;
}

#endif /* MD_INPUT_EVENT_H */
