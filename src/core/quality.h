#ifndef MD_QUALITY_H
#define MD_QUALITY_H
#include <stdbool.h>
#include <stdint.h>
#include <cjson/cJSON.h>
#include "capture.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Zero is a preference for the host default, not permission to exceed policy. */
typedef struct { uint32_t max_width, max_height, bitrate, fps; } MdQuality;
#define MD_QUALITY_MAX_DIMENSION 16384u
#define MD_QUALITY_MAX_FPS 240u
bool md_quality_valid(const MdQuality *q);
MdQuality md_quality_clamp(const MdQuality *requested, const MdQuality *policy);
/* Strict optional quality member. Missing is legacy; malformed never defaults. */
int md_quality_read(const cJSON *root, MdQuality *q, bool *present);
int md_quality_write(cJSON *root, const MdQuality *q);
/* Owned scratch is reused; input remains borrowed and unchanged. */
int md_quality_scale(const MdQuality *q, const MdFrame *src, MdFrame *out,
                     uint8_t **scratch, size_t *capacity);
/* Bound-stream quality packets use the same strict object as signaling. */
char *md_quality_control(const MdQuality *q);
#ifdef __cplusplus
}
#endif
#endif
