/* Standalone process fixture using the production signal/stop implementation. */
#include "../src/host/lifecycle.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <wchar.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc == 2 && !strcmp(argv[1], "--managed-stop")) return md_host_lifecycle_stop();
#else
    sigset_t block, prior; sigemptyset(&block);
    sigaddset(&block, SIGTERM); sigaddset(&block, SIGINT);
    if (sigprocmask(SIG_BLOCK, &block, &prior)) return 2;
#endif
    if (md_host_lifecycle_init(true)) return 2;
    if (argc == 2 && !strcmp(argv[1], "--config")) return md_host_lifecycle_finish(78);
    if (argc == 2 && !strcmp(argv[1], "--failure")) return md_host_lifecycle_finish(1);
    puts("lifecycle-ready"); fflush(stdout);
#ifdef _WIN32
    if (argc == 3 && !strcmp(argv[1], "--ready-event")) {
        HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, argv[2]);
        if (!ready || !SetEvent(ready)) return 2;
        CloseHandle(ready);
    }
#endif
#ifdef _WIN32
    /* Open the same session-local user event, independently of the module's
     * private handle, so this fixture waits on an event, not elapsed time. */
    HANDLE token; DWORD n = 0; BYTE buffer[512]; LPWSTR sid = NULL; wchar_t name[256];
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) ||
        !GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &n) ||
        !ConvertSidToStringSidW(((TOKEN_USER *)buffer)->User.Sid, &sid)) return 2;
    swprintf(name,256,L"Local\\Metadesk.Host.Stop.%ls",sid); LocalFree(sid); CloseHandle(token);
    HANDLE event=OpenEventW(SYNCHRONIZE,FALSE,name); if (!event) return 2;
    DWORD result=WaitForSingleObject(event,30000); CloseHandle(event);
    if (result != WAIT_OBJECT_0) return 2;
#else
    while (!md_host_lifecycle_stopping()) sigsuspend(&prior);
#endif
    /* A stop overrides an interrupted startup failure, not a crash status. */
    return md_host_lifecycle_finish(1);
}
