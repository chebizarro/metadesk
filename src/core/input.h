/*
 * metadesk — input.h
 * Platform-agnostic input injection HAL.
 * See spec §2.3.3 — platform backends selected at compile time.
 *
 * Backends:
 *   Linux:   RemoteDesktop portal, uinput, or opt-in Wayland virtual input
 *   macOS:   CGEvent / Quartz        (input_cgevent.m)
 *   Windows: SendInput               (input_sendinput.cpp)
 *
 * No platform-specific headers appear in this file.
 */
#ifndef MD_INPUT_H
#define MD_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "action.h"   /* MD_MAX_KEYS: a key combo is a list of action keys */

/* Documented fallback used only when callers cannot provide/display-detect
 * dimensions. Callers should pass the capture/display size whenever possible. */
#define MD_INPUT_FALLBACK_SCREEN_WIDTH   1920u
#define MD_INPUT_FALLBACK_SCREEN_HEIGHT  1080u
#define MD_INPUT_MIN_SCREEN_DIMENSION    64u

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward declarations ────────────────────────────────────── */

typedef struct MdInputCtx MdInputCtx;

/* ── Configuration ───────────────────────────────────────────── */

typedef struct {
    uint32_t screen_width;   /* absolute mouse coordinate range */
    uint32_t screen_height;
    /* NULL/"auto": on Linux Wayland, prefer RemoteDesktop v2+ when available
     * and initialization succeeds, otherwise try the platform backend.
     * Other sessions keep the platform backend. Explicit "portal", "uinput"
     * and "wayland" (-Dwayland_input=true) never fall back. */
    const char *backend;
    /* Wayland requires an absolute socket path. Ambient WAYLAND_DISPLAY and
     * WAYLAND_SOCKET are deliberately ignored. Strings are borrowed. */
    const char *wayland_display;
    /* Portal consent + EIS startup failure bound; 0 = 30s, max = 180s. */
    uint32_t portal_timeout_ms;
    /* Never present consent UI. RemoteDesktop cannot guarantee silent token
     * restoration, so portal input fails before opening a session. */
    bool no_interaction;
    /* Global desktop bounds of the captured output that the client's
     * frame-relative coordinates address (md_capture_get_output_rect; macOS:
     * CoreGraphics global points; Windows: physical virtual-desktop pixels,
     * the outputs' bounding rectangle for DXGI "all").
     * output_width == 0 keeps the legacy mapping: frame coordinates are
     * global, display at the origin. */
    int32_t  output_x, output_y;
    uint32_t output_width, output_height;
} MdInputConfig;

/* ── Mouse button identifiers ────────────────────────────────── */

typedef enum {
    MD_MOUSE_LEFT   = 0,
    MD_MOUSE_RIGHT  = 1,
    MD_MOUSE_MIDDLE = 2,
} MdMouseButton;

/* ── Backend vtable (spec §2.3.3) ────────────────────────────── */

typedef struct MdInputBackend {
    /* ctx->config is populated (dimensions already validated) before init. */
    int   (*init)(MdInputCtx *ctx);
    int   (*mouse_move)(MdInputCtx *ctx, int x, int y);
    int   (*mouse_button)(MdInputCtx *ctx, int button, int pressed);
    int   (*mouse_scroll)(MdInputCtx *ctx, int dx, int dy);
    int   (*key_event)(MdInputCtx *ctx, uint32_t keysym, int pressed);
    int   (*type_text)(MdInputCtx *ctx, const char *utf8);
    void  (*destroy)(MdInputCtx *ctx);
    /* Optional synchronized readiness accessor for stateful backends. */
    bool  (*is_ready)(const MdInputCtx *ctx);
} MdInputBackend;

/* ── Input context ───────────────────────────────────────────── */

struct MdInputCtx {
    const MdInputBackend *vtable;
    MdInputConfig         config;
    void                 *backend_data;  /* backend-private state */
    bool                  ready;
};

/* ── Factory ─────────────────────────────────────────────────── */

/* Create the platform-appropriate input backend.
 * Each platform's backend source file implements this function. */
const MdInputBackend *md_input_backend_create(void);
#ifdef MD_HAVE_PORTAL_INPUT
bool md_input_portal_available(void);
const MdInputBackend *md_input_portal_backend(void);
#endif
#ifdef MD_HAVE_WAYLAND_INPUT
const MdInputBackend *md_input_wayland_backend(void);
#endif

/* ── Public convenience API ──────────────────────────────────── */

/* The convenience API provides the same interface that callers used
 * previously (MdInput* → MdInputCtx*).  Higher-level operations like
 * click, double-click, and key combo are implemented here on top of
 * the vtable primitives so they work on every platform. */

