#ifndef MD_OPENH264_RUNTIME_H
#define MD_OPENH264_RUNTIME_H
#include <stdbool.h>
/* Public API headers are identical for these two reviewed tags, not all 2.x. */
static inline bool md_openh264_version_supported(unsigned a, unsigned b, unsigned c, unsigned d) {
    return a == 2 && c == 0 && ((b == 6 && d == 2502) || (b == 7 && d == 2610));
}
#ifdef _WIN32
#if defined(_M_ARM64) || defined(__aarch64__)
#define MD_OH_NAME "openh264-2.7.0-win-arm64.dll"
#elif defined(_WIN64)
#define MD_OH_NAME "openh264-2.7.0-win64.dll"
#else
#define MD_OH_NAME "openh264-2.7.0-win32.dll"
#endif
#elif defined(__APPLE__)
#define MD_OH_NAME "libopenh264.8.dylib"
#else
#define MD_OH_NAME "libopenh264.so.8"
#endif
#endif
