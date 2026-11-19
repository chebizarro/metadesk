/*
 * metadesk — win_dpi.h
 * Windows only, C and C++: read desktop geometry in physical pixels.
 *
 * A process that is not per-monitor DPI aware sees DPI-virtualized desktop
 * coordinates (GetMonitorInfo, GetSystemMetrics, DXGI_OUTPUT_DESC, UI
 * Automation bounding rectangles), scaled differently per monitor on a
 * mixed-DPI desktop. Desktop Duplication images and SendInput's
 * MOUSEEVENTF_VIRTUALDESK normalisation are in physical pixels, so capture,
 * accessibility and input agree only when all of them read geometry in
 * physical pixels.
 *
 * md_win_dpi_process_init (C and C++) makes the whole process per-monitor
 * aware. The host calls it first thing in main(), before any window, UIA,
 * DXGI or input call, so every thread it creates later starts per-monitor
 * aware. MdPhysicalPixels (C++) makes the calling thread per-monitor aware
 * for its scope and restores the previous context. The backends scope their
 * geometry queries with it, so they stay correct in a process that did not
 * call md_win_dpi_process_init (tools, tests, an embedding application).
 * Windows before 10 1607 has no thread DPI contexts; the scope is then a
 * no-op and the process's own awareness applies.
 */
#ifndef MD_WIN_DPI_H
#define MD_WIN_DPI_H

#include <windows.h>

/* DPI_AWARENESS_CONTEXT values (Windows 10 1607+), spelled out because
 * older SDK headers lack them. DPI_AWARENESS_CONTEXT is a handle. */
#define MD_DPI_CONTEXT_UNAWARE           ((HANDLE)(INT_PTR)-1)
#define MD_DPI_CONTEXT_PER_MONITOR       ((HANDLE)(INT_PTR)-3)
#define MD_DPI_CONTEXT_PER_MONITOR_V2    ((HANDLE)(INT_PTR)-4)

/* What md_win_dpi_process_init achieved. */
#define MD_DPI_PROCESS_PER_MONITOR_V2  2 /* Windows 10 1703+ */
#define MD_DPI_PROCESS_PER_MONITOR     1 /* v1 (8.1 / 10 1607) */
#define MD_DPI_PROCESS_NOT_PER_MONITOR 0 /* system aware or unaware */

/* user32 / shcore entry points, resolved at run time so the binary still
 * loads on systems without them. GetProcAddress results are cast through
 * MdWinProc, the generic function pointer type -Wcast-function-type accepts. */
typedef void (*MdWinProc)(void);
typedef BOOL(WINAPI *MdSetProcessDpiContextFn)(HANDLE);
typedef HANDLE(WINAPI *MdGetDpiContextFn)(void);
typedef HANDLE(WINAPI *MdGetProcessDpiContextFn)(HANDLE);
typedef BOOL(WINAPI *MdDpiContextsEqualFn)(HANDLE, HANDLE);
typedef int(WINAPI *MdDpiContextAwarenessFn)(HANDLE);
typedef HRESULT(WINAPI *MdSetProcessDpiAwarenessFn)(int);
typedef HRESULT(WINAPI *MdGetProcessDpiAwarenessFn)(HANDLE, int *);

/* An export of a System32 module, or NULL. The module is loaded if the
 * process does not import it (user32 is not loaded into a process that
 * calls none of its functions; shcore never is by default) and stays
 * loaded. */
