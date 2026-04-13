/*
 * metadesk — capture.h
 * PipeWire screen capture via ScreenCast portal (DMA-BUF / SHM).
 * See spec §2.1 and milestone 1.1.
 *
 * Architecture:
 *   1. Open a D-Bus connection to org.freedesktop.portal.ScreenCast
 *   2. CreateSession → SelectSources → Start (user picks a screen)
 *   3. Portal returns a PipeWire fd and node ID
 *   4. Connect to PipeWire on that fd
 *   5. Negotiate buffers (prefer DMA-BUF, fall back to SHM mmap)
 *   6. For each frame: invoke callback with MdFrame
 *
 * The capture loop runs on PipeWire's own thread (pw_main_loop).
 * The frame callback is invoked from that thread — callers must
 * handle synchronisation if needed.
 */
#ifndef MD_CAPTURE_H
#define MD_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque capture context */
typedef struct MdCapture MdCapture;

/* Buffer type negotiated with PipeWire */
typedef enum {
    MD_BUF_SHM,      /* Shared memory (mmap'd pointer)   */
    MD_BUF_DMABUF,   /* DMA-BUF file descriptor           */
} MdBufferType;

/* Frame metadata delivered to callback */
typedef struct {
    uint32_t     width;
    uint32_t     height;
    uint32_t     stride;
    uint32_t     format;        /* DRM/SPA fourcc (e.g. SPA_VIDEO_FORMAT_BGRx) */
    MdBufferType buf_type;
    int          dmabuf_fd;     /* valid if buf_type == MD_BUF_DMABUF           */
    uint8_t     *data;          /* valid if buf_type == MD_BUF_SHM              */
    size_t       data_size;     /* mapped buffer size in bytes                  */
    uint64_t     timestamp_ns;  /* PipeWire monotonic timestamp (nanoseconds)   */
    uint32_t     seq;           /* monotonic frame sequence number              */
} MdFrame;

/* Callback invoked for each captured frame.
 * Called from PipeWire's thread — must be fast, must not block. */
typedef void (*MdFrameCallback)(const MdFrame *frame, void *userdata);

/* Configuration */
typedef struct {
    uint32_t target_fps;        /* desired framerate, 0 = PipeWire default */
    bool     prefer_dmabuf;     /* true: request DMA-BUF, false: SHM only */
    bool     show_cursor;       /* include cursor in capture               */
} MdCaptureConfig;

/* Create capture context. Does NOT start capture yet.
 * Returns NULL on failure (e.g. PipeWire not available). */
MdCapture *md_capture_create(const MdCaptureConfig *cfg);

/* Start the ScreenCast portal flow and begin capturing.
 * This is asynchronous — returns immediately, portal dialog
 * appears to user, frames arrive via callback once approved.
 * Returns 0 on success, -1 on error. */
int md_capture_start(MdCapture *ctx, MdFrameCallback cb, void *userdata);

/* Stop capturing and release PipeWire stream.
 * Safe to call from any thread. */
void md_capture_stop(MdCapture *ctx);

/* Query whether capture is currently active and producing frames. */
bool md_capture_is_active(const MdCapture *ctx);

/* Get the negotiated frame dimensions (valid after first frame). */
int md_capture_get_size(const MdCapture *ctx, uint32_t *width, uint32_t *height);

/* Destroy capture context, stopping capture if still active. */
void md_capture_destroy(MdCapture *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MD_CAPTURE_H */
