/* Incompatible ABI: loader MUST reject before touching the encoder vtable. */
#include "openh264_headers/codec_api.h"
#include <stdlib.h>
#ifdef _WIN32
__declspec(dllexport)
#endif
void WelsGetCodecVersionEx(OpenH264Version *v) { *v = (OpenH264Version){99, 0, 0, 0}; }
#ifdef _WIN32
__declspec(dllexport)
#endif
int WelsCreateSVCEncoder(ISVCEncoder **encoder) { (void)encoder; abort(); }
#ifdef _WIN32
__declspec(dllexport)
#endif
void WelsDestroySVCEncoder(ISVCEncoder *encoder) { (void)encoder; abort(); }
