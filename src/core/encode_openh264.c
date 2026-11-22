/* Direct, optional OpenH264 2.7.0 ABI. No OpenH264 implementation is linked.
 * Public C vtable; see the documented ForceIntraFrame signature correction
 * in openh264_headers/README.md. Runtime installation: encoder-runtime.md. */
#include "encode_openh264.h"
#include "openh264_headers/codec_api.h"
#include "openh264_headers/codec_ver.h"
#include <libyuv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
typedef HMODULE Library;
static Library load_library(const char *path) {
    wchar_t wide[32768];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768)) return NULL;
    return LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
static void *symbol(Library lib, const char *name) { return (void *)GetProcAddress(lib, name); }
static void close_library(Library lib) { FreeLibrary(lib); }
#if defined(_M_ARM64) || defined(__aarch64__)
#define MD_OH_NAME "openh264-2.7.0-win-arm64.dll"
#elif defined(_WIN64)
#define MD_OH_NAME "openh264-2.7.0-win64.dll"
#else
#define MD_OH_NAME "openh264-2.7.0-win32.dll"
#endif
#else
#include <dlfcn.h>
typedef void *Library;
static Library load_library(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *symbol(Library lib, const char *name) { return dlsym(lib, name); }
static void close_library(Library lib) { dlclose(lib); }
#ifdef __APPLE__
#define MD_OH_NAME "libopenh264.8.dylib"
#else
#define MD_OH_NAME "libopenh264.so.8"
#endif
#endif

struct MdOpenH264 {
    Library library;
    ISVCEncoder *encoder;
    void (*destroy)(ISVCEncoder *);
    uint8_t *i420, *packet;
    size_t packet_capacity;
    int width, height, fps;
    bool initialized;
};

static bool absolute_path(const char *path) {
#ifdef _WIN32
    return (strlen(path) >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
           (path[0] == '\\' && path[1] == '\\');
#else
    return path[0] == '/';
#endif
}

static bool bind_library(MdOpenH264 *enc, const char *path) {
    Library lib = load_library(path);
    if (!lib) return false;
    void (*version)(OpenH264Version *) = (void (*)(OpenH264Version *))symbol(lib, "WelsGetCodecVersionEx");
    int (*create)(ISVCEncoder **) = (int (*)(ISVCEncoder **))symbol(lib, "WelsCreateSVCEncoder");
    void (*destroy)(ISVCEncoder *) = (void (*)(ISVCEncoder *))symbol(lib, "WelsDestroySVCEncoder");
    OpenH264Version v = {0};
    if (version) version(&v);
    /* Reject before calling any version-dependent vtable or structure. */
    if (!version || !create || !destroy || v.uMajor != OPENH264_MAJOR ||
        v.uMinor != OPENH264_MINOR || v.uRevision != OPENH264_REVISION ||
        v.uReserved != OPENH264_RESERVED) {
        fprintf(stderr, "OpenH264: rejected %s (need 2.7.0.2610, found %u.%u.%u.%u)\n",
                path, v.uMajor, v.uMinor, v.uRevision, v.uReserved);
        close_library(lib);
        return false;
    }
    enc->library = lib;
    enc->destroy = destroy;
    if (create(&enc->encoder) != 0 || !enc->encoder) return false;
    fprintf(stderr, "OpenH264: loaded %s (2.7.0.2610)\n", path);
    return true;
}

MdOpenH264 *md_openh264_create(const MdEncoderConfig *cfg) {
    MdOpenH264 *enc = calloc(1, sizeof(*enc));
    if (!enc) return NULL;
    const char *explicit_path = cfg->openh264_path;
    if (!explicit_path || !*explicit_path) explicit_path = getenv("METADESK_OPENH264_PATH");
    bool loaded = false;
    if (explicit_path && *explicit_path) {
        /* An explicit path is authoritative: never substitute another binary. */
        if (!absolute_path(explicit_path))
            fprintf(stderr, "OpenH264: explicit library path must be absolute\n");
        else loaded = bind_library(enc, explicit_path);
    } else {
        loaded = bind_library(enc, MD_OH_NAME);
        if (!loaded && !enc->library) {
            char cache[4096];
#ifdef _WIN32
            const char *base = getenv("LOCALAPPDATA");
            if (base && *base && absolute_path(base)) {
                int n = snprintf(cache, sizeof(cache), "%s/metadesk/openh264/%s", base, MD_OH_NAME);
#elif defined(__APPLE__)
            const char *base = getenv("HOME");
            if (base && *base && absolute_path(base)) {
                int n = snprintf(cache, sizeof(cache), "%s/Library/Caches/metadesk/openh264/%s", base, MD_OH_NAME);
#else
            const char *base = getenv("XDG_CACHE_HOME");
            const char *home = getenv("HOME");
            if ((base && *base == '/') || (home && *home == '/')) {
                int n = base && *base == '/'
                      ? snprintf(cache, sizeof(cache), "%s/metadesk/openh264/%s", base, MD_OH_NAME)
                      : snprintf(cache, sizeof(cache), "%s/.cache/metadesk/openh264/%s", home, MD_OH_NAME);
#endif
                if (n > 0 && (size_t)n < sizeof(cache)) loaded = bind_library(enc, cache);
            }
        }
    }
    if (!loaded) {
        fprintf(stderr, "OpenH264 unavailable: install Cisco's 2.7.0 binary from "
                "https://github.com/cisco/openh264/releases/tag/v2.7.0, verify its published "
                "signature/checksum, decompress it, then set --openh264-path (or "
                "METADESK_OPENH264_PATH) to the absolute library filename. "
                "metadesk never downloads or bundles this binary.\n");
        md_openh264_destroy(enc);
        return NULL;
    }
    enc->width = (int)cfg->width;
    enc->height = (int)cfg->height;
    enc->fps = cfg->fps ? (int)cfg->fps : MD_ENCODER_DEFAULT_FPS;
    SEncParamExt params = {0};
    if ((*enc->encoder)->GetDefaultParams(enc->encoder, &params) != 0) {
        md_openh264_destroy(enc); return NULL;
    }
    params.iUsageType = SCREEN_CONTENT_REAL_TIME;
    params.iPicWidth = enc->width;
    params.iPicHeight = enc->height;
    params.iTargetBitrate = cfg->bitrate ? (int)cfg->bitrate : MD_ENCODER_DEFAULT_BITRATE;
    params.iRCMode = RC_BITRATE_MODE;
    params.fMaxFrameRate = (float)enc->fps;
    params.iTemporalLayerNum = 1;
    params.iSpatialLayerNum = 1;
    params.iComplexityMode = LOW_COMPLEXITY;
    params.uiIntraPeriod = (unsigned)enc->fps * 2u;
    params.iMultipleThreadIdc = 1;
    params.bEnableFrameSkip = true;
    params.bEnableAdaptiveQuant = false;
    params.bEnableBackgroundDetection = false;
    params.iMaxBitrate = MD_ENCODER_MAX_BITRATE;
    params.sSpatialLayers[0].iVideoWidth = enc->width;
    params.sSpatialLayers[0].iVideoHeight = enc->height;
    params.sSpatialLayers[0].fFrameRate = (float)enc->fps;
    params.sSpatialLayers[0].iSpatialBitrate = params.iTargetBitrate;
    params.sSpatialLayers[0].iMaxSpatialBitrate = MD_ENCODER_MAX_BITRATE;
    if ((*enc->encoder)->InitializeExt(enc->encoder, &params) != 0) {
        md_openh264_destroy(enc); return NULL;
    }
    enc->initialized = true;
    size_t pixels = (size_t)enc->width * enc->height;
    enc->i420 = malloc(pixels + pixels / 2);
    if (!enc->i420) { md_openh264_destroy(enc); return NULL; }
    return enc;
}

int md_openh264_encode(MdOpenH264 *enc, const uint8_t *y, int sy,
                       const uint8_t *uv, int suv, int64_t pts, bool key,
                       MdEncodedPacket *out) {
    size_t pixels = (size_t)enc->width * enc->height;
    uint8_t *u = enc->i420 + pixels, *v = u + pixels / 4;
    if (NV12ToI420(y, sy, uv, suv, enc->i420, enc->width,
                          u, enc->width / 2, v, enc->width / 2,
                          enc->width, enc->height) != 0) return -1;
    if (key && (*enc->encoder)->ForceIntraFrame(enc->encoder, true, -1) != 0) return -1;
    SSourcePicture pic = {0};
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = enc->width;
    pic.iStride[1] = pic.iStride[2] = enc->width / 2;
    pic.pData[0] = enc->i420; pic.pData[1] = u; pic.pData[2] = v;
    pic.iPicWidth = enc->width; pic.iPicHeight = enc->height;
    /* Avoid multiplication overflow for long-lived/caller-provided clocks. */
    if (pts < 0 || pts > INT64_MAX / 1000) return -1;
    pic.uiTimeStamp = (pts / enc->fps) * 1000 + (pts % enc->fps) * 1000 / enc->fps;
    SFrameBSInfo info = {0};
    if ((*enc->encoder)->EncodeFrame(enc->encoder, &pic, &info) != 0) return -1;
    *out = (MdEncodedPacket){0};
    if (info.eFrameType == videoFrameTypeSkip) return 0;
    if (info.iLayerNum < 0 || info.iLayerNum > MAX_LAYER_NUM_OF_FRAME ||
        info.iFrameSizeInBytes <= 0) return -1;
    if ((size_t)info.iFrameSizeInBytes > enc->packet_capacity) {
        void *p = realloc(enc->packet, (size_t)info.iFrameSizeInBytes);
        if (!p) return -1;
        enc->packet = p;
        enc->packet_capacity = (size_t)info.iFrameSizeInBytes;
    }
    size_t size = 0;
    for (int i = 0; i < info.iLayerNum; i++) {
        const SLayerBSInfo *layer = &info.sLayerInfo[i];
        size_t bytes = 0;
        for (int j = 0; j < layer->iNalCount; j++) {
            int len = layer->pNalLengthInByte[j];
            if (len < 0 || (size_t)len > (size_t)info.iFrameSizeInBytes - bytes) return -1;
            bytes += (size_t)len;
        }
        if (bytes > (size_t)info.iFrameSizeInBytes - size) return -1;
        memcpy(enc->packet + size, layer->pBsBuf, bytes);
        size += bytes;
    }
    if (size != (size_t)info.iFrameSizeInBytes) return -1;
    out->data = enc->packet; out->size = size;
    /* EncodeFrame completes this picture synchronously, with no reordering. */
    out->pts = out->dts = pts;
    out->is_key = info.eFrameType == videoFrameTypeIDR;
    return 0;
}

int md_openh264_bitrate(MdOpenH264 *enc, uint32_t bitrate) {
    SBitrateInfo br = {SPATIAL_LAYER_ALL, (int)bitrate};
    return (*enc->encoder)->SetOption(enc->encoder, ENCODER_OPTION_BITRATE, &br) == 0 ? 0 : -1;
}

void md_openh264_destroy(MdOpenH264 *enc) {
    if (!enc) return;
    if (enc->encoder) {
        if (enc->initialized) (*enc->encoder)->Uninitialize(enc->encoder);
        enc->destroy(enc->encoder);
    }
    if (enc->library) close_library(enc->library);
    free(enc->i420); free(enc->packet); free(enc);
}
