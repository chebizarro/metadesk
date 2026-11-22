/* Incompatible ABI: loader MUST reject before touching the encoder vtable. */
#include "openh264_headers/codec_api.h"
#include <cstdlib>
extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#endif
void WelsGetCodecVersionEx(OpenH264Version *v) { *v = {99, 0, 0, 0}; }
#ifdef _WIN32
__declspec(dllexport)
#endif
int WelsCreateSVCEncoder(ISVCEncoder **) { abort(); }
#ifdef _WIN32
__declspec(dllexport)
#endif
void WelsDestroySVCEncoder(ISVCEncoder *) { abort(); }
}
