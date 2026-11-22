#include "encoder_chain.h"
#include "md_test.h"
#include <string.h>
#include <stdio.h>

typedef struct { const char *available; const char *seen[8]; unsigned count; } Fake;
static bool attempt(void *userdata, const char *name) {
    Fake *f = userdata;
    f->seen[f->count++] = name;
    return f->available && strcmp(f->available, name) == 0;
}
static void check(MdEncoderPlatform platform, bool nvenc, const char **expected, unsigned n) {
    /* Make every entry succeed in turn, verifying all prior failures fall through. */
    for (unsigned winner = 0; winner <= n; winner++) {
        Fake f = {.available = winner < n ? expected[winner] : NULL};
        const char *selected = md_encoder_select(platform, nvenc, NULL, attempt, &f);
        MD_CHECK(f.count == (winner < n ? winner + 1 : n));
        for (unsigned i = 0; i < f.count; i++) MD_CHECK(strcmp(f.seen[i], expected[i]) == 0);
        MD_CHECK(winner < n ? selected && strcmp(selected, expected[winner]) == 0 : !selected);
    }
}
int main(void) {
    const char *mac[] = {"h264_nvenc", "h264_videotoolbox", "openh264", "libx264"};
    const char *linux_chain[] = {"h264_nvenc", "h264_vaapi", "h264_qsv", "openh264", "libx264"};
    const char *win[] = {"h264_nvenc", "h264_amf", "h264_mf", "openh264", "libx264"};
    check(MD_ENCODER_MACOS, true, mac, 4); check(MD_ENCODER_MACOS, false, mac + 1, 3);
    check(MD_ENCODER_LINUX, true, linux_chain, 5); check(MD_ENCODER_LINUX, false, linux_chain + 1, 4);
    check(MD_ENCODER_WINDOWS, true, win, 5); check(MD_ENCODER_WINDOWS, false, win + 1, 4);
    Fake f = {0};
    MD_CHECK(!md_encoder_select(MD_ENCODER_WINDOWS, true, "openh264", attempt, &f));
    MD_CHECK(f.count == 1 && strcmp(f.seen[0], "openh264") == 0);
    f.count = 0;
    f.available = "h264_nvenc";
    MD_CHECK(md_encoder_select(MD_ENCODER_WINDOWS, false, "h264_nvenc", attempt, &f) != NULL);
    MD_CHECK(f.count == 1 && strcmp(f.seen[0], "h264_nvenc") == 0);
    f.count = 0;
    MD_CHECK(!md_encoder_select(MD_ENCODER_LINUX, true, "unknown", attempt, &f));
    MD_CHECK(f.count == 0);
    puts("encoder chain: all platform orders, failed opens, explicit selection, no encoder");
    return md_test_report();
}
