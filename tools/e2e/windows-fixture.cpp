// Owned Win32 damage/input target and native child supervisor for attended E2E.
// Usage: windows-fixture.exe COMMAND_FILE STOP_FILE [DONE_FILE]
// COMMAND_FILE is private UTF-8 CreateProcess argv, never printed. STOP_FILE is
// an owned cancellation marker, not a readiness/completion signal. Run in an
// isolated interactive scheduled-task console, never in an operator terminal.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

static HWND label, clicks_label;
static HANDLE child, job;
static DWORD child_pid;
static unsigned frames, clicks;
static std::wstring stop_file, done_file;
static bool stopping, expired;
static ULONGLONG stopped_at, started_at;
// Attended checks may wait on an external person; this is only a leak guard.
static constexpr ULONGLONG safety_ms = 12ULL * 60 * 60 * 1000;
static const GUID display_state = {0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};

static void stop_child() {
    if (!stopping) {
        stopping = true;
        stopped_at = GetTickCount64();
        // This task owns the console and every process attached to it. The
        // helper handles Ctrl+C; the product receives its normal SIGINT.
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        std::puts("fixture: owned console cancellation requested");
    }
}

static LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        label = CreateWindowW(L"STATIC", L"md-l7 frame 0", WS_CHILD|WS_VISIBLE,
                              20,20,400,30,w,nullptr,nullptr,nullptr);
        clicks_label = CreateWindowW(L"STATIC", L"md-l7 clicks 0", WS_CHILD|WS_VISIBLE,
                                     20,60,400,30,w,nullptr,nullptr,nullptr);
        CreateWindowW(L"BUTTON", L"md-l7 action", WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                      20,100,240,50,w,reinterpret_cast<HMENU>(1),nullptr,nullptr);
        CreateWindowW(L"EDIT", L"md-l7 owned text", WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,
                      20,180,400,40,w,reinterpret_cast<HMENU>(2),nullptr,nullptr);
        SetTimer(w,1,100,nullptr); // Damage generation and owned process safety only.
        PostMessageW(w,WM_SYSCOMMAND,SC_MONITORPOWER,-1);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp)==1 && HIWORD(wp)==BN_CLICKED) {
            wchar_t s[80]; swprintf(s,80,L"md-l7 clicks %u",++clicks);
            SetWindowTextW(clicks_label,s);
            std::printf("fixture: OWNED button clicks=%u\n",clicks);
        } else if (LOWORD(wp)==2 && HIWORD(wp)==EN_CHANGE) {
            std::printf("fixture: OWNED text chars=%d\n",GetWindowTextLengthW(reinterpret_cast<HWND>(lp)));
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp==PBT_POWERSETTINGCHANGE) {
            auto p=reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
            if (IsEqualGUID(p->PowerSetting,display_state) && p->DataLength==sizeof(DWORD) &&
                *reinterpret_cast<DWORD*>(p->Data)==0)
                PostMessageW(w,WM_SYSCOMMAND,SC_MONITORPOWER,-1);
        }
        return TRUE;
    case WM_TIMER: {
        wchar_t s[80]; swprintf(s,80,L"md-l7 frame %u",++frames);
        SetWindowTextW(label,s);
        if (GetFileAttributesW(stop_file.c_str())!=INVALID_FILE_ATTRIBUTES) stop_child();
        ULONGLONG now=GetTickCount64();
        if (now-started_at>=safety_ms) { expired=true; stop_child(); }
        if (child && WaitForSingleObject(child,0)==WAIT_OBJECT_0) {
            DWORD code; GetExitCodeProcess(child,&code);
            std::printf("fixture: child pid=%lu exit=%lu frames=%u clicks=%u\n",child_pid,code,frames,clicks);
            PostQuitMessage(expired ? 124 : static_cast<int>(code));
        } else if (stopping && now-stopped_at>=10000) {
            std::puts("fixture: FAIL forced owned job termination");
            TerminateJobObject(job,124);
            PostQuitMessage(124);
        }
        return 0;
    }
    case WM_CLOSE: stop_child(); return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}

static BOOL WINAPI console_handler(DWORD type) {
    return type==CTRL_C_EVENT || type==CTRL_BREAK_EVENT;
}

static std::wstring widen(const std::string &s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring w(n,L'\0');
    if (n) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),w.data(),n);
    return w;
}

int main(int argc,char **argv) {
    if (argc!=3 && argc!=4) return 2;
    setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream file(argv[1],std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    auto command=widen(text); stop_file=widen(argv[2]);
    if (argc==4) done_file=widen(argv[3]);
    if (command.empty() || stop_file.empty() || GetFileAttributesW(stop_file.c_str())!=INVALID_FILE_ATTRIBUTES ||
        (argc==4 && (done_file.empty() || GetFileAttributesW(done_file.c_str())!=INVALID_FILE_ATTRIBUTES))) return 2;
    started_at=GetTickCount64();
    WNDCLASSW wc{};wc.lpfnWndProc=window_proc;wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"md-l7-owned-window";wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if (!RegisterClassW(&wc)) return 1;
    HWND w=CreateWindowW(wc.lpszClassName,L"md-l7 Owned Windows Target",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                         50,50,540,360,nullptr,nullptr,wc.hInstance,nullptr);
    if (!w) return 1;
    HPOWERNOTIFY notify=RegisterPowerSettingNotification(w,&display_state,DEVICE_NOTIFY_WINDOW_HANDLE);
    job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit))) return 1;
    SetConsoleCtrlHandler(console_handler,TRUE);
    SetThreadExecutionState(ES_CONTINUOUS|ES_DISPLAY_REQUIRED|ES_SYSTEM_REQUIRED);
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED,nullptr,nullptr,&si,&pi)) return 1;
    if (!AssignProcessToJobObject(job,pi.hProcess)) {
        TerminateProcess(pi.hProcess,125);WaitForSingleObject(pi.hProcess,INFINITE);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(job);return 125;
    }
    child=pi.hProcess;child_pid=pi.dwProcessId;ResumeThread(pi.hThread);CloseHandle(pi.hThread);
    std::printf("fixture: ready owned-window child-pid=%lu\n",child_pid);
    MSG message{};int result;
    while ((result=GetMessageW(&message,nullptr,0,0))>0) { TranslateMessage(&message);DispatchMessageW(&message); }
    CloseHandle(job);WaitForSingleObject(child,INFINITE);
    DWORD child_code=STILL_ACTIVE;
    bool have_code=GetExitCodeProcess(child,&child_code)!=FALSE;
    CloseHandle(child);
    if (notify) UnregisterPowerSettingNotification(notify);
    DestroyWindow(w);SetThreadExecutionState(ES_CONTINUOUS);
    int code=have_code ? (result<0 ? 1 : static_cast<int>(message.wParam)) : 126;
    // Ctrl+C may abort the enclosing batch before it can write a done marker.
    // Publish after child exit and cleanup, independently of that batch.
    if (!done_file.empty()) {
        std::wstring temporary=done_file+L".tmp";
        HANDLE f=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
                             CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (f==INVALID_HANDLE_VALUE) return 126;
        std::string body="{\"native_exit\":"+std::to_string(have_code ? child_code : STILL_ACTIVE)+
            ",\"helper_result\":"+std::to_string(code)+",\"deadline\":"+(expired ? "true" : "false")+"}\n";
        DWORD written=0;
        bool ok=WriteFile(f,body.data(),static_cast<DWORD>(body.size()),&written,nullptr)!=FALSE && written==body.size();
        CloseHandle(f);
        if (!ok || !MoveFileExW(temporary.c_str(),done_file.c_str(),MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());return 126;
        }
    }
    return code;
}
