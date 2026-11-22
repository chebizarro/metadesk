#include "host-client.h"
#include "md_test.h"
#include <time.h>
#include "manager_fake_host.h"

static gboolean finalization_received(gpointer data) {
    MD_CHECK(g_main_context_is_owner(g_main_context_default()));
    *(gboolean *)data=TRUE;
    return G_SOURCE_REMOVE;
}
static void host_gone(gpointer data,GObject *object) {
    (void)object;
    /* GTask can release its last source-object reference on its worker. Always
     * deliver the cleanup receipt on the test context, including its wakeup. */
    GSource *receipt=g_idle_source_new();
    g_source_set_callback(receipt,finalization_received,data,NULL);
    g_source_attach(receipt,g_main_context_default());
    g_source_unref(receipt);
}
static gpointer release_last_reference(gpointer object) {
    g_object_unref(object);
    return NULL;
}
int main(void) {
    /* Force worker-thread finalization. Joining it must not update main-context
     * state; only dispatching the explicit receipt may complete the wait. */
    gboolean worker_finalized=FALSE;
    GObject *probe=g_object_new(G_TYPE_OBJECT,NULL);
    g_object_weak_ref(probe,host_gone,&worker_finalized);
    GThread *worker=g_thread_new("manager-finalize",release_last_reference,probe);
    g_thread_join(worker);
    MD_CHECK(!worker_finalized);
    MANAGER_WAIT(worker_finalized);
    cJSON *j=fake_snapshot(1,TRUE);
    g_autoptr(GError) error=NULL;
    MD_CHECK(md_manager_host_validate(j,&error));
    g_autofree char *caps=md_manager_caps(MD_HOST_ACCESS_CAPS | 32u);
    MD_CHECK(strstr(caps,"Unknown permissions (0x20)"));
    MD_CHECK(strstr(caps,"Monitor selection") && strstr(caps,"Clipboard"));
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(j,"revision"),1.5);
    MD_CHECK(!md_manager_host_validate(j,&error)); g_clear_error(&error); cJSON_Delete(j);
    j=cJSON_Parse("{\"v\":1,\"v\":1}"); MD_CHECK(!md_manager_host_validate(j,&error)); g_clear_error(&error); cJSON_Delete(j);
    ManagerFakeHost fake; fake_start(&fake);
    g_autoptr(MdManagerHost) host=md_manager_host_new();
    md_manager_host_connect(host,fake.path);
    MANAGER_WAIT(md_manager_host_policy(host)&&!md_manager_host_busy(host));
    MD_CHECK(md_manager_host_ready(host));
    MD_CHECK(md_manager_json_uint(md_manager_host_policy(host),"policy_version")==8);
    cJSON *command=md_manager_host_mutation(md_manager_host_snapshot(host),"requests.decide");
    cJSON_AddStringToObject(command,"request_id","bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    cJSON_AddStringToObject(command,"decision","approve"); cJSON_AddNumberToObject(command,"caps",1); cJSON_AddBoolToObject(command,"remember",FALSE);
    md_manager_host_command(host,command);
    MANAGER_WAIT(fake.commands>=2);
    MD_CHECK(cJSON_Compare(command,fake.last,TRUE)); cJSON_Delete(command);
    MANAGER_WAIT(!md_manager_host_busy(host));
    fake_push(&fake,fake_snapshot(3,FALSE));
    MANAGER_WAIT(md_manager_json_uint(md_manager_host_snapshot(host),"revision")==3);
    fake_push(&fake,fake_snapshot(2,TRUE));
    /* A following frame is a semantic barrier for the older one. */
    fake_push(&fake,fake_snapshot(4,FALSE));
    MANAGER_WAIT(md_manager_json_uint(md_manager_host_snapshot(host),"revision")==4);
    MD_CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(md_manager_host_snapshot(host),"requests"))==0);
    j=fake_snapshot(5,TRUE); cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(j,"generation"),"dddddddddddddddddddddddddddddddd"); fake_push(&fake,j);
    MANAGER_WAIT(!md_manager_host_ready(host)); MD_CHECK(!md_manager_host_snapshot(host));
    fake.wrong_version=TRUE; md_manager_host_connect(host,fake.path);
    MANAGER_WAIT(strstr(md_manager_host_status(host),"version mismatch"));
    MD_CHECK(!md_manager_host_snapshot(host));
    fake.wrong_version=FALSE;
    md_manager_host_connect(host,fake.path);
    MANAGER_WAIT(md_manager_host_policy(host)&&!md_manager_host_busy(host));
    // The client has a fixed-size framing buffer, even without a terminating LF.
    char oversized[8194]; memset(oversized,'x',sizeof(oversized)); gsize written=0;
    MD_CHECK(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(fake.subscriber)),oversized,sizeof(oversized),&written,NULL,NULL));
    MANAGER_WAIT(!md_manager_host_ready(host));
    MD_CHECK(strstr(md_manager_host_status(host),"oversized"));
    md_manager_host_connect(host,fake.path);
    MANAGER_WAIT(md_manager_host_policy(host)&&!md_manager_host_busy(host));
    MD_CHECK(g_io_stream_close(G_IO_STREAM(fake.subscriber),NULL,NULL));
    MANAGER_WAIT(!md_manager_host_ready(host));
    MD_CHECK(!md_manager_host_snapshot(host) && !md_manager_host_policy(host));
    md_manager_host_connect(host,fake.path); md_manager_host_connect(host,"/tmp/does-not-exist.sock");
    MANAGER_WAIT(strstr(md_manager_host_status(host),"not running"));
    MD_CHECK(!md_manager_host_ready(host));
    md_manager_host_stop(host);
    gboolean finalized=FALSE; g_object_weak_ref(G_OBJECT(host),host_gone,&finalized);
    g_clear_object(&host); MANAGER_WAIT(finalized);
    fake_stop(&fake);
    puts("PASS: fake host, exact command, policy snapshot, monotonic reconciliation, generation/version refusal, connect fencing and cleanup");
    return md_test_report();
}
