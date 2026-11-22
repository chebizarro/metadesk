/* Opt-in product-session fixture. Snapshot ALL pasteboard types in RAM before
 * any mutation, refuse incomplete/oversized snapshots, restore after products stop. */
#include "clipboard.h"
#include "session.h"
#import <AppKit/AppKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
 @autoreleasepool {
    NSPasteboard *pb=[NSPasteboard generalPasteboard];
    NSMutableArray<NSPasteboardItem*> *saved=[NSMutableArray array]; NSUInteger total=0;
    for (NSPasteboardItem *item in pb.pasteboardItems) {
        NSPasteboardItem *copy=[[NSPasteboardItem alloc] init];
        for (NSPasteboardType type in item.types) {
            NSData *data=[item dataForType:type];
            if (!data || data.length>8*1024*1024-total || ![copy setData:data forType:type]) {
                fprintf(stderr,"incomplete clipboard snapshot; no mutation performed\n"); return 1;
            }
            total+=data.length;
        }
        [saved addObject:copy];
    }
    MdClipboard *c=md_clipboard_create(MD_CAP_CLIPBOARD,MD_CLIPBOARD_BOTH);
    if (!c) return 1;
    puts("READY"); fflush(stdout); char line[512]; bool mutated=false; int rc=0;
    while (fgets(line,sizeof(line),stdin)) {
        line[strcspn(line,"\r\n")]=0;
        if (!strcmp(line,"restore")) break;
        if (!strncmp(line,"set md-w15-",11)) {
            const char *value=line+4;
            if (md_clipboard_receive(c,(const uint8_t*)value,strlen(value))) { rc=1; break; }
            mutated=true; puts("SET"); fflush(stdout);
        } else if (!strncmp(line,"check md-w15-",13)) {
            NSString *value=[pb stringForType:NSPasteboardTypeString];
            if (!value || strcmp(value.UTF8String,line+6)) { rc=1; break; }
            puts("MATCH"); fflush(stdout);
        } else { rc=1; break; }
    }
    if (mutated) {
        NSString *current=[pb stringForType:NSPasteboardTypeString];
        if (![current hasPrefix:@"md-w15-"]) {
            fprintf(stderr,"clipboard changed outside test; not overwriting user's newer clipboard\n"); rc=1;
        } else {
            [pb clearContents];
            if (saved.count && ![pb writeObjects:saved]) rc=1;
        }
    }
    md_clipboard_destroy(c); puts(rc ? "RESTORE_ERROR" : "RESTORED"); fflush(stdout); return rc;
 }
}
