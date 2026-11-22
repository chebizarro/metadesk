#ifndef MD_VIDEO_GEOMETRY_H
#define MD_VIDEO_GEOMETRY_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { int x, y, w, h; } MdVideoRect;

/* Fit in physical drawable pixels, shared by presentation and input mapping. */
static inline MdVideoRect md_video_fit(uint32_t fw, uint32_t fh, int ow, int oh) {
    MdVideoRect r = {0};
    if (!fw || !fh || ow <= 0 || oh <= 0) return r;
    if ((uint64_t)fw * (uint32_t)oh >= (uint64_t)fh * (uint32_t)ow) {
        r.w = ow;
        r.h = (int)((uint64_t)(uint32_t)ow * fh / fw);
    } else {
        r.h = oh;
        r.w = (int)((uint64_t)(uint32_t)oh * fw / fh);
    }
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = (ow - r.w) / 2;
    r.y = (oh - r.h) / 2;
    return r;
}

/* SDL input is in window points; reject bars/outside instead of edge-clamping. */
static inline bool md_video_map(uint32_t fw, uint32_t fh, int ww, int wh,
                                int ow, int oh, int wx, int wy,
                                uint32_t *fx, uint32_t *fy) {
    MdVideoRect r = md_video_fit(fw, fh, ow, oh);
    if (!fx || !fy || ww <= 0 || wh <= 0 || r.w <= 0 || r.h <= 0 ||
        wx < 0 || wy < 0 || wx >= ww || wy >= wh) return false;
    int64_t px = (int64_t)wx * ow / ww;
    int64_t py = (int64_t)wy * oh / wh;
    if (px < r.x || py < r.y || px >= (int64_t)r.x + r.w || py >= (int64_t)r.y + r.h)
        return false;
    *fx = (uint32_t)((uint64_t)(px - r.x) * fw / (uint32_t)r.w);
    *fy = (uint32_t)((uint64_t)(py - r.y) * fh / (uint32_t)r.h);
    return true;
}
#endif
