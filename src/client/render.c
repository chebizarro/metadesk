/*
 * metadesk — render.c
 * SDL2 frame display for the human video client.
 *
 * Architecture:
 *   - One SDL_Window with SDL_RENDERER_ACCELERATED
 *   - One SDL_Texture in STREAMING mode (RGBA, updated per frame)
 *   - vsync disabled for lowest latency (spec §9.1: <2ms present)
 *   - Texture is recreated on resolution change
 *   - Window is resizable; frame is scaled to fit
 *
 * The renderer exposes SDL handles for ImGui integration (M1.8).
 * ImGui will render on top of the video frame texture.
 */
#include "render.h"

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <stdio.h>

struct MdRenderer {
    SDL_Window   *window;
    SDL_Renderer *sdl_renderer;
    SDL_Texture  *texture;

    uint32_t      tex_width;     /* current texture dimensions     */
    uint32_t      tex_height;
    uint32_t      win_width;     /* current window dimensions      */
    uint32_t      win_height;
    uint32_t      host_width;    /* host screen dimensions for scaling */
    uint32_t      host_height;
    bool          open;          /* false after window close event  */
    bool          sdl_inited;    /* true if we called SDL_Init      */

    MdInputCallback input_cb;    /* keyboard/mouse event callback   */
    void           *input_userdata;
};

/* ── Internal helpers ────────────────────────────────────────── */

/* (Re)create the streaming texture to match frame dimensions. */
static int ensure_texture(MdRenderer *r, uint32_t width, uint32_t height) {
    if (r->texture && r->tex_width == width && r->tex_height == height)
        return 0;

    if (r->texture) {
        SDL_DestroyTexture(r->texture);
        r->texture = NULL;
    }

    /* SDL uses ABGR8888 for RGBA byte order on little-endian.
     * Our decoded frames are RGBA (R in byte 0), which maps to
     * SDL_PIXELFORMAT_ABGR8888 on LE or SDL_PIXELFORMAT_RGBA32. */
    r->texture = SDL_CreateTexture(r->sdl_renderer,
                                   SDL_PIXELFORMAT_ABGR8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   (int)width, (int)height);
    if (!r->texture) {
        fprintf(stderr, "render: failed to create %ux%u texture: %s\n",
                width, height, SDL_GetError());
        return -1;
    }

    r->tex_width  = width;
    r->tex_height = height;

    /* Resize window to match stream resolution (first frame or resolution change) */
    SDL_SetWindowSize(r->window, (int)width, (int)height);
    r->win_width  = width;
    r->win_height = height;

    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

MdRenderer *md_renderer_create(uint32_t width, uint32_t height, const char *title) {
    MdRenderer *r = calloc(1, sizeof(MdRenderer));
    if (!r) return NULL;

    /* Initialize SDL video subsystem (idempotent if already done) */
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            fprintf(stderr, "render: SDL_Init failed: %s\n", SDL_GetError());
            free(r);
            return NULL;
        }
        r->sdl_inited = true;
    }

    /* Create window — resizable, initially at the given dimensions */
    r->window = SDL_CreateWindow(
        title ? title : "metadesk",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)width, (int)height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!r->window) {
        fprintf(stderr, "render: SDL_CreateWindow failed: %s\n", SDL_GetError());
        if (r->sdl_inited) SDL_Quit();
        free(r);
        return NULL;
    }

    /* Create accelerated renderer, vsync disabled for low latency.
     * SDL_RENDERER_PRESENTVSYNC is intentionally NOT set (spec §9.1). */
    r->sdl_renderer = SDL_CreateRenderer(r->window, -1,
                                          SDL_RENDERER_ACCELERATED);
    if (!r->sdl_renderer) {
        /* Fall back to software renderer */
        r->sdl_renderer = SDL_CreateRenderer(r->window, -1,
                                              SDL_RENDERER_SOFTWARE);
    }
    if (!r->sdl_renderer) {
        fprintf(stderr, "render: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(r->window);
        if (r->sdl_inited) SDL_Quit();
        free(r);
        return NULL;
    }

    /* Set scaling quality to linear for smooth resize */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    r->win_width  = width;
    r->win_height = height;
    r->open = true;

    return r;
}

