/*
 * metadesk — input_cgevent.m
 * macOS input backend: CGEvent / Quartz Event Services (spec §2.3.3).
 *
 * Uses:
 *   - CGEventCreateMouseEvent for mouse move/button
 *   - CGEventCreateScrollWheelEvent2 for scroll
 *   - CGEventCreateKeyboardEvent for key press/release
 *   - CGEventKeyboardSetUnicodeString for type_text (Unicode-aware)
 *   - CGEventPost with kCGHIDEventTap for injection
 *
 * Requires Input Monitoring permission (System Settings >
 * Privacy & Security > Input Monitoring) for synthetic events.
 *
 * Built as Objective-C (.m) for @autoreleasepool / NSScreen.
 */
#include "input.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>  /* kVK_* virtual keycodes */
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Backend-private state ───────────────────────────────────── */

typedef struct {
    uint32_t screen_w;
    uint32_t screen_h;
    CGPoint  last_mouse;  /* track last mouse position for button events */
} CGEventState;

/* Small delay between events for system to process them. */
static void cg_delay(void) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 }; /* 5ms */
    nanosleep(&ts, NULL);
}

/* ── Linux KEY_* → macOS kVK_* translation ───────────────────── */

/* Our canonical keysym space uses Linux KEY_* scan codes (set in input.c).
 * This table maps them to macOS virtual keycodes for CGEventCreateKeyboardEvent. */

typedef struct {
    uint32_t linux_key;
    uint16_t mac_vk;
} MdKeyMapEntry;

static const MdKeyMapEntry key_map[] = {
    /* Modifiers */
    { 0x001D, kVK_Control      },  /* KEY_LEFTCTRL  */
    { 0x0061, kVK_RightControl },  /* KEY_RIGHTCTRL */
    { 0x002A, kVK_Shift        },  /* KEY_LEFTSHIFT */
    { 0x0036, kVK_RightShift   },  /* KEY_RIGHTSHIFT */
    { 0x0038, kVK_Option       },  /* KEY_LEFTALT   */
    { 0x0064, kVK_RightOption  },  /* KEY_RIGHTALT  */
    { 0x007D, kVK_Command      },  /* KEY_LEFTMETA  → Cmd on macOS */

    /* Special keys */
    { 0x001C, kVK_Return       },  /* KEY_ENTER     */
    { 0x000F, kVK_Tab          },  /* KEY_TAB       */
    { 0x0001, kVK_Escape       },  /* KEY_ESC       */
    { 0x000E, kVK_Delete       },  /* KEY_BACKSPACE */
    { 0x006F, kVK_ForwardDelete },  /* KEY_DELETE    */
    { 0x006E, kVK_Help         },  /* KEY_INSERT (no direct equiv, use Help) */
    { 0x0039, kVK_Space        },  /* KEY_SPACE     */
    { 0x003A, kVK_CapsLock     },  /* KEY_CAPSLOCK  */

    /* Navigation */
    { 0x0067, kVK_UpArrow      },  /* KEY_UP        */
    { 0x006C, kVK_DownArrow    },  /* KEY_DOWN      */
    { 0x0069, kVK_LeftArrow    },  /* KEY_LEFT      */
    { 0x006A, kVK_RightArrow   },  /* KEY_RIGHT     */
    { 0x0066, kVK_Home         },  /* KEY_HOME      */
    { 0x006B, kVK_End          },  /* KEY_END       */
    { 0x0068, kVK_PageUp       },  /* KEY_PAGEUP    */
    { 0x006D, kVK_PageDown     },  /* KEY_PAGEDOWN  */

    /* F-keys */
    { 0x003B, kVK_F1  }, { 0x003C, kVK_F2  }, { 0x003D, kVK_F3  },
    { 0x003E, kVK_F4  }, { 0x003F, kVK_F5  }, { 0x0040, kVK_F6  },
    { 0x0041, kVK_F7  }, { 0x0042, kVK_F8  }, { 0x0043, kVK_F9  },
    { 0x0044, kVK_F10 }, { 0x0057, kVK_F11 }, { 0x0058, kVK_F12 },

    /* Letters (QWERTY layout — macOS vk codes are physical-position based) */
    { 0x001E, kVK_ANSI_A }, { 0x0030, kVK_ANSI_B }, { 0x002E, kVK_ANSI_C },
    { 0x0020, kVK_ANSI_D }, { 0x0012, kVK_ANSI_E }, { 0x0021, kVK_ANSI_F },
    { 0x0022, kVK_ANSI_G }, { 0x0023, kVK_ANSI_H }, { 0x0017, kVK_ANSI_I },
    { 0x0024, kVK_ANSI_J }, { 0x0025, kVK_ANSI_K }, { 0x0026, kVK_ANSI_L },
    { 0x0032, kVK_ANSI_M }, { 0x0031, kVK_ANSI_N }, { 0x0018, kVK_ANSI_O },
    { 0x0019, kVK_ANSI_P }, { 0x0010, kVK_ANSI_Q }, { 0x0013, kVK_ANSI_R },
    { 0x001F, kVK_ANSI_S }, { 0x0014, kVK_ANSI_T }, { 0x0016, kVK_ANSI_U },
    { 0x002F, kVK_ANSI_V }, { 0x0011, kVK_ANSI_W }, { 0x002D, kVK_ANSI_X },
    { 0x0015, kVK_ANSI_Y }, { 0x002C, kVK_ANSI_Z },

    /* Digits */
    { 0x000B, kVK_ANSI_0 }, { 0x0002, kVK_ANSI_1 }, { 0x0003, kVK_ANSI_2 },
    { 0x0004, kVK_ANSI_3 }, { 0x0005, kVK_ANSI_4 }, { 0x0006, kVK_ANSI_5 },
    { 0x0007, kVK_ANSI_6 }, { 0x0008, kVK_ANSI_7 }, { 0x0009, kVK_ANSI_8 },
    { 0x000A, kVK_ANSI_9 },

    /* Punctuation */
    { 0x000C, kVK_ANSI_Minus        },  /* KEY_MINUS      */
    { 0x000D, kVK_ANSI_Equal        },  /* KEY_EQUAL      */
    { 0x001A, kVK_ANSI_LeftBracket  },  /* KEY_LEFTBRACE  */
    { 0x001B, kVK_ANSI_RightBracket },  /* KEY_RIGHTBRACE */
    { 0x0027, kVK_ANSI_Semicolon    },  /* KEY_SEMICOLON  */
    { 0x0028, kVK_ANSI_Quote        },  /* KEY_APOSTROPHE */
    { 0x0029, kVK_ANSI_Grave        },  /* KEY_GRAVE      */
    { 0x002B, kVK_ANSI_Backslash    },  /* KEY_BACKSLASH  */
    { 0x0033, kVK_ANSI_Comma        },  /* KEY_COMMA      */
    { 0x0034, kVK_ANSI_Period       },  /* KEY_DOT        */
    { 0x0035, kVK_ANSI_Slash        },  /* KEY_SLASH      */

    { 0, 0 } /* sentinel */
};

