/*
 * metadesk — platform.h
 * Cross-platform memory security primitives.
 *
 * Provides:
 *   md_mem_lock / md_mem_unlock   — prevent memory from being paged to swap
 *   md_secure_zero                — guaranteed-not-optimised-away memory zeroing
 *
 * Platform dispatch:
 *   Linux/macOS: mlock/munlock + explicit_bzero/memset_s
 *   Windows:     VirtualLock/VirtualUnlock + SecureZeroMemory
 *
 * See spec §7 for secret handling requirements.
 */
#ifndef MD_PLATFORM_H
#define MD_PLATFORM_H

#include <stddef.h>

/* Enable C11 Annex K (memset_s) where available */
#ifndef __STDC_WANT_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1
#endif
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/mman.h>   /* mlock, munlock */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Memory locking (prevent swap) ───────────────────────────── */

/*
 * Lock a region of memory so it won't be paged to swap.
 * Returns 0 on success, -1 on failure.
 *
 * On Windows, the process working set must be large enough.
 * Call SetProcessWorkingSetSize if VirtualLock fails.
 */
static inline int md_mem_lock(void *ptr, size_t size) {
#ifdef _WIN32
    return VirtualLock(ptr, size) ? 0 : -1;
#else
    return mlock(ptr, size);
#endif
}

/*
 * Unlock a previously locked memory region.
 * Returns 0 on success, -1 on failure.
 */
static inline int md_mem_unlock(void *ptr, size_t size) {
#ifdef _WIN32
    return VirtualUnlock(ptr, size) ? 0 : -1;
#else
    return munlock(ptr, size);
#endif
}

/* ── Secure memory zeroing ───────────────────────────────────── */

/*
 * Zero memory in a way that the compiler cannot optimise away.
 * Used to clear secrets (tokens, keys) before freeing buffers.
 *
 * Platform implementations:
 *   Windows:      SecureZeroMemory (compiler intrinsic)
 *   macOS:        memset_s (C11 Annex K, always available on Apple)
 *   glibc ≥2.25: explicit_bzero
 *   Fallback:     volatile function pointer trick
 */
static inline void md_secure_zero(void *ptr, size_t size) {
    if (!ptr || size == 0) return;

#if defined(_WIN32)
    SecureZeroMemory(ptr, size);
#elif defined(__APPLE__)
    /* macOS always provides memset_s via <string.h> */
    memset_s(ptr, size, 0, size);
#elif defined(__GLIBC__) && defined(__GLIBC_MINOR__) && \
      (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(ptr, size);
#else
    /* Portable fallback: volatile function pointer prevents
     * dead-store elimination by the compiler. */
    static void *(*const volatile memset_func)(void *, int, size_t) = memset;
    memset_func(ptr, 0, size);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* MD_PLATFORM_H */
