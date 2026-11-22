#include "management_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <limits.h>
#include "md_time.h"
#define PEERS 8
#define LINE_MAX_BYTES 8192

typedef struct { struct MdManagementIpc *server; int fd; pthread_t thread; bool used, done; } Peer;
struct MdManagementIpc {
    MdHostAccess *access;
    MdNostr *nostr;
    uint32_t supported;
    void (*wake)(void *);
    void *data;
    int listener;
    int stop_pipe[2];
    pthread_t thread;
    pthread_mutex_t mu;
    bool stopping;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    dev_t dev;
    ino_t ino;
    Peer peers[PEERS];
};
static bool private_dir(const char *path) {
    char resolved[PATH_MAX]; struct stat st;
    return path[0] == '/' && realpath(path, resolved) && !strcmp(path, resolved) &&
        !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0077);
}
static bool private_parent(const char *path) {
    if (!path || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return false;
    char parent[PATH_MAX]; snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1] || !strcmp(slash+1, ".") || !strcmp(slash+1, "..")) return false;
    *slash = 0; return private_dir(parent);
}
int md_management_path(const char *key, char *out, size_t size) {
    if (!key || strlen(key) != 64 || strspn(key, "0123456789abcdef") != 64) return -1;
    const char *base = getenv("XDG_RUNTIME_DIR");
    char dir[PATH_MAX];
#ifdef __APPLE__
    /* Darwin's per-user temp path plus a full host key exceeds sun_path.
     * Use a checked private user directory under the canonical sticky tmp. */
    if (!base || !base[0]) {
        if (snprintf(dir,sizeof(dir),"/private/tmp/metadesk-%lu",(unsigned long)geteuid()) >= (int)sizeof(dir)) return -1;
    } else
#endif
    {
        if (!base || !private_dir(base)) return -1;
        if (snprintf(dir, sizeof(dir), "%s/metadesk", base) >= (int)sizeof(dir)) return -1;
    }
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    if (!private_dir(dir)) return -1;
    int n = snprintf(out, size, "%s/host-%s.sock", dir, key);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}
bool md_management_peer_is_user(int fd, uint64_t uid) {
#ifdef __linux__
    struct ucred cred; socklen_t size = sizeof(cred);
    return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) && size == sizeof(cred) && (uint64_t)cred.uid == uid;
#elif defined(__APPLE__)
    uid_t peer; gid_t group;
    return !getpeereid(fd, &peer, &group) && (uint64_t)peer == uid;
#else
    (void)fd; (void)uid; return false;
#endif
}
static int unix_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) { close(fd); return -1; }
#ifdef __APPLE__
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}
static bool ready(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t left = deadline - md_now_ms();
        if (left <= 0) return false;
        struct pollfd p = { .fd = fd, .events = events };
        int rc = poll(&p, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0 && (p.revents & events);
    }
}
static bool send_json(int fd, const cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    if (!text) return false;
    size_t len = strlen(text); text[len++] = '\n';
    size_t pos = 0; int64_t deadline = md_now_ms() + 5000;
    while (pos < len && ready(fd, POLLOUT, deadline)) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, text + pos, len-pos, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, text + pos, len-pos, 0);
#endif
        if (n > 0) pos += (size_t)n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN)) break;
    }
    free(text); return pos == len;
}
/* One bounded JSON line per connection; subscriptions are server-output only.
 * Byte reads prevent consuming part of the next frame. Max 8 private peers. */
