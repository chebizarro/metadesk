/*
 * metadesk — tests/test_time.c
 * md-5aua: every metadesk deadline, timeout and frame time reads one
 * monotonic clock, md_monotonic_ns(). On macOS, Homebrew libnsync exports a
 * clock_gettime that returns gettimeofday() wall time for any clock id and,
 * linked ahead of libSystem, used to shadow it; these checks fail if either
 * the helper or a plain clock_gettime(CLOCK_MONOTONIC) call in a binary
 * linked like metadesk's reads wall time again.
 */
#include "md_time.h"
#include "capture.h"
#include "stream.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "md_test.h"

/* The platform's own clocks, read without going through clock_gettime on
 * macOS (clock_gettime_nsec_np is libSystem-only, beyond libnsync's reach). */
static uint64_t ref_ns(clockid_t id) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(id);
#else
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void test_helper_is_platform_monotonic(void) {
    md_test_name = "helper_is_platform_monotonic";
    /* Back-to-back reads bracket the reference exactly when both are the
     * same clock; wall time (~1.8e18 ns) would land far_deadline outside. */
    const uint64_t a = md_monotonic_ns();
    const uint64_t ref = ref_ns(CLOCK_MONOTONIC);
    const uint64_t b = md_monotonic_ns();
    MD_CHECK(a <= ref && ref <= b);
    MD_CHECK(b - a < 1000000000ull);

    /* Far from the epoch: time since boot, not since 1970. Half of wall
     * time is ~28 years, which no uptime reaches. */
    const uint64_t wall = ref_ns(CLOCK_REALTIME);
    MD_CHECK(wall > 1600000000ull * 1000000000ull); /* sanity: after 2020 */
    MD_CHECK(a < wall / 2);
}

static void test_derived_clocks_agree(void) {
    md_test_name = "derived_clocks_agree";
    uint64_t a = md_monotonic_ns();
    const uint64_t frame = md_capture_clock_ns();
    uint64_t b = md_monotonic_ns();
    MD_CHECK(a <= frame && frame <= b);

    a = md_monotonic_ns() / 1000000u;
    const int64_t ms = md_now_ms();
    const uint32_t stream_ms = md_stream_now_ms();
    b = md_monotonic_ns() / 1000000u;
    MD_CHECK(ms >= 0 && a <= (uint64_t)ms && (uint64_t)ms <= b);
    /* md_stream_now_ms wraps at 2^32; compare modulo that. */
    MD_CHECK((uint32_t)(stream_ms - (uint32_t)a) <= (uint32_t)(b - a));
}

/* Option (b): meson links libSystem ahead of libnsync on darwin, so a plain
 * clock_gettime(CLOCK_MONOTONIC) in this executable (and libmetadesk, and
 * the static nostrc code inside it) is libSystem's, not the wall-time shim.
 * Linux: glibc's, trivially. */
static void test_linked_clock_gettime_is_monotonic(void) {
    md_test_name = "linked_clock_gettime_is_monotonic";
    struct timespec ts;
    MD_CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    const uint64_t got = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    const uint64_t ref = ref_ns(CLOCK_MONOTONIC);
    MD_CHECK(got <= ref && ref - got < 1000000000ull);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool flag;
} Waiter;

static void *signal_later(void *arg) {
    Waiter *w = arg;
    struct timespec nap = { 0, 20 * 1000000L };
    nanosleep(&nap, NULL);
    pthread_mutex_lock(&w->mu);
    w->flag = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

static void test_cond_timedwait_until(void) {
    md_test_name = "cond_timedwait_until";
    Waiter w = { .flag = false };
    MD_CHECK(pthread_mutex_init(&w.mu, NULL) == 0);
    MD_CHECK(md_cond_init_monotonic(&w.cv) == 0);

    /* A passed deadline times out without waiting. */
    pthread_mutex_lock(&w.mu);
    MD_CHECK(md_cond_timedwait_until(&w.cv, &w.mu, md_monotonic_ns() - 1) == ETIMEDOUT);

    /* ETIMEDOUT only once the deadline has really passed. */
    const uint64_t start = md_monotonic_ns();
    const uint64_t deadline = start + 50 * 1000000ull;
    int rc;
    while ((rc = md_cond_timedwait_until(&w.cv, &w.mu, deadline)) == 0) {}
    const uint64_t end = md_monotonic_ns();
    MD_CHECK(rc == ETIMEDOUT);
    MD_CHECK(end >= deadline);
    MD_CHECK(end - start < 5000000000ull);

    /* A signal wakes the wait long before a far_deadline deadline. */
    pthread_t tid;
    MD_CHECK(pthread_create(&tid, NULL, signal_later, &w) == 0);
    const uint64_t far_deadline = md_monotonic_ns() + 30 * 1000000000ull;
    rc = 0;
    while (!w.flag && rc == 0)
        rc = md_cond_timedwait_until(&w.cv, &w.mu, far_deadline);
    MD_CHECK(w.flag && rc == 0);
    MD_CHECK(md_monotonic_ns() < far_deadline - 25 * 1000000000ull);
    pthread_mutex_unlock(&w.mu);
    pthread_join(tid, NULL);

    pthread_cond_destroy(&w.cv);
    pthread_mutex_destroy(&w.mu);
}

int main(void) {
    printf("test_time: monotonic clock (md-5aua)\n");
    test_helper_is_platform_monotonic();
    test_derived_clocks_agree();
    test_linked_clock_gettime_is_monotonic();
    test_cond_timedwait_until();
    return md_test_report();
}
