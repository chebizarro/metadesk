/*
 * metadesk — render.c
 * SDL2 frame display for the human video client.
 *
 * Architecture:
 *   - One SDL_Window; accelerated SDL renderer chosen by a probed plan
 *     (md-rnc), software last
 *   - One SDL_Texture in STREAMING mode (RGBA, updated per frame)
 *   - vsync disabled for lowest latency (spec §9.1: <2ms present)
 *   - Texture is recreated on resolution change
 *   - Window is resizable; frame is scaled to fit
 *
 * The renderer exposes SDL handles for ImGui integration (M1.8).
 * ImGui will render on top of the video frame texture.
 */
#include "render.h"
#include "video_geometry.h"

#include <SDL2/SDL.h>

#include <stdlib.h>
#include <stdio.h>

struct MdRenderer {
    SDL_Window   *window;
    SDL_Renderer *sdl_renderer;
    SDL_Texture  *texture;

    uint32_t      tex_width;     /* current texture dimensions     */
    uint32_t      tex_height;
    uint32_t      win_width;     /* current window dimensions (points) */
    uint32_t      win_height;
    uint32_t      out_width;     /* renderer output size (pixels)      */
    uint32_t      out_height;
    float         hidpi_scale_x; /* pixels / points (1.0 on non-HiDPI) */
    float         hidpi_scale_y;
    bool          open;          /* false after window close event  */

    MdSdlEventCallback event_cb; /* receives every SDL event first    */
    void              *event_userdata;
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

/* SDL drivers that accept windows but present them nowhere a person can
 * see. SDL 2 selects "offscreen" on its own when no display is reachable. */
static const char *const k_headless_video_drivers[] = {
    "offscreen", "dummy", "evdev",
};

static bool g_display_open;

/* ── Renderer selection (md-rnc) ─────────────────────────────── */

size_t md_render_driver_plan(const MdRenderDriver *drivers, size_t n,
                             const char *hint, MdRenderDriverUsable usable,
                             void *userdata, int *order, size_t cap) {
    size_t count = 0;
    int hinted = -1;
    for (size_t i = 0; hint && *hint && i < n; i++) {
        if (drivers[i].name && SDL_strcasecmp(drivers[i].name, hint) == 0) {
            hinted = (int)i;
            break;
        }
    }
    /* Pass 0: the hint; pass 1: accelerated drivers; pass 2: the rest. */
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < n && count < cap; i++) {
            bool in_pass = pass == 0 ? (int)i == hinted
                         : (int)i != hinted && drivers[i].accelerated == (pass == 1);
            if (!in_pass || !drivers[i].name)
                continue;
            if (usable && !usable(drivers[i].name, userdata))
                continue;
            order[count++] = (int)i;
        }
    }
    return count;
}

const char *md_render_framebuffer_accel(const MdRenderDriver *drivers,
                                        const int *order, size_t planned) {
    for (size_t k = 0; k < planned; k++) {
        if (drivers[order[k]].accelerated)
            return drivers[order[k]].name;
    }
    return "0";
}

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__ANDROID__)
/* True if `env` (an SDL library override, used alone when set) or else `a`
 * or `b` loads; otherwise reports what could not be loaded. */
static bool gl_library_loads(const char *env, const char *a, const char *b,
                             char *missing, size_t missing_len) {
    const char *forced = SDL_getenv(env);
    const char *names[] = { forced && *forced ? forced : a, forced && *forced ? NULL : b };
    for (size_t i = 0; i < 2; i++) {
        void *lib = names[i] ? SDL_LoadObject(names[i]) : NULL;
        if (lib) {
            SDL_UnloadObject(lib);
            return true;
        }
    }
    SDL_ClearError();
    snprintf(missing, missing_len, "%s%s%s", names[0], names[1] ? " or " : "",
             names[1] ? names[1] : "");
    return false;
}
#endif

/* SDL 2.30's GL renderers on EGL video drivers (Wayland, KMSDRM) segfault
 * instead of failing when a library they need is absent: "opengl" without
 * libGL.so.1/libOpenGL.so.0 or without libEGL, "opengles2" without
 * libGLESv2 or libEGL. A crash cannot be retried, so each GL driver's
 * libraries are probed before it is tried. X11 creates GL contexts through
 * GLX (GLES included) and reports load failures, so there only desktop GL is
 * required of "opengl". Apple and Windows always ship their system GL. */