static uint16_t linux_key_to_mac_vk(uint32_t keysym) {
    for (const MdKeyMapEntry *m = key_map; m->linux_key != 0; m++) {
        if (m->linux_key == keysym)
            return m->mac_vk;
    }
    return UINT16_MAX; /* not found */
}

/* Determine CGEventFlags for modifier keysyms */
static CGEventFlags modifier_flag_for_keysym(uint32_t keysym) {
    switch (keysym) {
    case 0x002A: case 0x0036: return kCGEventFlagMaskShift;     /* shift */
    case 0x001D: case 0x0061: return kCGEventFlagMaskControl;   /* ctrl */
    case 0x0038: case 0x0064: return kCGEventFlagMaskAlternate; /* alt/option */
    case 0x007D:              return kCGEventFlagMaskCommand;    /* meta/cmd */
    default:                  return 0;
    }
}

/* ── Vtable implementation ───────────────────────────────────── */

static int cg_init(MdInputCtx *ctx, const MdInputConfig *cfg) {
    CGEventState *st = calloc(1, sizeof(CGEventState));
    if (!st) return -1;

    st->screen_w = (cfg && cfg->screen_width  > 0) ? cfg->screen_width  : 1920;
    st->screen_h = (cfg && cfg->screen_height > 0) ? cfg->screen_height : 1080;
    st->last_mouse = CGPointMake(0, 0);

    ctx->backend_data = st;

    /* Test if we can post events (requires Input Monitoring permission).
     * Create a null event to check — if this fails, events will be silently dropped. */
    CGEventRef test = CGEventCreate(NULL);
    if (test) {
        /* Read current mouse position */
        st->last_mouse = CGEventGetLocation(test);
        CFRelease(test);
        ctx->ready = true;
    } else {
        fprintf(stderr, "input_cgevent: WARNING — cannot create CGEvent. "
                "Check Input Monitoring permission.\n");
        ctx->ready = false;
    }

    return 0;
}

