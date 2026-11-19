/*
 * metadesk — capture.c
 * Platform-agnostic capture convenience API.
 *
 * Delegates to the backend vtable returned by md_capture_backend_create().
 * The actual platform implementation lives in capture_pipewire.c,
 * capture_screencapturekit.m, or capture_dxgi.cpp.
 */
#include "capture.h"
#include "md_time.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ── Frame clock ─────────────────────────────────────────────── */

uint64_t md_capture_clock_ns(void) {
    return md_monotonic_ns();
}

uint64_t md_capture_rebase_ns(uint64_t src_ts, uint64_t src_now,
                              uint64_t dst_now) {
    if (src_ts >= src_now) return dst_now;
    const uint64_t age = src_now - src_ts;
    return age < dst_now ? dst_now - age : 0;
}

static bool ascii_iequal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return *a == *b;
}

int md_capture_display_choose(const MdCaptureDisplay *displays, size_t count,
                              const char *sel) {
    if (!displays || count == 0) return -1;
    if (!sel || !*sel) {
        for (size_t i = 0; i < count; i++)
            if (displays[i].primary) return (int)i;
        return 0;
    }
    const size_t digits = strspn(sel, "0123456789");
    if (sel[digits] == '\0') {
        if (digits > 4) return -1;
        const size_t index = (size_t)atoi(sel);
        return index < count ? (int)index : -1;
    }
    if ((sel[0] == 'i' || sel[0] == 'I') && (sel[1] == 'd' || sel[1] == 'D') &&
        sel[2] == ':') {
        const char *num = sel + 3;
        if (!isdigit((unsigned char)*num)) return -1;
        char *end = NULL;
        errno = 0;
        const unsigned long long id = strtoull(num, &end, 0);
        if (errno || *end || id > UINT32_MAX) return -1;
        for (size_t i = 0; i < count; i++)
            if (displays[i].id == (uint32_t)id) return (int)i;
        return -1;
    }
    int pick = -1;
    for (size_t i = 0; i < count; i++) {
        if (!displays[i].name || !ascii_iequal(sel, displays[i].name)) continue;
        if (pick >= 0) return -1; /* ambiguous: select by index or id */
        pick = (int)i;
    }
    return pick;
}

uint32_t md_capture_consent_timeout_ms(const MdCaptureConfig *cfg) {
    uint32_t timeout = cfg && cfg->consent_timeout_ms ? cfg->consent_timeout_ms :
                       (cfg && cfg->unattended ? 10000 : 180000);
    if (cfg && cfg->unattended && timeout > 10000) timeout = 10000;
    return timeout > 180000 ? 180000 : timeout;
}

bool md_capture_stall_may_forget(const MdCaptureConfig *cfg) {
    return cfg && cfg->forget_authorization_on_stall && !cfg->unattended;
}

MdCaptureRecovery md_capture_watchdog_step(MdCaptureWatchdog *w,
    int result, bool has_client, uint32_t now, uint32_t stall, uint32_t max_restarts) {
    if (!w) return MD_CAPTURE_RECOVERY_NONE;
    if (!has_client || result == 0) {
        memset(w, 0, sizeof(*w));
        return MD_CAPTURE_RECOVERY_NONE;
    }
    if (result == MD_CAPTURE_IDLE) {
        w->armed = false;
        return MD_CAPTURE_RECOVERY_NONE;
    }
    if (w->gave_up) return MD_CAPTURE_RECOVERY_NONE;
    if (!w->armed) {
        w->deadline_ms = now + stall;
        w->armed = true;
        return MD_CAPTURE_RECOVERY_NONE;
    }
    if ((int32_t)(now - w->deadline_ms) < 0) return MD_CAPTURE_RECOVERY_NONE;
    if (w->restarts >= max_restarts) {
        w->gave_up = true;
        return MD_CAPTURE_RECOVERY_EXHAUSTED;
    }
    w->restarts++;
    w->deadline_ms = now + stall;
    return MD_CAPTURE_RECOVERY_RESTART;
}

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>

/* Only the metadesk leaf is chmod'd; XDG_STATE_HOME may hold other apps. */
static int token_dir_open(void) {
    char path[PATH_MAX];
    const char *base = getenv("XDG_STATE_HOME");
    int len;
    if (base && base[0]) {
        if (base[0] != '/') return -1;
        len = snprintf(path, sizeof(path), "%s/metadesk", base);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') return -1;
        len = snprintf(path, sizeof(path), "%s/.local/state/metadesk", home);
    }
    if (len < 0 || (size_t)len >= sizeof(path)) return -1;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        int rc = mkdir(path, 0700);
        *p = '/';
        if (rc < 0 && errno != EEXIST) return -1;
    }
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_uid != geteuid() || fchmod(fd, 0700) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

size_t md_capture_restore_token_load(char *buf, size_t size) {
    if (!buf || size < 2) return 0;
    buf[0] = '\0';
    int dir = token_dir_open();
    if (dir < 0) return 0;
    int fd = openat(dir, "screencast.token", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    close(dir);
    if (fd < 0) return 0;
    struct stat st;
    size_t len = 0;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        /* A concurrent atomic rename can unlink the already-open old inode. */
        st.st_nlink > 1 || st.st_size <= 0 || (uint64_t)st.st_size >= size ||
        fchmod(fd, 0600) < 0) goto done;
    while (len < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + len, (size_t)st.st_size - len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { len = 0; goto done; }
        len += (size_t)n;
    }
    if (memchr(buf, '\0', len)) { len = 0; goto done; }
    while (len && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) len--;
done:
    close(fd);
    buf[len] = '\0';
    return len;
}

