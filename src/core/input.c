/*
 * metadesk — input.c
 * uinput injection. Stub — implementation in M1.5.
 */
#include "input.h"
#include <stdlib.h>

struct MdInput {
    int kbd_fd;
    int mouse_fd;
};

MdInput *md_input_create(void) {
    MdInput *inp = calloc(1, sizeof(MdInput));
    if (inp) {
        inp->kbd_fd = -1;
        inp->mouse_fd = -1;
    }
    /* TODO: create uinput virtual devices (M1.5) */
    return inp;
}

int md_input_click(MdInput *inp, int x, int y, int button) {
    if (!inp) return -1;
    (void)x; (void)y; (void)button;
    return 0;
}

int md_input_dbl_click(MdInput *inp, int x, int y) {
    if (!inp) return -1;
    (void)x; (void)y;
    return 0;
}

int md_input_scroll(MdInput *inp, int dx, int dy) {
    if (!inp) return -1;
    (void)dx; (void)dy;
    return 0;
}

int md_input_key_combo(MdInput *inp, const char **keys, int key_count) {
    if (!inp) return -1;
    (void)keys; (void)key_count;
    return 0;
}

int md_input_type_text(MdInput *inp, const char *text) {
    if (!inp) return -1;
    (void)text;
    return 0;
}

int md_input_mouse_move(MdInput *inp, int x, int y) {
    if (!inp) return -1;
    (void)x; (void)y;
    return 0;
}

void md_input_destroy(MdInput *inp) {
    if (!inp) return;
    /* TODO: close uinput fds (M1.5) */
    free(inp);
}
