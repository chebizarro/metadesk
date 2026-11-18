/* Standalone production-HAL consumer for the private Sway input proof.
 * Compile with input.c, input_wayland.c, input_uinput.c, log.c and the two
 * scanner-generated protocol C files (see input-validation.md). */
#include "input.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080,
        .backend = "wayland", .wayland_display = argv[1] };
    MdInput *input = md_input_create(&cfg);
    if (!md_input_is_ready(input)) { md_input_destroy(input); return 3; }
    int rc = 0;
    rc |= md_input_mouse_move(input, 100, 150);
    rc |= md_input_mouse_move(input, 400, 300);
    for (int b = 0; b < 3; b++) rc |= md_input_click(input, 400, 300, b);
    rc |= md_input_scroll(input, -2, 3);
    rc |= md_input_type_text(input, "ah9-9 Wayland!\n");
    const char *combo[] = { "ctrl", "a" };
    rc |= md_input_key_combo(input, combo, 2);
    if (md_input_type_text(input, "reject\xc3\xa9") != -1) rc = -1;
    md_input_destroy(input);
    puts(rc == 0 ? "HAL input matrix sent" : "HAL input matrix FAILED");
    return rc ? 1 : 0;
}
