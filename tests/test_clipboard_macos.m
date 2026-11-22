#include "clipboard.h"
#include "session.h"
#include "md_test.h"
#import <AppKit/AppKit.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    @autoreleasepool {
        NSPasteboard *pb=[NSPasteboard pasteboardWithUniqueName];
        MdClipboard *c=md_clipboard_create_with_backend(MD_CAP_CLIPBOARD,MD_CLIPBOARD_BOTH,md_clipboard_backend(),(__bridge void*)pb);
        const char *s="W15 UTF-8 \xe2\x9c\x93";
        MD_CHECK(!md_clipboard_receive(c,(const uint8_t*)s,strlen(s)));
        char *text=NULL; size_t n=0;
        MD_CHECK(md_clipboard_poll(c,&text,&n)==0);
        [pb clearContents]; [pb setString:@"Local edit" forType:NSPasteboardTypeString];
        MD_CHECK(md_clipboard_poll(c,&text,&n)==1 && n==10 && !strcmp(text,"Local edit"));
        free(text); md_clipboard_destroy(c); [pb releaseGlobally];
    }
    return md_test_report();
}
