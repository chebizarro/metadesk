#ifndef MD_TEST_INPUT_WAYLAND_SERVER_H
#define MD_TEST_INPUT_WAYLAND_SERVER_H
#include <stdbool.h>
#include <stdint.h>
typedef struct InputTestServer InputTestServer;
typedef struct {
    uint32_t x, y, width, height;
    unsigned moves, frames, keymaps, key_down, key_up, modifier_events;
    unsigned button_down[3], button_up[3];
    int scroll_x, scroll_y;
    bool saw_ctrl, saw_shift, keymap_valid;
    char text[256], release_text[256];
} InputTestEvents;
/* globals: 1 seat, 2 pointer, 4 keyboard; 8 reject keyboard, 16 disconnect on motion, 32 unresponsive peer */
InputTestServer *input_test_server_start(unsigned globals);
const char *input_test_server_path(InputTestServer *s);
InputTestEvents input_test_server_events(InputTestServer *s);
void input_test_server_stop(InputTestServer *s);
#endif
