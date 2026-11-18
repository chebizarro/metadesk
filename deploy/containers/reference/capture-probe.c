/* validated on rootless Podman 4.9.3 / Ubuntu 24.04 aarch64; see video-validation.md.
 * Local qualification harness, not a replacement host or supported executable.
 * Link against the SAME libmetadesk.so shipped to the image. No fake backend.
 */
#include "core/capture.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t last_timestamp;

static void resources(const char *stage) {
    const char *trace = getenv("MD_CAPTURE_TRACE_BUFFERS");
    if (!trace || strcmp(trace, "1") != 0) return;
    unsigned fds = 0, mappings = 0;
    DIR *dir = opendir("/proc/self/fd");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            char *end;
            long fd = strtol(entry->d_name, &end, 10);
            if (*entry->d_name && !*end && fd != dirfd(dir)) fds++;
        }
        closedir(dir);
    }
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[2048];
        while (fgets(line, sizeof(line), maps))
            if (strstr(line, "xdpw-shm-") || strstr(line, "memfd:")) mappings++;
        fclose(maps);
    }
    printf("resources=%s fds=%u shm_mappings=%u\n", stage, fds, mappings);
}

typedef struct {
    MdCaptureCtx *ctx;
    atomic_bool completed;
} StopTask;

static void *stop_capture(void *arg) {
    StopTask *task = arg;
    md_capture_stop(task->ctx);
    atomic_store_explicit(&task->completed, true, memory_order_release);
    return NULL;
}

