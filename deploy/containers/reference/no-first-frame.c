/* Explicit test-only fault injection, never bundled or enabled by default.
 * Suppress buffers on the host's first capture stream until it is destroyed.
 * Subsequent streams use the real backend unchanged. This exercises md-3nv's
 * deadline + the actual host watchdog, NOT the upstream allocation race.
 */
#define _GNU_SOURCE
#include <pipewire/pipewire.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static struct pw_stream *first_stream;
static bool completed;
static unsigned dropped, passthrough;
static int (*real_connect)(struct pw_stream *, enum pw_direction, uint32_t,
                           enum pw_stream_flags, const struct spa_pod **, uint32_t);
static struct pw_buffer *(*real_dequeue)(struct pw_stream *);
static int (*real_queue)(struct pw_stream *, struct pw_buffer *);
static void (*real_destroy)(struct pw_stream *);

static void resolve(void) {
    real_connect = dlsym(RTLD_NEXT, "pw_stream_connect");
    real_dequeue = dlsym(RTLD_NEXT, "pw_stream_dequeue_buffer");
    real_queue = dlsym(RTLD_NEXT, "pw_stream_queue_buffer");
    real_destroy = dlsym(RTLD_NEXT, "pw_stream_destroy");
    if (!real_connect || !real_dequeue || !real_queue || !real_destroy) abort();
}

static void receipt(const char *event) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "FAULT_INJECTION no-first-frame %s dropped=%u passthrough=%u monotonic=%lld.%09ld\n",
            event, dropped, passthrough, (long long)ts.tv_sec, ts.tv_nsec);
}

int pw_stream_connect(struct pw_stream *stream, enum pw_direction direction,
                      uint32_t target, enum pw_stream_flags flags,
                      const struct spa_pod **params, uint32_t n_params) {
    pthread_once(&once, resolve);
    pthread_mutex_lock(&lock);
    if (!completed && !first_stream) {
        first_stream = stream;
        receipt("armed");
    }
    pthread_mutex_unlock(&lock);
    return real_connect(stream, direction, target, flags, params, n_params);
}

/* Reference-only CPU RGBx output is fixed at 1920x1080. Be conservative:
 * pass through anything other than a complete readable reference frame so
 * production validation can still expose natural descriptor faults. */
static bool complete_reference_frame(const struct pw_buffer *buffer) {
    if (!buffer || !buffer->buffer || buffer->buffer->n_datas != 1) return false;
    const struct spa_data *d = &buffer->buffer->datas[0];
    if ((d->type != SPA_DATA_MemFd && d->type != SPA_DATA_MemPtr) || !d->data ||
        !d->chunk || d->chunk->stride < 1920 * 4 ||
        ((d->flags & SPA_DATA_FLAG_WRITABLE) && !(d->flags & SPA_DATA_FLAG_READABLE)) ||
        d->chunk->offset > d->maxsize || d->chunk->size > d->maxsize - d->chunk->offset)
        return false;
    return (uint64_t)1079 * d->chunk->stride + 1920 * 4 <= d->chunk->size;
}

struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream) {
    pthread_once(&once, resolve);
    struct pw_buffer *buffer = real_dequeue(stream);
    pthread_mutex_lock(&lock);
    bool selected = !completed && stream == first_stream && buffer != NULL;
    bool suppress = selected && complete_reference_frame(buffer);
    if (suppress) dropped++;
    else if (selected) passthrough++;
    pthread_mutex_unlock(&lock);
    if (suppress) {
        real_queue(stream, buffer);
        return NULL;
    }
    return buffer;
}

void pw_stream_destroy(struct pw_stream *stream) {
    pthread_once(&once, resolve);
    pthread_mutex_lock(&lock);
    if (stream == first_stream && !completed) {
        completed = true;
        receipt("destroyed");
    }
    pthread_mutex_unlock(&lock);
    real_destroy(stream);
}
