/*
 * metadesk — capture_dxgi.cpp
 * Windows capture backend: DXGI Desktop Duplication (spec §2.3.1).
 *
 * Uses:
 *   - IDXGIFactory1 to enumerate adapters and their attached outputs
 *   - IDXGIOutputDuplication for screen capture
 *   - ID3D11Device / ID3D11DeviceContext on the output's adapter
 *   - a CPU-accessible staging texture for the frame data
 *
 * Flow:
 *   1. Pick the output: config.output, default the primary display
 *   2. D3D11CreateDevice on that output's adapter (DuplicateOutput needs it)
 *   3. DuplicateOutput → IDXGIOutputDuplication
 *   4. AcquireNextFrame → ID3D11Texture2D (GPU), pointer position and shape
 *   5. CopyResource to staging texture → Map for CPU access → ReleaseFrame
 *   6. With config.show_cursor, composite the pointer into the mapped copy
 *   7. Deliver the mapped staging texture until release_frame unmaps it
 *
 * HAL contract (capture.h): get_frame waits at most timeout_ms and returns
 * 0, MD_CAPTURE_IDLE (no damage) or MD_CAPTURE_ERROR (stopped, or
 * duplication lost). Losing duplication (desktop switch such as UAC or the
 * lock screen, display mode change, device removal) clears ctx->active; the
 * HAL's recovery is stop + start, and start re-creates the duplication
 * (re-resolving the output selection).
 * stop only clears ctx->active, so it is safe while another thread waits in
 * get_frame; everything else runs on the capture thread or after it joined.
 *
 * Output selection: config.output is NULL/"" for the primary display (the
 * one at the desktop origin), a decimal index into the attached outputs in
 * DXGI order (adapter by adapter), or a GDI device name such as
 * "\\.\DISPLAY2" (prefix optional, case-insensitive). An unknown selection
 * fails init and logs the attached outputs.
 *
 * Damage: an unchanged desktop is IDLE. A pointer-only update (LastPresentTime
 * 0) leaves the desktop image unchanged. Without show_cursor it is IDLE, since
 * the frame would be identical. With show_cursor it is damage when it changes
 * the composited pointer: a move while visible, show, hide, or a new shape
 * while visible. The pointer is then repainted over the previous copy (the
 * pixels it covered are restored first) without copying the desktop again.
 * The first frame after (re)duplicating is always delivered: the caller has
 * no image yet, and that frame carries the whole desktop.
 *
 * Pointer: Desktop Duplication never draws the pointer into the desktop
 * image. PointerPosition.Position is already the top-left of the shape, not
 * the hot spot: DXGI documents that the hot spot is not used to place the
 * shape, so the shape is drawn at Position and HotSpot is informative only.
 * Monochrome (AND/XOR masks), color (premultiplied alpha: a 50% red cursor
 * pixel arrives as 0x80800000) and masked-color (alpha 0 replaces, 0xFF
 * XORs) shapes are supported. PointerPosition.Visible is false while the
 * pointer is hidden or on another output, and also for a software cursor:
 * when the driver cannot show a shape as a hardware pointer (the Basic
 * Display adapter with monochrome shapes, for one), DWM draws it into the
 * desktop image itself, so it is captured with or without show_cursor.
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
#include <cstring>
#include "log.h"
#define MD_LOG_TAG "capture_dxgi"

/* ── Backend-private state ───────────────────────────────────── */

struct PointerRect {
    uint32_t x, y, w, h;   /* w == 0: empty */
};

struct PointerState {
    bool        visible;
    LONG        x, y;          /* shape top-left in output coordinates */
    bool        has_shape;     /* shape/shape_info hold a drawable shape */
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
    uint8_t    *shape;
    UINT        shape_cap;
    /* Staging pixels the composited pointer covers, restored before the
     * pointer is drawn again over the same desktop copy. */
    uint8_t    *under;
    size_t      under_cap;
    PointerRect under_rect;
};

