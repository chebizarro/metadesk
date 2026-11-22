#pragma once
#include "host-client.h"
#include "md_test.h"
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

typedef struct {
    GSocketService *service;
    GSocketConnection *subscriber;
    GPtrArray *peers;
    cJSON *snapshot, *last;
    char *directory, *path;
    guint commands;
    gboolean wrong_version, hold, unknown_caps;
} ManagerFakeHost;
static cJSON *fake_snapshot(guint revision,gboolean pending) {
    cJSON *j=cJSON_Parse("{\"v\":1,\"ok\":true,\"generation\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"revision\":1,\"supported\":3,\"requests\":[],\"session\":null}");
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(j,"revision"),revision);
    if (pending) {
        cJSON *q=cJSON_Parse("{\"request_id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"peer\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"requested\":15,\"granted\":0,\"expires_at\":0,\"state\":\"pending\"}");
        cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(q,"expires_at"),(double)time(NULL)+120);
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(j,"requests"),q);
    }
    char padding[1500]; memset(padding,'x',sizeof(padding)-1); padding[sizeof(padding)-1]=0;
    cJSON_AddStringToObject(j,"forward_compatible_metadata",padding);
    return j;
}
static gboolean fake_write(GSocketConnection *connection,const cJSON *j) {
    char *text=cJSON_PrintUnformatted(j); gsize length=strlen(text);
    text[length]='\n';
    gsize written=0;
    gboolean ok=g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),text,length+1,&written,NULL,NULL);
    free(text); return ok;
}
static void fake_line(GObject *source,GAsyncResult *result,gpointer data) {
    ManagerFakeHost *fake=data;
    gsize size=0; char *text=g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source),result,&size,NULL);
    /* A credential-verified client may cancel before writing its command. */
    if (!text) { g_object_unref(source); return; }
    cJSON *j=cJSON_Parse(text); g_free(text); MD_CHECK(j);
    GSocketConnection *connection=g_object_get_data(source,"connection");
    if (!strcmp(md_manager_json_string(j,"method"),"management.subscribe")) {
        g_set_object(&fake->subscriber,connection);
        cJSON *reply=cJSON_Duplicate(fake->snapshot,TRUE);
        if (fake->wrong_version) cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(reply,"v"),99);
        fake_write(connection,reply); cJSON_Delete(reply);
    } else {
        ++fake->commands; cJSON_Delete(fake->last); fake->last=cJSON_Duplicate(j,TRUE);
        cJSON *reply=cJSON_Duplicate(fake->snapshot,TRUE);
        if (!strcmp(md_manager_json_string(j,"method"),"access.list")) {
            cJSON_AddBoolToObject(reply,"policy_available",TRUE); cJSON_AddNumberToObject(reply,"policy_version",8);
            cJSON *clients=cJSON_AddArrayToObject(reply,"clients");
            cJSON_AddItemToArray(clients,cJSON_Parse("{\"peer\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\"caps\":\"video,agent\",\"review_required\":false}"));
            if (fake->unknown_caps) cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(clients,0),"caps"),"video,clipboard");
        }
        if (!fake->hold) MD_CHECK(fake_write(connection,reply));
        cJSON_Delete(reply);
    }
    cJSON_Delete(j); g_object_unref(source);
}
static gboolean fake_incoming(GSocketService *service,GSocketConnection *connection,GObject *source,gpointer data) {
    (void)service; (void)source; ManagerFakeHost *fake=data;
    g_ptr_array_add(fake->peers,g_object_ref(connection));
    GDataInputStream *stream=g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_object_set_data_full(G_OBJECT(stream),"connection",g_object_ref(connection),g_object_unref);
    g_data_input_stream_read_line_async(stream,G_PRIORITY_DEFAULT,NULL,fake_line,fake); return TRUE;
}
static void fake_start(ManagerFakeHost *fake) {
    memset(fake,0,sizeof(*fake));
    fake->directory=g_dir_make_tmp("md-w16-fake-XXXXXX",NULL);
    fake->path=g_build_filename(fake->directory,"host.sock",NULL);
    fake->snapshot=fake_snapshot(1,TRUE); fake->peers=g_ptr_array_new_with_free_func(g_object_unref);
    fake->service=g_socket_service_new();
    g_autoptr(GSocketAddress) address=g_unix_socket_address_new(fake->path);
    MD_CHECK(g_socket_listener_add_address(G_SOCKET_LISTENER(fake->service),address,G_SOCKET_TYPE_STREAM,G_SOCKET_PROTOCOL_DEFAULT,NULL,NULL,NULL));
    MD_CHECK(!g_chmod(fake->path,0600));
    g_signal_connect(fake->service,"incoming",G_CALLBACK(fake_incoming),fake); g_socket_service_start(fake->service);
}
static void fake_push(ManagerFakeHost *fake,cJSON *snapshot) {
    cJSON_Delete(fake->snapshot); fake->snapshot=snapshot;
    MD_CHECK(fake->subscriber); MD_CHECK(fake_write(fake->subscriber,snapshot));
}
static void fake_stop(ManagerFakeHost *fake) {
    g_socket_service_stop(fake->service); g_socket_listener_close(G_SOCKET_LISTENER(fake->service));
    for(guint i=0;i<fake->peers->len;++i) g_io_stream_close(g_ptr_array_index(fake->peers,i),NULL,NULL);
    g_clear_object(&fake->subscriber); g_clear_object(&fake->service); g_ptr_array_unref(fake->peers);
    cJSON_Delete(fake->snapshot); cJSON_Delete(fake->last);
    g_unlink(fake->path); g_rmdir(fake->directory); g_free(fake->path); g_free(fake->directory);
}
static gboolean manager_timeout(gpointer data) { *(gboolean *)data=TRUE; return G_SOURCE_REMOVE; }
#define MANAGER_WAIT(condition) do { \
    gboolean deadline=FALSE; guint watchdog=g_timeout_add_seconds(10,manager_timeout,&deadline); \
    while (!(condition)&&!deadline) g_main_context_iteration(NULL,TRUE); \
    if (!deadline) g_source_remove(watchdog); \
    MD_CHECK((condition) && !deadline); if (deadline) return md_test_report(); \
} while (0)