int md_renderer_present(MdRenderer *r, const uint8_t *rgba,
                        uint32_t width, uint32_t height) {
    if (!r || !r->open || !rgba || width == 0 || height == 0)
        return -1;

    /* Ensure texture matches frame dimensions */
    if (ensure_texture(r, width, height) < 0)
        return -1;

    /* Upload RGBA pixel data to the streaming texture.
     * SDL_UpdateTexture copies from our buffer to the GPU texture. */
    int stride = (int)(width * 4);
    if (SDL_UpdateTexture(r->texture, NULL, rgba, stride) < 0) {
        fprintf(stderr, "render: SDL_UpdateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Clear, copy texture (scaled to window), present */
    SDL_RenderClear(r->sdl_renderer);
    SDL_RenderCopy(r->sdl_renderer, r->texture, NULL, NULL);
    SDL_RenderPresent(r->sdl_renderer);

    return 0;
}

int md_renderer_poll_events(MdRenderer *r) {
    if (!r || !r->open)
        return -1;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            r->open = false;
            return -1;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (event.key.keysym.sym == SDLK_ESCAPE && event.type == SDL_KEYDOWN) {
                r->open = false;
                return -1;
            }
            if (r->input_cb) {
                r->input_cb(MD_INPUT_KEY,
                            event.key.keysym.scancode,
                            event.type == SDL_KEYDOWN ? 1 : 0,
                            r->input_userdata);
            }
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                r->open = false;
                return -1;
            }
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                r->win_width  = (uint32_t)event.window.data1;
                r->win_height = (uint32_t)event.window.data2;
            }
            break;

        case SDL_MOUSEMOTION:
            if (r->input_cb) {
                int mx = event.motion.x;
                int my = event.motion.y;
                /* Scale from client window to host screen coordinates */
                if (r->host_width && r->host_height &&
                    r->win_width && r->win_height) {
                    mx = (int)((int64_t)mx * r->host_width / r->win_width);
                    my = (int)((int64_t)my * r->host_height / r->win_height);
                }
                r->input_cb(MD_INPUT_MOUSE_MOVE, mx, my,
                            r->input_userdata);
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (r->input_cb) {
                r->input_cb(MD_INPUT_MOUSE_BUTTON,
                            event.button.button,
                            event.type == SDL_MOUSEBUTTONDOWN ? 1 : 0,
                            r->input_userdata);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (r->input_cb) {
                r->input_cb(MD_INPUT_SCROLL,
                            event.wheel.x, event.wheel.y,
                            r->input_userdata);
            }
            break;

        default:
            break;
        }
    }

    return 0;
}

int md_renderer_get_window_size(const MdRenderer *r, uint32_t *w, uint32_t *h) {
    if (!r || !w || !h) return -1;
    *w = r->win_width;
    *h = r->win_height;
    return 0;
}

void *md_renderer_get_sdl_window(MdRenderer *r) {
    return r ? r->window : NULL;
}

void *md_renderer_get_sdl_renderer(MdRenderer *r) {
    return r ? r->sdl_renderer : NULL;
}

bool md_renderer_is_open(const MdRenderer *r) {
    return r ? r->open : false;
}

void md_renderer_set_input_callback(MdRenderer *r, MdInputCallback cb, void *userdata) {
    if (!r) return;
    r->input_cb = cb;
    r->input_userdata = userdata;
}

void md_renderer_set_host_size(MdRenderer *r, uint32_t host_w, uint32_t host_h) {
    if (!r) return;
    r->host_width = host_w;
    r->host_height = host_h;
}

void md_renderer_destroy(MdRenderer *r) {
    if (!r) return;

    if (r->texture)
        SDL_DestroyTexture(r->texture);
    if (r->sdl_renderer)
        SDL_DestroyRenderer(r->sdl_renderer);
    if (r->window)
        SDL_DestroyWindow(r->window);
    if (r->sdl_inited)
        SDL_Quit();

    free(r);
}
