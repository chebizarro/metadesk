# Vendored public headers

Source: Cisco OpenH264 v2.7.0, `codec/api/wels/`, under the BSD license in COPYING.
Version and layout remain pinned to 2.7.0.2610.

One local correction in codec_api.h: the C ISVCEncoderVtbl ForceIntraFrame slot
has an explicit `int iLayerId` argument, matching the C++ public declaration
`ForceIntraFrame(bool bIDR, int iLayerId = -1)` and the 2.7.0 implementation.
The upstream C declaration omitted that argument. Calling it without the layer
argument would use an indeterminate register/stack value. The C adapter passes
`-1` (all layers), just as the former C++ default argument did. No implementation
code or C++ runtime is vendored. Other header contents are unchanged.

Upstream declaration and implementation:
- https://github.com/cisco/openh264/blob/v2.7.0/codec/api/wels/codec_api.h
- https://github.com/cisco/openh264/blob/v2.7.0/codec/encoder/plus/src/welsEncoderExt.cpp