static cJSON *read_json(int fd) {
    char buf[LINE_MAX_BYTES + 1]; size_t len = 0;
    int64_t deadline = md_now_ms() + 5000;
    while (len < LINE_MAX_BYTES && ready(fd, POLLIN, deadline)) {
        char c; ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n != 1 || !c) return NULL;
        if (c == '\n') { buf[len] = 0; return cJSON_ParseWithOpts(buf, NULL, true); }
        buf[len++] = c;
    }
    return NULL;
}
static cJSON *item(const cJSON *j, const char *key) { return cJSON_GetObjectItemCaseSensitive(j, key); }
static const char *str(const cJSON *j, const char *key) { cJSON *v = item(j,key); return cJSON_IsString(v) ? v->valuestring : NULL; }
static bool number(const cJSON *j, const char *key, uint64_t *out) {
    cJSON *v = item(j,key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > 9007199254740991.0 || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (uint64_t)v->valuedouble; return true;
}
static bool schema(const cJSON *j) {
    if (!cJSON_IsObject(j)) return false;
    const cJSON *a, *b;
    cJSON_ArrayForEach(a,j) {
        if (!a->string) return false;
        for (b = a->next; b; b = b->next) if (!strcmp(a->string, b->string)) return false;
        if (strcmp(a->string,"v") && strcmp(a->string,"method") && strcmp(a->string,"generation") &&
            strcmp(a->string,"revision") && strcmp(a->string,"request_id") && strcmp(a->string,"session_id") &&
            strcmp(a->string,"decision") && strcmp(a->string,"caps") && strcmp(a->string,"remember")) return false;
    }
    uint64_t v; return number(j,"v",&v) && v == 1 && str(j,"method");
}
static cJSON *request_json(const MdHostRequest *q) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j,"request_id",q->request.request_id);
    cJSON_AddStringToObject(j,"peer",q->peer);
    cJSON_AddNumberToObject(j,"requested",q->request.capabilities);
    cJSON_AddNumberToObject(j,"granted",q->granted);
    cJSON_AddNumberToObject(j,"expires_at",(double)q->request.expires_at);
    cJSON_AddStringToObject(j,"state",q->state == MD_HOST_PENDING ? "pending" : q->state == MD_HOST_REMEMBERING ? "remembering" : "approved");
    return j;
}
static cJSON *snapshot(MdHostAccess *a, const char *error) {
    MdHostAccessSnapshot v; md_host_access_snapshot(a, (int64_t)time(NULL), &v);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j,"v",1);
    cJSON_AddBoolToObject(j,"ok",!error);
    if (error) cJSON_AddStringToObject(j,"error",error);
    cJSON_AddStringToObject(j,"generation",v.generation);
    cJSON_AddNumberToObject(j,"revision",(double)v.revision);
    cJSON *qs = cJSON_AddArrayToObject(j,"requests");
    for (size_t i=0; i<v.count; ++i) cJSON_AddItemToArray(qs, request_json(&v.requests[i]));
    if (v.occupied) {
        cJSON *active = request_json(&v.active);
        cJSON_AddStringToObject(active,"session_id",v.session_id);
        cJSON_AddBoolToObject(active,"revoked",v.revoked);
        cJSON_AddItemToObject(j,"session",active);
    } else cJSON_AddNullToObject(j,"session");
    return j;
}
static void caps_text(uint32_t caps, char out[32]) {
    out[0] = 0;
    if (caps & MD_CAP_VIDEO) strcat(out,"video,");
    if (caps & MD_CAP_AGENT) strcat(out,"agent,");
    if (caps & MD_CAP_INPUT) strcat(out,"input,");
    size_t n = strlen(out); if (n) out[n-1] = 0;
}
static const char *dispatch(MdManagementIpc *s, cJSON *j) {
    if (!schema(j)) return "invalid";
    const char *method = str(j,"method");
    if (!strcmp(method,"management.snapshot") || !strcmp(method,"management.subscribe") ||
        !strcmp(method,"session.list") || !strcmp(method,"requests.list"))
        return cJSON_GetArraySize(j) == 2 ? NULL : "invalid";
    const char *g = str(j,"generation"); uint64_t rev;
    if (!g || !number(j,"revision",&rev)) return "invalid";
    if (!strcmp(method,"session.kick")) {
        const char *id = str(j,"session_id");
        if (!id || cJSON_GetArraySize(j) != 5) return "invalid";
        if (md_host_access_kick(s->access,g,rev,id)) return "conflict";
    } else if (!strcmp(method,"requests.decide")) {
        const char *id = str(j,"request_id"), *decision = str(j,"decision");
        uint64_t bits = 0;
        if (!id || !decision) return "invalid";
        bool approve = !strcmp(decision,"approve");
        if (!approve && strcmp(decision,"deny")) return "invalid";
        if (cJSON_GetArraySize(j) != (approve ? 8 : 6)) return "invalid";
        cJSON *remember = item(j,"remember");
        if (approve && (!cJSON_IsBool(remember) || !number(j,"caps",&bits) || bits > MD_HOST_ACCESS_CAPS)) return "invalid";
        bool save = approve && cJSON_IsTrue(remember);
        if (save && (!s->nostr || md_nostr_allowlist_state(s->nostr) != MD_NOSTR_ACL_LOADED)) return "policy_unavailable";
        MdHostRequest q;
        if (md_host_access_decide(s->access,g,rev,id,approve,save,(uint32_t)bits,s->supported,
                                  (int64_t)time(NULL),&q)) return "conflict";
        if (save) {
            char caps[32]; caps_text(q.granted,caps);
            bool published = md_nostr_allowlist_add(s->nostr,q.peer,caps) == 0;
            if (md_host_access_remembered(s->access,id,published,(int64_t)time(NULL)))
                return published ? "cancelled_or_expired_after_publish" : "publish_failed";
        }
        if (!approve && s->nostr) {
            MdSessionReject reject = {0};
            strcpy(reject.request_id, q.request.request_id);
            strcpy(reject.reason, "request_refused");
            char *json = md_session_reject_to_json(&reject);
            int rc = json ? md_nostr_send_session_accept(s->nostr,q.peer,json) : -1;
            free(json);
            if (rc) return "deny_delivery_failed";
        }
    } else return "unsupported_method";
    if (s->wake) s->wake(s->data);
    return NULL;
}
static void notify_pipe(void *data) {
    int fd = *(int *)data; char byte = 1;
    ssize_t n;
    do { n = write(fd,&byte,1); } while (n < 0 && errno == EINTR);
    /* EAGAIN already means a wake is pending. */
}
static void *serve_peer(void *data) {
    Peer *p = data; MdManagementIpc *s = p->server;
    cJSON *j = read_json(p->fd);
    const char *error = j ? dispatch(s,j) : "invalid";
    bool subscribe = !error && str(j,"method") && !strcmp(str(j,"method"),"management.subscribe");
    int wake[2] = {-1,-1}; bool watched = false;
    if (subscribe) {
        if (!pipe(wake)) {
            bool flags = true;
            for (int i=0; i<2; ++i) if (fcntl(wake[i],F_SETFD,FD_CLOEXEC) || fcntl(wake[i],F_SETFL,O_NONBLOCK)) flags=false;
            watched = flags && md_host_access_watch(s->access,notify_pipe,&wake[1]) == 0;
        }
        if (!watched) { subscribe=false; error="subscription_unavailable"; }
    }
    cJSON *reply = snapshot(s->access,error);
    uint64_t sent_revision = 0;
    while (reply) {
        uint64_t revision = (uint64_t)item(reply,"revision")->valuedouble;
        bool sent = revision == sent_revision || send_json(p->fd,reply);
        cJSON_Delete(reply); reply = NULL;
        sent_revision = revision;
        if (!sent || !subscribe) break;
        MdHostAccessSnapshot v;
        md_host_access_snapshot(s->access,(int64_t)time(NULL),&v);
        if (v.revision != sent_revision) { reply=snapshot(s->access,NULL); continue; }
        int timeout = -1;
        for (size_t i=0; i<v.count; ++i) {
            int64_t left = (v.requests[i].request.expires_at - (int64_t)time(NULL))*1000;
            int ms = left < 0 ? 0 : (int)left;
            if (timeout < 0 || ms < timeout) timeout=ms;
        }
        struct pollfd fds[2] = {{.fd=p->fd,.events=POLLIN},{.fd=wake[0],.events=POLLIN}};
        int rc;
        do { rc=poll(fds,2,timeout); } while (rc < 0 && errno == EINTR);
        /* EOF or extra client input ends a server-output-only subscription. */
        if (rc < 0 || fds[0].revents) break;
        char bytes[128]; while (read(wake[0],bytes,sizeof(bytes)) > 0) {}
        pthread_mutex_lock(&s->mu); bool stop=s->stopping; pthread_mutex_unlock(&s->mu);
        if (stop) break;
        reply=snapshot(s->access,NULL);
    }
    if (watched) md_host_access_unwatch(s->access,&wake[1]);
    for (int i=0;i<2;++i) if (wake[i]>=0) close(wake[i]);
    cJSON_Delete(j);
    pthread_mutex_lock(&s->mu);
    close(p->fd); p->fd = -1; p->done = true;
    pthread_mutex_unlock(&s->mu);
    return NULL;
}
static void *accept_peers(void *data) {
    MdManagementIpc *s = data;
    for (;;) {
        struct pollfd pollfds[2] = {
            { .fd = s->listener, .events = POLLIN },
            { .fd = s->stop_pipe[0], .events = POLLIN },
        };
        int rc = poll(pollfds,2,-1);
        if (rc < 0 && errno == EINTR) continue;
        pthread_mutex_lock(&s->mu);
        bool stop = s->stopping;
        pthread_mutex_unlock(&s->mu);
        if (stop || rc < 0) break;
        int fd = accept(s->listener,NULL,NULL);
        if (fd < 0) continue;
        fcntl(fd,F_SETFD,FD_CLOEXEC); fcntl(fd,F_SETFL,O_NONBLOCK);
#ifdef __APPLE__
        int one = 1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
        if (!md_management_peer_is_user(fd,(uint64_t)geteuid())) { close(fd); continue; }
        Peer *slot = NULL;
        for (size_t i=0; i<PEERS; ++i) {
            Peer *p = &s->peers[i];
            pthread_mutex_lock(&s->mu); bool done = p->used && p->done; pthread_mutex_unlock(&s->mu);
            if (done) { pthread_join(p->thread,NULL); p->used = false; }
            if (!p->used && !slot) slot = p;
        }
        if (!slot) { close(fd); continue; }
        *slot = (Peer){ .server=s, .fd=fd, .used=true };
        if (pthread_create(&slot->thread,NULL,serve_peer,slot)) { close(fd); slot->used=false; }
    }
    return NULL;
}
MdManagementIpc *md_management_start(MdHostAccess *a, MdNostr *n, const char *key,
    const char *path, uint32_t supported, void (*wake)(void *), void *data) {
    MdManagementIpc *s = calloc(1,sizeof(*s)); if (!s) return NULL;
    s->listener = -1; s->stop_pipe[0] = s->stop_pipe[1] = -1; s->access=a; s->nostr=n; s->supported=supported; s->wake=wake; s->data=data;
    if (path) { if (strlen(path) >= sizeof(s->path)) goto fail; strcpy(s->path,path); }
    else if (md_management_path(key,s->path,sizeof(s->path))) goto fail;
    if (!private_parent(s->path)) goto fail;
    struct stat st;
    if (!lstat(s->path,&st) || errno != ENOENT) goto fail;
    s->listener = unix_socket(); if (s->listener < 0) goto fail;
    struct sockaddr_un addr = { .sun_family=AF_UNIX }; strcpy(addr.sun_path,s->path);
    if (bind(s->listener,(struct sockaddr *)&addr,sizeof(addr))) goto fail;
    if (lstat(s->path,&st)) goto bound_fail;
    s->dev=st.st_dev; s->ino=st.st_ino;
    if (chmod(s->path,0600) || listen(s->listener,PEERS)) goto bound_fail;
    if (pipe(s->stop_pipe)) goto bound_fail;
    for (int i=0; i<2; ++i) {
        if (fcntl(s->stop_pipe[i],F_SETFD,FD_CLOEXEC) ||
            fcntl(s->stop_pipe[i],F_SETFL,O_NONBLOCK)) goto bound_fail;
    }
    if (pthread_mutex_init(&s->mu,NULL)) goto bound_fail;
    if (pthread_create(&s->thread,NULL,accept_peers,s)) { pthread_mutex_destroy(&s->mu); goto bound_fail; }
    return s;
bound_fail:
    unlink(s->path);
fail:
    for (int i=0; i<2; ++i) if (s->stop_pipe[i] >= 0) close(s->stop_pipe[i]);
    if (s->listener >= 0) close(s->listener);
    free(s); return NULL;
}
void md_management_stop(MdManagementIpc *s) {
    if (!s) return;
    md_host_access_stop(s->access);
    pthread_mutex_lock(&s->mu); s->stopping = true; pthread_mutex_unlock(&s->mu);
    char stop = 1;
    (void)write(s->stop_pipe[1],&stop,1);
    pthread_join(s->thread,NULL);
    for (size_t i=0; i<PEERS; ++i) if (s->peers[i].used) {
        pthread_mutex_lock(&s->mu);
        if (s->peers[i].fd >= 0) shutdown(s->peers[i].fd,SHUT_RDWR);
        pthread_mutex_unlock(&s->mu);
        pthread_join(s->peers[i].thread,NULL);
    }
    close(s->listener);
    close(s->stop_pipe[0]); close(s->stop_pipe[1]);
    struct stat st;
    if (!lstat(s->path,&st) && st.st_dev == s->dev && st.st_ino == s->ino) unlink(s->path);
    pthread_mutex_destroy(&s->mu); free(s);
}
cJSON *md_management_call(const char *path, const cJSON *j) {
    struct stat st;
    if (!private_parent(path) || lstat(path,&st) || !S_ISSOCK(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077)) return NULL;
    int fd = unix_socket(); if (fd < 0) return NULL;
    struct sockaddr_un addr = { .sun_family=AF_UNIX }; strcpy(addr.sun_path,path);
    cJSON *reply = NULL;
    int rc = connect(fd,(struct sockaddr *)&addr,sizeof(addr));
    if (rc && errno == EINPROGRESS) {
        int error = 0; socklen_t size = sizeof(error);
        rc = !ready(fd,POLLOUT,md_now_ms()+5000) || getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size) || error;
    }
    if (!rc && md_management_peer_is_user(fd,(uint64_t)geteuid()) && send_json(fd,j)) reply = read_json(fd);
    close(fd); return reply;
}
#else
struct MdManagementIpc { int unused; };
int md_management_path(const char *k,char *p,size_t n) { (void)k;(void)p;(void)n;return -1; }
bool md_management_peer_is_user(int fd,uint64_t uid) { (void)fd;(void)uid;return false; }
MdManagementIpc *md_management_start(MdHostAccess *a,MdNostr *n,const char *k,const char *p,
    uint32_t caps,void (*wake)(void *),void *d) {
    (void)a;(void)n;(void)k;(void)p;(void)caps;(void)wake;(void)d;
    fprintf(stderr,"management: unsupported on Windows (owner-DACL/client-SID named pipe not implemented)\n"); return NULL;
}
void md_management_stop(MdManagementIpc *s) { (void)s; }
cJSON *md_management_call(const char *p,const cJSON *j) { (void)p;(void)j;return NULL; }
#endif

