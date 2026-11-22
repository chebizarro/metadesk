#include "encoder_chain.h"
#include <stddef.h>
#include <string.h>

const char *md_encoder_select(MdEncoderPlatform platform, bool nvenc,
                              const char *only, MdEncoderTry attempt, void *userdata) {
    const char *chain[8];
    unsigned n = 0;
    if (nvenc || (only && strcmp(only, "h264_nvenc") == 0))
        chain[n++] = "h264_nvenc";
    if (platform == MD_ENCODER_MACOS) chain[n++] = "h264_videotoolbox";
    if (platform == MD_ENCODER_WINDOWS) {
        chain[n++] = "h264_amf";
        chain[n++] = "h264_mf";
    }
    if (platform == MD_ENCODER_LINUX) {
        chain[n++] = "h264_vaapi";
        chain[n++] = "h264_qsv";
    }
    chain[n++] = "openh264";
    chain[n++] = "libx264";
    for (unsigned i = 0; i < n; i++) {
        if (only && strcmp(only, chain[i]) != 0) continue;
        if (attempt(userdata, chain[i])) return chain[i];
    }
    return NULL;
}
