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
 *   1. Pick the output(s): config.output, default the primary display
 *   2. D3D11CreateDevice on each output's adapter (DuplicateOutput needs it)
 *   3. DuplicateOutput → IDXGIOutputDuplication per output
 *   4. AcquireNextFrame → ID3D11Texture2D (GPU), pointer position and shape
 *   5. CopyResource to staging texture → Map for CPU access → ReleaseFrame;
 *      in canvas mode ("all" or a rotated output) the mapped copy is
 *      copied, rotated as needed, into the CPU frame and unmapped
 *   6. With config.show_cursor, composite the pointer into the frame
 *   7. Deliver the frame (zero-copy: the mapped staging texture) until
 *      release_frame
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
 * Output selection (md_capture_display_choose): config.output is NULL/"" for
 * the primary display (the one at the desktop origin), a decimal index into
 * the attached outputs in DXGI order (adapter by adapter), a GDI device name
 * such as "\\.\DISPLAY2" (prefix optional, case-insensitive), or "id:2" for
 * \\.\DISPLAY2. An unknown selection fails init and logs the attached
 * outputs.
 *
 * All outputs (md-246): config.output "all" duplicates every attached output
 * (each on its own adapter's device) and composes them into one frame
 * covering the bounding rectangle of their desktop rectangles. That
 * rectangle may start at negative coordinates, and gaps between outputs are
 * opaque black. Each output keeps its own rotation and first-frame tracking,
 * and the pointer follows the output that reports it (see pointer_apply).
 * ctx->output_rect is then the frame's bounding rectangle, so input maps onto
 * the composed frame linearly. Limits:
 *   - DXGI has no waitable handle per duplication, so get_frame polls the
 *     other outputs and blocks on one in turn for at most a frame interval.
 *     An update elsewhere can wait that long, and an idle desktop costs one
 *     wake-up per interval.
 *   - Every update copies that output's whole image into the frame.
 *   - Outputs attached or detached later are picked up on the next restart.
 *     Display changes normally cost the duplications access and restart them.
 *   - A frame larger than the encoder supports is the caller's problem.
 *
 * Desktop placement: ctx->output_rect (md_capture_get_output_rect) is the
 * captured output's rectangle in physical virtual-desktop pixels, which the
 * host copies into MdInputConfig.output_* so SendInput aims at the captured
 * output. DXGI_OUTPUT_DESC.DesktopCoordinates is DPI-virtualized for a
 * process that is not per-monitor DPI aware, so the rectangle comes from
 * GetMonitorInfo on the output's monitor, queried per-monitor aware
 * (win_dpi.h); the frame itself is always physical pixels. Agent and MCP
 * clicks arrive in UI Automation coordinates, which are physical only in a
 * per-monitor aware process (md-m5x4: the host sets that first thing in
 * main), so init logs when the process is not.
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
 * Rotation: Desktop Duplication delivers the desktop image in the output's
 * native (unrotated) orientation, the same size as DXGI_OUTDUPL_DESC.ModeDesc,
 * and reports DXGI_OUTDUPL_DESC.Rotation. For a rotated output the mapped
 * copy is rotated into a CPU frame in desktop orientation (ROTATE90: the
 * image turned 90 degrees clockwise, as in Microsoft's DXGIDesktopDuplication
 * sample and libwebrtc), so frames, ctx->width/height and ctx->output_rect
 * are all in desktop orientation and input maps onto them linearly. The
 * pointer needs no transform: DXGI reports its position in desktop
 * coordinates and its shape upright (the sample draws it unrotated at that
 * position). Unrotated outputs keep the zero-copy path (the frame is the
 * mapped staging texture). A desktop image whose size does not match the
 * mode is handled like a mode change: duplication is lost and restart
 * re-reads the mode.
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

#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include "log.h"
#include "win_dpi.h"
#define MD_LOG_TAG "capture_dxgi"

/* ── Backend-private state ───────────────────────────────────── */

struct PointerRect {
    uint32_t x, y, w, h;   /* w == 0: empty */
};

struct PointerState {
    bool        visible;
    LONG        x, y;          /* shape top-left in frame coordinates */
    UINT        owner;         /* output whose update placed it last */
    LONGLONG    updated;       /* that update's LastMouseUpdateTime */
    bool        has_shape;     /* shape/shape_info hold a drawable shape */
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info;
    uint8_t    *shape;
    UINT        shape_cap;
    /* Frame pixels the composited pointer covers, restored before the
     * pointer is drawn again over the same desktop copy. */
    uint8_t    *under;
    size_t      under_cap;
    PointerRect under_rect;
};

enum { MAX_OUTPUTS = 16 };

/* One duplicated output. */
struct OutputDup {
    ID3D11Device            *device;
    ID3D11DeviceContext     *context;
    IDXGIOutputDuplication  *duplication; /* nullptr after loss until start() */
    ID3D11Texture2D         *staging;     /* CPU copy of its desktop image */
    LUID                     adapter_luid; /* the device's adapter */

    DXGI_OUTPUT_DESC         desc;
    RECT                     rect;        /* physical desktop rectangle */
    UINT                     index;       /* in the attached-output list */
    uint32_t                 image_w;     /* desktop image (and staging) size, */
    uint32_t                 image_h;     /* native orientation */
    int                      turns;       /* clockwise quarter turns image -> frame */
    uint32_t                 width;       /* its frame region, desktop orientation */
    uint32_t                 height;
    uint32_t                 frame_x;     /* the region's top-left in the frame */
    uint32_t                 frame_y;

    bool                     mapped;      /* staging is mapped */
    bool                     delivered;   /* its image reached the frame since (re)duplicating */
};

struct DxgiState {
    OutputDup                out[MAX_OUTPUTS];
    UINT                     count;       /* duplicated outputs: 1, or all of them */
    bool                     all;         /* config.output "all" */
    UINT                     next;        /* output the next sweep starts with */
    uint32_t                 width;       /* frame size */
    uint32_t                 height;
    /* Canvas mode ("all", or a rotated output): the frame is a CPU buffer
     * each output's image is copied into. Otherwise it is out[0]'s mapped
     * staging texture (zero-copy), at map_data/map_pitch. */
    bool                     canvas_mode;
    uint8_t                 *canvas;
    size_t                   canvas_cap;
    bool                     pending;     /* canvas changed since the last delivery */
    uint8_t                 *map_data;
    uint32_t                 map_pitch;

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

/* ── Rotation (pure) ────────────────────────────────────────────── */

/* Clockwise quarter turns from the desktop image to desktop orientation. */
static int rotation_turns(DXGI_MODE_ROTATION r) {
    switch (r) {
    case DXGI_MODE_ROTATION_ROTATE90:  return 1;
    case DXGI_MODE_ROTATION_ROTATE180: return 2;
    case DXGI_MODE_ROTATION_ROTATE270: return 3;
    default:                           return 0; /* IDENTITY, UNSPECIFIED */
    }
}

static void rotated_size(int turns, uint32_t w, uint32_t h, uint32_t *rw, uint32_t *rh) {
    *rw = turns % 2 ? h : w;
    *rh = turns % 2 ? w : h;
}

/* The image pixel shown at frame pixel (x, y) of a w x h image turned TURNS
 * quarter turns clockwise. */
template <int TURNS>
static inline void rotation_source(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   uint32_t *sx, uint32_t *sy) {
    if constexpr (TURNS == 1)      { *sx = y;         *sy = h - 1 - x; }
    else if constexpr (TURNS == 2) { *sx = w - 1 - x; *sy = h - 1 - y; }
    else                           { *sx = w - 1 - y; *sy = x; }
}

template <int TURNS>
static void rotate_tiles(const uint8_t *src, uint32_t src_stride, uint32_t w, uint32_t h,
                         uint8_t *dst, uint32_t dst_stride) {
    uint32_t fw, fh;
    rotated_size(TURNS, w, h, &fw, &fh);
    const uint32_t tile = 32; /* keeps the column-wise source reads in cache */
    for (uint32_t ty = 0; ty < fh; ty += tile) {
        const uint32_t ye = ty + tile < fh ? ty + tile : fh;
        for (uint32_t tx = 0; tx < fw; tx += tile) {
            const uint32_t xe = tx + tile < fw ? tx + tile : fw;
            for (uint32_t y = ty; y < ye; y++) {
                uint8_t *d = dst + (size_t)y * dst_stride;
                for (uint32_t x = tx; x < xe; x++) {
                    uint32_t sx, sy;
                    rotation_source<TURNS>(x, y, w, h, &sx, &sy);
                    memcpy(d + (size_t)x * 4, src + (size_t)sy * src_stride + (size_t)sx * 4, 4);
                }
            }
        }
    }
}

/* Copy a w x h BGRA image into dst turned `turns` quarter turns clockwise
 * (dst is rotated_size(turns, w, h) pixels). */
static void rotate_image(const uint8_t *src, uint32_t src_stride, uint32_t w, uint32_t h,
                         int turns, uint8_t *dst, uint32_t dst_stride) {
    switch (turns) {
    case 1: rotate_tiles<1>(src, src_stride, w, h, dst, dst_stride); break;
    case 2: rotate_tiles<2>(src, src_stride, w, h, dst, dst_stride); break;
    case 3: rotate_tiles<3>(src, src_stride, w, h, dst, dst_stride); break;
    default:
        for (uint32_t y = 0; y < h; y++)
            memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, (size_t)w * 4);
    }
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

/* Fold output `output`'s pointer update into p; that output's region
 * starts at (ox, oy) in the frame. drawn_before is pointer_drawn() before
 * the update, and new_shape whether p->shape was just replaced. Returns
 * whether the composited frame looks different: the pointer appeared or
 * disappeared, or moved or changed shape while drawn.
 * Each duplication reports the pointer Visible only while it is on that
 * output. As in Microsoft's DXGIDesktopDuplication sample, a "not visible"
 * report counts only from the output that placed the pointer last (from any
 * other it just means the pointer is elsewhere), and when two outputs both
 * report it visible the newer update wins. */
static bool pointer_apply(PointerState *p, const DXGI_OUTDUPL_FRAME_INFO &info, UINT output,
                          LONG ox, LONG oy, bool drawn_before, bool new_shape,
                          uint32_t fw, uint32_t fh) {
    const DXGI_OUTDUPL_POINTER_POSITION &pos = info.PointerPosition;
    const LONGLONG when = info.LastMouseUpdateTime.QuadPart; /* 0: position is stale */
    const bool elsewhere = output != p->owner;
    bool moved = false;
    if (when != 0 && (pos.Visible || !elsewhere) &&
        !(pos.Visible && p->visible && elsewhere && p->updated > when)) {
        const LONG x = ox + pos.Position.x, y = oy + pos.Position.y;
        moved = x != p->x || y != p->y;
        p->visible = pos.Visible != FALSE;
        p->x = x;
        p->y = y;
        p->owner = output;
        p->updated = when;
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

struct OutputList {
    UINT              count;
    IDXGIAdapter1    *adapter[MAX_OUTPUTS];
    IDXGIOutput      *output[MAX_OUTPUTS];
    DXGI_OUTPUT_DESC  desc[MAX_OUTPUTS];
    RECT              rect[MAX_OUTPUTS]; /* physical desktop rectangles */
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

/* N of a GDI device name "DISPLAYN" (prefix stripped), or 0. */
static uint32_t display_number(const char *bare) {
    if (_strnicmp(bare, "DISPLAY", 7) != 0 || !bare[7]) return 0;
    const size_t digits = strspn(bare + 7, "0123456789");
    return bare[7 + digits] == '\0' && digits <= 9 ? (uint32_t)strtoul(bare + 7, nullptr, 10) : 0;
}

/* "all": every attached output as one frame (md-246). */
static bool output_is_all(const char *spec) {
    return spec && _stricmp(spec, "all") == 0;
}

/* Index of the output `spec` selects in `descs`, or -1: the shared parser
 * over the outputs' device names without the "\\.\" prefix, the primary
 * flagged, and id N meaning \\.\DISPLAYN. */
static int output_choose(const char *spec, const DXGI_OUTPUT_DESC *descs, UINT count) {
    if (count > MAX_OUTPUTS) count = MAX_OUTPUTS;
    char names[MAX_OUTPUTS][128];
    MdCaptureDisplay displays[MAX_OUTPUTS];
    for (UINT i = 0; i < count; i++) {
        char full[128];
        output_name(descs[i], full, (int)sizeof(full));
        const char *bare = strip_device_prefix(full);
        memcpy(names[i], bare, strlen(bare) + 1);
        displays[i] = { display_number(names[i]), names[i], output_is_primary(descs[i]) };
    }
    return md_capture_display_choose(displays, count, spec ? strip_device_prefix(spec) : spec);
}

/* The output's desktop rectangle in physical pixels: its monitor's, read
 * per-monitor DPI aware. Falls back to DesktopCoordinates. */
static RECT output_physical_rect(const DXGI_OUTPUT_DESC &d) {
    MdPhysicalPixels physical;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    return d.Monitor && GetMonitorInfoW(d.Monitor, &mi) ? mi.rcMonitor
                                                        : d.DesktopCoordinates;
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
                list->rect[list->count] = output_physical_rect(desc);
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
        const RECT &r = list.rect[i];
        MD_LOG_E("  %u: %s %ldx%ld at (%ld,%ld)%s", i, name, r.right - r.left,
                 r.bottom - r.top, r.left, r.top,
                 output_is_primary(list.desc[i]) ? " primary" : "");
    }
}

/* ── Frame layout (pure: output geometry only) ───────────────── */

/* Size an output's frame region for a w x h desktop image shown with
 * `rotation` (desktop orientation). */
static void output_configure(OutputDup *o, uint32_t w, uint32_t h, DXGI_MODE_ROTATION rotation) {
    o->image_w = w;
    o->image_h = h;
    o->turns = rotation_turns(rotation);
    rotated_size(o->turns, w, h, &o->width, &o->height);
}

/* Place outputs in one frame: each region starts at its desktop rectangle's
 * top-left and is its mode's size in desktop orientation, so an image always
 * fits its region. The frame is the regions' bounding rectangle, which may
 * start at negative desktop coordinates and contain gaps. Returns it. */
static RECT frame_place(OutputDup *outs, UINT n) {
    int64_t l = INT64_MAX, t = INT64_MAX, r = INT64_MIN, b = INT64_MIN;
    for (UINT i = 0; i < n; i++) {
        const int64_t x = outs[i].rect.left, y = outs[i].rect.top;
        l = x < l ? x : l;
        t = y < t ? y : t;
        r = x + outs[i].width > r ? x + outs[i].width : r;
        b = y + outs[i].height > b ? y + outs[i].height : b;
    }
    for (UINT i = 0; i < n; i++) {
        outs[i].frame_x = (uint32_t)(outs[i].rect.left - l);
        outs[i].frame_y = (uint32_t)(outs[i].rect.top - t);
    }
    return { (LONG)l, (LONG)t, (LONG)r, (LONG)b };
}

/* Lay the frame out over the duplicated outputs (frame_place) and allocate
 * the CPU frame canvas mode needs; gaps between outputs are opaque black.
 * Publishes the frame size and its desktop rectangle on ctx. */
static HRESULT frame_layout(MdCaptureCtx *ctx, DxgiState *st) {
    const RECT b = frame_place(st->out, st->count);
    st->width = (uint32_t)(b.right - b.left);
    st->height = (uint32_t)(b.bottom - b.top);
    st->canvas_mode = st->all || st->out[0].turns != 0;
    st->pending = false;
    if (st->canvas_mode) {
        if (st->height && st->width > SIZE_MAX / 4 / st->height) return E_OUTOFMEMORY;
        const size_t pixels = (size_t)st->width * st->height;
        if (pixels * 4 > st->canvas_cap) {
            auto *grown = (uint8_t *)realloc(st->canvas, pixels * 4);
            if (!grown) return E_OUTOFMEMORY;
            st->canvas = grown;
            st->canvas_cap = pixels * 4;
        }
        const uint32_t black = 0xFF000000u; /* BGRA 0,0,0,255 */
        for (size_t i = 0; i < pixels; i++) memcpy(st->canvas + i * 4, &black, 4);
    }
    ctx->width = st->width;
    ctx->height = st->height;
    /* One output: its physical rectangle (the frame is scaled onto it if its
     * mode differs). "all": the frame's own bounds. */
    const RECT &r = st->all ? b : st->out[0].rect;
    ctx->output_rect = { (int32_t)r.left, (int32_t)r.top, (uint32_t)(r.right - r.left),
                         (uint32_t)(r.bottom - r.top) };
    return S_OK;
}

/* Copy output o's mapped desktop image into its frame region (canvas mode),
 * first taking the pointer off the frame: the copy replaces the pixels its
 * save-under describes. */
static void output_blit(DxgiState *st, const OutputDup &o, const uint8_t *image, uint32_t pitch) {
    const uint32_t stride = st->width * 4;
    if (st->pointer.under_rect.w) pointer_restore_under(&st->pointer, st->canvas, stride);
    rotate_image(image, pitch, o.image_w, o.image_h, o.turns,
                 st->canvas + (size_t)o.frame_y * stride + (size_t)o.frame_x * 4, stride);
}

/* ── Device and duplication ──────────────────────────────────── */

static void output_unmap(OutputDup *o) {
    if (o->mapped) {
        o->context->Unmap(o->staging, 0);
        o->mapped = false;
    }
}

static void unmap_all(DxgiState *st) {
    for (UINT i = 0; i < st->count; i++) output_unmap(&st->out[i]);
}

static void output_release(OutputDup *o) {
    output_unmap(o);
    safe_release(o->staging);
    safe_release(o->duplication);
    safe_release(o->context);
    safe_release(o->device);
}

static bool outputs_duplicated(const DxgiState *st) {
    for (UINT i = 0; i < st->count; i++)
        if (!st->out[i].duplication) return false;
    return st->count > 0;
}

static void free_state(DxgiState *st) {
    for (OutputDup &o : st->out) output_release(&o);
    free(st->canvas);
    free(st->pointer.shape);
    free(st->pointer.under);
    free(st);
}

/* Create a CPU-accessible staging texture the size of the output's desktop
 * image; the pointer is composited into it in zero-copy mode, so it is
 * writable when show_cursor is set. */
static HRESULT create_staging_texture(const MdCaptureCtx *ctx, OutputDup *o) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = o->image_w;
    desc.Height             = o->image_h;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags     = (UINT)D3D11_CPU_ACCESS_READ |
                              (ctx->config.show_cursor ? (UINT)D3D11_CPU_ACCESS_WRITE : 0u);

    return o->device->CreateTexture2D(&desc, nullptr, &o->staging);
}

/* Duplicate entry i of `outs` into o: create the D3D11 device on its adapter
 * if needed (DuplicateOutput requires it), and size the staging texture and
 * frame region to its current mode and rotation. */
static HRESULT output_open(const MdCaptureCtx *ctx, OutputDup *o, const OutputList &outs, UINT i) {
    DXGI_ADAPTER_DESC1 adapter_desc = {};
    HRESULT hr = outs.adapter[i]->GetDesc1(&adapter_desc);
    if (SUCCEEDED(hr) && o->device &&
        memcmp(&adapter_desc.AdapterLuid, &o->adapter_luid, sizeof(LUID)) != 0)
        output_release(o); /* the slot now holds an output of another adapter */
    if (SUCCEEDED(hr) && !o->device) {
        hr = D3D11CreateDevice(
            outs.adapter[i],
            D3D_DRIVER_TYPE_UNKNOWN,    /* required with an explicit adapter */
            nullptr,                    /* no software module */
            0,                          /* flags */
            nullptr, 0,                 /* default feature levels */
            D3D11_SDK_VERSION,
            &o->device,
            nullptr,
            &o->context);
        if (FAILED(hr)) MD_LOG_I("D3D11CreateDevice failed: 0x%08lx", hr);
        else o->adapter_luid = adapter_desc.AdapterLuid;
    }

    IDXGIOutput1 *output1 = nullptr;
    if (SUCCEEDED(hr))
        hr = outs.output[i]->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
    if (SUCCEEDED(hr)) {
        hr = output1->DuplicateOutput(o->device, &o->duplication);
        if (FAILED(hr))
            MD_LOG_I("DuplicateOutput failed: 0x%08lx\n  Ensure the process has desktop access and is not running in a different session.", hr);
    }
    safe_release(output1);
    o->desc = outs.desc[i];
    o->rect = outs.rect[i];
    o->index = i;
    if (FAILED(hr)) return hr;

    DXGI_OUTDUPL_DESC dupl_desc;
    o->duplication->GetDesc(&dupl_desc);
    const DXGI_MODE_ROTATION rotation = dupl_desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED
                                      ? dupl_desc.Rotation : o->desc.Rotation;
    const bool resized = !o->staging || dupl_desc.ModeDesc.Width != o->image_w ||
                         dupl_desc.ModeDesc.Height != o->image_h;
    output_configure(o, dupl_desc.ModeDesc.Width, dupl_desc.ModeDesc.Height, rotation);
    if (resized) {
        safe_release(o->staging);
        hr = create_staging_texture(ctx, o);
        if (FAILED(hr)) {
            MD_LOG_E("failed to create staging texture: 0x%08lx", hr);
            safe_release(o->duplication);
            return hr;
        }
    }
    char name[128];
    output_name(o->desc, name, (int)sizeof(name));
    const RECT &r = o->rect;
    MD_LOG_I("duplicating output %u %s (%ux%u at %ld,%ld, rotated %d degrees)", o->index, name,
             o->width, o->height, r.left, r.top, o->turns * 90);
    if ((uint32_t)(r.right - r.left) != o->width || (uint32_t)(r.bottom - r.top) != o->height)
        MD_LOG_I("output rectangle %ldx%ld differs from its %ux%u mode; input is scaled onto it",
                 r.right - r.left, r.bottom - r.top, o->width, o->height);
    o->delivered = false;
    return S_OK;
}

/* Resolve config.output (one output, or every one for "all"), duplicate it
 * and lay the frame out. */
static HRESULT open_duplication(MdCaptureCtx *ctx, DxgiState *st) {
    OutputList outs = {};
    HRESULT hr = outputs_enum(&outs);
    if (FAILED(hr)) {
        MD_LOG_I("CreateDXGIFactory1 failed: 0x%08lx", hr);
        return hr;
    }
    const char *spec = ctx->config.output;
    const bool all = output_is_all(spec);
    UINT picks[MAX_OUTPUTS], n = 0;
    if (all) {
        for (UINT i = 0; i < outs.count; i++) picks[n++] = i;
    } else {
        const int pick = output_choose(spec, outs.desc, outs.count);
        if (pick >= 0) picks[n++] = (UINT)pick;
    }
    if (n == 0) {
        outputs_log(outs, all ? nullptr : spec);
        outputs_release(&outs);
        return DXGI_ERROR_NOT_FOUND;
    }
    for (UINT k = n; k < MAX_OUTPUTS; k++) output_release(&st->out[k]); /* gone since */
    for (UINT k = 0; k < n && SUCCEEDED(hr); k++) hr = output_open(ctx, &st->out[k], outs, picks[k]);
    outputs_release(&outs);
    st->count = n;
    st->all = all;
    if (SUCCEEDED(hr)) {
        hr = frame_layout(ctx, st);
        if (FAILED(hr)) MD_LOG_E("out of memory for the %ux%u frame", st->width, st->height);
    }
    if (FAILED(hr)) {
        for (UINT k = 0; k < n; k++) safe_release(st->out[k].duplication);
        return hr;
    }
    if (all)
        MD_LOG_I("capturing %u output(s) as one %ux%u frame at %d,%d", n, st->width, st->height,
                 ctx->output_rect.x, ctx->output_rect.y);
    /* A new duplication resends the pointer; the frame is stale. */
    st->pointer.visible = false;
    st->pointer.has_shape = false;
    st->pointer.under_rect.w = 0;
    st->pointer.owner = 0;
    st->pointer.updated = 0;
    st->next = 0;
    return S_OK;
}

/* AcquireNextFrame or the copy failed. Access loss and device removal
 * invalidate the duplication (every output's: restart re-resolves them all);
 * anything else is reported without tearing it down. */
static int frame_failed(MdCaptureCtx *ctx, DxgiState *st, HRESULT hr) {
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        MD_LOG_I("duplication lost (0x%08lx); restart capture to recover", hr);
        unmap_all(st);
        for (UINT i = 0; i < st->count; i++) safe_release(st->out[i].duplication);
        ctx->active = false;
    } else {
        MD_LOG_I("AcquireNextFrame failed: 0x%08lx", hr);
    }
    return MD_CAPTURE_ERROR;
}

/* What one sweep over the outputs collected. */
struct Sweep {
    bool     desktop;    /* a desktop image reached the frame */
    bool     pointer;    /* the composited pointer changed */
    bool     timed_out;  /* the blocking wait ran out */
    LONGLONG present;    /* newest LastPresentTime */
    LONGLONG mouse;      /* newest LastMouseUpdateTime */
};

/* Take output i's next update, waiting at most `wait` ms, into the frame:
 * consumed if it is not damage (see the header comment), else copied (canvas
 * mode) or mapped (zero-copy). 0, or MD_CAPTURE_ERROR. */
static int take_update(MdCaptureCtx *ctx, DxgiState *st, UINT i, UINT wait, Sweep *sw) {
    OutputDup &o = st->out[i];
    const bool cursor = ctx->config.show_cursor;
    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource *resource = nullptr;
    HRESULT hr = o.duplication->AcquireNextFrame(wait, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (wait) sw->timed_out = true;
        return 0;
    }
    if (FAILED(hr)) return frame_failed(ctx, st, hr);
    const bool desktop = info.LastPresentTime.QuadPart != 0 || !o.delivered;
    bool pointer = false;
    if (cursor) {
        PointerRect r;
        const bool drawn = pointer_drawn(&st->pointer, st->width, st->height, &r);
        if (info.PointerShapeBufferSize)
            pointer_fetch_shape(&st->pointer, o.duplication, info.PointerShapeBufferSize);
        pointer = pointer_apply(&st->pointer, info, i, (LONG)o.frame_x, (LONG)o.frame_y, drawn,
                                info.PointerShapeBufferSize != 0, st->width, st->height);
    }
    if (!desktop && !pointer) {
        st->skipped++;
        safe_release(resource);
        o.duplication->ReleaseFrame();
        return 0;
    }

    /* A pointer-only update reuses the previous copy in the staging texture
     * (zero-copy) or the frame (canvas mode). */
    D3D11_TEXTURE2D_DESC image = {};
    if (desktop) {
        ID3D11Texture2D *texture = nullptr;
        hr = resource
            ? resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture)
            : E_POINTER;
        if (SUCCEEDED(hr)) {
            texture->GetDesc(&image);
            if (image.Width == o.image_w && image.Height == o.image_h) {
                o.context->CopyResource(o.staging, texture);
                if (!st->canvas_mode) st->pointer.under_rect.w = 0; /* copy replaced them */
            }
            safe_release(texture);
        }
    }
    safe_release(resource);
    if (SUCCEEDED(hr) && desktop && (image.Width != o.image_w || image.Height != o.image_h)) {
        o.duplication->ReleaseFrame();
        MD_LOG_I("desktop image %ux%u does not match the %ux%u mode", image.Width,
                 image.Height, o.image_w, o.image_h);
        return frame_failed(ctx, st, DXGI_ERROR_ACCESS_LOST); /* restart re-reads it */
    }
    /* Canvas mode maps the copy only to put a new image into the frame. */
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(hr) && (!st->canvas_mode || desktop))
        hr = o.context->Map(o.staging, 0, cursor && !st->canvas_mode ? D3D11_MAP_READ_WRITE
                                                                     : D3D11_MAP_READ,
                            0, &mapped);
    if (SUCCEEDED(hr) && st->canvas_mode) {
        if (desktop) {
            output_blit(st, o, (const uint8_t *)mapped.pData, (uint32_t)mapped.RowPitch);
            o.context->Unmap(o.staging, 0);
            o.delivered = true;
            st->pending = true; /* delivered by this call or, if it fails, the next */
        }
    } else if (SUCCEEDED(hr)) {
        o.mapped = true;
        st->map_data = (uint8_t *)mapped.pData;
        st->map_pitch = (uint32_t)mapped.RowPitch;
    }
    /* Map waited for the copy, so the desktop image can go back to DWM now
     * rather than being held while the caller encodes. */
    o.duplication->ReleaseFrame();
    if (FAILED(hr)) {
        /* The copy may be stale: the next update copies the desktop. */
        o.delivered = false;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            return frame_failed(ctx, st, hr);
        MD_LOG_I("staging copy failed: 0x%08lx", hr);
        return MD_CAPTURE_ERROR;
    }
    sw->desktop = sw->desktop || desktop;
    sw->pointer = sw->pointer || pointer;
    if (info.LastPresentTime.QuadPart > sw->present) sw->present = info.LastPresentTime.QuadPart;
    if (info.LastMouseUpdateTime.QuadPart > sw->mouse) sw->mouse = info.LastMouseUpdateTime.QuadPart;
    return 0;
}

