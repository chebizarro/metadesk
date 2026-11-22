/* Own process/window-station clipboard only. Never changes the console station. */
#include "clipboard.h"
#include "session.h"
#include "md_test.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    HWINSTA original=GetProcessWindowStation(); HDESK desktop=GetThreadDesktop(GetCurrentThreadId());
    HWINSTA station=CreateWindowStationW(NULL,0,WINSTA_ALL_ACCESS,NULL);
    if (!station) { fprintf(stderr,"SKIP: private window station unavailable (%lu)\n",GetLastError()); return 77; }
    if (!SetProcessWindowStation(station)) { CloseWindowStation(station); fprintf(stderr,"SKIP: private window station cannot be selected\n"); return 77; }
    HDESK owned=CreateDesktopW(L"w15-clipboard",NULL,NULL,0,GENERIC_ALL,NULL);
    if (!owned || !SetThreadDesktop(owned)) {
        SetProcessWindowStation(original); if (owned) CloseDesktop(owned); CloseWindowStation(station);
        fprintf(stderr,"SKIP: private clipboard desktop unavailable\n"); return 77;
    }
    MdClipboard *receiver=md_clipboard_create(MD_CAP_CLIPBOARD,MD_CLIPBOARD_RECEIVE);
    const char *value="W15 unicode \xe2\x9c\x93";
    MD_CHECK(receiver && !md_clipboard_receive(receiver,(const uint8_t*)value,strlen(value)));
    MdClipboard *sender=md_clipboard_create(MD_CAP_CLIPBOARD,MD_CLIPBOARD_SEND);
    char *text=NULL; size_t size=0;
    MD_CHECK(sender && md_clipboard_poll(sender,&text,&size)==1 && size==strlen(value) && !strcmp(text,value));
    free(text); md_clipboard_destroy(receiver); md_clipboard_destroy(sender);
    MD_CHECK(SetProcessWindowStation(original)); MD_CHECK(SetThreadDesktop(desktop));
    MD_CHECK(CloseDesktop(owned)); MD_CHECK(CloseWindowStation(station));
    return md_test_report();
}