static int cg_mouse_move(MdInputCtx *ctx, int x, int y) {
    CGEventState *st = ctx->backend_data;
    if (!st) return -1;

    CGPoint point = CGPointMake((CGFloat)x, (CGFloat)y);
    st->last_mouse = point;

    CGEventRef event = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved,
                                                point, kCGMouseButtonLeft);
    if (!event) return -1;

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return 0;
}

static int cg_mouse_button(MdInputCtx *ctx, int button, int pressed) {
    CGEventState *st = ctx->backend_data;
    if (!st) return -1;

    CGEventType type;
    CGMouseButton cgButton;

    switch (button) {
    case 0: /* left */
        cgButton = kCGMouseButtonLeft;
        type = pressed ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        break;
    case 1: /* right */
        cgButton = kCGMouseButtonRight;
        type = pressed ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        break;
    case 2: /* middle */
        cgButton = kCGMouseButtonCenter;
        type = pressed ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        break;
    default:
        return -1;
    }

    CGEventRef event = CGEventCreateMouseEvent(NULL, type, st->last_mouse, cgButton);
    if (!event) return -1;

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return 0;
}

static int cg_mouse_scroll(MdInputCtx *ctx, int dx, int dy) {
    (void)ctx;

    /* CGEventCreateScrollWheelEvent2 with pixel units.
     * Note: macOS scroll is inverted from Linux convention —
     * positive dy = scroll up (content moves down). We match the
     * caller's convention: positive dy = scroll up. */
    CGEventRef event = CGEventCreateScrollWheelEvent(NULL,
        kCGScrollEventUnitPixel, 2, (int32_t)dy, (int32_t)dx);
    if (!event) return -1;

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return 0;
}

static int cg_key_event(MdInputCtx *ctx, uint32_t keysym, int pressed) {
    (void)ctx;

    uint16_t vk = linux_key_to_mac_vk(keysym);
    if (vk == UINT16_MAX) {
        fprintf(stderr, "input_cgevent: unknown keysym 0x%04x\n", keysym);
        return -1;
    }

    CGEventRef event = CGEventCreateKeyboardEvent(NULL, vk, pressed ? true : false);
    if (!event) return -1;

    /* If this is a regular key and modifiers are held, we need to set
     * the modifier flags on the event. However, the system tracks modifier
     * state from our prior CGEventPost calls, so just posting works.
     * For explicit modifier keys, set the flag on the event. */
    CGEventFlags flag = modifier_flag_for_keysym(keysym);
    if (flag != 0) {
        CGEventFlags current = CGEventGetFlags(event);
        if (pressed)
            CGEventSetFlags(event, current | flag);
        else
            CGEventSetFlags(event, current & ~flag);
    }

    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return 0;
}

static int cg_type_text(MdInputCtx *ctx, const char *utf8) {
    (void)ctx;
    if (!utf8) return -1;

    @autoreleasepool {
        NSString *nsStr = [NSString stringWithUTF8String:utf8];
        if (!nsStr) return -1;

        /* Type each character via CGEventKeyboardSetUnicodeString.
         * This correctly handles Unicode, accented characters, emoji, etc.
         * We use a dummy keycode (0) — the Unicode string overrides it. */
        for (NSUInteger i = 0; i < nsStr.length; i++) {
            unichar ch = [nsStr characterAtIndex:i];

            /* Key down */
            CGEventRef down = CGEventCreateKeyboardEvent(NULL, 0, true);
            if (!down) continue;
            CGEventKeyboardSetUnicodeString(down, 1, &ch);
            CGEventPost(kCGHIDEventTap, down);
            CFRelease(down);

            /* Key up */
            CGEventRef up = CGEventCreateKeyboardEvent(NULL, 0, false);
            if (!up) continue;
            CGEventKeyboardSetUnicodeString(up, 1, &ch);
            CGEventPost(kCGHIDEventTap, up);
            CFRelease(up);

            cg_delay();
        }
    }

    return 0;
}

static void cg_destroy(MdInputCtx *ctx) {
    CGEventState *st = ctx->backend_data;
    if (!st) return;
    free(st);
    ctx->backend_data = NULL;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdInputBackend cgevent_backend = {
    .init         = cg_init,
    .mouse_move   = cg_mouse_move,
    .mouse_button = cg_mouse_button,
    .mouse_scroll = cg_mouse_scroll,
    .key_event    = cg_key_event,
    .type_text    = cg_type_text,
    .destroy      = cg_destroy,
};

const MdInputBackend *md_input_backend_create(void) {
    return &cgevent_backend;
}
