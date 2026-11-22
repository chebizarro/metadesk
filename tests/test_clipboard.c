#include "clipboard.h"
#include "session.h"
#include "nostr.h"
#include "md_test.h"
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned reads,writes,revisions; uint64_t seq; char text[128]; } Fake;
static int revision(void *ctx,uint64_t *out) { Fake *f=ctx; f->revisions++; *out=f->seq; return 0; }
static int read_text(void *ctx,char **out,size_t *n) { Fake *f=ctx; f->reads++; *out=strdup(f->text); *n=strlen(f->text); return 0; }
static int write_text(void *ctx,const uint8_t *text,size_t n) { Fake *f=ctx; f->writes++; MD_CHECK(n<sizeof(f->text)); memcpy(f->text,text,n); f->text[n]=0; f->seq++; return 0; }
static const MdClipboardBackend backend={revision,read_text,write_text};
static void gates(void) {
    Fake f={.text="local"}; char *text=NULL; size_t n=0;
    MdClipboard *c=md_clipboard_create_with_backend(MD_CAP_VIDEO,3,&backend,&f);
    MD_CHECK(!c && md_clipboard_poll(c,&text,&n)==0 && md_clipboard_receive(c,(uint8_t*)"x",1)<0);
    MD_CHECK(!f.reads && !f.writes && !f.revisions);
    c=md_clipboard_create_with_backend(MD_CAP_CLIPBOARD,MD_CLIPBOARD_RECEIVE,&backend,&f);
    MD_CHECK(c && md_clipboard_poll(c,&text,&n)==0 && !f.reads && !f.revisions);
    MD_CHECK(!md_clipboard_receive(c,(uint8_t*)"remote",6) && f.writes==1 && !f.reads); md_clipboard_destroy(c);
    c=md_clipboard_create_with_backend(MD_CAP_CLIPBOARD,MD_CLIPBOARD_BOTH,&backend,&f);
    MD_CHECK(md_clipboard_poll(c,&text,&n)==1 && n==6); free(text);
    MD_CHECK(md_clipboard_poll(c,&text,&n)==0 && f.reads==1);
    MD_CHECK(!md_clipboard_receive(c,(uint8_t*)"update",6));
    MD_CHECK(md_clipboard_poll(c,&text,&n)==0); /* echo suppression */
    MD_CHECK(!md_clipboard_receive(c,(uint8_t*)"received",8));
    strcpy(f.text,"intervening local edit"); f.seq++;
    MD_CHECK(md_clipboard_poll(c,&text,&n)==1 && !strcmp(text,"intervening local edit")); free(text);
    f.seq++; MD_CHECK(md_clipboard_poll(c,&text,&n)==0); /* same text under another native revision */
    unsigned writes=f.writes;
    MD_CHECK(md_clipboard_receive(c,(uint8_t*)"a\0b",3)<0 && f.writes==writes);
    MD_CHECK(md_clipboard_receive(c,(uint8_t*)"x",MD_CLIPBOARD_MAX_UTF8+1)<0 && f.writes==writes);
    md_clipboard_destroy(c);
    c=md_clipboard_create_with_backend(MD_CAP_CLIPBOARD,MD_CLIPBOARD_SEND,&backend,&f);
    MD_CHECK(md_clipboard_receive(c,(uint8_t*)"x",1)<0 && f.writes==writes); md_clipboard_destroy(c);
}
static void protocol(void) {
    uint32_t caps=0;
    MD_CHECK(!md_nostr_parse_caps("video,clipboard",&caps) && caps==(MD_CAP_VIDEO|MD_CAP_CLIPBOARD));
    MD_CHECK(md_nostr_parse_caps("clipboard,video",&caps)<0);
    MdSessionRequest r={.capabilities=MD_CAP_VIDEO|MD_CAP_CLIPBOARD,.clipboard_directions=MD_CLIPBOARD_SEND}, rr;
    char *s=md_session_request_to_json(&r); MD_CHECK(s && !md_session_request_from_json(s,&rr));
    MD_CHECK(rr.clipboard_directions==MD_CLIPBOARD_SEND && (rr.capabilities&MD_CAP_CLIPBOARD)); free(s);
    MD_CHECK(md_session_request_from_json("{\"type\":\"session_request\",\"capabilities\":[\"clipboard\"],\"clipboard\":\"both\\u0000junk\"}",&rr)<0);
    MD_CHECK(md_clipboard_reverse(MD_CLIPBOARD_SEND)==MD_CLIPBOARD_RECEIVE);
    const uint8_t good[]={0xf0,0x9f,0x98,0x80};
    MD_CHECK(md_clipboard_text_valid(good,sizeof(good)));
    const uint8_t bad[][4]={{0xc0,0x80},{0xed,0xa0,0x80},{0xf4,0x90,0x80,0x80},{0xf0,0x9f}};
    const size_t lengths[]={2,3,4,2};
    for (unsigned i=0;i<4;i++) MD_CHECK(!md_clipboard_text_valid(bad[i],lengths[i]));
    uint8_t *max=malloc(MD_CLIPBOARD_MAX_UTF8); memset(max,'x',MD_CLIPBOARD_MAX_UTF8);
    MD_CHECK(md_clipboard_text_valid(max,MD_CLIPBOARD_MAX_UTF8)); free(max);
}
int main(void) { gates(); protocol(); return md_test_report(); }
