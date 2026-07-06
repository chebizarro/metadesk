/*
 * metadesk — input.h
 * Platform-agnostic input injection HAL.
 * See spec §2.3.3 — platform backends selected at compile time.
 *
 * Backends:
 *   Linux:   uinput virtual devices  (input_uinput.c)
 *   macOS:   CGEvent / Quartz        (input_cgevent.m)
 *   Windows: SendInput               (input_sendinput.cpp)
 *
 * No platform-specific headers appear in this file.
 */
#ifndef MD_INPUT_H
#define MD_INPUT_H

#include <stdint.h>
#include <stdbool.h>

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
} MdInputConfig;

/* ── Mouse button identifiers ────────────────────────────────── */

typedef enum {
    MD_MOUSE_LEFT   = 0,
    MD_MOUSE_RIGHT  = 1,
    MD_MOUSE_MIDDLE = 2,
} MdMouseButton;

/* ── Backend vtable (spec §2.3.3) ────────────────────────────── */

typedef struct MdInputBackend {
    int   (*init)(MdInputCtx *ctx, const MdInputConfig *cfg);
    int   (*mouse_move)(MdInputCtx *ctx, int x, int y);
    int   (*mouse_button)(MdInputCtx *ctx, int button, int pressed);
    int   (*mouse_scroll)(MdInputCtx *ctx, int dx, int dy);
    int   (*key_event)(MdInputCtx *ctx, uint32_t keysym, int pressed);
    int   (*type_text)(MdInputCtx *ctx, const char *utf8);
    void  (*destroy)(MdInputCtx *ctx);
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
 * capture/display dimensions. Returns NULL on failure. */
MdInput *md_input_create(const MdInputConfig *cfg);

/* ── Mouse injection ─────────────────────────────────────────── */

int md_input_mouse_move(MdInput *inp, int x, int y);
int md_input_click(MdInput *inp, int x, int y, int button);
int md_input_dbl_click(MdInput *inp, int x, int y);
int md_input_right_click(MdInput *inp, int x, int y);
int md_input_scroll(MdInput *inp, int dx, int dy);

/* ── Keyboard injection ──────────────────────────────────────── */

/* Maximum keys in a combo */
#define MD_INPUT_MAX_COMBO_KEYS 8

/* Inject a key combo, e.g. ["ctrl", "s"].
 * Keys are pressed in order, then released in reverse. */
int md_input_key_combo(MdInput *inp, const char **keys, int key_count);

/* Type a UTF-8 string. */
int md_input_type_text(MdInput *inp, const char *text);

/* ── Action dispatch ─────────────────────────────────────────── */

struct MdAction;
int md_input_execute_action(MdInput *inp, const struct MdAction *action);

/* ── Lifecycle ───────────────────────────────────────────────── */

bool md_input_is_ready(const MdInput *inp);
void md_input_destroy(MdInput *inp);

/* ── Key name → keysym resolution (platform-agnostic) ────────── */

/* Resolve a key name string (e.g. "ctrl", "a", "f1") to a platform-
 * neutral keysym value suitable for passing to key_event().
 * Returns 0 if the name is not recognised. */
uint32_t md_input_keysym_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MD_INPUT_H */
