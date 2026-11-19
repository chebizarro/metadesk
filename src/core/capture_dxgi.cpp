/*
 * metadesk — capture_dxgi.cpp
 * Windows capture backend: DXGI Desktop Duplication (spec §2.3.1).
 *
 * Uses:
 *   - IDXGIOutputDuplication for screen capture
 *   - ID3D11Device / ID3D11DeviceContext for GPU resource access
 *   - a CPU-readable staging texture for the frame data
 *
 * Flow:
 *   1. D3D11CreateDevice → get ID3D11Device
 *   2. QueryInterface → IDXGIDevice → GetAdapter → EnumOutputs
 *   3. DuplicateOutput → IDXGIOutputDuplication
 *   4. AcquireNextFrame → ID3D11Texture2D (GPU)
 *   5. CopyResource to staging texture → Map for CPU read → ReleaseFrame
 *   6. Deliver the mapped staging texture until release_frame unmaps it
 *
 * HAL contract (capture.h): get_frame waits at most timeout_ms and returns
 * 0, MD_CAPTURE_IDLE (no desktop damage) or MD_CAPTURE_ERROR (stopped, or
 * duplication lost). Losing duplication (desktop switch such as UAC or the
 * lock screen, display mode change, device removal) clears ctx->active; the
 * HAL's recovery is stop + start, and start re-creates the duplication.
 * stop only clears ctx->active, so it is safe while another thread waits in
 * get_frame; everything else runs on the capture thread or after it joined.
 *
 * Requires Windows 8+ for Desktop Duplication API.
 * The calling thread must be on the desktop that owns the display.
 */
extern "C" {
#include "capture.h"
}

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdlib>
#include <cstdint>
#include "log.h"
#define MD_LOG_TAG "capture_dxgi"

/* ── Backend-private state ───────────────────────────────────── */

struct DxgiState {
    ID3D11Device            *device;
    ID3D11DeviceContext     *context;
    IDXGIOutputDuplication  *duplication; /* nullptr after loss until start() */
    ID3D11Texture2D         *staging;     /* CPU-readable copy of the desktop */

    uint32_t                 width;
    uint32_t                 height;

    bool                     mapped;      /* staging is borrowed by the caller */
    bool                     delivered;   /* a frame since (re)duplicating */
    uint32_t                 seq;
    LARGE_INTEGER            qpc_hz;
};

/* ── Helpers ─────────────────────────────────────────────────── */

template <typename T>
static void safe_release(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/* QueryPerformanceCounter ticks (DXGI's LastPresentTime) to nanoseconds. */
static uint64_t qpc_to_ns(const DxgiState *st, LONGLONG ticks) {
    const uint64_t hz = (uint64_t)st->qpc_hz.QuadPart, t = (uint64_t)ticks;
    return t / hz * 1000000000ull + t % hz * 1000000000ull / hz;
}

static void unmap_staging(DxgiState *st) {
    if (st->mapped) {
        st->context->Unmap(st->staging, 0);
        st->mapped = false;
    }
}

static void free_state(DxgiState *st) {
    unmap_staging(st);
    safe_release(st->staging);
    safe_release(st->duplication);
    safe_release(st->context);
    safe_release(st->device);
    free(st);
}

/* Create a CPU-readable staging texture matching the display size. */
static HRESULT create_staging_texture(DxgiState *st) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = st->width;
    desc.Height             = st->height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    return st->device->CreateTexture2D(&desc, nullptr, &st->staging);
}

/* Create the D3D11 device if needed, duplicate the primary output and size
 * the staging texture to its current mode. */
static HRESULT open_duplication(MdCaptureCtx *ctx, DxgiState *st) {
    HRESULT hr = S_OK;
    if (!st->device) {
        hr = D3D11CreateDevice(
            nullptr,                    /* default adapter */
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,                    /* no software module */
            0,                          /* flags */
            nullptr, 0,                 /* default feature levels */
            D3D11_SDK_VERSION,
            &st->device,
            nullptr,
            &st->context);
        if (FAILED(hr)) {
            MD_LOG_I("D3D11CreateDevice failed: 0x%08lx", hr);
            return hr;
        }
    }

    IDXGIDevice  *dxgi_device = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIOutput  *output = nullptr;
    IDXGIOutput1 *output1 = nullptr;
    hr = st->device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_device);
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(0, &output);  /* primary display */
    if (SUCCEEDED(hr))
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
    if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(st->device, &st->duplication);
    safe_release(output1);
    safe_release(output);
    safe_release(adapter);
    safe_release(dxgi_device);
    if (FAILED(hr)) {
        MD_LOG_I("DuplicateOutput failed: 0x%08lx\n  Ensure the process has desktop access and is not running in a different session.", hr);
        return hr;
    }

    DXGI_OUTDUPL_DESC dupl_desc;
    st->duplication->GetDesc(&dupl_desc);
    if (!st->staging || dupl_desc.ModeDesc.Width != st->width ||
        dupl_desc.ModeDesc.Height != st->height) {
        safe_release(st->staging);
        st->width  = dupl_desc.ModeDesc.Width;
        st->height = dupl_desc.ModeDesc.Height;
        hr = create_staging_texture(st);
        if (FAILED(hr)) {
            MD_LOG_E("failed to create staging texture: 0x%08lx", hr);
            safe_release(st->duplication);
            return hr;
        }
    }
    st->delivered = false;
    ctx->width  = st->width;
    ctx->height = st->height;
    return S_OK;
}

