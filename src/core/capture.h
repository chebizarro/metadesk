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
 * The frame is borrowed until release_frame(); release before requesting
 * another frame. Concurrent stop waits for outstanding borrowed frames. */
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
    bool     unattended;   /* Linux: consent budget capped at 10 seconds */
    bool     forget_authorization_on_stall; /* opt-in, ignored unattended */
    uint32_t consent_timeout_ms; /* 0 = 180s interactive / 10s unattended */
} MdCaptureConfig;

/* ── Backend vtable (spec §2.3.1) ────────────────────────────── */

typedef struct MdCaptureBackend {
    /* ctx->config is populated before init. */
    int   (*init)(MdCaptureCtx *ctx);
    int   (*start)(MdCaptureCtx *ctx);
    /* Wait at most timeout_ms; see md_capture_get_frame for results. */
    int   (*get_frame)(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms);
    void  (*release_frame)(MdCaptureCtx *ctx, MdFrame *frame);
    void  (*stop)(MdCaptureCtx *ctx);
    void  (*destroy)(MdCaptureCtx *ctx);
    /* Optional: forget any persisted capture authorization (e.g. the PipeWire
     * screencast restore_token) so the next start re-prompts for consent.
     * May be NULL. Kept last so positional vtable initializers stay valid. */
    void  (*forget_authorization)(MdCaptureCtx *ctx);
    /* Optional: for backends that learn the size asynchronously, wait up to
     * timeout_ms for negotiation (0 on success, -1 on timeout/broken stream).
     * NULL means ctx->width/height are set synchronously by init/start. */
    int   (*wait_size)(MdCaptureCtx *ctx, uint32_t timeout_ms,
                       uint32_t *width, uint32_t *height);
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

enum { MD_CAPTURE_ERROR = -1, MD_CAPTURE_IDLE = 1 };

/* Shared policy used by the portal and host watchdog. */
uint32_t md_capture_consent_timeout_ms(const MdCaptureConfig *cfg);
bool md_capture_stall_may_forget(const MdCaptureConfig *cfg);

typedef struct {
    uint32_t deadline_ms;
    uint32_t restarts;
    bool armed;
    bool gave_up;
} MdCaptureWatchdog;
typedef enum {
    MD_CAPTURE_RECOVERY_NONE,
    MD_CAPTURE_RECOVERY_RESTART,
    MD_CAPTURE_RECOVERY_EXHAUSTED,
} MdCaptureRecovery;
/* Damage-idle disarms the fault timer but only a frame resets retry history. */
MdCaptureRecovery md_capture_watchdog_step(MdCaptureWatchdog *watchdog,
    int frame_result, bool has_client, uint32_t now_ms,
    uint32_t stall_ms, uint32_t max_restarts);

/* POSIX restore-token store. Opaque content, private directory/file, atomic
 * durable replacement. Load returns 0 if absent/unsafe; save returns -1 on error.
 * Neither load nor a failed save removes existing authorization. */
size_t md_capture_restore_token_load(char *buf, size_t size);
int md_capture_restore_token_save(const char *token);
int md_capture_restore_token_forget(void);

/* Get the next captured frame, waiting at most timeout_ms (0 = poll).
 * Returns 0 on success, MD_CAPTURE_IDLE when a healthy stream delivered no
 * damage within the timeout, or MD_CAPTURE_ERROR for a broken/stopped stream
 * (returned promptly, without waiting out the timeout). */
int md_capture_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms);

/* Release a frame returned by get_frame (returns buffer to backend). */
void md_capture_release_frame(MdCaptureCtx *ctx, MdFrame *frame);

/* Query whether capture is currently active. */
bool md_capture_is_active(const MdCaptureCtx *ctx);

/* Wait up to timeout_ms for the negotiated frame dimensions. Returns 0 with
 * the size, or -1 on timeout, a broken stream or invalid arguments. */
int md_capture_wait_size(MdCaptureCtx *ctx, uint32_t timeout_ms,
                         uint32_t *width, uint32_t *height);

/* Non-blocking md_capture_wait_size(ctx, 0, ...). */
int md_capture_get_size(MdCaptureCtx *ctx, uint32_t *width, uint32_t *height);

/* Stop capture. Release borrowed frames first. A concurrent stop waits for
 * the consumer's release before destroying its buffer storage. */
void md_capture_stop(MdCaptureCtx *ctx);

/* Forget any persisted capture authorization so the next md_capture_start()
 * requires fresh user consent. No-op if the backend doesn't support it. */
void md_capture_forget_authorization(MdCaptureCtx *ctx);

/* Stop and destroy capture context, freeing all resources. */
void md_capture_destroy(MdCaptureCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MD_CAPTURE_H */
