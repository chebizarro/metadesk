/* Exercise the public factory without linking any native input backend. */
#include "input.h"
#include "md_test.h"
#include <stdlib.h>
#include <string.h>

static unsigned platform_inits, portal_inits, portal_probes;
static bool platform_fails, portal_fails, portal_available = true;
static bool expected_no_interaction;

static int platform_init(MdInputCtx *ctx) {
    platform_inits++;
    MD_CHECK(ctx->config.no_interaction == expected_no_interaction);
    ctx->ready = !platform_fails;
    return platform_fails ? -1 : 0;
}

static int portal_init(MdInputCtx *ctx) {
    portal_inits++;
    MD_CHECK(ctx->config.no_interaction == expected_no_interaction);
    ctx->ready = !portal_fails;
    return portal_fails ? -1 : 0;
}

static const MdInputBackend platform = { .init = platform_init };
static const MdInputBackend portal = { .init = portal_init };

const MdInputBackend *md_input_backend_create(void) { return &platform; }
const MdInputBackend *md_input_portal_backend(void) { return &portal; }
bool md_input_portal_available(void) { portal_probes++; return portal_available; }
#ifdef MD_HAVE_WAYLAND_INPUT
/* This fixture tests selection only, not a compositor connection. */
const MdInputBackend *md_input_wayland_backend(void) { return &platform; }
#endif

static void reset(void) {
    platform_inits = portal_inits = portal_probes = 0;
    platform_fails = portal_fails = false;
    expected_no_interaction = false;
}

static void check_auto(const char *name, bool available, bool portal_fail, bool platform_fail,
                       bool wayland, bool managed) {
    reset();
    portal_available = available;
    platform_fails = platform_fail;
    portal_fails = portal_fail;
    expected_no_interaction = managed;
    bool prefer_portal = false;
#ifdef __linux__
    prefer_portal = wayland && available;
#else
    (void)wayland;
#endif
    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080, .backend = name,
                          .no_interaction = managed };
    MdInput *input = md_input_create(&cfg);
    bool use_platform = !prefer_portal || portal_fail;
    MD_CHECK(platform_inits == (unsigned)use_platform);
    MD_CHECK(portal_inits == (unsigned)prefer_portal);
#ifdef __linux__
    MD_CHECK(portal_probes == (unsigned)wayland);
#else
    MD_CHECK(portal_probes == 0);
#endif
    if (use_platform && platform_fail) MD_CHECK(input == NULL);
    else {
        MD_CHECK(input != NULL);
        if (input) MD_CHECK(input->vtable == (use_platform ? &platform : &portal) && md_input_is_ready(input));
    }
    md_input_destroy(input);
}

int main(void) {
    const char *old_session = getenv("XDG_SESSION_TYPE");
    char *saved_session = old_session ? strdup(old_session) : NULL;
    const char *names[] = { NULL, "auto" };
    const char *sessions[] = { NULL, "", "x11", "wayland", "other" };
    for (unsigned s = 0; s < 5; s++) {
#ifdef _WIN32
        _putenv_s("XDG_SESSION_TYPE", sessions[s] ? sessions[s] : "");
#else
        if (sessions[s]) setenv("XDG_SESSION_TYPE", sessions[s], 1);
        else unsetenv("XDG_SESSION_TYPE");
#endif
        for (unsigned i = 0; i < 2; i++)
            for (unsigned available = 0; available < 2; available++)
                for (unsigned portal_fail = 0; portal_fail < 2; portal_fail++)
                    for (unsigned platform_fail = 0; platform_fail < 2; platform_fail++)
                        for (unsigned managed = 0; managed < 2; managed++)
                            check_auto(names[i], available, portal_fail, platform_fail,
                                       sessions[s] && !strcmp(sessions[s], "wayland"), managed);
    }

    reset();
    MdInput *input = md_input_create(NULL);
    MD_CHECK(input != NULL);
    MD_CHECK(platform_inits == 1 && portal_inits == 0 && portal_probes == 0);
    md_input_destroy(input);

    for (unsigned fails = 0; fails < 2; fails++) {
        reset();
        portal_fails = fails;
        MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080, .backend = "portal" };
        input = md_input_create(&cfg);
        MD_CHECK(portal_inits == 1 && platform_inits == 0 && portal_probes == 0);
        MD_CHECK((input == NULL) == !!fails);
        if (input) MD_CHECK(input->vtable == &portal && md_input_is_ready(input));
        md_input_destroy(input);
    }
#ifdef __linux__
    reset();
    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080, .backend = "uinput" };
    input = md_input_create(&cfg);
    MD_CHECK(input != NULL);
    MD_CHECK(platform_inits == 1 && portal_inits == 0 && portal_probes == 0);
    md_input_destroy(input);
#endif
#ifdef _WIN32
    _putenv_s("XDG_SESSION_TYPE", saved_session ? saved_session : "");
#else
    if (saved_session) setenv("XDG_SESSION_TYPE", saved_session, 1);
    else unsetenv("XDG_SESSION_TYPE");
#endif
    free(saved_session);
    return md_test_report();
}
