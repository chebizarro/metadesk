#ifndef MD_ENCODE_OPENH264_H
#define MD_ENCODE_OPENH264_H
#include "encode.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MdOpenH264 MdOpenH264;
MdOpenH264 *md_openh264_create(const MdEncoderConfig *cfg);
/* Synchronous single-spatial-layer encoder: no buffered frames or B frames. */
int md_openh264_encode(MdOpenH264 *enc, const uint8_t *y, int sy,
                       const uint8_t *uv, int suv, int64_t pts, bool key,
                       MdEncodedPacket *out);
int md_openh264_bitrate(MdOpenH264 *enc, uint32_t bitrate);
void md_openh264_destroy(MdOpenH264 *enc);
#ifdef __cplusplus
}
#endif
#endif
