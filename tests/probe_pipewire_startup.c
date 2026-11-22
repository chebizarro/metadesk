/* Real-backend md-3nv regression. Run ONLY in the isolated Sway reference,
 * using validate-video.py's render-cell Podman isolation/device arguments and
 * allow-isolated-output consent; mount this probe as the container command.
 * Build on matching Linux, against the library mounted in the container:
 *   cc -Wall -Wextra -o startup-probe tests/probe_pipewire_startup.c -Isrc \
 *      -Lbuild -Wl,-rpath,$PWD/build -Wl,--export-dynamic -lmetadesk \
 *      $(pkg-config --cflags --libs libpipewire-0.3) -ldl
 * The unchanged baseline must fail with PAUSED/IDLE after 6500ms. The fixed
 * library must report PAUSED/ERROR near 5000ms, then a valid restarted frame.
 * Do not run against the live desktop's session bus or PipeWire instance. */
#define _GNU_SOURCE
#include "core/capture.h"
#include <pipewire/pipewire.h>
#include <dlfcn.h>
#include <stdio.h>

static bool inactive_start = true;
static struct pw_stream *observed_stream;

/* Leave the first authorized stream PAUSED with no frames. This reproduces
 * the state that used to disarm the host watchdog, without corrupting buffers
 * or depending on the intermittent restored-node failure. */
int pw_stream_connect(struct pw_stream *stream, enum pw_direction direction,
                      uint32_t target_id, enum pw_stream_flags flags,
                      const struct spa_pod **params, uint32_t n_params) {
    typedef int (*Connect)(struct pw_stream *, enum pw_direction, uint32_t,
                            enum pw_stream_flags, const struct spa_pod **, uint32_t);
    Connect connect = (Connect)dlsym(RTLD_NEXT, "pw_stream_connect");
    if (!connect) return -1;
    observed_stream = stream;
    if (inactive_start) flags |= PW_STREAM_FLAG_INACTIVE;
    return connect(stream, direction, target_id, flags, params, n_params);
}

int main(void) {
    MdCaptureConfig cfg = {.target_fps = 15, .unattended = true};
    MdCaptureCtx *capture = md_capture_create(&cfg);
    if (!capture) return 1;
    int status = 1;
    MdFrame frame;
    if (md_capture_start(capture) != 0) goto done;
    uint64_t start = md_capture_clock_ns();
    int result = md_capture_get_frame(capture, &frame, 6500);
    uint64_t elapsed = md_capture_clock_ns() - start;
    enum pw_stream_state state = pw_stream_get_state(observed_stream, NULL);
    printf("paused startup: state=%s result=%d elapsed_ms=%llu\n",
           pw_stream_state_as_string(state), result,
           (unsigned long long)(elapsed / 1000000));
    if (result == 0) md_capture_release_frame(capture, &frame);
    if (state != PW_STREAM_STATE_PAUSED || result != MD_CAPTURE_ERROR ||
        elapsed < 4000000000ULL || elapsed > 6200000000ULL) goto done;

    /* The watchdog's restart uses this same stop/start path and restore token. */
    md_capture_stop(capture);
    inactive_start = false;
    if (md_capture_start(capture) != 0) goto done;
    result = md_capture_get_frame(capture, &frame, 6500);
    if (result != 0) { fprintf(stderr, "restart: result=%d\n", result); goto done; }
    bool valid = frame.buf_type == MD_BUF_CPU && frame.data && frame.data_size &&
                 frame.width && frame.height && frame.timestamp_ns;
    md_capture_release_frame(capture, &frame);
    if (!valid) goto done;
    puts("restart: valid CPU frame after paused first-frame failure");
    status = 0;
done:
    md_capture_destroy(capture);
    return status;
}