static int usage(void) {
    fprintf(stderr,"Usage: metadesk-host session|requests [--socket ABSOLUTE_PATH | --host-key HEX] list\n"
        "       metadesk-host session [endpoint] kick SESSION_ID GENERATION REVISION\n"
        "       metadesk-host requests [endpoint] approve REQUEST_ID GENERATION REVISION CAPS [--remember]\n"
        "       metadesk-host requests [endpoint] deny REQUEST_ID GENERATION REVISION\n"
        "Use exact generation/revision from list JSON; caps: canonical video,agent,input subset.\n");
    return 2;
}
int md_host_management_cli(const char *area, int argc, char **argv) {
#ifdef _WIN32
    (void)area; (void)argc; (void)argv;
    fprintf(stderr,"management: unsupported on Windows; requires owner-only named pipe and client-SID verification\n"); return 1;
#else
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (argc < 3) return usage();
    if (!strcmp(argv[0],"--socket")) {
        if (strlen(argv[1]) >= sizeof(path)) return usage(); strcpy(path,argv[1]);
    } else if (!strcmp(argv[0],"--host-key")) {
        if (md_management_path(argv[1],path,sizeof(path))) return usage();
    } else return usage();
    argc -= 2; argv += 2;
    bool list = !strcmp(argv[0],"list"), requests = !strcmp(area,"requests");
    bool approve = requests && !strcmp(argv[0],"approve");
    bool deny = requests && !strcmp(argv[0],"deny");
    bool kick = !requests && !strcmp(argv[0],"kick");
    bool remember = approve && argc == 6 && !strcmp(argv[5],"--remember");
    if ((!list && !approve && !deny && !kick) || (list && argc != 1) ||
        ((deny || kick) && argc != 4) || (approve && argc != 5 && !remember)) return usage();
    cJSON *j = cJSON_CreateObject(); cJSON_AddNumberToObject(j,"v",1);
    cJSON_AddStringToObject(j,"method",list ? (requests ? "requests.list" : "session.list") : kick ? "session.kick" : "requests.decide");
    if (!list) {
        char *end; if (!argv[3][0] || strspn(argv[3],"0123456789") != strlen(argv[3])) { cJSON_Delete(j); return usage(); }
        double rev = strtod(argv[3],&end);
        if (*end || rev < 1 || rev > 9007199254740991.0 || floor(rev) != rev) { cJSON_Delete(j); return usage(); }
        cJSON_AddStringToObject(j,kick ? "session_id" : "request_id",argv[1]);
        cJSON_AddStringToObject(j,"generation",argv[2]); cJSON_AddNumberToObject(j,"revision",rev);
        if (!kick) cJSON_AddStringToObject(j,"decision",approve ? "approve" : "deny");
        if (approve) {
            uint32_t caps;
            if (md_nostr_parse_caps(argv[4],&caps) || !caps) { cJSON_Delete(j); return usage(); }
            cJSON_AddNumberToObject(j,"caps",caps); cJSON_AddBoolToObject(j,"remember",remember);
        }
    }
    cJSON *reply = md_management_call(path,j); cJSON_Delete(j);
    if (!reply) { fprintf(stderr,"management: endpoint unavailable, unsafe, wrong owner, or response deadline exceeded\n"); return 1; }
    char *text = cJSON_PrintUnformatted(reply);
    if (text) { puts(text); free(text); }
    int rc = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply,"ok")) ? 0 : 1;
    cJSON_Delete(reply); return rc;
#endif
}
