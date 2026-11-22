/* Opt-in console qualification: creates its own window and injects one click
 * into its own button. Does NOT change display settings. Run at the requested
 * real display scale; the optional argument is the required window DPI. */
#include <windows.h>
#include <uiautomation.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include "win_dpi.h"
#include "capture.h"
#include "input.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (error=%lu)\n", __LINE__, #x, GetLastError()); ExitProcess(1); } } while (0)
static HANDLE ready, clicked;
static HWND window, button;
static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == 101 && HIWORD(wp) == BN_CLICKED)
        SetEvent(clicked);
    if (msg == WM_CLOSE) { DestroyWindow(w); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(w, msg, wp, lp);
}
static DWORD WINAPI fixture(void *) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"metadesk-owned-dpi-fixture";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    CHECK(RegisterClassW(&wc));
    window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"metadesk owned DPI fixture",
        WS_POPUP | WS_VISIBLE, 80, 80, 500, 350, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(window);
    button = CreateWindowW(L"BUTTON", L"Owned DPI click target", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        60, 80, 240, 80, window, (HMENU)101, wc.hInstance, nullptr);
    CHECK(button);
    SetForegroundWindow(window);
    SendMessageW(window, WM_SYSCOMMAND, SC_MONITORPOWER, -1);
    SetEvent(ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
static RECT uia_rect(IUIAutomation *uia, HWND w) {
    IUIAutomationElement *element = nullptr;
    CHECK(SUCCEEDED(uia->ElementFromHandle(w, &element)) && element);
    RECT actual, expected;
    CHECK(SUCCEEDED(element->get_CurrentBoundingRectangle(&actual)));
    CHECK(GetWindowRect(w, &expected));
    printf("UIA=[%ld,%ld,%ld,%ld] GetWindowRect=[%ld,%ld,%ld,%ld]\n",
        actual.left, actual.top, actual.right, actual.bottom,
        expected.left, expected.top, expected.right, expected.bottom);
    CHECK(EqualRect(&actual, &expected));
    element->Release();
    return actual;
}
int main(int argc, char **argv) {
    /* The resource is deliberately the product manifest: prove both the loader
     * default and init's already-set/ERROR_ACCESS_DENIED branch. */
    CHECK(md_win_dpi_process_awareness() == MD_DPI_PROCESS_PER_MONITOR_V2);
    SetLastError(0);
    CHECK(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    CHECK(GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(md_win_dpi_process_init() == MD_DPI_PROCESS_PER_MONITOR_V2);
    puts("PASS manifest PerMonitorV2 and init ERROR_ACCESS_DENIED path");
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    clicked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(ready && clicked);
    POINT original_pointer;
    CHECK(GetCursorPos(&original_pointer));
    HANDLE thread = CreateThread(nullptr, 0, fixture, nullptr, 0, nullptr);
    CHECK(thread && WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0);
    UINT dpi = GetDpiForWindow(window);
    printf("window DPI=%u scale=%u%% monitors=%d\n", dpi, dpi * 100 / 96, GetSystemMetrics(SM_CMONITORS));
    if (argc > 1) CHECK(dpi == (UINT)atoi(argv[1]));
    IUIAutomation *uia = nullptr;
    CHECK(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_IUIAutomation, (void **)&uia)));
    uia_rect(uia, window);
    RECT target = uia_rect(uia, button);

    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    IDXGIOutput1 *output1 = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *immediate = nullptr;
    IDXGIOutputDuplication *duplication = nullptr;
    CHECK(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)));
    CHECK(SUCCEEDED(factory->EnumAdapters1(0, &adapter)));
    DXGI_ADAPTER_DESC1 adapter_desc;
    CHECK(SUCCEEDED(adapter->GetDesc1(&adapter_desc)));
    printf("DXGI adapter=%ls vendor=%04x device=%04x flags=%u\n", adapter_desc.Description,
        adapter_desc.VendorId, adapter_desc.DeviceId, adapter_desc.Flags);
    CHECK(SUCCEEDED(adapter->EnumOutputs(0, &output)));
    DXGI_OUTPUT_DESC desc;
    CHECK(SUCCEEDED(output->GetDesc(&desc)));
    CHECK(SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1)));
    CHECK(SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &immediate)));
    CHECK(SUCCEEDED(output1->DuplicateOutput(device, &duplication)));
    DXGI_OUTDUPL_DESC mode;
    duplication->GetDesc(&mode);
    /* Rotation needs its own qualification; don't call swapped dimensions a pass. */
    CHECK(mode.Rotation == DXGI_MODE_ROTATION_IDENTITY);
    MdCaptureConfig capture_cfg = {};
    capture_cfg.output = "0";
    MdCaptureCtx *capture = md_capture_create(&capture_cfg);
    CHECK(capture && md_capture_start(capture) == 0);
    MdCaptureRect rect;
    CHECK(md_capture_get_output_rect(capture, &rect) == 0);
    printf("capture=[%d,%d,%u,%u] duplicate=%ux%u output=[%ld,%ld,%ld,%ld]\n",
        rect.x, rect.y, rect.width, rect.height, mode.ModeDesc.Width, mode.ModeDesc.Height,
        desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
        desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom);
    CHECK(rect.x == desc.DesktopCoordinates.left && rect.y == desc.DesktopCoordinates.top);
    CHECK(rect.width == mode.ModeDesc.Width && rect.height == mode.ModeDesc.Height);
    MdInputConfig input_cfg = {};
    input_cfg.screen_width = rect.width;
    input_cfg.screen_height = rect.height;
    input_cfg.output_x = rect.x; input_cfg.output_y = rect.y;
    input_cfg.output_width = rect.width; input_cfg.output_height = rect.height;
    MdInput *input = md_input_create(&input_cfg);
    CHECK(input);
    int x = (target.left + target.right) / 2, y = (target.top + target.bottom) / 2;
    POINT center = {x, y};
    CHECK(WindowFromPoint(center) == button); /* Never inject onto another app. */
    CHECK(md_input_click(input, x - rect.x, y - rect.y, MD_MOUSE_LEFT) == 0);
    CHECK(WaitForSingleObject(clicked, 10000) == WAIT_OBJECT_0);
    printf("PASS production SendInput click at UIA physical center %d,%d delivered BN_CLICKED\n", x, y);
    md_input_destroy(input);
    md_capture_destroy(capture);
    duplication->Release(); immediate->Release(); device->Release();
    output1->Release(); output->Release(); adapter->Release(); factory->Release();
    uia->Release();
    PostMessageW(window, WM_CLOSE, 0, 0);
    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CHECK(SetCursorPos(original_pointer.x, original_pointer.y));
    CloseHandle(thread); CloseHandle(clicked); CloseHandle(ready);
    CoUninitialize();
    puts("ALL LIVE DPI TESTS PASSED");
    return 0;
}
