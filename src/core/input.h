/*
 * metadesk — input.h
 * uinput virtual device creation and input injection.
 *
 * Creates two virtual input devices via Linux uinput:
 *   1. Virtual keyboard — supports all standard keysyms
 *   2. Virtual mouse — absolute positioning, buttons, scroll
 *
 * The host uses these to inject actions received from remote clients
 * (both human clients forwarding keyboard/mouse and agent clients
 * sending structured MdAction commands).
 *
 * Requires /dev/uinput access (typically root or input group).
 *
 * See spec milestone 1.5.
 */
#ifndef MD_INPUT_H
#define MD_INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque input context */
typedef struct MdInput MdInput;

/* Configuration for virtual devices */
typedef struct {
    uint32_t screen_width;   /* absolute mouse coordinate range */
    uint32_t screen_height;
} MdInputConfig;

/* Create input context with virtual mouse + keyboard devices.
 * cfg: screen dimensions for absolute mouse positioning.
 *      If NULL, defaults to 1920x1080.
 * Returns NULL on failure (e.g. no /dev/uinput access). */
MdInput *md_input_create(const MdInputConfig *cfg);

/* ── Mouse injection ─────────────────────────────────────────── */

/* Move mouse to absolute position (x, y). */
int md_input_mouse_move(MdInput *inp, int x, int y);

/* Inject a mouse click at (x, y). button: 0=left, 1=right, 2=middle. */
int md_input_click(MdInput *inp, int x, int y, int button);

/* Inject a double-click at (x, y). */
int md_input_dbl_click(MdInput *inp, int x, int y);

/* Inject a right-click at (x, y). */
int md_input_right_click(MdInput *inp, int x, int y);

/* Inject mouse scroll by (dx, dy). Positive dy = scroll up. */
int md_input_scroll(MdInput *inp, int dx, int dy);

/* ── Keyboard injection ──────────────────────────────────────── */

/* Inject a key combo, e.g. ["ctrl", "s"].
 * Keys are pressed in order, then released in reverse order.
 * Key names match X11 keysym names (lowercase). */
int md_input_key_combo(MdInput *inp, const char **keys, int key_count);

/* Type a UTF-8 string character by character.
 * Each character is mapped to the appropriate keysym + shift state. */
int md_input_type_text(MdInput *inp, const char *text);

/* ── Action dispatch ─────────────────────────────────────────── */

/* Forward declaration — defined in action.h */
struct MdAction;

/* Execute an MdAction by dispatching to the appropriate injection method.
 * This is the main entry point for the host's action handler.
 * Returns 0 on success, -1 on error. */
int md_input_execute_action(MdInput *inp, const struct MdAction *action);

/* ── Lifecycle ───────────────────────────────────────────────── */

/* Check if virtual devices are operational. */
bool md_input_is_ready(const MdInput *inp);

/* Destroy input context and release virtual devices. */
void md_input_destroy(MdInput *inp);

#ifdef __cplusplus
}
#endif

#endif /* MD_INPUT_H */
