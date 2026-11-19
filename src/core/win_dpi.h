/*
 * metadesk — win_dpi.h
 * Windows only, C++: read desktop geometry in physical pixels.
 *
 * A process that is not per-monitor DPI aware sees DPI-virtualized desktop
 * coordinates (GetMonitorInfo, GetSystemMetrics, DXGI_OUTPUT_DESC), scaled
 * differently per monitor on a mixed-DPI desktop. Desktop Duplication images
 * and SendInput's MOUSEEVENTF_VIRTUALDESK normalisation are in physical
 * pixels, so capture and input agree only when both query geometry in
 * physical pixels. MdPhysicalPixels makes the calling thread per-monitor
 * aware (v2, else v1) for its scope and restores the previous context.
 * Windows before 10 1607 has no thread DPI contexts; the scope is then a
 * no-op and the process's own awareness applies.
 */
#ifndef MD_WIN_DPI_H
#define MD_WIN_DPI_H

#include <windows.h>

class MdPhysicalPixels {
public:
    MdPhysicalPixels() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32)
            set_ = reinterpret_cast<SetContext>(reinterpret_cast<void *>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
        if (!set_) return;
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (-4), else _V1 (-3). */
        previous_ = set_(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        if (!previous_) previous_ = set_(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-3)));
    }
    ~MdPhysicalPixels() {
        if (previous_) set_(previous_);
    }
    MdPhysicalPixels(const MdPhysicalPixels &) = delete;
    MdPhysicalPixels &operator=(const MdPhysicalPixels &) = delete;

private:
    /* SetThreadDpiAwarenessContext; DPI_AWARENESS_CONTEXT is a handle. */
    typedef HANDLE(WINAPI *SetContext)(HANDLE);
    SetContext set_ = nullptr;
    HANDLE previous_ = nullptr;
};

#endif /* MD_WIN_DPI_H */