struct DxgiState {
    ID3D11Device            *device;
    ID3D11DeviceContext     *context;
    IDXGIOutputDuplication  *duplication; /* nullptr after loss until start() */
    ID3D11Texture2D         *staging;     /* CPU copy of the desktop */
    LUID                     adapter_luid; /* the device's adapter */

    DXGI_OUTPUT_DESC         output_desc; /* the duplicated output */
    UINT                     output_index;
    uint32_t                 width;
    uint32_t                 height;

    bool                     mapped;      /* staging is borrowed by the caller */
    bool                     delivered;   /* a frame since (re)duplicating */
    uint32_t                 seq;
    uint32_t                 skipped;     /* updates consumed as no damage */
    uint64_t                 last_ts;
    LARGE_INTEGER            qpc_hz;
    PointerState             pointer;
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

/* ── Pointer compositing (pure: frame and shape buffers only) ── */

static uint32_t pointer_shape_height(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &si) {
    /* A monochrome shape is the AND mask followed by the XOR mask. */
    return si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? si.Height / 2
                                                                 : si.Height;
}

/* Whether `size` bytes of shape data described by `si` can be drawn. */
static bool pointer_shape_valid(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &si, UINT size) {
    uint64_t row = 0;
    switch (si.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        if (si.Height % 2) return false;
        row = ((uint64_t)si.Width + 7) / 8;
        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        row = (uint64_t)si.Width * 4;
        break;
    default:
        return false;
    }
    return si.Width > 0 && si.Height > 0 && si.Pitch >= row &&
           (uint64_t)si.Pitch * (si.Height - 1) + row <= size;
}

/* The part of a shape at (x, y) that lies inside a fw x fh frame. */
static bool pointer_clip(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &si, LONG x, LONG y,
                         uint32_t fw, uint32_t fh, PointerRect *r) {
    const int64_t x0 = x > 0 ? x : 0, y0 = y > 0 ? y : 0;
    const int64_t xe = (int64_t)x + si.Width, ye = (int64_t)y + pointer_shape_height(si);
    const int64_t x1 = xe < (int64_t)fw ? xe : (int64_t)fw;
    const int64_t y1 = ye < (int64_t)fh ? ye : (int64_t)fh;
    if (x0 >= x1 || y0 >= y1) return false;
    *r = { (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
    return true;
}

/* Draw the clipped rectangle r of a validated shape placed at (x, y) into a
 * BGRA frame. Only B, G and R change; the frame's alpha is kept. */
static void pointer_blend(uint8_t *frame, uint32_t stride, const PointerRect &r,
                          const uint8_t *shape, const DXGI_OUTDUPL_POINTER_SHAPE_INFO &si,
                          LONG x, LONG y) {
    const uint32_t xor_rows = si.Height / 2; /* monochrome only */
    for (uint32_t row = 0; row < r.h; row++) {
        const uint32_t sy = (uint32_t)((int64_t)r.y + row - y);
        uint8_t *dst = frame + (size_t)(r.y + row) * stride + (size_t)r.x * 4;
        for (uint32_t col = 0; col < r.w; col++, dst += 4) {
            const uint32_t sx = (uint32_t)((int64_t)r.x + col - x);
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                /* AND 1 keeps the screen, 0 clears it; XOR 1 then inverts. */
                const uint8_t bit = (uint8_t)(0x80u >> (sx % 8));
                const size_t at = (size_t)sy * si.Pitch + sx / 8;
                const bool keep = (shape[at] & bit) != 0;
                const bool invert = (shape[at + (size_t)xor_rows * si.Pitch] & bit) != 0;
                for (int c = 0; c < 3; c++) {
                    const uint8_t v = keep ? dst[c] : 0;
                    dst[c] = invert ? (uint8_t)~v : v;
                }
                continue;
            }
            const uint8_t *src = shape + (size_t)sy * si.Pitch + (size_t)sx * 4;
            if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                /* Premultiplied over; clamp colour that exceeds its alpha. */
                const unsigned a = src[3];
                for (int c = 0; c < 3; c++) {
                    const unsigned v = src[c] + (dst[c] * (255u - a) + 127u) / 255u;
                    dst[c] = (uint8_t)(v > 255u ? 255u : v);
                }
            } else if (src[3] == 0) { /* masked color: replace */
                for (int c = 0; c < 3; c++) dst[c] = src[c];
            } else {                  /* masked color: XOR */
                for (int c = 0; c < 3; c++) dst[c] = (uint8_t)(dst[c] ^ src[c]);
            }
        }
    }
}

static bool pointer_save_under(PointerState *p, const uint8_t *frame, uint32_t stride,
                               const PointerRect &r) {
    const size_t row = (size_t)r.w * 4, need = row * r.h;
    if (need > p->under_cap) {
        auto *grown = (uint8_t *)realloc(p->under, need);
        if (!grown) return false;
        p->under = grown;
        p->under_cap = need;
    }
    for (uint32_t y = 0; y < r.h; y++)
        memcpy(p->under + row * y, frame + (size_t)(r.y + y) * stride + (size_t)r.x * 4, row);
    p->under_rect = r;
    return true;
}

static void pointer_restore_under(PointerState *p, uint8_t *frame, uint32_t stride) {
    const PointerRect r = p->under_rect;
    const size_t row = (size_t)r.w * 4;
    for (uint32_t y = 0; y < r.h; y++)
        memcpy(frame + (size_t)(r.y + y) * stride + (size_t)r.x * 4, p->under + row * y, row);
    p->under_rect.w = 0;
}

/* Whether the pointer lands on a fw x fh frame, and where. */
static bool pointer_drawn(const PointerState *p, uint32_t fw, uint32_t fh, PointerRect *r) {
    return p->visible && p->has_shape &&
           pointer_clip(p->shape_info, p->x, p->y, fw, fh, r);
}

/* Repaint the pointer into a frame that holds either a fresh desktop copy
 * (under_rect empty) or the previous composited copy. Fails only when the
 * pixels under the pointer cannot be saved (out of memory). */
static bool pointer_composite(PointerState *p, uint8_t *frame, uint32_t stride,
                              uint32_t fw, uint32_t fh) {
    pointer_restore_under(p, frame, stride);
    PointerRect r;
    if (!pointer_drawn(p, fw, fh, &r)) return true;
    /* Without saved pixels the pointer could not be removed again. */
    if (!pointer_save_under(p, frame, stride, r)) return false;
    pointer_blend(frame, stride, r, p->shape, p->shape_info, p->x, p->y);
    return true;
}

/* Fold one frame's pointer update into p. drawn_before is pointer_drawn()
 * before the update, and new_shape whether p->shape was just replaced.
 * Returns whether the composited frame looks different: the pointer
 * appeared or disappeared, or moved or changed shape while drawn. */
static bool pointer_apply(PointerState *p, const DXGI_OUTDUPL_FRAME_INFO &info,
                          bool drawn_before, bool new_shape, uint32_t fw, uint32_t fh) {
    bool moved = false;
    if (info.LastMouseUpdateTime.QuadPart != 0) { /* else position is stale */
        const DXGI_OUTDUPL_POINTER_POSITION &pos = info.PointerPosition;
        moved = pos.Position.x != p->x || pos.Position.y != p->y;
        p->visible = pos.Visible != FALSE;
        p->x = pos.Position.x;
        p->y = pos.Position.y;
    }
    PointerRect r;
    const bool drawn = pointer_drawn(p, fw, fh, &r);
    return drawn != drawn_before || (drawn && (moved || new_shape));
}

/* Replace the pointer shape; call between AcquireNextFrame and ReleaseFrame.
 * An unusable shape is not drawn until the next valid one arrives. */
static void pointer_fetch_shape(PointerState *p, IDXGIOutputDuplication *dup, UINT size) {
    p->has_shape = false;
    if (size > p->shape_cap) {
        auto *grown = (uint8_t *)realloc(p->shape, size);
        if (!grown) return;
        p->shape = grown;
        p->shape_cap = size;
    }
    UINT used = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
    HRESULT hr = dup->GetFramePointerShape(p->shape_cap, p->shape, &used, &si);
    if (used == 0 || used > p->shape_cap) used = size;
    if (SUCCEEDED(hr) && pointer_shape_valid(si, used)) {
        p->shape_info = si;
        p->has_shape = true;
    } else {
        MD_LOG_I("pointer shape not drawn (0x%08lx, type %u, %ux%u, pitch %u, %u bytes)",
                 hr, si.Type, si.Width, si.Height, si.Pitch, used);
    }
}

/* ── Output selection ────────────────────────────────────────── */

enum { MAX_OUTPUTS = 16 };

struct OutputList {
    UINT              count;
    IDXGIAdapter1    *adapter[MAX_OUTPUTS];
    IDXGIOutput      *output[MAX_OUTPUTS];
    DXGI_OUTPUT_DESC  desc[MAX_OUTPUTS];
};

/* The primary display is the one at the virtual desktop origin. */
static bool output_is_primary(const DXGI_OUTPUT_DESC &d) {
    return d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0;
}

static void output_name(const DXGI_OUTPUT_DESC &d, char *buf, int size) {
    if (WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, buf, size, nullptr, nullptr) <= 0)
        buf[0] = '\0';
}

