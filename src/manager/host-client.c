#include "host-client.h"
#include "management_ipc.h"
#include <math.h>
#include <string.h>
#include <unistd.h>

const char *md_manager_json_string(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
guint64 md_manager_json_uint(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsNumber(v) ? (guint64)v->valuedouble : 0;
}
static gboolean uint_field(const cJSON *j, const char *key, double max) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 &&
        v->valuedouble <= max && floor(v->valuedouble) == v->valuedouble;
}
static gboolean hex_field(const cJSON *j, const char *key, guint length) {
    const char *s = md_manager_json_string(j,key);
    return strlen(s) == length && strspn(s,"0123456789abcdef") == length;
}
static gboolean unique(const cJSON *j) {
    const cJSON *a, *b;
    cJSON_ArrayForEach(a,j) {
        if (cJSON_IsString(a) && !g_utf8_validate(a->valuestring,-1,NULL)) return FALSE;
        if (a->string && !g_utf8_validate(a->string,-1,NULL)) return FALSE;
        if (cJSON_IsObject(j)) for (b=a->next; b; b=b->next)
            if (!strcmp(a->string,b->string)) return FALSE;
        if ((cJSON_IsObject(a) || cJSON_IsArray(a)) && !unique(a)) return FALSE;
    }
    return TRUE;
}
static gboolean request_valid(const cJSON *j) {
    const char *state = md_manager_json_string(j,"state");
    return cJSON_IsObject(j) && hex_field(j,"peer",64) && hex_field(j,"request_id",32) &&
        uint_field(j,"requested",G_MAXUINT32) && uint_field(j,"granted",G_MAXUINT32) &&
        uint_field(j,"expires_at",9007199254740991.0) &&
        (!strcmp(state,"pending") || !strcmp(state,"remembering") || !strcmp(state,"approved"));
}
gboolean md_manager_host_validate(const cJSON *j, GError **error) {
    if (!cJSON_IsObject(j) || !unique(j)) goto invalid;
    if (!uint_field(j,"v",1) || md_manager_json_uint(j,"v") != 1) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_NOT_SUPPORTED,
            "Host management version mismatch — update the host and manager together"); return FALSE;
    }
    const cJSON *qs = cJSON_GetObjectItemCaseSensitive(j,"requests");
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(j,"session");
    if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j,"ok")) || !hex_field(j,"generation",32) ||
        !uint_field(j,"revision",9007199254740991.0) || !cJSON_IsArray(qs) || cJSON_GetArraySize(qs)>16 ||
        !(cJSON_IsNull(session) || (request_valid(session) &&
          cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(session,"revoked")) &&
          cJSON_IsString(cJSON_GetObjectItemCaseSensitive(session,"session_id")) &&
          strlen(md_manager_json_string(session,"session_id")) < 64))) goto invalid;
    if (cJSON_GetObjectItemCaseSensitive(j,"supported") && !uint_field(j,"supported",G_MAXUINT32)) goto invalid;
    const cJSON *q, *other;
    cJSON_ArrayForEach(q,qs) {
        if (!request_valid(q)) goto invalid;
        for (other=q->next; other; other=other->next)
            if (!strcmp(md_manager_json_string(q,"request_id"),md_manager_json_string(other,"request_id"))) goto invalid;
    }
    const cJSON *clients = cJSON_GetObjectItemCaseSensitive(j,"clients");
    if (clients) {
        if (!cJSON_IsArray(clients) || !uint_field(j,"policy_version",9007199254740991.0) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j,"policy_available"))) goto invalid;
        cJSON_ArrayForEach(q,clients) {
            if (!hex_field(q,"peer",64) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(q,"caps")) ||
                strlen(md_manager_json_string(q,"caps"))>63 ||
                !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(q,"review_required"))) goto invalid;
        }
    }
    return TRUE;
invalid:
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,"Invalid host management snapshot"); return FALSE;
}
char *md_manager_caps(guint32 bits) {
    GString *text = g_string_new(NULL);
    if (bits&1) g_string_append(text,"Video, ");
    if (bits&2) g_string_append(text,"Accessibility tree, ");
    if (bits&4) g_string_append(text,"Input, ");
    if (bits&~7u) g_string_append_printf(text,"Unknown permissions (0x%x), ",bits&~7u);
    if (text->len) g_string_truncate(text,text->len-2); else g_string_append(text,"None");
    return g_string_free(text,FALSE);
}
cJSON *md_manager_host_mutation(const cJSON *snapshot, const char *method) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j,"v",1); cJSON_AddStringToObject(j,"method",method);
    cJSON_AddStringToObject(j,"generation",md_manager_json_string(snapshot,"generation"));
    cJSON_AddNumberToObject(j,"revision",(double)md_manager_json_uint(snapshot,"revision"));
    return j;
}

