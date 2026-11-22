/* Exercise the public factory without linking any native input backend.
 * A portal may be available, but unaccepted input must remain explicit-only. */
#include "input.h"
#include "md_test.h"

static unsigned platform_inits, portal_inits, portal_probes;
static bool platform_fails, portal_fails, portal_available = true;

static int platform_init(MdInputCtx *ctx) {
    platform_inits++;
    ctx->ready = !platform_fails;
    return platform_fails ? -1 : 0;
}

static int portal_init(MdInputCtx *ctx) {
    portal_inits++;
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
}

static void check_auto(const char *name, bool available, bool fails) {
    reset();
    portal_available = available;
    platform_fails = fails;
    MdInputConfig cfg = { .screen_width = 1920, .screen_height = 1080, .backend = name };
    MdInput *input = md_input_create(&cfg);
    MD_CHECK(platform_inits == 1);
    MD_CHECK(portal_inits == 0);
    MD_CHECK(portal_probes == 0);
    if (fails) MD_CHECK(input == NULL);
    else {
        MD_CHECK(input != NULL);
        if (input) MD_CHECK(input->vtable == &platform && md_input_is_ready(input));
    }
    md_input_destroy(input);
}

int main(void) {
    const char *names[] = { NULL, "auto" };
    for (unsigned i = 0; i < 2; i++)
        for (unsigned available = 0; available < 2; available++)
            for (unsigned fails = 0; fails < 2; fails++)
                check_auto(names[i], available, fails);

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
    return md_test_report();
}