static const char *strip_device_prefix(const char *name) {
    return strncmp(name, "\\\\.\\", 4) == 0 ? name + 4 : name;
}

/* Index of the output `spec` selects in `descs`, or -1. */
static int output_choose(const char *spec, const DXGI_OUTPUT_DESC *descs, UINT count) {
    if (!spec || !*spec) {
        for (UINT i = 0; i < count; i++)
            if (output_is_primary(descs[i])) return (int)i;
        return count ? 0 : -1;
    }
    const size_t digits = strspn(spec, "0123456789");
    if (spec[digits] == '\0' && digits <= 4) {
        const UINT index = (UINT)atoi(spec);
        return index < count ? (int)index : -1;
    }
    for (UINT i = 0; i < count; i++) {
        char name[128];
        output_name(descs[i], name, (int)sizeof(name));
        if (_stricmp(strip_device_prefix(spec), strip_device_prefix(name)) == 0)
            return (int)i;
    }
    return -1;
}

/* Attached outputs of every adapter, in DXGI enumeration order. */
static HRESULT outputs_enum(OutputList *list) {
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    if (FAILED(hr)) return hr;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT a = 0; list->count < MAX_OUTPUTS &&
                     SUCCEEDED(factory->EnumAdapters1(a, &adapter)); a++) {
        IDXGIOutput *output = nullptr;
        for (UINT o = 0; list->count < MAX_OUTPUTS &&
                         SUCCEEDED(adapter->EnumOutputs(o, &output)); o++) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                adapter->AddRef();
                list->adapter[list->count] = adapter;
                list->output[list->count] = output;
                list->desc[list->count++] = desc;
                output = nullptr;
            }
            safe_release(output);
        }
        safe_release(adapter);
    }
    safe_release(factory);
    return S_OK;
}

