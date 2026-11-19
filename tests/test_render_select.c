/*
 * test_render_select.c — SDL render driver selection plan (md-rnc).
 *
 * The plan decides which SDL renderers md_renderer_create() tries, in order.
 * A driver the probe rejects (SDL's "opengl" without a desktop GL library,
 * which segfaults instead of failing) must never be tried, not even by SDL's
 * software-renderer framebuffer; GLES2 and then software remain as
 * fallbacks; an explicit SDL_RENDER_DRIVER goes first.
 */
#include "render.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include "md_test.h"

/* SDL 2.30's Linux driver list, in SDL's order. */
static const MdRenderDriver k_linux[] = {
    { "opengl", true }, { "opengles2", true }, { "opengles", true }, { "software", false },
};
#define N_LINUX (sizeof(k_linux) / sizeof(*k_linux))

static bool accept_all(const char *name, void *ud) { (void)name; (void)ud; return true; }

/* No desktop GL: the probe rejects "opengl" (and counts what it was asked). */
static bool no_desktop_gl(const char *name, void *ud) {
    (*(int *)ud)++;
    return strcmp(name, "opengl") != 0;
}

static bool no_accelerated(const char *name, void *ud) {
    (void)ud;
    return strcmp(name, "software") == 0;
}

static void expect(const int *order, size_t got, const char *const *names, size_t want) {
    MD_CHECK(got == want);
    for (size_t i = 0; i < got && i < want; i++)
        MD_CHECK(strcmp(k_linux[order[i]].name, names[i]) == 0);
}

static void test_default_order(void) {
    int order[8];
    size_t n = md_render_driver_plan(k_linux, N_LINUX, NULL, accept_all, NULL, order, 8);
    const char *want[] = { "opengl", "opengles2", "opengles", "software" };
    expect(order, n, want, 4);
    MD_CHECK(md_render_driver_plan(k_linux, N_LINUX, "", NULL, NULL, order, 8) == 4);
}

static void test_no_desktop_gl_falls_back_to_gles2(void) {
    int order[8], asked = 0;
    size_t n = md_render_driver_plan(k_linux, N_LINUX, NULL, no_desktop_gl, &asked, order, 8);
    const char *want[] = { "opengles2", "opengles", "software" };
    expect(order, n, want, 3);
    MD_CHECK(asked == 4);  /* every driver is probed exactly once */
}

static void test_software_last_resort(void) {
    int order[8];
    size_t n = md_render_driver_plan(k_linux, N_LINUX, NULL, no_accelerated, NULL, order, 8);
    const char *want[] = { "software" };
    expect(order, n, want, 1);
}

static void test_hint_first(void) {
    int order[8], asked = 0;
    size_t n = md_render_driver_plan(k_linux, N_LINUX, "SOFTWARE", accept_all, NULL, order, 8);
    const char *soft[] = { "software", "opengl", "opengles2", "opengles" };
    expect(order, n, soft, 4);
    n = md_render_driver_plan(k_linux, N_LINUX, "opengles2", accept_all, NULL, order, 8);
    const char *gles[] = { "opengles2", "opengl", "opengles", "software" };
    expect(order, n, gles, 4);
    /* A hint does not bypass the probe: a known crash is still skipped. */
    n = md_render_driver_plan(k_linux, N_LINUX, "opengl", no_desktop_gl, &asked, order, 8);
    const char *probed[] = { "opengles2", "opengles", "software" };
    expect(order, n, probed, 3);
    /* An unknown hint is ignored. */
    n = md_render_driver_plan(k_linux, N_LINUX, "vulkan", accept_all, NULL, order, 8);
    const char *dflt[] = { "opengl", "opengles2", "opengles", "software" };
    expect(order, n, dflt, 4);
}

static void test_bounds(void) {
    int order[2] = { -1, -1 };
    MD_CHECK(md_render_driver_plan(k_linux, N_LINUX, NULL, NULL, NULL, order, 2) == 2);
    MD_CHECK(order[0] == 0 && order[1] == 1);
    MD_CHECK(md_render_driver_plan(k_linux, 0, "opengl", NULL, NULL, order, 2) == 0);
    const MdRenderDriver unnamed[] = { { NULL, true }, { "software", false } };
    MD_CHECK(md_render_driver_plan(unnamed, 2, NULL, NULL, NULL, order, 2) == 1);
    MD_CHECK(order[0] == 1);
}

/* Once a driver is skipped, SDL's own software-framebuffer choice is pinned
 * to the plan: the first planned accelerated driver, else none at all. */
static void test_framebuffer_accel(void) {
    int order[8], asked = 0;
    size_t n = md_render_driver_plan(k_linux, N_LINUX, "software", no_desktop_gl, &asked, order, 8);
    MD_CHECK(strcmp(md_render_framebuffer_accel(k_linux, order, n), "opengles2") == 0);
    n = md_render_driver_plan(k_linux, N_LINUX, NULL, no_accelerated, NULL, order, 8);
    MD_CHECK(strcmp(md_render_framebuffer_accel(k_linux, order, n), "0") == 0);
    MD_CHECK(strcmp(md_render_framebuffer_accel(k_linux, order, 0), "0") == 0);
}

/* The real SDL driver list (no video init needed): every driver is planned
 * once, accelerated ones before the rest, and software stays available. */
static void test_real_sdl_list(void) {
    MdRenderDriver drivers[16];
    SDL_RendererInfo info;
    int count = SDL_GetNumRenderDrivers();
    MD_CHECK(count > 0 && count <= 16);
    if (count <= 0 || count > 16)
        return;
    for (int i = 0; i < count; i++) {
        MD_CHECK(SDL_GetRenderDriverInfo(i, &info) == 0);
        drivers[i].name = info.name;
        drivers[i].accelerated = (info.flags & SDL_RENDERER_ACCELERATED) != 0;
    }
    int order[16];
    size_t n = md_render_driver_plan(drivers, (size_t)count, NULL, NULL, NULL, order, 16);
    MD_CHECK(n == (size_t)count);
    bool seen_unaccelerated = false, has_software = false;
    for (size_t i = 0; i < n; i++) {
        const MdRenderDriver *d = &drivers[order[i]];
        MD_CHECK(!(d->accelerated && seen_unaccelerated));
        seen_unaccelerated |= !d->accelerated;
        has_software |= strcmp(d->name, "software") == 0;
        printf("  plan[%zu] = %s%s\n", i, d->name, d->accelerated ? " (accelerated)" : "");
    }
    MD_CHECK(has_software);
}

int main(void) {
    printf("render driver selection tests\n");
    RUN_TEST(test_default_order);
    RUN_TEST(test_no_desktop_gl_falls_back_to_gles2);
    RUN_TEST(test_software_last_resort);
    RUN_TEST(test_hint_first);
    RUN_TEST(test_bounds);
    RUN_TEST(test_framebuffer_accel);
    RUN_TEST(test_real_sdl_list);
    return md_test_report();
}
