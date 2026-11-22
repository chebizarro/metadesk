#include "clipboard.h"
#include "session.h"
#include <stdlib.h>
#include <string.h>
struct MdClipboard { const MdClipboardBackend *backend; void *ctx; uint8_t directions; bool seen; uint64_t revision; char *received; size_t received_size; };
uint8_t md_clipboard_reverse(uint8_t d) { return ((d&1u)<<1)|((d&2u)>>1); }
int md_clipboard_directions_parse(const char *s) {
    if (!s) return -1;
    const char *names[]={"off","send","receive","both"};
    for (int i=0;i<4;i++) if (!strcmp(s,names[i])) return i;
    return -1;
}
const char *md_clipboard_directions_name(uint8_t d) {
    const char *names[]={"off","send","receive","both"}; return d<4 ? names[d] : "invalid";
}
bool md_clipboard_text_valid(const uint8_t *s,size_t n) {
    if (n>MD_CLIPBOARD_MAX_UTF8 || (!s && n)) return false;
    for (size_t i=0;i<n;) {
        uint32_t c=s[i++], min=0; unsigned extra=0;
        if (!c) return false;
        if (c<0x80) continue;
        if (c>=0xc2 && c<=0xdf) { c&=0x1f; extra=1; min=0x80; }
        else if (c>=0xe0 && c<=0xef) { c&=0x0f; extra=2; min=0x800; }
        else if (c>=0xf0 && c<=0xf4) { c&=7; extra=3; min=0x10000; }
        else return false;
        if (extra>n-i) return false;
        while (extra--) { uint8_t b=s[i++]; if ((b&0xc0)!=0x80) return false; c=(c<<6)|(b&0x3f); }
        if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return false;
    }
    return true;
}
MdClipboard *md_clipboard_create_with_backend(uint32_t caps,uint8_t directions,const MdClipboardBackend *b,void *ctx) {
    if (!(caps&MD_CAP_CLIPBOARD) || !directions || directions>MD_CLIPBOARD_BOTH ||
        !b || !b->revision || !b->read || !b->write) return NULL;
    MdClipboard *c=calloc(1,sizeof(*c));
    if (c) { c->backend=b; c->ctx=ctx; c->directions=directions; } return c;
}
MdClipboard *md_clipboard_create(uint32_t caps,uint8_t directions) {
    if (!(caps&MD_CAP_CLIPBOARD) || !directions) return NULL;
    return md_clipboard_create_with_backend(caps,directions,md_clipboard_backend(),NULL);
}
int md_clipboard_poll(MdClipboard *c,char **text,size_t *size) {
    if (!text || !size) return -1;
    *text=NULL; *size=0;
    if (!c || !(c->directions&MD_CLIPBOARD_SEND)) return 0;
    uint64_t revision;
    if (c->backend->revision(c->ctx,&revision)) return -1;
    if (c->seen && revision==c->revision) return 0;
    c->seen=true; c->revision=revision;
    int rc=c->backend->read(c->ctx,text,size);
    if (rc || !md_clipboard_text_valid((uint8_t*)*text,*size)) {
        free(*text); *text=NULL; *size=0; return rc==1 ? 0 : -1;
    }
    if (c->received) {
        bool same=*size==c->received_size && (!*size || !memcmp(*text,c->received,*size));
        if (same) { free(*text); *text=NULL; *size=0; return 0; }
    }
    char *known=malloc(*size+1);
    if (!known) { free(*text); *text=NULL; *size=0; return -1; }
    if (*size) memcpy(known,*text,*size); known[*size]=0;
    free(c->received); c->received=known; c->received_size=*size;
    return 1;
}
int md_clipboard_receive(MdClipboard *c,const uint8_t *text,size_t size) {
    if (!c || !(c->directions&MD_CLIPBOARD_RECEIVE) || !md_clipboard_text_valid(text,size)) return -1;
    char *copy=malloc(size+1); if (!copy) return -1;
    if (size) memcpy(copy,text,size); copy[size]=0;
    if (c->backend->write(c->ctx,text,size)) { free(copy); return -1; }
    /* Compare content on the next native revision, not a post-write revision
     * snapshot that could accidentally suppress an intervening local edit. */
    free(c->received); c->received=copy; c->received_size=size;
    c->seen=false; return 0;
}
void md_clipboard_destroy(MdClipboard *c) { if (c) free(c->received); free(c); }
#ifdef __linux__
const MdClipboardBackend *md_clipboard_backend(void) { return NULL; }
const char *md_clipboard_unavailable_reason(void) { return "text clipboard unsupported: Linux Wayland/X11 clipboard backend is not available in this build"; }
#endif
