/* Seam test of the actual production on_process validator, not a copy.
 * Only PipeWire's dequeue/queue transport is substituted; no backend is run.
 * Build on Linux with the matching libmetadesk (see video-validation.md).
 */
#include "../../../src/core/capture_pipewire.c"

static struct pw_buffer *next_buffer;
static unsigned queued;

struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream) {
    (void)stream;
    struct pw_buffer *result = next_buffer;
    next_buffer = NULL;
    return result;
}

int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer) {
    (void)stream;
    (void)buffer;
    queued++;
    return 0;
}

static unsigned checks;
static void check_descriptor(uint32_t flags, uint32_t type, bool mapped,
                             uint32_t maxsize, uint32_t offset, uint32_t size,
                             int32_t stride, bool accepted) {
    uint8_t pixels[16] = {0};
    struct spa_chunk chunk = {.offset = offset, .size = size, .stride = stride};
    struct spa_data data = {.type = type, .flags = flags, .fd = -1,
                           .data = mapped ? pixels : NULL, .maxsize = maxsize, .chunk = &chunk};
    struct spa_buffer spa = {.n_datas = 1, .datas = &data};
    struct pw_buffer buffer = {.buffer = &spa};
    MdCaptureCtx ctx = {.width = 2, .height = 2};
    PipewireState pw = {.ctx = &ctx, .src_width = 2, .src_height = 2,
                        .stride = 8, .spa_format = SPA_VIDEO_FORMAT_RGBx};
    ctx.backend_data = &pw;
    atomic_init(&pw.seq, 0);
    pthread_mutex_init(&pw.frame_lock, NULL);
    pthread_cond_init(&pw.frame_cond, NULL);
    queued = 0;
    next_buffer = &buffer;
    on_process(&pw);
    checks++;
    bool correct = accepted ?
        (pw.frame_ready && pw.received_frame && !pw.broken && queued == 0 &&
         pw.pending_frame.data == pixels && pw.pending_frame.data_size == size &&
         pw.pending_frame.buf_type == MD_BUF_CPU && pw.pending_frame.timestamp_ns > 0) :
        (!pw.frame_ready && !pw.received_frame && pw.broken && queued == 1);
    pthread_cond_destroy(&pw.frame_cond);
    pthread_mutex_destroy(&pw.frame_lock);
    if (!correct) {
        fprintf(stderr, "FAIL descriptor flags=%u type=%u mapped=%d max=%u offset=%u size=%u stride=%d accepted=%d\n",
                flags, type, mapped, maxsize, offset, size, stride, accepted);
        exit(1);
    }
}

int main(void) {
    /* Every combination of the four defined flag bits. Only access=WRITABLE
     * alone is rejected; MAPPABLE and DYNAMIC never imply write-only access. */
    static const struct { uint32_t flags; bool accepted; } cases[] = {
        {0, true}, {8, true}, /* observed 1.0.5 and 1.2.7/1.4.10 respectively */
        {1, true}, {2, false}, {3, true}, {4, true}, {5, true}, {6, false},
        {7, true}, {9, true}, {10, false}, {11, true}, {12, true},
        {13, true}, {14, false}, {15, true},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        check_descriptor(cases[i].flags, SPA_DATA_MemFd, true, 16, 0, 16, 8, cases[i].accepted);
    check_descriptor(SPA_DATA_FLAG_MAPPABLE, SPA_DATA_MemPtr, true, 16, 0, 16, 8, true);
    /* Invalid descriptors remain fail-closed even with otherwise valid flags. */
    for (unsigned i = 0; i < 2; i++) {
        uint32_t flags = i ? SPA_DATA_FLAG_MAPPABLE : 0;
        check_descriptor(flags, SPA_DATA_MemFd, false, 16, 0, 16, 8, false);
        check_descriptor(flags, SPA_ID_INVALID, false, 0, 0, 16, 8, false);
        check_descriptor(flags, SPA_ID_INVALID, true, 16, 0, 16, 8, false);
        check_descriptor(flags, SPA_DATA_MemFd, true, 0, 0, 16, 8, false);
        check_descriptor(flags, SPA_DATA_MemFd, true, 16, 17, 16, 8, false);
        check_descriptor(flags, SPA_DATA_MemFd, true, 16, 8, 16, 8, false);
        check_descriptor(flags, SPA_DATA_MemFd, true, 16, 0, 8, 8, false);
        check_descriptor(flags, SPA_DATA_MemFd, true, 16, 0, 16, 4, false);
        check_descriptor(flags, SPA_DATA_MemFd, true, 16, 0, 16, -8, false);
    }
    printf("PASS production descriptor seam: %u cases\n", checks);
    return 0;
}