/* ── Vtable implementation ───────────────────────────────────── */

static int dxgi_init(MdCaptureCtx *ctx) {
    auto *st = (DxgiState *)calloc(1, sizeof(DxgiState));
    if (!st) return -1;
    QueryPerformanceFrequency(&st->qpc_hz);
    if (md_win_dpi_process_awareness() == MD_DPI_PROCESS_NOT_PER_MONITOR)
        MD_LOG_I("process is not per-monitor DPI aware: on displays scaled above 100%% UI "
                 "Automation coordinates (agent and MCP clicks) are DPI-virtualized "
                 "(call md_win_dpi_process_init first)");
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
    if (!outputs_duplicated(st)) {
        /* Recovering from loss. A removed device must be recreated too. */
        unmap_all(st);
        for (OutputDup &o : st->out)
            if (o.device && FAILED(o.device->GetDeviceRemovedReason())) output_release(&o);
        if (FAILED(open_duplication(ctx, st))) return -1;
    }
    ctx->active = true; /* AcquireNextFrame drives capture; nothing to start */
    return 0;
}

static int dxgi_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms) {
    auto *st = (DxgiState *)ctx->backend_data;
    if (!st || !ctx->active || !outputs_duplicated(st)) return MD_CAPTURE_ERROR;

    /* A previous frame the caller did not release. */
    unmap_all(st);

    /* An unchanged desktop is DXGI_ERROR_WAIT_TIMEOUT: healthy idle, not a
     * device error. Updates that are not damage (see the header comment) are
     * consumed and the wait continues until the deadline. One output waits
     * in AcquireNextFrame for the whole remaining time. Several ("all") have
     * no common waitable handle, so each sweep polls every output and then
     * blocks on one of them, in turn, for at most a frame interval: an update
     * on another output waits at most that long. */
    const bool cursor = ctx->config.show_cursor;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    const UINT fps = ctx->config.target_fps ? ctx->config.target_fps : 60;
    const UINT slice = st->count > 1 ? (1000 + fps - 1) / fps : INFINITE - 1;
    Sweep sw = {};
    sw.desktop = st->pending; /* copied by a call that then failed */
    for (;;) {
        if (!ctx->active) return MD_CAPTURE_ERROR; /* stopped meanwhile */
        UINT wait = 0, remaining = 0;
        sw.timed_out = false;
        for (UINT k = 0; k < st->count; k++) {
            const UINT i = (st->next + k) % st->count;
            if (k + 1 == st->count) {
                /* Block only while there is nothing to deliver yet. */
                const ULONGLONG now = GetTickCount64();
                remaining = now < deadline ? (UINT)(deadline - now) : 0;
                wait = sw.desktop || sw.pointer ? 0 : remaining < slice ? remaining : slice;
            }
            const int rc = take_update(ctx, st, i, k + 1 == st->count ? wait : 0, &sw);
            if (rc != 0) return rc;
        }
        st->next = (st->next + 1) % st->count;
        if (sw.desktop || sw.pointer) break;
        if (remaining == 0 || (sw.timed_out && wait == remaining)) return MD_CAPTURE_IDLE;
    }

    uint8_t *data = st->canvas;
    uint32_t stride = st->width * 4;
    if (!st->canvas_mode) {
        data = st->map_data;
        stride = st->map_pitch;
    }
    if (cursor && !pointer_composite(&st->pointer, data, stride, st->width, st->height)) {
        unmap_all(st);
        for (UINT i = 0; i < st->count; i++) st->out[i].delivered = false;
        MD_LOG_E("out of memory compositing the pointer");
        return MD_CAPTURE_ERROR;
    }
    if (!st->canvas_mode) st->out[0].delivered = true;
    st->pending = false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG ticks = sw.present ? sw.present : sw.mouse ? sw.mouse : now.QuadPart;
    uint64_t ts = qpc_to_ns(st, ticks);
    if (ts < st->last_ts) ts = st->last_ts; /* keep timestamps monotonic */
    st->last_ts = ts;
    out->width        = st->width;
    out->height       = st->height;
    out->stride       = stride;
    out->format       = MD_PIX_CAPTURE_BGRA;
    out->buf_type     = MD_BUF_CPU;
    out->dmabuf_fd    = -1;
    out->data         = data;
    out->gpu_handle   = nullptr;
    out->data_size    = (size_t)stride * st->height;
    out->timestamp_ns = ts;
    out->seq          = st->seq++;
    return 0;
}

static void dxgi_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    auto *st = (DxgiState *)ctx->backend_data;
    (void)frame;
    if (st) unmap_all(st);
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
