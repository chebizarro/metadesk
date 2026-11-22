#include "session.h"
#include "capture.h"
#include "md_test.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
static int native_outputs(void) {
#ifdef _WIN32
    DWORD session=0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(),&session) || session!=WTSGetActiveConsoleSessionId()) {
        fprintf(stderr,"SKIP: native display enumeration requires the unlocked console, not SSH/session 0\n"); return 77;
    }
#endif
    MdCaptureOutputs out={0};
    MD_CHECK(!md_capture_outputs(&out) && out.count>0 && out.count<=MD_CAPTURE_OUTPUTS_MAX);
    printf("native outputs: %zu, switch support: %d\n",out.count,out.switch_supported);
    for (size_t i=0;i<out.count;i++) MD_CHECK(md_capture_output_id_valid(out.items[i].id));
    for (size_t i=0;i<out.count;i++) printf("%s %s\n",out.items[i].id,out.items[i].name);
#ifdef __linux__
    MD_CHECK(!out.switch_supported && out.count==1 && !strcmp(out.selected,"portal"));
#endif
    return md_test_report();
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--native")) return native_outputs();
    MdCaptureOutputs out={.count=2,.switch_supported=true,.selected="id:1"};
    strcpy(out.items[0].id,"id:1"); strcpy(out.items[0].name,"Primary");
    strcpy(out.items[1].id,"id:2"); strcpy(out.items[1].name,"Second");
    out.items[1].rect=(MdCaptureRect){-1920,0,1920,1080};
    char *s=md_session_outputs_json(&out,NULL); MD_CHECK(s);
    cJSON *j=cJSON_Parse(s); MdCaptureOutputs parsed; bool present;
    MD_CHECK(!md_session_outputs_read(j,&parsed,&present) && present);
    MD_CHECK(parsed.count==2 && parsed.items[1].rect.x==-1920 && parsed.switch_supported);
    cJSON_Delete(j); free(s);
    MdSessionRequest r={.capabilities=MD_CAP_VIDEO|MD_CAP_MONITOR,.output="id:2"}, rr;
    s=md_session_request_to_json(&r); MD_CHECK(s && !md_session_request_from_json(s,&rr));
    MD_CHECK(!strcmp(rr.output,"id:2") && (rr.capabilities&MD_CAP_MONITOR)); free(s);
    MD_CHECK(!md_capture_output_id_valid("id:-1") && !md_capture_output_id_valid("id:4294967296"));
    strcpy(out.items[1].id,"id:1"); s=md_session_outputs_json(&out,NULL); j=cJSON_Parse(s);
    MD_CHECK(md_session_outputs_read(j,&parsed,&present)<0); free(s); cJSON_Delete(j);
    return md_test_report();
}
