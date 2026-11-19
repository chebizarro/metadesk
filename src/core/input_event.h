/*
 * metadesk — input_event.h
 * Wire format for raw human input forwarded from the video client to the
 * host (MD_PKT_INPUT). Deliberately tiny and endian-explicit so the client
 * (macOS) and host (Linux/Windows) agree regardless of struct padding.
 *
 * Payload layout (little-endian): an 8-byte fixed header, `text_len` bytes
 * of UTF-8 (key name for KEY, typed text for TEXT), then an 8-byte frame tag:
 *
 *   offset 0  u8   kind           (MdInputEventKind)
 *   offset 1  u8   button         (1=left 2=middle 3=right; 0 otherwise)
 *   offset 2  i16  x              MOVE/BUTTON: remote x;  WHEEL: dx
 *   offset 4  i16  y              MOVE/BUTTON: remote y;  WHEEL: dy
 *   offset 6  u16  text_len       trailing UTF-8 bytes (KEY name / TEXT)
 *   offset 8  text_len bytes of UTF-8
 *   then      u16  frame_w        decoded frame the client mapped x/y onto:
 *             u16  frame_h        its size (0x0: no frame displayed) and the
 *             u32  frame_pkt_seq  frame-channel sequence of the video packet
 *                                 it was decoded from
 *
 * The frame tag (md-383x, docs/metadesk-spec.md §9.3) lets the host
 * interpret a point against the frame the client was showing, not the one
 * it captures now. It trails the text, so a host built before the tag
 * existed ignores it. A payload without it (8 + text_len bytes, from a client
 * built before the tag) fails to unpack: the host drops it rather than
 * guess the geometry. Bytes after the tag are reserved for later fields and
 * are ignored.
 */
#ifndef MD_INPUT_EVENT_H
#define MD_INPUT_EVENT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MD_INPUT_EVENT_HDR_LEN 8u
#define MD_INPUT_EVENT_TAG_LEN 8u
#define MD_INPUT_EVENT_MAX_TEXT 256u
#define MD_INPUT_EVENT_MAX_LEN \
    (MD_INPUT_EVENT_HDR_LEN + MD_INPUT_EVENT_MAX_TEXT + MD_INPUT_EVENT_TAG_LEN)

typedef enum {
    MD_INPUT_EV_MOVE   = 1,  /* absolute mouse move to (x,y)            */
    MD_INPUT_EV_BUTTON = 2,  /* click (press+release) at (x,y), button  */
    MD_INPUT_EV_WHEEL  = 3,  /* scroll by (x=dx, y=dy)                  */
    MD_INPUT_EV_KEY    = 4,  /* single key by name (e.g. "Return")      */
    MD_INPUT_EV_TEXT   = 5,  /* type UTF-8 text                         */
} MdInputEventKind;

/* Serialize an input event into buf (must hold >= 16 + text_len bytes).
 * frame_w x frame_h and frame_pkt_seq describe the decoded frame (x, y)
 * were mapped onto.
 * Returns total bytes written, or 0 on overflow. */
static inline size_t md_input_event_pack(uint8_t *buf, size_t cap,
                                         uint8_t kind, uint8_t button,
                                         int16_t x, int16_t y,
                                         const char *text, uint16_t text_len,
                                         uint16_t frame_w, uint16_t frame_h,
                                         uint32_t frame_pkt_seq)
{
    if (text_len > MD_INPUT_EVENT_MAX_TEXT)
        return 0;
    const size_t total = (size_t)MD_INPUT_EVENT_HDR_LEN + text_len +
                         MD_INPUT_EVENT_TAG_LEN;
    if (cap < total)
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
    uint8_t *tag = buf + MD_INPUT_EVENT_HDR_LEN + text_len;
    tag[0] = (uint8_t)(frame_w & 0xFF);
    tag[1] = (uint8_t)((frame_w >> 8) & 0xFF);
    tag[2] = (uint8_t)(frame_h & 0xFF);
    tag[3] = (uint8_t)((frame_h >> 8) & 0xFF);
    for (int i = 0; i < 4; i++)
        tag[4 + i] = (uint8_t)((frame_pkt_seq >> (8 * i)) & 0xFF);
    return total;
}

/* Parse an input event from buf. Returns 0 on success and sets the out
 * params; *text points into buf (not NUL-terminated). -1 if too short,
 * including a payload without the frame tag. */
static inline int md_input_event_unpack(const uint8_t *buf, size_t len,
                                        uint8_t *kind, uint8_t *button,
                                        int16_t *x, int16_t *y,
                                        const char **text, uint16_t *text_len,
                                        uint16_t *frame_w, uint16_t *frame_h,
                                        uint32_t *frame_pkt_seq)
{
    if (len < MD_INPUT_EVENT_HDR_LEN)
        return -1;
    uint16_t tl = (uint16_t)(buf[6] | (buf[7] << 8));
    if (tl > MD_INPUT_EVENT_MAX_TEXT ||
        (size_t)MD_INPUT_EVENT_HDR_LEN + tl + MD_INPUT_EVENT_TAG_LEN > len)
        return -1;
    const uint8_t *tag = buf + MD_INPUT_EVENT_HDR_LEN + tl;
    *kind     = buf[0];
    *button   = buf[1];
    *x        = (int16_t)(buf[2] | (buf[3] << 8));
    *y        = (int16_t)(buf[4] | (buf[5] << 8));
    *text_len = tl;
    *text     = tl ? (const char *)(buf + MD_INPUT_EVENT_HDR_LEN) : NULL;
    *frame_w  = (uint16_t)(tag[0] | (tag[1] << 8));
    *frame_h  = (uint16_t)(tag[2] | (tag[3] << 8));
    *frame_pkt_seq = (uint32_t)tag[4] | ((uint32_t)tag[5] << 8) |
                 ((uint32_t)tag[6] << 16) | ((uint32_t)tag[7] << 24);
    return 0;
}

#endif /* MD_INPUT_EVENT_H */
