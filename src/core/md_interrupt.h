#ifndef MD_INTERRUPT_H
#define MD_INTERRUPT_H
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
/* Process-lifetime console-control notification; no callback retains a bridge. */
HANDLE md_win_interrupt_event(void);
#endif
#endif