/* Legacy type alias for source compatibility */
typedef MdInputCtx MdInput;

/* Create and initialise an input context with the platform backend.
 * cfg may be NULL only for legacy callers; that uses the documented
 * fallback dimensions above and logs a warning. Prefer passing actual
 * capture/display dimensions. Returns NULL on failure.
 * Linux uinput waits for the exact new device to be enabled by a local X11
 * consumer when DISPLAY is set; known Wayland sessions require portal input.
 * With no desktop environment, uinput only guarantees kernel device creation. */
MdInput *md_input_create(const MdInputConfig *cfg);

/* ── Mouse injection ─────────────────────────────────────────── */

int md_input_mouse_move(MdInput *inp, int x, int y);
int md_input_click(MdInput *inp, int x, int y, int button);
int md_input_dbl_click(MdInput *inp, int x, int y);
int md_input_right_click(MdInput *inp, int x, int y);
/* Wheel detents: positive X is right, positive Y is up (evdev convention). */
int md_input_scroll(MdInput *inp, int dx, int dy);

/* ── Keyboard injection ──────────────────────────────────────── */

/* A key combo is exactly a list of action keys — one constant, defined by
 * MdAction, reused here rather than re-declared. */
#define MD_INPUT_MAX_COMBO_KEYS MD_MAX_KEYS

/* Inject a key combo, e.g. ["ctrl", "s"].
 * Keys are pressed in order, then released in reverse. */
int md_input_key_combo(MdInput *inp, const char **keys, int key_count);

/* Type a UTF-8 string if supported by the backend. Wayland synthesizes a
 * fixed US keymap: printable ASCII, tab and newline only, at most 4096 bytes;
 * unsupported text, held keys or latched/locked modifiers fail before input.
 * Portal input preflights ASCII against the compositor's actual XKB keymap
 * and active group, and refuses held or active modifiers before typing. */
int md_input_type_text(MdInput *inp, const char *text);

/* ── Action dispatch ─────────────────────────────────────────── */

/* Click coordinates in actions are global desktop coordinates (as reported
 * by the accessibility tree), unlike md_input_mouse_move/click, which take
 * frame-relative points. With output bounds configured, the point is
 * converted to frame space and must lie on the captured output (-1 if not). */
struct MdAction;
int md_input_execute_action(MdInput *inp, const struct MdAction *action);

/* ── Frame geometry ────────────────────────────────────────────── */

/* The MdInputConfig fields that follow the captured frame. */
typedef struct {
    uint32_t screen_width, screen_height;
    int32_t  output_x, output_y;
    uint32_t output_width, output_height;
} MdInputBounds;

/* Retarget an existing context at a new frame size and output rectangle,
 * e.g. after a capture restart or display change, without recreating its
 * virtual devices. Later frame-relative points use the new mapping. Returns
 * -1 and changes nothing if the screen size is below the minimum.
 * Not synchronized: the caller must serialize it with every other call on
 * inp (the host holds the lock its packet dispatch runs under). */
int md_input_set_bounds(MdInput *inp, const MdInputBounds *bounds);

/* ── Lifecycle ───────────────────────────────────────────────── */

bool md_input_is_ready(const MdInput *inp);
void md_input_destroy(MdInput *inp);

/* ── Key name → keysym resolution (platform-agnostic) ────────── */

/* Resolve a key name string (e.g. "ctrl", "a", "f1") to a platform-
 * neutral keysym value suitable for passing to key_event().
 * Returns 0 if the name is not recognised. */
uint32_t md_input_keysym_from_name(const char *name);

/* Resolve a US-layout ASCII character (printable, tab, newline) to its
 * keysym and whether Shift is required. O(1). Returns false otherwise. */
bool md_input_ascii_key(char c, uint32_t *keysym, bool *shift);

/* ── Coordinate mapping ────────────────────────────────────────── */

/* Map a frame-relative point, clamped to the screen_width x screen_height
 * frame, into cfg's output bounds in global desktop space (scaled when the
 * frame and output sizes differ). Without output bounds the point is
 * returned unchanged and unclamped (legacy/global callers). Pure. */
void md_input_map_to_output(const MdInputConfig *cfg, int x, int y,
                            double *gx, double *gy);

/* ── Event pacing ──────────────────────────────────────────────── */

/* One pacing policy for every backend: the gap between synthetic events
 * (press/release, consecutive clicks, typed characters) so the target's
 * event loop keeps up. Not a device-readiness wait. */
#define MD_INPUT_EVENT_SETTLE_MS 5u
void md_input_settle(void);

#ifdef __cplusplus
}
#endif

#endif /* MD_INPUT_H */
