#include "clipboard.h"
#import <AppKit/AppKit.h>
#include <stdlib.h>
#include <string.h>
static NSPasteboard *pasteboard(void *ctx) { return ctx ? (__bridge NSPasteboard*)ctx : [NSPasteboard generalPasteboard]; }
static int revision(void *ctx,uint64_t *out) {
    @autoreleasepool { *out=(uint64_t)pasteboard(ctx).changeCount; return 0; }
}
static int read_text(void *ctx,char **out,size_t *size) {
    @autoreleasepool {
        NSString *s=[pasteboard(ctx) stringForType:NSPasteboardTypeString];
        if (!s) return 1;
        NSUInteger n=[s lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        if (n>MD_CLIPBOARD_MAX_UTF8) return -1;
        const char *p=[s UTF8String]; if (!p || strlen(p)!=n) return -1;
        *out=malloc(n+1); if (!*out) return -1;
        memcpy(*out,p,n+1); *size=n; return 0;
    }
}
static int write_text(void *ctx,const uint8_t *text,size_t size) {
    @autoreleasepool {
        NSString *s=[[NSString alloc] initWithBytes:text ? text : (const uint8_t*)"" length:size encoding:NSUTF8StringEncoding];
        if (!s) return -1;
        NSPasteboard *pb=pasteboard(ctx); [pb clearContents];
        return [pb setString:s forType:NSPasteboardTypeString] ? 0 : -1;
    }
}
const MdClipboardBackend *md_clipboard_backend(void) {
    static const MdClipboardBackend b={revision,read_text,write_text}; return &b;
}
const char *md_clipboard_unavailable_reason(void) { return "NSPasteboard unavailable"; }
