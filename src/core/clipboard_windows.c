#include "clipboard.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
static int revision(void *ctx,uint64_t *out) { (void)ctx; *out=GetClipboardSequenceNumber(); return 0; }
static int read_text(void *ctx,char **out,size_t *size) {
    (void)ctx; if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return 1;
    if (!OpenClipboard(NULL)) return -1;
    int rc=-1; HANDLE h=GetClipboardData(CF_UNICODETEXT);
    SIZE_T bytes=h ? GlobalSize(h) : 0;
    if (!bytes || bytes>2*(MD_CLIPBOARD_MAX_UTF8+1)) goto done;
    const WCHAR *w=GlobalLock(h); if (!w) goto done;
    size_t count=0,limit=bytes/sizeof(WCHAR);
    while (count<limit && w[count]) count++;
    if (count<limit) {
        int n=count ? WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w,(int)count,NULL,0,NULL,NULL) : 0;
        if ((!count || n>0) && n<=(int)MD_CLIPBOARD_MAX_UTF8) {
            char *p=malloc((size_t)n+1);
            if (p && (!n || WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w,(int)count,p,n,NULL,NULL)==n)) {
                p[n]=0; *out=p; *size=(size_t)n; rc=0;
            } else free(p);
        }
    }
    GlobalUnlock(h);
 done: CloseClipboard(); return rc;
}
static int write_text(void *ctx,const uint8_t *text,size_t size) {
    (void)ctx;
    int n=size ? MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char*)text,(int)size,NULL,0) : 0;
    if (size && n<=0) return -1;
    HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,((size_t)n+1)*sizeof(WCHAR)); if (!h) return -1;
    WCHAR *w=GlobalLock(h); if (!w) { GlobalFree(h); return -1; }
    bool ok=!n || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(const char*)text,(int)size,w,n)==n;
    w[n]=0; GlobalUnlock(h);
    /* EmptyClipboard with a NULL owner makes SetClipboardData fail. A
     * private non-delayed owner needs no message pump or interactive window. */
    HWND owner=ok ? CreateWindowExW(0,L"STATIC",L"metadesk-clipboard",0,0,0,0,0,
                                    HWND_MESSAGE,NULL,GetModuleHandleW(NULL),NULL) : NULL;
    if (!owner || !OpenClipboard(owner)) { if (owner) DestroyWindow(owner); GlobalFree(h); return -1; }
    ok=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,h)!=NULL;
    CloseClipboard(); DestroyWindow(owner); if (!ok) GlobalFree(h); return ok ? 0 : -1;
}
const MdClipboardBackend *md_clipboard_backend(void) { static const MdClipboardBackend b={revision,read_text,write_text}; return &b; }
const char *md_clipboard_unavailable_reason(void) { return "Windows clipboard unavailable (interactive desktop required)"; }
