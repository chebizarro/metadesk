/* Incompatible ABI: loader MUST reject before touching the encoder vtable. */
#include "openh264_headers/codec_api.h"
#include <stdlib.h>
#ifndef MD_FAKE_OPENH264_VERSION
#define MD_FAKE_OPENH264_VERSION 99, 0, 0, 0
#endif
#ifdef _WIN32
__declspec(dllexport)
#endif
void WelsGetCodecVersionEx(OpenH264Version *v) { *v = (OpenH264Version){MD_FAKE_OPENH264_VERSION}; }
#ifdef _WIN32
__declspec(dllexport)
#endif
int WelsCreateSVCEncoder(ISVCEncoder **encoder) { (void)encoder; abort(); }
#ifdef _WIN32
__declspec(dllexport)
#endif
void WelsDestroySVCEncoder(ISVCEncoder *encoder) { (void)encoder; abort(); }
