/* metadesk — md_time.c: the monotonic clock and condvar waits on it. */
#include "md_time.h"

#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

uint64_t md_monotonic_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER hz, now;
    QueryPerformanceFrequency(&hz);
    QueryPerformanceCounter(&now);
    const uint64_t f = (uint64_t)hz.QuadPart, t = (uint64_t)now.QuadPart;
    return t / f * 1000000000ull + t % f * 1000000000ull / f;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_MONOTONIC);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

int md_cond_init_monotonic(pthread_cond_t *cv)
{
#if defined(__APPLE__) || defined(_WIN32)
    return pthread_cond_init(cv, NULL);
#else
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(cv, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
#endif
}

static struct timespec ns_timespec(uint64_t ns)
{
    return (struct timespec){ .tv_sec = (time_t)(ns / 1000000000u),
                              .tv_nsec = (long)(ns % 1000000000u) };
}

int md_cond_timedwait_until(pthread_cond_t *cv, pthread_mutex_t *mu,
                            uint64_t deadline_ns)
{
    const uint64_t now = md_monotonic_ns();
    if (now >= deadline_ns) return ETIMEDOUT;
#if defined(__APPLE__) || defined(_WIN32)
    /* winpthreads supports relative waits, but not a MONOTONIC condattr. */
    struct timespec rel = ns_timespec(deadline_ns - now);
    int rc = pthread_cond_timedwait_relative_np(cv, mu, &rel);
#else
    /* md_monotonic_ns() is CLOCK_MONOTONIC here, the condvar's clock. */
    struct timespec abs = ns_timespec(deadline_ns);
    int rc = pthread_cond_timedwait(cv, mu, &abs);
#endif
    if (rc == ETIMEDOUT && md_monotonic_ns() < deadline_ns) return 0;
    return rc;
}