static void outputs_release(OutputList *list) {
    for (UINT i = 0; i < list->count; i++) {
        safe_release(list->output[i]);
        safe_release(list->adapter[i]);
    }
    list->count = 0;
}

static void outputs_log(const OutputList &list, const char *spec) {
    if (!spec || !*spec) {
        MD_LOG_I("no display output is attached to this session's desktop");
        return;
    }
    MD_LOG_E("capture output \"%s\" not found; attached outputs (select by index or device name):%s",
             spec, list.count ? "" : " none");
    for (UINT i = 0; i < list.count; i++) {
        char name[128];
        output_name(list.desc[i], name, (int)sizeof(name));
        const RECT &r = list.desc[i].DesktopCoordinates;
        MD_LOG_E("  %u: %s %ldx%ld at (%ld,%ld)%s", i, name, r.right - r.left,
                 r.bottom - r.top, r.left, r.top,
                 output_is_primary(list.desc[i]) ? " primary" : "");
    }
}

/* ── Device and duplication ──────────────────────────────────── */

static void unmap_staging(DxgiState *st) {
    if (st->mapped) {
        st->context->Unmap(st->staging, 0);
        st->mapped = false;
    }
}

static void release_device(DxgiState *st) {
    unmap_staging(st);
    safe_release(st->staging);
    safe_release(st->duplication);
    safe_release(st->context);
    safe_release(st->device);
}

