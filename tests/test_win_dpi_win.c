/* metadesk — tests/test_win_dpi_win.c (md-m5x4)
 *
 * Standalone Windows test for md_win_dpi_process_init, compiled as C with
 * the same include block as src/host/main.c (the host's only use of
 * win_dpi.h; the full meson Windows build is still blocked). In the console
 * session the process starts DPI unaware (no manifest) and becomes
 * per-monitor v2. It stays so on a repeated call (Windows refuses the
 * change with ERROR_ACCESS_DENIED), and threads created afterwards inherit
 * it. Over SSH (session 0) Windows already starts the process per-monitor
 * (v1) aware and refuses any change; init must then report that awareness.
 * Safe in any session: no window, no input.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "win_dpi.h"
#endif

#include <stdio.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    ExitProcess(1); \
} } while (0)

static int thread_is_per_monitor_v2(void) {
    MdGetDpiContextFn get =
        (MdGetDpiContextFn)md_win_dpi_proc(L"user32.dll", "GetThreadDpiAwarenessContext");
    MdDpiContextsEqualFn equal =
        (MdDpiContextsEqualFn)md_win_dpi_proc(L"user32.dll", "AreDpiAwarenessContextsEqual");
    return get && equal && equal(get(), MD_DPI_CONTEXT_PER_MONITOR_V2);
}

static DWORD WINAPI report_thread(LPVOID out) {
    *(int *)out = thread_is_per_monitor_v2();
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const int initial = md_win_dpi_process_awareness();
#ifdef MD_TEST_MANIFEST
    CHECK(initial == MD_DPI_PROCESS_PER_MONITOR_V2);
    MdSetProcessDpiContextFn set_context = (MdSetProcessDpiContextFn)md_win_dpi_proc(
        L"user32.dll", "SetProcessDpiAwarenessContext");
    CHECK(set_context);
    SetLastError(0);
    CHECK(!set_context(MD_DPI_CONTEXT_PER_MONITOR_V2));
    CHECK(GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(md_win_dpi_process_init() == MD_DPI_PROCESS_PER_MONITOR_V2);
    puts("PASS product manifest PerMonitorV2: init handles ERROR_ACCESS_DENIED");
#endif
    if (initial != MD_DPI_PROCESS_NOT_PER_MONITOR) {
        /* Preset by the environment (session 0): kept and reported. */
        CHECK(md_win_dpi_process_init() == initial);
        CHECK(md_win_dpi_process_awareness() == initial);
        printf("PASS process already per-monitor (v%d) at start: init reports it\n",
               initial == MD_DPI_PROCESS_PER_MONITOR_V2 ? 2 : 1);
        puts("ALL WIN DPI TESTS PASSED (preset awareness)");
        return 0;
    }
    CHECK(!thread_is_per_monitor_v2());

    CHECK(md_win_dpi_process_init() == MD_DPI_PROCESS_PER_MONITOR_V2);
    CHECK(md_win_dpi_process_awareness() == MD_DPI_PROCESS_PER_MONITOR_V2);
    CHECK(thread_is_per_monitor_v2());
    puts("PASS process init: DPI unaware -> per-monitor v2");

    /* Already set: Windows refuses, and the awareness in effect is reported. */
    CHECK(md_win_dpi_process_init() == MD_DPI_PROCESS_PER_MONITOR_V2);
    puts("PASS repeated init reports the awareness already in effect");

    int inherited = 0;
    HANDLE thread = CreateThread(NULL, 0, report_thread, &inherited, 0, NULL);
    CHECK(thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    CHECK(inherited);
    puts("PASS threads created afterwards are per-monitor v2");
    puts("ALL WIN DPI TESTS PASSED");
    return 0;
}
