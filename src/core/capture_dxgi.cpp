/*
 * metadesk — capture_dxgi.cpp
 * Windows capture backend: DXGI Desktop Duplication (spec §2.3.1).
 *
 * Uses:
 *   - IDXGIOutputDuplication for screen capture
 *   - ID3D11Device / ID3D11DeviceContext for GPU resource access
 *   - AcquireNextFrame → MapDesktopSurface for CPU-accessible frame data
 *
 * Flow:
 *   1. D3D11CreateDevice → get ID3D11Device
 *   2. QueryInterface → IDXGIDevice → GetAdapter → EnumOutputs
 *   3. DuplicateOutput → IDXGIOutputDuplication
 *   4. AcquireNextFrame → ID3D11Texture2D (GPU)
 *   5. CopyResource to staging texture → Map for CPU read
 *   6. Deliver frame via get_frame / release_frame
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
#include <cstring>
#include <cstdio>
#include <cstdint>

/* ── Backend-private state ───────────────────────────────────── */

struct DxgiState {
    ID3D11Device            *device;
    ID3D11DeviceContext     *context;
    IDXGIOutputDuplication  *duplication;
    ID3D11Texture2D         *staging;     /* CPU-readable staging texture */
    DXGI_OUTPUT_DESC         outputDesc;

    uint32_t                 width;
    uint32_t                 height;
    uint32_t                 stride;

    /* Frame tracking */
    bool                     frame_acquired;
    IDXGIResource           *desktop_resource;  /* from AcquireNextFrame */
    uint32_t                 seq;
};

/* ── Helpers ─────────────────────────────────────────────────── */

static void safe_release(IUnknown **ppunk) {
    if (*ppunk) {
        (*ppunk)->Release();
        *ppunk = nullptr;
    }
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

/* ── Vtable implementation ───────────────────────────────────── */

static int dxgi_init(MdCaptureCtx *ctx, const MdCaptureConfig *cfg) {
    (void)cfg;

    auto *st = (DxgiState *)calloc(1, sizeof(DxgiState));
    if (!st) return -1;

    /* Create D3D11 device */
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,                    /* default adapter */
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    /* no software module */
        0,                          /* flags */
        nullptr, 0,                 /* default feature levels */
        D3D11_SDK_VERSION,
        &st->device,
        &featureLevel,
        &st->context);

    if (FAILED(hr)) {
        fprintf(stderr, "capture_dxgi: D3D11CreateDevice failed: 0x%08lx\n", hr);
        free(st);
        return -1;
    }

    /* Get DXGI device → adapter → output */
    IDXGIDevice *dxgiDevice = nullptr;
    hr = st->device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
    if (FAILED(hr)) goto fail;

    IDXGIAdapter *adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) goto fail;

    IDXGIOutput *output = nullptr;
    hr = adapter->EnumOutputs(0, &output);  /* primary display */
    adapter->Release();
    if (FAILED(hr)) goto fail;

    output->GetDesc(&st->outputDesc);

    /* Duplicate the output */
    IDXGIOutput1 *output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
    output->Release();
    if (FAILED(hr)) goto fail;

    hr = output1->DuplicateOutput(st->device, &st->duplication);
    output1->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "capture_dxgi: DuplicateOutput failed: 0x%08lx\n"
                "  Ensure the process has desktop access and is not running "
                "in a different session.\n", hr);
        goto fail;
    }

    /* Get output dimensions */
    DXGI_OUTDUPL_DESC duplDesc;
    st->duplication->GetDesc(&duplDesc);
    st->width  = duplDesc.ModeDesc.Width;
    st->height = duplDesc.ModeDesc.Height;
    st->stride = st->width * 4; /* BGRA = 4 bytes */

    /* Create staging texture for CPU reads */
    hr = create_staging_texture(st);
    if (FAILED(hr)) {
        fprintf(stderr, "capture_dxgi: failed to create staging texture: 0x%08lx\n", hr);
        goto fail;
    }

    ctx->backend_data = st;
    ctx->width  = st->width;
    ctx->height = st->height;
    return 0;

fail:
    if (st->duplication) st->duplication->Release();
    if (st->context)     st->context->Release();
    if (st->device)      st->device->Release();
    free(st);
    return -1;
}