static bool render_driver_usable(const char *name, void *userdata) {
    int *skipped = userdata;
#if defined(__APPLE__) || defined(_WIN32) || defined(__ANDROID__)
    (void)name;
    (void)skipped;
    return true;
#else
    bool gl = SDL_strcasecmp(name, "opengl") == 0;
    bool gles2 = SDL_strcasecmp(name, "opengles2") == 0;
    bool gles1 = SDL_strcasecmp(name, "opengles") == 0;
    const char *video = SDL_GetCurrentVideoDriver();
    bool x11 = video && SDL_strcasecmp(video, "x11") == 0;
    if (!gl && (x11 || (!gles2 && !gles1)))
        return true;
    char missing[256] = "";
    bool ok = (gl ? gl_library_loads("SDL_VIDEO_GL_DRIVER", "libGL.so.1", "libOpenGL.so.0",
                                     missing, sizeof(missing))
                  : gl_library_loads("SDL_VIDEO_GL_DRIVER",
                                     gles2 ? "libGLESv2.so.2" : "libGLESv1_CM.so.1", NULL,
                                     missing, sizeof(missing)))
           && (x11 || gl_library_loads("SDL_VIDEO_EGL_DRIVER", "libEGL.so.1", NULL,
                                       missing, sizeof(missing)));
    if (!ok) {
        fprintf(stderr, "render: skipping SDL renderer '%s': cannot load %s\n", name, missing);
        (*skipped)++;
    }
    return ok;
#endif
}

#define MD_MAX_RENDER_DRIVERS 16