static void free_state(DxgiState *st) {
    release_device(st);
    free(st->pointer.shape);
    free(st->pointer.under);
    free(st);
}

/* Create a CPU-accessible staging texture matching the display size; the
 * pointer is composited into it, so it is writable when show_cursor is set. */
static HRESULT create_staging_texture(const MdCaptureCtx *ctx, DxgiState *st) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = st->width;
    desc.Height             = st->height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags     = (UINT)D3D11_CPU_ACCESS_READ |
                              (ctx->config.show_cursor ? (UINT)D3D11_CPU_ACCESS_WRITE : 0u);

    return st->device->CreateTexture2D(&desc, nullptr, &st->staging);
}

/* Resolve config.output, create the D3D11 device on its adapter if needed,
 * duplicate it and size the staging texture to its current mode. */
static HRESULT open_duplication(MdCaptureCtx *ctx, DxgiState *st) {
    OutputList outs = {};
    HRESULT hr = outputs_enum(&outs);
    if (FAILED(hr)) {
        MD_LOG_I("CreateDXGIFactory1 failed: 0x%08lx", hr);
        return hr;
    }
    const int pick = output_choose(ctx->config.output, outs.desc, outs.count);
    if (pick < 0) {
        outputs_log(outs, ctx->config.output);
        outputs_release(&outs);
        return DXGI_ERROR_NOT_FOUND;
    }

    DXGI_ADAPTER_DESC1 adapter_desc = {};
    hr = outs.adapter[pick]->GetDesc1(&adapter_desc);
    if (SUCCEEDED(hr) && st->device &&
        memcmp(&adapter_desc.AdapterLuid, &st->adapter_luid, sizeof(LUID)) != 0)
        release_device(st); /* the selection moved to another adapter */
    if (SUCCEEDED(hr) && !st->device) {
        hr = D3D11CreateDevice(
            outs.adapter[pick],
            D3D_DRIVER_TYPE_UNKNOWN,    /* required with an explicit adapter */
            nullptr,                    /* no software module */
            0,                          /* flags */
            nullptr, 0,                 /* default feature levels */
            D3D11_SDK_VERSION,
            &st->device,
            nullptr,
            &st->context);
        if (FAILED(hr)) MD_LOG_I("D3D11CreateDevice failed: 0x%08lx", hr);
        else st->adapter_luid = adapter_desc.AdapterLuid;
    }

    IDXGIOutput1 *output1 = nullptr;
    if (SUCCEEDED(hr))
        hr = outs.output[pick]->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
    if (SUCCEEDED(hr)) {
        hr = output1->DuplicateOutput(st->device, &st->duplication);
        if (FAILED(hr))
            MD_LOG_I("DuplicateOutput failed: 0x%08lx\n  Ensure the process has desktop access and is not running in a different session.", hr);
    }
    safe_release(output1);
    st->output_desc = outs.desc[pick];
    st->output_index = (UINT)pick;
    outputs_release(&outs);
    if (FAILED(hr)) return hr;

    DXGI_OUTDUPL_DESC dupl_desc;
    st->duplication->GetDesc(&dupl_desc);
    if (!st->staging || dupl_desc.ModeDesc.Width != st->width ||
        dupl_desc.ModeDesc.Height != st->height) {
        safe_release(st->staging);
        st->width  = dupl_desc.ModeDesc.Width;
        st->height = dupl_desc.ModeDesc.Height;
        hr = create_staging_texture(ctx, st);
        if (FAILED(hr)) {
            MD_LOG_E("failed to create staging texture: 0x%08lx", hr);
            safe_release(st->duplication);
            return hr;
        }
    }
    char name[128];
    output_name(st->output_desc, name, (int)sizeof(name));
    const RECT &r = st->output_desc.DesktopCoordinates;
    MD_LOG_I("duplicating output %u %s (%ux%u at %ld,%ld)", st->output_index, name,
             st->width, st->height, r.left, r.top);
    /* A new duplication resends the pointer; the staging copy is stale. */
    st->pointer.visible = false;
    st->pointer.has_shape = false;
    st->pointer.under_rect.w = 0;
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
        if (st->device && FAILED(st->device->GetDeviceRemovedReason()))
            release_device(st);
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
     * device error. Updates that are not damage (see the header comment) are
     * consumed and the wait continues until the deadline. */
    const bool cursor = ctx->config.show_cursor;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *resource = nullptr;
    bool desktop = false;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const UINT wait = now < deadline ? (UINT)(deadline - now) : 0;
        HRESULT hr = st->duplication->AcquireNextFrame(wait, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return MD_CAPTURE_IDLE;
        if (FAILED(hr)) return frame_failed(ctx, st, hr);
        desktop = info.LastPresentTime.QuadPart != 0 || !st->delivered;
        bool pointer = false;
        if (cursor) {
            PointerRect r;
            const bool drawn = pointer_drawn(&st->pointer, st->width, st->height, &r);
            if (info.PointerShapeBufferSize)
                pointer_fetch_shape(&st->pointer, st->duplication, info.PointerShapeBufferSize);
            pointer = pointer_apply(&st->pointer, info, drawn, info.PointerShapeBufferSize != 0,
                                    st->width, st->height);
        }
        if (desktop || pointer) break;
        st->skipped++;
        safe_release(resource);
        st->duplication->ReleaseFrame();
        if (wait == 0) return MD_CAPTURE_IDLE;
    }

    /* A pointer-only update reuses the previous copy in the staging texture. */
    HRESULT hr = S_OK;
    if (desktop) {
        ID3D11Texture2D *texture = nullptr;
        hr = resource
            ? resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture)
            : E_POINTER;
        if (SUCCEEDED(hr)) {
            st->context->CopyResource(st->staging, texture);
            safe_release(texture);
            st->pointer.under_rect.w = 0; /* the copy replaced those pixels */
        }
    }
    safe_release(resource);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(hr))
        hr = st->context->Map(st->staging, 0, cursor ? D3D11_MAP_READ_WRITE : D3D11_MAP_READ,
                              0, &mapped);
    /* Map waited for the copy, so the desktop image can go back to DWM now
     * rather than being held while the caller encodes. */
    st->duplication->ReleaseFrame();
    if (FAILED(hr)) {
        /* The staging copy may be stale: the next frame copies the desktop. */
        st->delivered = false;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            return frame_failed(ctx, st, hr);
        MD_LOG_I("staging copy failed: 0x%08lx", hr);
        return MD_CAPTURE_ERROR;
    }
    st->mapped = true;
    if (cursor && !pointer_composite(&st->pointer, (uint8_t *)mapped.pData,
                                     (uint32_t)mapped.RowPitch, st->width, st->height)) {
        unmap_staging(st);
        st->delivered = false;
        MD_LOG_E("out of memory compositing the pointer");
        return MD_CAPTURE_ERROR;
    }
    st->delivered = true;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG ticks = info.LastPresentTime.QuadPart ? info.LastPresentTime.QuadPart
                         : info.LastMouseUpdateTime.QuadPart ? info.LastMouseUpdateTime.QuadPart
                         : now.QuadPart;
    uint64_t ts = qpc_to_ns(st, ticks);
    if (ts < st->last_ts) ts = st->last_ts; /* keep timestamps monotonic */
    st->last_ts = ts;
    out->width        = st->width;
    out->height       = st->height;
    out->stride       = (uint32_t)mapped.RowPitch;
    out->format       = MD_PIX_CAPTURE_BGRA;
    out->buf_type     = MD_BUF_CPU;
    out->dmabuf_fd    = -1;
    out->data         = (uint8_t *)mapped.pData;
    out->gpu_handle   = nullptr;
    out->data_size    = (size_t)mapped.RowPitch * st->height;
    out->timestamp_ns = ts;
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