/* AcquireNextFrame failed. Access loss and device removal invalidate the
 * duplication; anything else is reported without tearing it down. */
static int frame_failed(MdCaptureCtx *ctx, DxgiState *st, HRESULT hr) {
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        MD_LOG_I("duplication lost (0x%08lx); restart capture to recover", hr);
        safe_release(st->duplication);
        ctx->active = false;
    } else {
        MD_LOG_I("AcquireNextFrame failed: 0x%08lx", hr);
    }
    return MD_CAPTURE_ERROR;
}

/* ── Vtable implementation ───────────────────────────────────── */

static int dxgi_init(MdCaptureCtx *ctx) {
    auto *st = (DxgiState *)calloc(1, sizeof(DxgiState));
    if (!st) return -1;
    QueryPerformanceFrequency(&st->qpc_hz);
    if (FAILED(open_duplication(ctx, st))) {
        free_state(st);
        return -1;
    }
    ctx->backend_data = st;
    return 0;
}

static int dxgi_start(MdCaptureCtx *ctx) {
    auto *st = (DxgiState *)ctx->backend_data;
    if (!st) return -1;
    if (!st->duplication) {
        /* Recovering from loss. A removed device must be recreated too. */
        unmap_staging(st);
        if (st->device && FAILED(st->device->GetDeviceRemovedReason())) {
            safe_release(st->staging);
            safe_release(st->context);
            safe_release(st->device);
        }
        if (FAILED(open_duplication(ctx, st))) return -1;
    }
    ctx->active = true; /* AcquireNextFrame drives capture; nothing to start */
    return 0;
}

static int dxgi_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms) {
    auto *st = (DxgiState *)ctx->backend_data;
    if (!st || !ctx->active || !st->duplication) return MD_CAPTURE_ERROR;

    /* A previous frame the caller did not release. */
    unmap_staging(st);

    /* An unchanged desktop is DXGI_ERROR_WAIT_TIMEOUT: healthy idle, not a
     * device error. A pointer-only update (LastPresentTime 0) leaves the
     * desktop image unchanged too, so keep waiting for damage until the
     * deadline, except for the first frame after (re)duplicating: the caller
     * has no image yet, and that one carries the whole desktop. */
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *resource = nullptr;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const UINT wait = now < deadline ? (UINT)(deadline - now) : 0;
        HRESULT hr = st->duplication->AcquireNextFrame(wait, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return MD_CAPTURE_IDLE;
        if (FAILED(hr)) return frame_failed(ctx, st, hr);
        if (info.LastPresentTime.QuadPart != 0 || !st->delivered) break;
        safe_release(resource);
        st->duplication->ReleaseFrame();
        if (wait == 0) return MD_CAPTURE_IDLE;
    }

    ID3D11Texture2D *texture = nullptr;
    HRESULT hr = resource
        ? resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture)
        : E_POINTER;
    safe_release(resource);
    if (SUCCEEDED(hr)) {
        st->context->CopyResource(st->staging, texture);
        safe_release(texture);
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(hr))
        hr = st->context->Map(st->staging, 0, D3D11_MAP_READ, 0, &mapped);
    /* Map waited for the copy, so the desktop image can go back to DWM now
     * rather than being held while the caller encodes. */
    st->duplication->ReleaseFrame();
    if (FAILED(hr)) {
        MD_LOG_I("staging copy failed: 0x%08lx", hr);
        return MD_CAPTURE_ERROR;
    }
    st->mapped = true;
    st->delivered = true;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    out->width        = st->width;
    out->height       = st->height;
    out->stride       = (uint32_t)mapped.RowPitch;
    out->format       = MD_PIX_CAPTURE_BGRA;
    out->buf_type     = MD_BUF_CPU;
    out->dmabuf_fd    = -1;
    out->data         = (uint8_t *)mapped.pData;
    out->gpu_handle   = nullptr;
    out->data_size    = (size_t)mapped.RowPitch * st->height;
    out->timestamp_ns = qpc_to_ns(st, info.LastPresentTime.QuadPart
                                          ? info.LastPresentTime.QuadPart
                                          : now.QuadPart);
    out->seq          = st->seq++;
    return 0;
}

static void dxgi_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    auto *st = (DxgiState *)ctx->backend_data;
    (void)frame;
    if (st) unmap_staging(st);
}

static void dxgi_stop(MdCaptureCtx *ctx) {
    ctx->active = false;
}

static void dxgi_destroy(MdCaptureCtx *ctx) {
    auto *st = (DxgiState *)ctx->backend_data;
    if (!st) return;
    free_state(st);
    ctx->backend_data = nullptr;
}

/* ── Singleton vtable ────────────────────────────────────────── */

/* Positional: must match the MdCaptureBackend member order in capture.h. */
static const MdCaptureBackend dxgi_backend = {
    dxgi_init,
    dxgi_start,
    dxgi_get_frame,
    dxgi_release_frame,
    dxgi_stop,
    dxgi_destroy,
    nullptr, /* forget_authorization: no persisted consent */
    nullptr, /* wait_size: init sets ctx->width/height synchronously */
};

extern "C"
const MdCaptureBackend *md_capture_backend_create(void) {
    return &dxgi_backend;
}

#else /* !_WIN32 */

extern "C"
const MdCaptureBackend *md_capture_backend_create(void) {
    MD_LOG_I("DXGI backend not available on this platform");
    return nullptr;
}

#endif /* _WIN32 */