static inline MdWinProc md_win_dpi_proc(const wchar_t *module, const char *name) {
    HMODULE m = GetModuleHandleW(module);
    if (!m) m = LoadLibraryExW(module, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m ? (MdWinProc)GetProcAddress(m, name) : NULL;
}

/* The process's current awareness, one of MD_DPI_PROCESS_*. */
static inline int md_win_dpi_process_awareness(void) {
    MdGetProcessDpiContextFn for_process = (MdGetProcessDpiContextFn)md_win_dpi_proc(
        L"user32.dll", "GetDpiAwarenessContextForProcess"); /* 10 1803+ */
    MdGetDpiContextFn for_thread =
        (MdGetDpiContextFn)md_win_dpi_proc(L"user32.dll", "GetThreadDpiAwarenessContext");
    MdDpiContextsEqualFn equal =
        (MdDpiContextsEqualFn)md_win_dpi_proc(L"user32.dll", "AreDpiAwarenessContextsEqual");
    MdDpiContextAwarenessFn awareness = (MdDpiContextAwarenessFn)md_win_dpi_proc(
        L"user32.dll", "GetAwarenessFromDpiAwarenessContext");
    /* 10 1607 without the process query: the thread's context is the
     * process's unless a scope changed it, which a caller of this does not. */
    HANDLE context = for_process ? for_process(NULL) : for_thread ? for_thread() : NULL;
    if (context && equal && awareness) {
        if (equal(context, MD_DPI_CONTEXT_PER_MONITOR_V2)) return MD_DPI_PROCESS_PER_MONITOR_V2;
        return awareness(context) == 2 /* DPI_AWARENESS_PER_MONITOR_AWARE */
            ? MD_DPI_PROCESS_PER_MONITOR : MD_DPI_PROCESS_NOT_PER_MONITOR;
    }
    MdGetProcessDpiAwarenessFn shcore_get =
        (MdGetProcessDpiAwarenessFn)md_win_dpi_proc(L"shcore.dll", "GetProcessDpiAwareness");
    int value = 0;
    return shcore_get && SUCCEEDED(shcore_get(NULL, &value)) && value == 2
        ? MD_DPI_PROCESS_PER_MONITOR : MD_DPI_PROCESS_NOT_PER_MONITOR;
}

/* Make the process per-monitor aware: v2 through user32 (Windows 10
 * 1703+), else v1 through shcore (8.1 to 10 1607). Call it before the
 * process creates any window. Once a manifest or an earlier call has set
 * the awareness, Windows rejects the change (ERROR_ACCESS_DENIED /
 * E_ACCESSDENIED) and the awareness already in effect is reported. Returns
 * one of MD_DPI_PROCESS_*. */
static inline int md_win_dpi_process_init(void) {
    MdSetProcessDpiContextFn set_context = (MdSetProcessDpiContextFn)md_win_dpi_proc(
        L"user32.dll", "SetProcessDpiAwarenessContext");
    if (set_context) {
        if (set_context(MD_DPI_CONTEXT_PER_MONITOR_V2)) return MD_DPI_PROCESS_PER_MONITOR_V2;
        if (GetLastError() != ERROR_ACCESS_DENIED &&
            set_context(MD_DPI_CONTEXT_PER_MONITOR))
            return MD_DPI_PROCESS_PER_MONITOR;
        return md_win_dpi_process_awareness();
    }
    /* Windows 8.1 to 10 1607: shcore. 2: PROCESS_PER_MONITOR_DPI_AWARE. */
    MdSetProcessDpiAwarenessFn shcore_set =
        (MdSetProcessDpiAwarenessFn)md_win_dpi_proc(L"shcore.dll", "SetProcessDpiAwareness");
    if (shcore_set && SUCCEEDED(shcore_set(2))) return MD_DPI_PROCESS_PER_MONITOR;
    return md_win_dpi_process_awareness();
}

#ifdef __cplusplus

class MdPhysicalPixels {
public:
    MdPhysicalPixels() {
        set_ = (SetContext)md_win_dpi_proc(L"user32.dll", "SetThreadDpiAwarenessContext");
        if (!set_) return;
        previous_ = set_(MD_DPI_CONTEXT_PER_MONITOR_V2);
        if (!previous_) previous_ = set_(MD_DPI_CONTEXT_PER_MONITOR);
    }
    ~MdPhysicalPixels() {
        if (previous_) set_(previous_);
    }
    MdPhysicalPixels(const MdPhysicalPixels &) = delete;
    MdPhysicalPixels &operator=(const MdPhysicalPixels &) = delete;

private:
    /* SetThreadDpiAwarenessContext. */
    typedef HANDLE(WINAPI *SetContext)(HANDLE);
    SetContext set_ = nullptr;
    HANDLE previous_ = nullptr;
};

#endif /* __cplusplus */

#endif /* MD_WIN_DPI_H */
