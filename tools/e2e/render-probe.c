/* Explicit attended native SDL pixel probe. Synthetic pixels in an OWNED window
 * only; no desktop capture, input injection, relay, or retained image. */
#include "render.h"
#include "md_test.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

static void pixel(const uint8_t *pixels, int w, int x, int y, bool red) {
    const uint8_t *p = pixels + ((size_t)y * (size_t)w + (size_t)x) * 4;
    if (red) MD_CHECK(p[0] > 150 && p[1] < 100 && p[2] < 100);
    else MD_CHECK(p[0] < 10 && p[1] < 10 && p[2] < 10);
}

int main(void) {
    MdRenderer *r = md_renderer_create(600, 600, "md-l7 owned aspect probe");
    if (!r) return 1;
    uint8_t image[120 * 72 * 4];
    for (size_t i = 0; i < sizeof(image); i += 4) {
        image[i] = 220; image[i + 1] = 30; image[i + 2] = 40; image[i + 3] = 255;
    }
    MD_CHECK(md_renderer_update_frame(r, image, 120, 72) == 0);
    SDL_Window *window = md_renderer_get_sdl_window(r);
    SDL_Renderer *renderer = md_renderer_get_sdl_renderer(r);
    SDL_RendererInfo info;
    MD_CHECK(SDL_GetRendererInfo(renderer, &info) == 0);
    printf("native renderer=%s\n", info.name);
    const int sizes[][2] = { {600, 600}, {900, 360}, {600, 360} };
    for (int i = 0; i < 3; i++) {
        SDL_SetWindowSize(window, sizes[i][0], sizes[i][1]);
        MD_CHECK(md_renderer_poll_events(r) == 0);
        md_renderer_update_hidpi_scale(r);
        /* Present the resize transaction before acquiring a drawable for
         * readback; Metal may still own the pre-resize drawable until then. */
        md_renderer_begin_frame(r);
        md_renderer_end_frame(r);
        md_renderer_begin_frame(r);
        SDL_RenderSetScale(renderer, 1.0f, 1.0f);
        int w = 0, h = 0;
        MD_CHECK(SDL_GetRendererOutputSize(renderer, &w, &h) == 0);
        if (w <= 0 || h <= 0) { MD_CHECK(false); break; }
        uint8_t *pixels = malloc((size_t)w * (size_t)h * 4);
        if (!pixels) { MD_CHECK(false); break; }
        int rc = SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
        MD_CHECK(rc == 0);
        if (rc == 0) {
            pixel(pixels, w, w / 2, h / 2, true);
            if (i == 0) {
                pixel(pixels, w, w / 2, h / 20, false);
                pixel(pixels, w, w / 2, h * 19 / 20, false);
            } else if (i == 1) {
                pixel(pixels, w, w / 20, h / 2, false);
                pixel(pixels, w, w * 19 / 20, h / 2, false);
            } else {
                pixel(pixels, w, w / 20, h / 20, true);
                pixel(pixels, w, w * 19 / 20, h * 19 / 20, true);
            }
        }
        printf("window=%dx%d output=%dx%d pixel-read=%d\n", sizes[i][0], sizes[i][1], w, h, rc);
        free(pixels);
        md_renderer_end_frame(r);
    }
    md_renderer_destroy(r);
    md_renderer_close_display();
    return md_test_report();
}
