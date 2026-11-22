/* Local capture diagnostic. No listener, signer, relay or remote grant exists. */
#include "capture.h"
#include <stdio.h>
#include <string.h>

int md_host_capture_cli(int argc, char **argv) {
    if (argc < 1 || argc > 2 || !strcmp(argv[0], "--help")) {
        fprintf(stderr, "Usage: metadesk-host capture-frame OUTPUT.raw [DISPLAY]\n"
                        "Capture one CPU frame locally; this does not start a remote session.\n");
        return argc == 1 && !strcmp(argv[0], "--help") ? 0 : 1;
    }
    MdCaptureConfig config = { .target_fps = 1, .show_cursor = true,
                               .output = argc == 2 ? argv[1] : NULL };
    MdCaptureCtx *capture = md_capture_create(&config);
    if (!capture) {
        fprintf(stderr, "capture-frame: backend creation failed\n");
        return 1;
    }
    int rc = 1;
    if (md_capture_start(capture) != 0) {
        fprintf(stderr, "capture-frame: backend start failed (interactive desktop/permission required)\n");
        goto done;
    }
    MdFrame frame;
    if (md_capture_get_frame(capture, &frame, 5000) != 0) {
        fprintf(stderr, "capture-frame: no frame received within the failure bound\n");
        goto done;
    }
    if (frame.buf_type != MD_BUF_CPU || !frame.data || !frame.data_size) {
        fprintf(stderr, "capture-frame: backend returned no CPU pixels; raw-file capture unsupported for this buffer\n");
    } else {
        FILE *file = fopen(argv[0], "wb");
        if (!file) {
            perror("capture-frame: open output");
        } else {
            size_t n = fwrite(frame.data, 1, frame.data_size, file);
            int closed = fclose(file);
            if (n == frame.data_size && closed == 0) {
                printf("host: captured one frame: %ux%u stride=%u format=%d bytes=%zu output=%s\n",
                       frame.width, frame.height, frame.stride, (int)frame.format, n, argv[0]);
                rc = 0;
            } else {
                fprintf(stderr, "capture-frame: incomplete output write\n");
                remove(argv[0]);
            }
        }
    }
    md_capture_release_frame(capture, &frame);
done:
    md_capture_destroy(capture);
    return rc;
}
