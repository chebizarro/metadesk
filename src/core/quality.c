#include "quality.h"
#include "encode.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <libyuv.h>
bool md_quality_valid(const MdQuality *q) {
    return q && (!q->max_width || (q->max_width >= 2 && q->max_width <= MD_QUALITY_MAX_DIMENSION)) &&
        (!q->max_height || (q->max_height >= 2 && q->max_height <= MD_QUALITY_MAX_DIMENSION)) &&
        (!q->bitrate || (q->bitrate >= MD_ENCODER_MIN_BITRATE && q->bitrate <= MD_ENCODER_MAX_BITRATE)) &&
        q->fps <= MD_QUALITY_MAX_FPS;
}
bool md_quality_frame_due(uint32_t fps,uint64_t last,uint64_t now) {
    return fps && (!last || (now>=last && now-last>=1000000000u/fps));
}
bool md_quality_accepts(const MdQuality *r,const MdQuality *e) {
    return md_quality_valid(e) && e->bitrate && e->fps &&
        (!r->max_width || (e->max_width && e->max_width<=r->max_width)) &&
        (!r->max_height || (e->max_height && e->max_height<=r->max_height)) &&
        (!r->bitrate || e->bitrate<=r->bitrate) && (!r->fps || e->fps<=r->fps);
}
static uint32_t bound(uint32_t a, uint32_t b) { return !a ? b : !b ? a : a < b ? a : b; }
MdQuality md_quality_clamp(const MdQuality *r, const MdQuality *p) {
    MdQuality q = { bound(r->max_width, p->max_width), bound(r->max_height, p->max_height),
        bound(r->bitrate, p->bitrate ? p->bitrate : MD_ENCODER_DEFAULT_BITRATE),
        bound(r->fps, p->fps ? p->fps : MD_ENCODER_DEFAULT_FPS) };
    if (q.bitrate < MD_ENCODER_MIN_BITRATE) q.bitrate = MD_ENCODER_MIN_BITRATE;
    if (q.bitrate > MD_ENCODER_MAX_BITRATE) q.bitrate = MD_ENCODER_MAX_BITRATE;
    if (q.fps > MD_QUALITY_MAX_FPS) q.fps = MD_QUALITY_MAX_FPS;
    return q;
}
int md_quality_read(const cJSON *root, MdQuality *q, bool *present) {
    memset(q, 0, sizeof(*q)); *present = false;
    const cJSON *obj = NULL, *it;
    cJSON_ArrayForEach(it, root) {
        if (it->string && !strcmp(it->string, "quality")) {
            if (obj) return -1;
            obj = it;
        }
    }
    if (!obj) return 0;
    if (!cJSON_IsObject(obj)) return -1;
    uint32_t *fields[] = { &q->max_width, &q->max_height, &q->bitrate, &q->fps };
    const char *names[] = { "max_width", "max_height", "bitrate", "fps" };
    unsigned seen = 0;
    cJSON_ArrayForEach(it, obj) {
        unsigned n;
        for (n = 0; n < 4; n++) if (it->string && !strcmp(it->string, names[n])) break;
        if (n == 4 || (seen & (1u << n)) || !cJSON_IsNumber(it) ||
            !isfinite(it->valuedouble) || it->valuedouble < 0 || it->valuedouble > UINT32_MAX ||
            floor(it->valuedouble) != it->valuedouble) return -1;
        seen |= 1u << n; *fields[n] = (uint32_t)it->valuedouble;
    }
    if (!md_quality_valid(q)) return -1;
    *present = true;
    return 0;
}
int md_quality_write(cJSON *root, const MdQuality *q) {
    if (!md_quality_valid(q)) return -1;
    cJSON *obj = cJSON_AddObjectToObject(root, "quality");
    return obj && cJSON_AddNumberToObject(obj, "max_width", q->max_width) &&
        cJSON_AddNumberToObject(obj, "max_height", q->max_height) &&
        cJSON_AddNumberToObject(obj, "bitrate", q->bitrate) &&
        cJSON_AddNumberToObject(obj, "fps", q->fps) ? 0 : -1;
}
char *md_quality_state(const MdQuality *q, const char *error) {
    cJSON *root = cJSON_CreateObject();
    char *json = root && cJSON_AddStringToObject(root, "type", "quality") &&
        md_quality_write(root, q) == 0 && (!error || cJSON_AddStringToObject(root,"error",error)) ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root); return json;
}

char *md_quality_control(const MdQuality *q) { return md_quality_state(q,NULL); }
int md_quality_scale(const MdQuality *q, const MdFrame *src, MdFrame *out,
                     uint8_t **scratch, size_t *capacity) {
    uint32_t w, h;
    if (!q || !src || !out || !scratch || !capacity || !src->data || src->buf_type != MD_BUF_CPU ||
        md_capture_fit_size(src->width, src->height, q->max_width, q->max_height, &w, &h)) return -1;
    *out = *src;
    if (w == src->width && h == src->height) return 0;
    w &= ~1u; h &= ~1u;
    if (w < 2 || h < 2) return -1;
    bool nv12 = src->format == MD_PIX_CAPTURE_NV12;
    size_t bytes = (size_t)w * h * (nv12 ? 3 : 8) / 2;
    if (*capacity < bytes) {
        uint8_t *p = realloc(*scratch, bytes);
        if (!p) return -1;
        *scratch = p; *capacity = bytes;
    }
    int rc;
    if (nv12) {
        rc = NV12Scale(src->data, src->stride, src->data + (size_t)src->stride * src->height,
            src->stride, src->width, src->height, *scratch, w, *scratch + (size_t)w * h,
            w, w, h, kFilterBox);
    } else {
        rc = md_capture_scale_frame(src->data, src->width, src->height, src->stride,
                                    *scratch, w, h, (size_t)w * 4);
    }
    if (rc) return -1;
    out->width = w; out->height = h; out->stride = w * (nv12 ? 1 : 4);
    out->data = *scratch; out->data_size = bytes;
    return 0;
}
