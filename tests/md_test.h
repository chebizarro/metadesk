#ifndef MD_TEST_H
#define MD_TEST_H
/*
 * metadesk — shared test harness.
 *
 * MD_CHECK is the one assertion primitive for the whole suite. Unlike
 * assert(), it is NEVER preprocessor-gated: it survives -DNDEBUG and a
 * release build, counts every check, and records failures. md_test_report()
 * turns any failure into a non-zero process exit, so a broken assertion
 * actually reddens `meson test` instead of exiting 0 having checked nothing.
 *
 * Every test main() ends with `return md_test_report();`.
 *
 * The TEST/PASS/FAIL/RUN_TEST names are compatibility shims for the four
 * reporting dialects this header replaced; all are backed by the same
 * failure counter.
 */
#include <stdio.h>

static int md_test_failures = 0;
static int md_test_checks   = 0;
static const char *md_test_name = "";

#define MD_CHECK(cond) do {                                            \
        md_test_checks++;                                              \
        if (!(cond)) {                                                 \
            md_test_failures++;                                        \
            fprintf(stderr, "  FAIL %s:%d: MD_CHECK(%s)%s%s\n",        \
                    __FILE__, __LINE__, #cond,                         \
                    md_test_name[0] ? " in " : "", md_test_name);      \
        }                                                              \
    } while (0)

#define MD_CHECK_MSG(cond, msg) do {                                   \
        md_test_checks++;                                              \
        if (!(cond)) {                                                 \
            md_test_failures++;                                        \
            fprintf(stderr, "  FAIL %s:%d: %s\n",                      \
                    __FILE__, __LINE__, (msg));                        \
        }                                                              \
    } while (0)

/* ── Legacy reporting-dialect shims ──────────────────────────────── *
 * Guarded so a file still carrying its own copy migrates cleanly; the
 * local copies are deleted as each file moves onto MD_CHECK. */
#ifndef TEST
#define TEST(name)   do { md_test_name = (name);                       \
                          printf("  test: %s ... ", (name)); } while (0)
#endif
#ifndef PASS
#define PASS(...)    printf("  PASS\n")
#endif
#ifndef FAIL
#define FAIL(msg)    do { md_test_failures++;                          \
                          printf("FAIL: %s\n", (msg)); } while (0)
#endif
#ifndef RUN_TEST
#define RUN_TEST(fn) do { md_test_name = #fn; fn(); } while (0)
#endif

static inline int md_test_report(void)
{
    if (md_test_failures) {
        fprintf(stderr, "FAILED: %d of %d checks failed\n",
                md_test_failures, md_test_checks);
        return 1;
    }
    printf("PASSED: %d checks\n", md_test_checks);
    return 0;
}

#endif /* MD_TEST_H */