int md_capture_restore_token_save(const char *token) {
    if (!token || !token[0] || strlen(token) >= 1024) return -1;
    int dir = token_dir_open();
    if (dir < 0) return -1;
    static atomic_uint serial;
    char tmp[96];
    int fd = -1, rc = -1;
    for (unsigned i = 0; i < 128; i++) {
        snprintf(tmp, sizeof(tmp), ".screencast.token.%ld.%u", (long)getpid(),
                 atomic_fetch_add(&serial, 1));
        fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { close(dir); return -1; }
    size_t len = strlen(token), off = 0;
    if (fchmod(fd, 0600) < 0) goto done;
    while (off < len) {
        ssize_t n = write(fd, token + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        off += (size_t)n;
    }
    if (fsync(fd) < 0) goto done;
    if (close(fd) < 0) { fd = -1; goto done; }
    fd = -1;
    if (renameat(dir, tmp, dir, "screencast.token") < 0) goto done;
    rc = fsync(dir);
done:
    if (fd >= 0) close(fd);
    if (rc < 0) unlinkat(dir, tmp, 0);
    close(dir);
    return rc;
}

int md_capture_restore_token_forget(void) {
    int dir = token_dir_open();
    if (dir < 0) return -1;
    int rc = unlinkat(dir, "screencast.token", 0);
    if (rc == 0) rc = fsync(dir);
    else if (errno == ENOENT) rc = 0;
    close(dir);
    return rc;
}
#else
size_t md_capture_restore_token_load(char *buf, size_t size) {
    if (buf && size) buf[0] = '\0';
    return 0;
}
int md_capture_restore_token_save(const char *token) { (void)token; return -1; }
int md_capture_restore_token_forget(void) { return -1; }
#endif

/* ── Public convenience API ──────────────────────────────────── */

MdCaptureCtx *md_capture_create(const MdCaptureConfig *cfg) {
    const MdCaptureBackend *vtable = md_capture_backend_create();
    if (!vtable) return NULL;

    MdCaptureCtx *ctx = calloc(1, sizeof(MdCaptureCtx));
    if (!ctx) return NULL;

    ctx->vtable = vtable;

    /* Apply config or defaults */
    if (cfg) {
        ctx->config = *cfg;
    } else {
        ctx->config.target_fps  = 60;
        ctx->config.show_cursor = true;
    }

    if (ctx->vtable->init(ctx) != 0) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

int md_capture_start(MdCaptureCtx *ctx) {
    if (!ctx || !ctx->vtable || !ctx->vtable->start) return -1;
    return ctx->vtable->start(ctx);
}

int md_capture_get_frame(MdCaptureCtx *ctx, MdFrame *out, uint32_t timeout_ms) {
    if (!ctx || !out || !ctx->vtable || !ctx->vtable->get_frame) return MD_CAPTURE_ERROR;
    int result = ctx->vtable->get_frame(ctx, out, timeout_ms);
    if (result == 0 && out->buf_type == MD_BUF_CPU && !out->data) {
        /* Enforce the HAL contract even if a backend violates its mapping
         * precondition. Return its borrowed storage before reporting failure. */
        md_capture_release_frame(ctx, out);
        memset(out, 0, sizeof(*out));
        return MD_CAPTURE_ERROR;
    }
    return result;
}

void md_capture_release_frame(MdCaptureCtx *ctx, MdFrame *frame) {
    if (!ctx || !ctx->vtable || !ctx->vtable->release_frame) return;
    ctx->vtable->release_frame(ctx, frame);
}

bool md_capture_is_active(const MdCaptureCtx *ctx) {
    return ctx ? ctx->active : false;
}

int md_capture_wait_size(MdCaptureCtx *ctx, uint32_t timeout_ms,
                         uint32_t *width, uint32_t *height) {
    if (!ctx || !width || !height) return -1;
    if (ctx->vtable && ctx->vtable->wait_size)
        return ctx->vtable->wait_size(ctx, timeout_ms, width, height);
    if (ctx->width == 0 || ctx->height == 0) return -1;
    *width  = ctx->width;
    *height = ctx->height;
    return 0;
}

int md_capture_get_size(MdCaptureCtx *ctx, uint32_t *width, uint32_t *height) {
    return md_capture_wait_size(ctx, 0, width, height);
}

int md_capture_get_output_rect(const MdCaptureCtx *ctx, MdCaptureRect *rect) {
    if (!ctx || !rect || ctx->output_rect.width == 0 ||
        ctx->output_rect.height == 0)
        return -1;
    *rect = ctx->output_rect;
    return 0;
}

void md_capture_stop(MdCaptureCtx *ctx) {
    if (!ctx || !ctx->vtable || !ctx->vtable->stop) return;
    ctx->vtable->stop(ctx);
}

void md_capture_forget_authorization(MdCaptureCtx *ctx) {
    if (!ctx || !ctx->vtable || !ctx->vtable->forget_authorization) return;
    ctx->vtable->forget_authorization(ctx);
}

void md_capture_destroy(MdCaptureCtx *ctx) {
    if (!ctx) return;
    md_capture_stop(ctx);
    if (ctx->vtable && ctx->vtable->destroy)
        ctx->vtable->destroy(ctx);
    free(ctx);
}
