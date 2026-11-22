#include "md_interrupt.h"
#ifdef _WIN32
static INIT_ONCE interrupt_once = INIT_ONCE_STATIC_INIT;
static HANDLE interrupt_event;
static BOOL WINAPI console_interrupt(DWORD kind) {
    switch (kind) {
    case CTRL_C_EVENT: case CTRL_BREAK_EVENT: case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT: case CTRL_SHUTDOWN_EVENT:
        SetEvent(interrupt_event);
        return TRUE;
    default: return FALSE;
    }
}
static BOOL CALLBACK interrupt_init(PINIT_ONCE once, PVOID arg, PVOID *context) {
    (void)once; (void)arg; (void)context;
    interrupt_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!interrupt_event) return FALSE;
    if (!SetConsoleCtrlHandler(console_interrupt, TRUE)) {
        CloseHandle(interrupt_event);
        interrupt_event = NULL;
        return FALSE;
    }
    return TRUE;
}
HANDLE md_win_interrupt_event(void) {
    return InitOnceExecuteOnce(&interrupt_once, interrupt_init, NULL, NULL)
        ? interrupt_event : NULL;
}
#endif