static int dxgi_start(MdCaptureCtx *ctx) {
    (void)ctx;
    ctx->active = true;
    return 0; /* DXGI doesn't need an explicit start — AcquireNextFrame drives it */
}

static int dxgi_get_frame(MdCaptureCtx *ctx, MdFrame *out) {
    auto *st = (DxgiState *)ctx->backend_data;
    if (!st || !st->duplication) return -1;

    /* Release previous frame if still held */
    if (st->frame_acquired) {
        st->duplication->ReleaseFrame();
        st->frame_acquired = false;
    }
    if (st->desktop_resource) {
        st->desktop_resource->Release();
        st->desktop_resource = nullptr;
    }

    /* Acquire the next desktop frame.
     * timeout_ms=100: wait up to 100ms for a new frame.
     * Returns DXGI_ERROR_WAIT_TIMEOUT if the desktop hasn't changed. */
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource *resource = nullptr;
    HRESULT hr;

    /* Retry loop — skip timeout results */
    for (int attempts = 0; attempts < 20; attempts++) {
        hr = st->duplication->AcquireNextFrame(100, &frameInfo, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            continue; /* no new frame yet, retry */

        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                /* Desktop switch or mode change — need to reinitialize */
                fprintf(stderr, "capture_dxgi: access lost, need reinit\n");
                ctx->active = false;
            }
            return -1;
        }
        break; /* got a frame */
    }

    if (!resource) return -1;

    st->frame_acquired = true;
    st->desktop_resource = resource;

    /* Get the desktop texture */
    ID3D11Texture2D *desktopTex = nullptr;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&desktopTex);
    if (FAILED(hr)) return -1;

    /* Copy to staging texture for CPU access */
    st->context->CopyResource(st->staging, desktopTex);
    desktopTex->Release();

    /* Map the staging texture */
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = st->context->Map(st->staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return -1;

    /* Build MdFrame */
    out->width        = st->width;
    out->height       = st->height;
    out->stride       = (uint32_t)mapped.RowPitch;
    out->format       = MD_PIX_CAPTURE_BGRA;
    out->buf_type     = MD_BUF_CPU;
    out->dmabuf_fd    = -1;
    out->data         = (uint8_t *)mapped.pData;
    out->gpu_handle   = nullptr;
    out->data_size    = (size_t)mapped.RowPitch * st->height;
    out->timestamp_ns = frameInfo.LastPresentTime.QuadPart * 100; /* 100ns → ns */
    out->seq          = st->seq++;

    return 0;
}

static void dxgi_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    auto *st = (DxgiState *)ctx->backend_data;
    (void)frame;
    if (!st) return;

    /* Unmap staging texture */
    if (st->staging)
        st->context->Unmap(st->staging, 0);

    /* Release the acquired frame */
    if (st->frame_acquired) {
        st->duplication->ReleaseFrame();
        st->frame_acquired = false;
    }
    if (st->desktop_resource) {
        st->desktop_resource->Release();
        st->desktop_resource = nullptr;
    }
}

static void dxgi_stop(MdCaptureCtx *ctx) {
    ctx->active = false;
}

static void dxgi_destroy(MdCaptureCtx *ctx) {
    auto *st = (DxgiState *)ctx->backend_data;
    if (!st) return;

    if (st->frame_acquired && st->duplication)
        st->duplication->ReleaseFrame();
    if (st->desktop_resource)
        st->desktop_resource->Release();
    if (st->staging)
        st->staging->Release();
    if (st->duplication)
        st->duplication->Release();
    if (st->context)
        st->context->Release();
    if (st->device)
        st->device->Release();

    free(st);
    ctx->backend_data = nullptr;
}

/* ── Singleton vtable ────────────────────────────────────────── */

static const MdCaptureBackend dxgi_backend = {
    dxgi_init,
    dxgi_start,
    dxgi_get_frame,
    dxgi_release_frame,
    dxgi_stop,
    dxgi_destroy,
};

extern "C"
const MdCaptureBackend *md_capture_backend_create(void) {
    return &dxgi_backend;
}

#else /* !_WIN32 */

extern "C"
const MdCaptureBackend *md_capture_backend_create(void) {
    fprintf(stderr, "capture: DXGI backend not available on this platform\n");
    return nullptr;
}

#endif /* _WIN32 */
