/*
 * metadesk — input.h
 * uinput virtual device creation and input injection.
 * See spec milestone 1.5.
 */
#ifndef MD_INPUT_H
#define MD_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque input context */
typedef struct MdInput MdInput;

/* Create input context with virtual mouse + keyboard devices. */
MdInput *md_input_create(void);

/* Inject a mouse click at (x, y). button: 0=left, 1=right, 2=middle. */
int md_input_click(MdInput *inp, int x, int y, int button);

/* Inject a double-click at (x, y). */
int md_input_dbl_click(MdInput *inp, int x, int y);

/* Inject mouse scroll by (dx, dy). */
int md_input_scroll(MdInput *inp, int dx, int dy);

/* Inject a key combo, e.g. ["ctrl", "s"]. */
int md_input_key_combo(MdInput *inp, const char **keys, int key_count);

/* Type a UTF-8 string. */
int md_input_type_text(MdInput *inp, const char *text);

/* Move mouse to absolute position. */
int md_input_mouse_move(MdInput *inp, int x, int y);

/* Destroy input context and release virtual devices. */
void md_input_destroy(MdInput *inp);

#ifdef __cplusplus
}
#endif

#endif /* MD_INPUT_H */
