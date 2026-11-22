#include "lifecycle.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <cjson/cJSON.h>
#ifdef __APPLE__
#include <os/log.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <wchar.h>
static HANDLE stop_event, shutdown_complete;
/* Local namespace separates interactive sessions. SID separates users. A
 * protected owner-only DACL prevents another user from requesting a stop. */
static bool event_identity(wchar_t name[256], PSECURITY_DESCRIPTOR *sd) {
    HANDLE token = NULL; DWORD size = 0; LPWSTR sid = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = malloc(size);
    bool ok = user && GetTokenInformation(token, TokenUser, user, size, &size) &&
        ConvertSidToStringSidW(user->User.Sid, &sid);
    if (ok) {
        swprintf(name, 256, L"Local\\Metadesk.Host.Stop.%ls", sid);
        if (sd) {
            wchar_t sddl[512];
            swprintf(sddl, 512, L"O:%lsD:P(A;;GA;;;%ls)", sid, sid);
            ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, sd, NULL);
        }
    }
    LocalFree(sid); free(user); CloseHandle(token); return ok;
}
static BOOL WINAPI interrupt(DWORD kind) {
    switch (kind) {
    case CTRL_C_EVENT: case CTRL_BREAK_EVENT:
        SetEvent(stop_event); return TRUE;
    case CTRL_CLOSE_EVENT: case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT:
        SetEvent(stop_event);
        /* Returning lets Windows tear down this process. Keep the callback
         * alive until normal cleanup completes; the OS owns its hard deadline. */
        WaitForSingleObject(shutdown_complete, INFINITE);
        return TRUE;
    default: return FALSE;
    }
}
int md_host_lifecycle_stop(void) {
    wchar_t name[256];
    if (!event_identity(name, NULL)) return 1;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!event) return 1;
    bool ok = SetEvent(event); CloseHandle(event); return ok ? 0 : 1;
}
#else
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void interrupt(int sig) { (void)sig; stopped = 1; }
#endif
static bool is_managed;
static void managed_sink(MdLogLevel level, const char *tag, const char *message, void *data) {
    (void)data;
    cJSON *j = cJSON_CreateObject();
    static const char *names[] = {"error", "warning", "info", "debug"};
    cJSON_AddStringToObject(j, "level", names[level <= MD_LOG_DEBUG ? level : MD_LOG_INFO]);
    cJSON_AddStringToObject(j, "component", tag ? tag : "host");
    cJSON_AddStringToObject(j, "message", message);
    char *line = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!line) return;
    fprintf(stderr, "%s\n", line);
#ifdef __APPLE__
    os_log_with_type(OS_LOG_DEFAULT, level == MD_LOG_ERROR ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
                     "%{public}s", line);
#endif
    free(line);
}
int md_host_lifecycle_init(bool managed) {
    is_managed = managed;
    if (managed) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        md_log_set_sink(managed_sink, NULL);
    }
#ifdef _WIN32
    shutdown_complete = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!shutdown_complete) return -1;
    if (managed) {
        wchar_t name[256]; PSECURITY_DESCRIPTOR sd = NULL;
        if (!event_identity(name, &sd)) return -1;
        SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
        stop_event = CreateEventW(&sa, TRUE, FALSE, name);
        DWORD error = GetLastError(); LocalFree(sd);
        /* Never adopt a pre-existing object's permissions or signal state. */
        if (!stop_event || error == ERROR_ALREADY_EXISTS) {
            if (stop_event) CloseHandle(stop_event);
            stop_event = NULL; return -1;
        }
    } else stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    return stop_event && SetConsoleCtrlHandler(interrupt, TRUE) ? 0 : -1;
#else
    struct sigaction sa = {0};
    sa.sa_handler = interrupt; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) return -1;
    return signal(SIGPIPE, SIG_IGN) == SIG_ERR ? -1 : 0;
#endif
}
bool md_host_lifecycle_stopping(void) {
#ifdef _WIN32
    return stop_event && WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
#else
    return stopped != 0;
#endif
}
void md_host_lifecycle_event(const char *state, int status) {
    if (!is_managed) return;
    char message[160];
    snprintf(message, sizeof(message), "event=host_lifecycle state=%s exit_code=%d", state, status);
    md_log_emit(status ? MD_LOG_ERROR : MD_LOG_INFO, "host", "%s", message);
}
int md_host_lifecycle_finish(int status) {
    if (md_host_lifecycle_stopping()) status = 0;
    md_host_lifecycle_event("exited", status);
#ifdef _WIN32
    if (shutdown_complete) SetEvent(shutdown_complete);
    if (stop_event) SetConsoleCtrlHandler(interrupt, FALSE);
    /* Process-lifetime handles: a close/logoff callback may still be returning
     * from its wait. Never CloseHandle concurrently with that wait. */
#endif
    return status;
}