static int probe(MdCaptureCtx *ctx) {
    double start = now();
    int rc = md_capture_start(ctx);
    printf("start_rc=%d elapsed=%.3f\n", rc, now() - start);
    if (rc) { printf("frames=0 start_failed=1\n"); return 3; }
    /* Two distinct rendered backgrounds: verify pixels, not just callbacks.
     * No input injection, no encoder, no network, no mock capture backend. */
    const char *commands[] = {
        "swaymsg 'output HEADLESS-1 bg #204060 solid_color' >/dev/null",
        "swaymsg 'output HEADLESS-1 bg #a08020 solid_color' >/dev/null"
    };
    unsigned matches[2] = {0, 0}, frames = 0, timeouts = 0;
    const unsigned rgb[2][3] = {{0x20, 0x40, 0x60}, {0xa0, 0x80, 0x20}};
    for (unsigned change = 0; change < 10; change++) {
        unsigned phase = change % 2, before = matches[phase];
        if (system(commands[phase]) != 0) { return 4; }
        double deadline = now() + 3, redraw = now() + .25;
        while (now() < deadline && matches[phase] == before) {
            if (now() >= redraw) {
                if (system(commands[phase]) != 0) { return 4; }
                redraw = now() + .25;
            }
            MdFrame f = {0};
            if (md_capture_get_frame(ctx, &f)) {
                timeouts++;
                /* The backend can return immediately before STREAMING. This
                 * is retry pacing, never a success criterion or consent wait. */
                const struct timespec pause = {.tv_nsec = 10000000};
                nanosleep(&pause, NULL);
                continue;
            }
            frames++;
            if (!f.timestamp_ns || f.timestamp_ns <= last_timestamp ||
                f.timestamp_ns > (uint64_t)(now() * 1e9)) {
                fprintf(stderr, "invalid/nonprogressing monotonic timestamp\n");
                md_capture_release_frame(ctx, &f);
                return 8;
            }
            last_timestamp = f.timestamp_ns;
            size_t offset = (size_t)(f.height / 2) * f.stride + (f.width / 2) * 4;
            if (f.width != 1920 || f.height != 1080 || f.buf_type != MD_BUF_CPU ||
                !f.data || offset + 4 > f.data_size) {
                fprintf(stderr, "unexpected frame width=%u height=%u stride=%u "
                        "type=%d format=%d data_present=%d bytes=%zu seq=%u\n",
                        f.width, f.height, f.stride, f.buf_type, f.format,
                        f.data != NULL, f.data_size, f.seq);
                md_capture_release_frame(ctx, &f);
                return 5;
            }
            const unsigned char *p = f.data + offset;
            int bgr = f.format == MD_PIX_CAPTURE_BGRX || f.format == MD_PIX_CAPTURE_BGRA;
            if (p[bgr ? 2 : 0] == rgb[phase][0] && p[1] == rgb[phase][1] &&
                p[bgr ? 0 : 2] == rgb[phase][2]) matches[phase]++;
            printf("phase=%u seq=%u size=%ux%u stride=%u format=%d bytes=%zu "
                   "pixel=%02x%02x%02x%02x timestamp=%" PRIu64 "\n",
                   phase, f.seq, f.width, f.height, f.stride, f.format, f.data_size,
                   p[0], p[1], p[2], p[3], f.timestamp_ns);
            md_capture_release_frame(ctx, &f);
        }
        if (matches[phase] == before) break;
    }
    /* A static, undamaged output can legitimately stop delivering new frames.
     * Measure that boundary separately from the changing-pixel success test. */
    if (matches[0] == 5 && matches[1] == 5 && !getenv("MD_CAPTURE_SKIP_IDLE")) {
        unsigned idle_frames = 0, idle_timeouts = 0, idle_errors = 0;
        char before[1024], after[1024];
        size_t token_len = md_capture_restore_token_load(before, sizeof(before));
        double idle_start = now();
        for (unsigned i = 0; i < 5; i++) {
            MdFrame f = {0};
            int result = md_capture_get_frame(ctx, &f);
            if (result == MD_CAPTURE_IDLE) idle_timeouts++;
            else if (result == MD_CAPTURE_ERROR) idle_errors++;
            else { idle_frames++; md_capture_release_frame(ctx, &f); }
        }
        bool retained = token_len && md_capture_restore_token_load(after, sizeof(after)) == token_len &&
                        strcmp(before, after) == 0;
        printf("static_frames=%u static_idle=%u static_errors=%u authorization_retained=%d elapsed=%.3f\n",
               idle_frames, idle_timeouts, idle_errors, retained, now() - idle_start);
        if (idle_errors || !retained) return 9;
    }
    if (matches[0] < 5 || matches[1] < 5) {
        printf("frames=%u timeouts=%u matching_pixels=%u,%u elapsed=%.3f\n",
               frames, timeouts, matches[0], matches[1], now() - start);
        return 6;
    }
    if (getenv("MD_CAPTURE_BREAK_PIPEWIRE")) {
        /* Only this container's PID namespace; never a host/live service. */
        if (system("python3 -c 'import pathlib,os,signal; [os.kill(int(p.name),signal.SIGTERM) for p in pathlib.Path(\"/proc\").iterdir() if p.name.isdigit() and (p/\"comm\").exists() and (p/\"comm\").read_text().strip()==\"pipewire\"]'") != 0) return 10;
        MdFrame f = {0};
        int broken = md_capture_get_frame(ctx, &f);
        if (broken == 0) md_capture_release_frame(ctx, &f);
        printf("broken_result=%d\n", broken);
        if (broken != MD_CAPTURE_ERROR) return 10;
        char token[1024];
        if (!md_capture_restore_token_load(token, sizeof(token))) return 10;
    } else {
        /* Acquire a real mapping, stop concurrently, prove the held pixels
         * remain readable until release, then prove no stale frame survives. */
        MdFrame held = {0};
        int held_result;
        double deadline = now() + 3;
        do {
            if (system(commands[0]) != 0) return 4;
            held_result = md_capture_get_frame(ctx, &held);
        } while (held_result != 0 && now() < deadline);
        if (held_result != 0 || !held.data) return 11;
        uint8_t pixel = held.data[0];
        pthread_t stopper;
        StopTask task = {.ctx = ctx};
        atomic_init(&task.completed, false);
        if (pthread_create(&stopper, NULL, stop_capture, &task) != 0) {
            md_capture_release_frame(ctx, &held);
            return 11;
        }
        /* get_frame observes stopping without taking another buffer. */
        MdFrame next = {0};
        int stopped = md_capture_get_frame(ctx, &next);
        if (stopped == 0) md_capture_release_frame(ctx, &next);
        bool waiting = stopped == MD_CAPTURE_ERROR &&
                       !atomic_load_explicit(&task.completed, memory_order_acquire);
        bool intact = waiting && held.data[0] == pixel;
        md_capture_release_frame(ctx, &held);
        pthread_join(stopper, NULL);
        int after_stop = md_capture_get_frame(ctx, &next);
        printf("held_buffer_intact=%d after_stop=%d stop_waiting=%d\n", intact, after_stop, waiting);
        if (!waiting || !intact || after_stop != MD_CAPTURE_ERROR) return 11;
    }
    printf("frames=%u timeouts=%u matching_pixels=%u,%u elapsed=%.3f\n",
           frames, timeouts, matches[0], matches[1], now() - start);
    return matches[0] >= 5 && matches[1] >= 5 ? 0 : 6;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    MdCaptureConfig cfg = {.target_fps = 15, .show_cursor = true, .unattended = true};
    const char *timeout = getenv("MD_CAPTURE_CONSENT_TIMEOUT_MS");
    if (timeout) cfg.consent_timeout_ms = (uint32_t)strtoul(timeout, NULL, 10);
    resources("before-create");
    MdCaptureCtx *ctx = md_capture_create(&cfg);
    if (!ctx) return 2;
    const char *repeat = getenv("MD_CAPTURE_REOPENS");
    unsigned count = repeat ? (unsigned)strtoul(repeat, NULL, 10) : 1;
    if (!count || count > 100) { md_capture_destroy(ctx); return 2; }
    printf("static_check_skipped=%d\n", getenv("MD_CAPTURE_SKIP_IDLE") != NULL);
    int rc = 0;
    for (unsigned i = 0; i < count; i++) {
        printf("reopen=%u\n", i);
        rc = probe(ctx);
        md_capture_stop(ctx);
        resources("after-stop");
        if (rc) break;
    }
    md_capture_destroy(ctx);
    resources("after-destroy");
    return rc;
}