static SDL_Renderer *create_sdl_renderer(SDL_Window *window) {
    MdRenderDriver drivers[MD_MAX_RENDER_DRIVERS];
    SDL_RendererInfo info;
    int n = SDL_GetNumRenderDrivers();
    size_t count = 0;
    for (int i = 0; i < n && count < MD_MAX_RENDER_DRIVERS; i++, count++) {
        bool ok = SDL_GetRenderDriverInfo(i, &info) == 0;
        drivers[count].name = ok ? info.name : NULL;  /* static driver strings */
        drivers[count].accelerated = ok && (info.flags & SDL_RENDERER_ACCELERATED);
    }
    int order[MD_MAX_RENDER_DRIVERS], skipped = 0;
    size_t planned = md_render_driver_plan(drivers, count, SDL_GetHint(SDL_HINT_RENDER_DRIVER),
                                           render_driver_usable, &skipped, order, MD_MAX_RENDER_DRIVERS);
    /* Normal priority: an operator's SDL_FRAMEBUFFER_ACCELERATION still wins. */
    if (skipped)
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION,
                    md_render_framebuffer_accel(drivers, order, planned));
    for (size_t k = 0; k < planned; k++) {
        const MdRenderDriver *d = &drivers[order[k]];
        SDL_Renderer *ren = SDL_CreateRenderer(window, order[k],
            d->accelerated ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE);
        if (ren)
            return ren;
        const char *err = SDL_GetError();
        fprintf(stderr, "render: SDL renderer '%s' failed: %s\n", d->name,
                err && *err ? err : "no error reported");
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────── */

int md_renderer_open_display(void) {
    if (g_display_open)
        return 0;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "render: SDL_Init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    const char *driver = SDL_GetCurrentVideoDriver();
    for (size_t i = 0; i < sizeof(k_headless_video_drivers) / sizeof(*k_headless_video_drivers); i++) {
        if (driver && SDL_strcasecmp(driver, k_headless_video_drivers[i]) == 0) {
            fprintf(stderr, "render: SDL video driver '%s' has no visible output\n", driver);
            SDL_Quit();
            return -1;
        }
    }
    fprintf(stderr, "render: SDL video driver '%s'\n", driver ? driver : "?");
    g_display_open = true;
    return 0;
}

void md_renderer_close_display(void) {
    if (!g_display_open)
        return;
    SDL_Quit();
    g_display_open = false;
}

MdRenderer *md_renderer_create(uint32_t width, uint32_t height, const char *title) {
    if (md_renderer_open_display() < 0)
        return NULL;
    MdRenderer *r = calloc(1, sizeof(MdRenderer));
    if (!r) return NULL;

    /* Create window — resizable, initially at the given dimensions */
    r->window = SDL_CreateWindow(
        title ? title : "metadesk",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (int)width, (int)height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!r->window) {
        fprintf(stderr, "render: SDL_CreateWindow failed: %s\n", SDL_GetError());
        free(r);
        return NULL;
    }

    /* vsync disabled for low latency: SDL_RENDERER_PRESENTVSYNC is
     * intentionally NOT set (spec §9.1). */
    r->sdl_renderer = create_sdl_renderer(r->window);
    if (!r->sdl_renderer) {
        fprintf(stderr, "render: SDL_CreateRenderer failed: no SDL renderer could be created\n");
        SDL_DestroyWindow(r->window);
        free(r);
        return NULL;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(r->sdl_renderer, &info) == 0)
        fprintf(stderr, "render: SDL renderer '%s' (%s)\n", info.name,
                (info.flags & SDL_RENDERER_ACCELERATED) ? "accelerated" : "software");

    /* Set scaling quality to linear for smooth resize */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    /* Query the size actually granted: the window manager may clamp the
     * requested size to the display (macOS does), and the HiDPI scale is
     * meaningless if computed against the size we asked for. */
    int actual_w = 0, actual_h = 0;
    SDL_GetWindowSize(r->window, &actual_w, &actual_h);
    r->win_width  = actual_w > 0 ? (uint32_t)actual_w : width;
    r->win_height = actual_h > 0 ? (uint32_t)actual_h : height;
    r->open = true;

    /* Compute HiDPI scale factor: renderer pixels vs window points */
    md_renderer_update_hidpi_scale(r);

    return r;
}

int md_renderer_update_frame(MdRenderer *r, const uint8_t *rgba,
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
    return 0;
}

void md_renderer_begin_frame(MdRenderer *r) {
    if (!r || !r->open)
        return;
    /* Draw the whole frame — video AND the ImGui overlay — in one coordinate
     * system: logical points at the HiDPI render scale. This keeps the
     * overlay's hit-testing (ImGui works in points) aligned with where it is
     * drawn, so dragging behaves correctly.
     *
     * Steps:
     *  - viewport: force the full output in pixels. On a HiDPI macOS window
     *    the default viewport is the window POINT size, which would clip the
     *    frame to the top-left quarter of the drawable.
     *  - video: fit the source aspect in physical pixels; clear the bars.
     *  - overlay: restore the HiDPI factor for ImGui's point-sized geometry.
     *  - clip: clear any leftover ImGui scissor rect. */
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(r->sdl_renderer, &out_w, &out_h);
    SDL_RenderSetScale(r->sdl_renderer, 1.0f, 1.0f);
    if (out_w > 0 && out_h > 0) {
        SDL_Rect vp = { 0, 0, out_w, out_h };
        SDL_RenderSetViewport(r->sdl_renderer, &vp);
    }
    SDL_RenderSetClipRect(r->sdl_renderer, NULL);
    SDL_SetRenderDrawColor(r->sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(r->sdl_renderer);
    if (r->texture) {
        MdVideoRect fit = md_video_fit(r->tex_width, r->tex_height, out_w, out_h);
        SDL_Rect dst = { fit.x, fit.y, fit.w, fit.h };
        if (dst.w > 0 && dst.h > 0)
            SDL_RenderCopy(r->sdl_renderer, r->texture, NULL, &dst);
    }
    SDL_RenderSetScale(r->sdl_renderer, r->hidpi_scale_x, r->hidpi_scale_y);
}

void md_renderer_end_frame(MdRenderer *r) {
    if (!r || !r->open)
        return;
    SDL_RenderPresent(r->sdl_renderer);
}

int md_renderer_present(MdRenderer *r, const uint8_t *rgba,
                        uint32_t width, uint32_t height) {
    if (md_renderer_update_frame(r, rgba, width, height) < 0)
        return -1;
    md_renderer_begin_frame(r);
    md_renderer_end_frame(r);
    return 0;
}

void md_renderer_reset_frame(MdRenderer *r) {
    if (!r) return;
    SDL_DestroyTexture(r->texture);
    r->texture = NULL;
    r->tex_width = r->tex_height = 0;
}

int md_renderer_poll_events(MdRenderer *r) {
    if (!r || !r->open)
        return -1;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        /* Give the overlay first sight of every event (single pump) */
        if (r->event_cb)
            r->event_cb(&event, r->event_userdata);

        switch (event.type) {
        case SDL_QUIT:
            r->open = false;
            return -1;

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (!r->event_cb && event.key.keysym.sym == SDLK_ESCAPE && event.type == SDL_KEYDOWN) {
                r->open = false;
                return -1;
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
                /* Recompute HiDPI scale on resize */
                md_renderer_update_hidpi_scale(r);
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

void md_renderer_set_event_callback(MdRenderer *r, MdSdlEventCallback cb,
                                        void *userdata) {
    if (!r) return;
    r->event_cb = cb;
    r->event_userdata = userdata;
}

void md_renderer_update_hidpi_scale(MdRenderer *r) {
    if (!r || !r->sdl_renderer || !r->window) return;

    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(r->sdl_renderer, &out_w, &out_h);
    r->out_width  = (uint32_t)(out_w > 0 ? out_w : r->win_width);
    r->out_height = (uint32_t)(out_h > 0 ? out_h : r->win_height);

    r->hidpi_scale_x = r->win_width  > 0
        ? (float)r->out_width  / (float)r->win_width  : 1.0f;
    r->hidpi_scale_y = r->win_height > 0
        ? (float)r->out_height / (float)r->win_height : 1.0f;

    if (r->hidpi_scale_x != 1.0f || r->hidpi_scale_y != 1.0f) {
        fprintf(stderr, "render: HiDPI scale %.2fx%.2f (window %ux%u, output %ux%u)\n",
                r->hidpi_scale_x, r->hidpi_scale_y,
                r->win_width, r->win_height,
                r->out_width, r->out_height);
    }
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

void md_renderer_destroy(MdRenderer *r) {
    if (!r) return;

    if (r->texture)
        SDL_DestroyTexture(r->texture);
    if (r->sdl_renderer)
        SDL_DestroyRenderer(r->sdl_renderer);
    if (r->window)
        SDL_DestroyWindow(r->window);

    free(r);
}
