/* metadesk — the one monotonic clock for deadlines, timeouts and frame times.
 *
 * Read time through md_monotonic_ns()/md_now_ms(), never through
 * clock_gettime(CLOCK_MONOTONIC): on macOS, Homebrew libnsync (a nostrc
 * dependency linked into libmetadesk) exports its own clock_gettime that
 * ignores the clock id and returns gettimeofday() wall time (md-5aua). */
#ifndef MD_TIME_H
#define MD_TIME_H

#include <errno.h> /* ETIMEDOUT, md_cond_timedwait_until's timeout result */
#include <pthread.h>
#include <stdint.h>

/* Nanoseconds since an arbitrary origin; never stepped by wall-clock changes.
 *   Linux/BSD: CLOCK_MONOTONIC (the clock md_cond_init_monotonic() selects).
 *   macOS:     CLOCK_MONOTONIC via clock_gettime_nsec_np (libSystem-only, so
 *              libnsync cannot shadow it); counts through sleep.
 *   Windows:   QueryPerformanceCounter, the clock DXGI frame times use. */
uint64_t md_monotonic_ns(void);

static inline int64_t md_now_ms(void)
{
    return (int64_t)(md_monotonic_ns() / 1000000u);
}

/* Initialise cv for md_cond_timedwait_until(). macOS has no
 * pthread_condattr_setclock, so there the wait is relative instead. */
int md_cond_init_monotonic(pthread_cond_t *cv);

/* Wait on cv (from md_cond_init_monotonic) with mu held until signalled or
 * md_monotonic_ns() reaches deadline_ns. Returns ETIMEDOUT only once the
 * deadline has passed; 0 means woken (possibly spuriously), so re-check the
 * predicate and call again. */
int md_cond_timedwait_until(pthread_cond_t *cv, pthread_mutex_t *mu,
                            uint64_t deadline_ns);

#endif
