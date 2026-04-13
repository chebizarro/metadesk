/*
 * metadesk — capture.h
 * Platform-agnostic screen capture HAL.
 * See spec §2.3.1 — platform backends selected at compile time.
 *
 * Backends:
 *   Linux:   PipeWire via ScreenCast portal (capture_pipewire.c)
 *   macOS:   ScreenCaptureKit            (capture_screencapturekit.m)
 *   Windows: DXGI Desktop Duplication    (capture_dxgi.cpp)
 *
 * No platform-specific headers appear in this file.
 */
#ifndef MD_CAPTURE_H
#define MD_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque types ────────────────────────────────────────────── */

/* Forward declarations — backends define the concrete structs. */
typedef struct MdCaptureCtx MdCaptureCtx;

/* ── Frame data ──────────────────────────────────────────────── */

/* Pixel format (platform-neutral subset of common capture outputs) */
typedef enum {
    MD_PIX_CAPTURE_BGRX,   /* 4 bytes/pixel, blue-green-red-pad  */
    MD_PIX_CAPTURE_BGRA,
    MD_PIX_CAPTURE_RGBX,
    MD_PIX_CAPTURE_RGBA,
    MD_PIX_CAPTURE_NV12,   /* semi-planar 4:2:0 (macOS, DXGI)    */
} MdCapturePixFmt;

/* Buffer type for the frame data pointer. */
typedef enum {
    MD_BUF_CPU,      /* CPU-accessible pointer (mmap'd SHM, staged copy, etc.) */
    MD_BUF_DMABUF,   /* Linux DMA-BUF file descriptor                          */
    MD_BUF_GPU,      /* Opaque GPU handle (platform-specific, cast as needed)   */
} MdBufferType;

/* Frame metadata delivered to callers.
 * The frame is valid only until release_frame() or the next get_frame(). */
typedef struct {
    uint32_t          width;
    uint32_t          height;
    uint32_t          stride;        /* bytes per row for the first (or only) plane  */
    MdCapturePixFmt   format;
    MdBufferType      buf_type;
    int               dmabuf_fd;     /* valid when buf_type == MD_BUF_DMABUF         */
    uint8_t          *data;          /* valid when buf_type == MD_BUF_CPU             */
    void             *gpu_handle;    /* valid when buf_type == MD_BUF_GPU             */
    size_t            data_size;     /* total mapped / accessible buffer size         */
    uint64_t          timestamp_ns;  /* monotonic capture timestamp (nanoseconds)     */
    uint32_t          seq;           /* monotonic frame sequence number               */
} MdFrame;

/* ── Configuration ───────────────────────────────────────────── */

typedef struct {
    uint32_t target_fps;   /* desired framerate, 0 = backend default     */
    bool     show_cursor;  /* include cursor in captured frames          */
} MdCaptureConfig;

/* ── Backend vtable (spec §2.3.1) ────────────────────────────── */

typedef struct MdCaptureBackend {
    int   (*init)(MdCaptureCtx *ctx, const MdCaptureConfig *cfg);
    int   (*start)(MdCaptureCtx *ctx);
    int   (*get_frame)(MdCaptureCtx *ctx, MdFrame *out);
    void  (*release_frame)(MdCaptureCtx *ctx, MdFrame *frame);
    void  (*stop)(MdCaptureCtx *ctx);
    void  (*destroy)(MdCaptureCtx *ctx);
} MdCaptureBackend;

/* ── Capture context ─────────────────────────────────────────── */

/* Generic capture context wrapping a backend.
 * Backends store their private state in `backend_data`. */
struct MdCaptureCtx {
    const MdCaptureBackend *vtable;
    MdCaptureConfig         config;
    void                   *backend_data;  /* backend-private state */
    volatile bool           active;
    uint32_t                width;
    uint32_t                height;
};

/* ── Factory ─────────────────────────────────────────────────── */

/* Create the platform-appropriate capture backend.
 * Each platform's backend source file implements this function.
 * Returns NULL on failure (e.g. required service not available). */
const MdCaptureBackend *md_capture_backend_create(void);

/* ── Public convenience API ──────────────────────────────────── */

/* These wrap the vtable calls with a consistent interface.
 * Callers may also use the vtable directly if preferred. */

/* Create and initialise a capture context with the platform backend.
 * Returns NULL on failure. */
MdCaptureCtx *md_capture_create(const MdCaptureConfig *cfg);

/* Start capturing. Returns 0 on success, -1 on error. */
int md_capture_start(MdCaptureCtx *ctx);

/* Get the next captured frame (may block until a frame is available).
 * Returns 0 on success, -1 on error / timeout. */
int md_capture_get_frame(MdCaptureCtx *ctx, MdFrame *out);

/* Release a frame returned by get_frame (returns buffer to backend). */
void md_capture_release_frame(MdCaptureCtx *ctx, MdFrame *frame);

/* Query whether capture is currently active. */
bool md_capture_is_active(const MdCaptureCtx *ctx);

/* Get the negotiated frame dimensions (valid after first frame). */
int md_capture_get_size(const MdCaptureCtx *ctx, uint32_t *width, uint32_t *height);

/* Stop capture. */
void md_capture_stop(MdCaptureCtx *ctx);

/* Stop and destroy capture context, freeing all resources. */
void md_capture_destroy(MdCaptureCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MD_CAPTURE_H */
