#include "quality.h"
#include "session.h"
#include "bitrate_ctrl.h"
#include "encode.h"
#include "md_test.h"
#include <stdlib.h>
#include <string.h>
static void parsing(void) {
    MdSessionRequest r = {.quality = {1280,720,1000000,30}, .quality_present = true,
                           .capabilities = MD_CAP_VIDEO};
    char *s = md_session_request_to_json(&r); MdSessionRequest parsed;
    MD_CHECK(s && !md_session_request_from_json(s, &parsed));
    MD_CHECK(parsed.quality_present && !memcmp(&r.quality,&parsed.quality,sizeof(r.quality))); free(s);
    const char *bad[] = {"null", "[]", "{\"fps\":1.5}", "{\"fps\":-1}",
        "{\"fps\":241}", "{\"fps\":30,\"fps\":60}", "{\"bitrate\":1}",
        "{\"max_width\":1}", "{\"unknown\":2}"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        char json[256]; snprintf(json,sizeof(json),"{\"type\":\"session_request\",\"capabilities\":[\"video\"],\"quality\":%s}",bad[i]);
        MD_CHECK(md_session_request_from_json(json,&parsed)<0);
    }
    MD_CHECK(!md_session_request_from_json("{\"type\":\"session_request\",\"capabilities\":[\"video\"]}",&parsed));
    MD_CHECK(!parsed.quality_present && !parsed.quality.fps);
    MdSessionAccept a = {.quality = r.quality, .quality_present = true, .granted = MD_CAP_VIDEO};
    s=md_session_accept_to_json(&a); MdSessionAccept b;
    MD_CHECK(s && !md_session_accept_from_json(s,&b) && b.quality_present && b.quality.fps==30); free(s);
}
static void policy(void) {
    MdQuality p={1920,1080,8000000,60}, r={3840,720,16000000,120};
    MdQuality q=md_quality_clamp(&r,&p);
    MD_CHECK(q.max_width==1920 && q.max_height==720 && q.bitrate==8000000 && q.fps==60);
    r=(MdQuality){0}; q=md_quality_clamp(&r,&p); MD_CHECK(!memcmp(&q,&p,sizeof(p)));
    MdBitrateCtrlConfig c={.max_bitrate=8000000,.initial_bitrate=8000000};
    MdBitrateCtrl *ctrl=md_bitrate_ctrl_create(&c); MD_CHECK(ctrl);
    MD_CHECK(md_bitrate_ctrl_set_max(ctrl,1000000)==1000000);
    for(unsigned i=0;i<100;i++) MD_CHECK(md_bitrate_ctrl_update(ctrl,1,i*1000)<=1000000);
    md_bitrate_ctrl_destroy(ctrl);
}
static void scale(void) {
    uint8_t pixels[16*8*4]; memset(pixels,123,sizeof(pixels));
    MdFrame f={.data=pixels,.width=16,.height=8,.stride=64,.data_size=sizeof(pixels),
        .format=MD_PIX_CAPTURE_BGRA,.buf_type=MD_BUF_CPU,.seq=33};
    MdQuality q={.max_width=8}; MdFrame scaled; uint8_t *buf=NULL; size_t cap=0;
    MD_CHECK(!md_quality_scale(&q,&f,&scaled,&buf,&cap));
    MD_CHECK(scaled.width==8 && scaled.height==4 && scaled.seq==33 && scaled.data[0]==123);
    MD_CHECK(f.width==16 && f.data==pixels);
    f.format=MD_PIX_CAPTURE_NV12; f.stride=16; f.data_size=16*8*3/2;
    MD_CHECK(!md_quality_scale(&q,&f,&scaled,&buf,&cap) && scaled.data[0]==123);
    free(buf);
    MdEncoderConfig cfg={.width=64,.height=64,.fps=60,.bitrate=1000000};
    MdEncoder *e=md_encoder_create(&cfg); MD_CHECK(e);
    MD_CHECK(!md_encoder_set_fps(e,30) && md_encoder_set_fps(e,0)<0);
    MD_CHECK(!md_encoder_resize(e,32,32)); md_encoder_destroy(e);
}
int main(void) { parsing(); policy(); scale(); return md_test_report(); }