struct _MdManagerHost {
    GObject parent_instance;
    char *path, *status;
    GCancellable *cancel;
    GSocketConnection *stream;
    GString *line;
    cJSON *snapshot, *policy;
    guint epoch;
    gboolean ready, busy;
};
G_DEFINE_TYPE(MdManagerHost, md_manager_host, G_TYPE_OBJECT)
static guint changed_signal;
static void changed(MdManagerHost *self) { g_signal_emit(self,changed_signal,0); }
static void status(MdManagerHost *self, const char *text) { g_free(self->status); self->status=g_strdup(text); }
void md_manager_host_stop(MdManagerHost *self) {
    ++self->epoch; self->ready=FALSE; self->busy=FALSE;
    if (self->cancel) g_cancellable_cancel(self->cancel);
    if (self->stream) g_io_stream_close(G_IO_STREAM(self->stream),NULL,NULL);
    g_clear_object(&self->cancel); g_clear_object(&self->stream);
    g_string_truncate(self->line,0);
    cJSON_Delete(self->snapshot); self->snapshot=NULL;
    cJSON_Delete(self->policy); self->policy=NULL;
}
static void failed(MdManagerHost *self, const char *message) {
    md_manager_host_stop(self); status(self,message); changed(self);
}
static void dispose(GObject *object) {
    md_manager_host_stop(MD_MANAGER_HOST(object));
    G_OBJECT_CLASS(md_manager_host_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
    MdManagerHost *self=MD_MANAGER_HOST(object);
    g_free(self->path); g_free(self->status); g_string_free(self->line,TRUE);
    G_OBJECT_CLASS(md_manager_host_parent_class)->finalize(object);
}
static void md_manager_host_class_init(MdManagerHostClass *klass) {
    G_OBJECT_CLASS(klass)->dispose=dispose; G_OBJECT_CLASS(klass)->finalize=finalize;
    changed_signal=g_signal_new("changed",G_TYPE_FROM_CLASS(klass),G_SIGNAL_RUN_LAST,0,NULL,NULL,NULL,G_TYPE_NONE,0);
}
static void md_manager_host_init(MdManagerHost *self) {
    self->line=g_string_new(NULL); status(self,"Host not connected");
}
MdManagerHost *md_manager_host_new(void) { return g_object_new(MD_TYPE_MANAGER_HOST,NULL); }
const cJSON *md_manager_host_snapshot(MdManagerHost *self) { return self->snapshot; }
const cJSON *md_manager_host_policy(MdManagerHost *self) { return self->policy; }
const char *md_manager_host_status(MdManagerHost *self) { return self->status; }
gboolean md_manager_host_ready(MdManagerHost *self) { return self->ready; }
gboolean md_manager_host_busy(MdManagerHost *self) { return self->busy; }

typedef struct { MdManagerHost *self; guint epoch; char *path; cJSON *command; char bytes[1024]; } Operation;
static Operation *operation(MdManagerHost *self) {
    Operation *op=g_new0(Operation,1); op->self=g_object_ref(self); op->epoch=self->epoch;
    op->path=g_strdup(self->path); return op;
}
static void operation_free(gpointer data) {
    Operation *op=data; g_object_unref(op->self); g_free(op->path); cJSON_Delete(op->command); g_free(op);
}
static gboolean apply(MdManagerHost *self, cJSON *j) {
    g_autoptr(GError) error=NULL;
    if (!md_manager_host_validate(j,&error)) { failed(self,error->message); return FALSE; }
    const char *generation=md_manager_json_string(j,"generation");
    if (self->snapshot && strcmp(generation,md_manager_json_string(self->snapshot,"generation"))) {
        failed(self,"Host restarted — reconnect to review the new generation"); return FALSE;
    }
    if (!self->snapshot || md_manager_json_uint(j,"revision") >= md_manager_json_uint(self->snapshot,"revision")) {
        cJSON_Delete(self->snapshot); self->snapshot=cJSON_Duplicate(j,TRUE);
    }
    self->ready=TRUE;
    return TRUE;
}
static void read_next(MdManagerHost *self);
static void read_done(GObject *source, GAsyncResult *result, gpointer data) {
    Operation *op=data; MdManagerHost *self=op->self;
    g_autoptr(GError) error=NULL;
    gssize size=g_input_stream_read_finish(G_INPUT_STREAM(source),result,&error);
    if (op->epoch != self->epoch) { operation_free(op); return; }
    if (size<=0) { failed(self,"Host disconnected — no approval authority is available"); operation_free(op); return; }
    for (gssize i=0;i<size;++i) {
        char ch=op->bytes[i];
        if (!ch || self->line->len>=8192) { failed(self,"Invalid or oversized host management frame"); break; }
        if (ch!='\n') { g_string_append_c(self->line,ch); continue; }
        cJSON *j=cJSON_ParseWithOpts(self->line->str,NULL,TRUE);
        g_string_truncate(self->line,0);
        gboolean accepted=apply(self,j);
        if (accepted && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"ok"))) {
            failed(self,"Host refused the management subscription"); accepted=FALSE;
        }
        cJSON_Delete(j);
        if (!accepted) break;
        if (!g_str_has_prefix(self->status,"Not completed:")) status(self,"Connected to the local host");
        changed(self);
        if (!self->policy && !self->busy) md_manager_host_refresh_access(self);
    }
    if (op->epoch==self->epoch) read_next(self);
    operation_free(op);
}
static void read_next(MdManagerHost *self) {
    Operation *op=operation(self);
    g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(self->stream)),op->bytes,
        sizeof(op->bytes),G_PRIORITY_DEFAULT,self->cancel,read_done,op);
}
static void subscribed(GObject *source,GAsyncResult *result,gpointer data) {
    Operation *op=data; MdManagerHost *self=op->self;
    g_autoptr(GError) error=NULL; gsize size=0;
    gboolean sent=g_output_stream_write_all_finish(G_OUTPUT_STREAM(source),result,&size,&error);
    if (op->epoch==self->epoch) {
        if (!sent) failed(self,"Could not subscribe to the host"); else read_next(self);
    }
    operation_free(op);
}
static void connect_work(GTask *task,gpointer source,gpointer data,GCancellable *cancel) {
    (void)source; (void)cancel;
    Operation *op=data;
    g_task_return_int(task,md_management_connect(op->path));
}
static void connected(GObject *source,GAsyncResult *result,gpointer data) {
    (void)data; MdManagerHost *self=MD_MANAGER_HOST(source);
    Operation *op=g_task_get_task_data(G_TASK(result));
    int fd=g_task_propagate_int(G_TASK(result),NULL);
    if (op->epoch!=self->epoch) { if(fd>=0) close(fd); return; }
    if (fd<0) { failed(self,"Host not running, or its management socket is not trusted"); return; }
    g_autoptr(GError) error=NULL;
    GSocket *socket=g_socket_new_from_fd(fd,&error);
    if (!socket) { close(fd); failed(self,"Could not open the verified host socket"); return; }
    self->stream=g_socket_connection_factory_create_connection(socket); g_object_unref(socket);
    static const char request[]="{\"v\":1,\"method\":\"management.subscribe\"}\n";
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(self->stream)),request,
        sizeof(request)-1,G_PRIORITY_DEFAULT,self->cancel,subscribed,operation(self));
}
void md_manager_host_connect(MdManagerHost *self,const char *path) {
    md_manager_host_stop(self); g_free(self->path); self->path=g_strdup(path);
    self->cancel=g_cancellable_new(); status(self,"Connecting to the local host…"); changed(self);
    /* Do not cancel GTask's result: an opened descriptor must always be collected. */
    GTask *task=g_task_new(self,NULL,connected,NULL);
    g_task_set_task_data(task,operation(self),operation_free);
    g_task_run_in_thread(task,connect_work); g_object_unref(task);
}
static void command_work(GTask *task,gpointer source,gpointer data,GCancellable *cancel) {
    (void)source; (void)cancel; Operation *op=data;
    g_task_return_pointer(task,md_management_call(op->path,op->command),(GDestroyNotify)cJSON_Delete);
}
static void command_done(GObject *source,GAsyncResult *result,gpointer data) {
    (void)data; MdManagerHost *self=MD_MANAGER_HOST(source);
    Operation *op=g_task_get_task_data(G_TASK(result));
    cJSON *j=g_task_propagate_pointer(G_TASK(result),NULL);
    if (op->epoch!=self->epoch) { cJSON_Delete(j); return; }
    self->busy=FALSE;
    if (!j) { failed(self,"Host reply unavailable — a policy write may have reached the relay; reconnect and review before retrying"); return; }
    if (apply(self,j)) {
        if (cJSON_GetObjectItemCaseSensitive(j,"clients")) {
            cJSON_Delete(self->policy); self->policy=cJSON_Duplicate(j,TRUE);
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,"ok"))) {
            if (strcmp(md_manager_json_string(op->command,"method"),"access.list")) status(self,"Connected to the local host");
        }
        else {
            g_autofree char *message=g_strdup_printf("Not completed: %s. Review current state before retrying; a policy publication may already be durable.",md_manager_json_string(j,"error"));
            status(self,message);
        }
        changed(self);
        if (!cJSON_GetObjectItemCaseSensitive(j,"clients")) md_manager_host_refresh_access(self);
    }
    cJSON_Delete(j);
}
void md_manager_host_command(MdManagerHost *self,const cJSON *command) {
    if (!self->ready || self->busy) return;
    self->busy=TRUE; changed(self);
    Operation *op=operation(self); op->command=cJSON_Duplicate(command,TRUE);
    GTask *task=g_task_new(self,NULL,command_done,NULL);
    g_task_set_task_data(task,op,operation_free); g_task_run_in_thread(task,command_work); g_object_unref(task);
}
void md_manager_host_refresh_access(MdManagerHost *self) {
    cJSON *j=cJSON_CreateObject(); cJSON_AddNumberToObject(j,"v",1); cJSON_AddStringToObject(j,"method","access.list");
    md_manager_host_command(self,j); cJSON_Delete(j);
}
