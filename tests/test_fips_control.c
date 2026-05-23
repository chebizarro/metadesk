/*
 * test_fips_control.c — FIPS control socket client seam tests.
 */
#include "fips_control.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define PASS(name) printf("  PASS  %s\n", name)

static int run_fips_dir_exists(void)
{
#ifndef _WIN32
    struct stat st;
    return stat("/run/fips", &st) == 0 && S_ISDIR(st.st_mode);
#else
    return 0;
#endif
}

static void test_result_strings(void)
{
    assert(strcmp(md_fips_control_result_string(MD_FIPS_CONTROL_OK), "ok") == 0);
    assert(strcmp(md_fips_control_result_string(MD_FIPS_CONTROL_DAEMON_ERROR),
                  "daemon_error") == 0);
    assert(strcmp(md_fips_control_result_string(MD_FIPS_CONTROL_DAEMON_UNAVAILABLE),
                  "daemon_unavailable") == 0);
    PASS("result strings");
}

static void test_path_override(void)
{
    char path[MD_FIPS_CONTROL_PATH_MAX];
    assert(md_fips_control_resolve_socket_path("/tmp/custom-fips.sock",
                                               path, sizeof(path)) == 0);
    assert(strcmp(path, "/tmp/custom-fips.sock") == 0);
    PASS("explicit socket path override");
}

static void test_default_path_order(void)
{
    char path[MD_FIPS_CONTROL_PATH_MAX];

#ifndef _WIN32
    const char *old_xdg = getenv("XDG_RUNTIME_DIR");
    char *old_xdg_copy = NULL;
    if (old_xdg) {
        old_xdg_copy = malloc(strlen(old_xdg) + 1);
        assert(old_xdg_copy != NULL);
        strcpy(old_xdg_copy, old_xdg);
    }

    setenv("XDG_RUNTIME_DIR", "/tmp/md-fips-control-xdg", 1);
    assert(md_fips_control_resolve_socket_path(NULL, path, sizeof(path)) == 0);
    if (run_fips_dir_exists())
        assert(strcmp(path, MD_FIPS_CONTROL_RUN_SOCKET) == 0);
    else
        assert(strcmp(path, "/tmp/md-fips-control-xdg/fips/control.sock") == 0);

    unsetenv("XDG_RUNTIME_DIR");
    assert(md_fips_control_resolve_socket_path(NULL, path, sizeof(path)) == 0);
    if (run_fips_dir_exists())
        assert(strcmp(path, MD_FIPS_CONTROL_RUN_SOCKET) == 0);
    else
        assert(strcmp(path, MD_FIPS_CONTROL_TMP_SOCKET) == 0);

    if (old_xdg_copy) {
        setenv("XDG_RUNTIME_DIR", old_xdg_copy, 1);
        free(old_xdg_copy);
    }
#else
    assert(md_fips_control_resolve_socket_path(NULL, path, sizeof(path)) < 0);
#endif

    PASS("default socket path order");
}

static void test_parse_ok_response(void)
{
    const char *json = "{\"status\":\"ok\",\"data\":{\"state\":\"running\",\"peer_count\":2}}\n";
    MdFipsControlResponse resp = {0};
    MdFipsControlResult r = md_fips_control_parse_response(json, strlen(json), &resp);
    assert(r == MD_FIPS_CONTROL_OK);
    assert(resp.result == MD_FIPS_CONTROL_OK);
    assert(resp.data != NULL);
    cJSON *state = cJSON_GetObjectItemCaseSensitive(resp.data, "state");
    assert(cJSON_IsString(state));
    assert(strcmp(state->valuestring, "running") == 0);
    md_fips_control_response_free(&resp);
    PASS("parse ok response");
}

static void test_parse_daemon_error_response(void)
{
    const char *json = "{\"status\":\"error\",\"message\":\"unknown command: nope\"}\n";
    MdFipsControlResponse resp = {0};
    MdFipsControlResult r = md_fips_control_parse_response(json, strlen(json), &resp);
    assert(r == MD_FIPS_CONTROL_DAEMON_ERROR);
    assert(resp.result == MD_FIPS_CONTROL_DAEMON_ERROR);
    assert(resp.message != NULL);
    assert(strcmp(resp.message, "unknown command: nope") == 0);
    md_fips_control_response_free(&resp);
    PASS("parse daemon error response");
}

static void test_parse_invalid_response(void)
{
    const char *json = "{\"status\":\"maybe\"}";
    MdFipsControlResponse resp = {0};
    MdFipsControlResult r = md_fips_control_parse_response(json, strlen(json), &resp);
    assert(r == MD_FIPS_CONTROL_INVALID_RESPONSE);
    assert(resp.result == MD_FIPS_CONTROL_INVALID_RESPONSE);
    assert(resp.message != NULL);
    md_fips_control_response_free(&resp);
    PASS("parse invalid response");
}

static void test_response_reuse(void)
{
    MdFipsControlResponse resp;
    md_fips_control_response_init(&resp);

    const char *ok = "{\"status\":\"ok\",\"data\":{\"state\":\"running\"}}";
    assert(md_fips_control_parse_response(ok, strlen(ok), &resp) ==
           MD_FIPS_CONTROL_OK);
    assert(resp.data != NULL);

    const char *err = "{\"status\":\"error\",\"message\":\"query timeout\"}";
    assert(md_fips_control_parse_response(err, strlen(err), &resp) ==
           MD_FIPS_CONTROL_DAEMON_ERROR);
    assert(resp.data == NULL);
    assert(resp.message != NULL);
    assert(strcmp(resp.message, "query timeout") == 0);

    md_fips_control_response_free(&resp);
    PASS("response reuse clears owned fields");
}

static void test_params_must_be_object(void)
{
    cJSON *params = cJSON_CreateArray();
    assert(params != NULL);
    MdFipsControlResponse resp = {0};
    MdFipsControlResult r = md_fips_control_request("/tmp/unused-fips.sock",
                                                    "connect", params, 10, &resp);
    assert(r == MD_FIPS_CONTROL_INVALID_ARGUMENT);
    assert(resp.result == MD_FIPS_CONTROL_INVALID_ARGUMENT);
    cJSON_Delete(params);
    md_fips_control_response_free(&resp);
    PASS("params object validation");
}

#ifndef _WIN32
typedef struct {
    int srv_fd;
    const char *response;
    char request[512];
    ssize_t request_len;
} FakeServer;

static void *fake_server_thread(void *arg)
{
    FakeServer *srv = arg;
    int fd = accept(srv->srv_fd, NULL, NULL);
    assert(fd >= 0);

    size_t off = 0;
    while (off + 1 < sizeof(srv->request)) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        assert(n == 1);
        srv->request[off++] = ch;
        if (ch == '\n') break;
    }
    srv->request[off] = '\0';
    srv->request_len = (ssize_t)off;

    size_t response_len = strlen(srv->response);
    ssize_t written = write(fd, srv->response, response_len);
    assert(written == (ssize_t)response_len);
    close(fd);
    return NULL;
}

static int make_fake_listener(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);

    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 1) == 0);
    return fd;
}

static void test_one_shot_request(void)
{
    char path[108];
    snprintf(path, sizeof(path), "/tmp/metadesk-fips-control-%d.sock", (int)getpid());

    FakeServer srv = {
        .srv_fd = make_fake_listener(path),
        .response = "{\"status\":\"ok\",\"data\":{\"state\":\"running\"}}\n",
        .request = {0},
        .request_len = 0,
    };

    pthread_t tid;
    assert(pthread_create(&tid, NULL, fake_server_thread, &srv) == 0);

    MdFipsControlResponse resp = {0};
    MdFipsControlResult r = md_fips_control_request(path, "show_status", NULL,
                                                    1000, &resp);
    assert(r == MD_FIPS_CONTROL_OK);
    assert(resp.result == MD_FIPS_CONTROL_OK);
    assert(resp.socket_path != NULL);
    assert(strcmp(resp.socket_path, path) == 0);
    assert(resp.data != NULL);

    assert(pthread_join(tid, NULL) == 0);
    assert(srv.request_len > 0);
    assert(srv.request[srv.request_len - 1] == '\n');
    assert(strstr(srv.request, "\"command\":\"show_status\"") != NULL);
    assert(strstr(srv.request, "fips-nat") == NULL);

    md_fips_control_response_free(&resp);
    close(srv.srv_fd);
    unlink(path);
    PASS("one-shot line-delimited request");
}

static void test_daemon_unavailable(void)
{
    char path[108];
    snprintf(path, sizeof(path), "/tmp/metadesk-fips-control-missing-%d.sock",
             (int)getpid());
    unlink(path);

    MdFipsControlResponse resp = {0};
    MdFipsControlResult r = md_fips_control_request(path, "show_status", NULL,
                                                    100, &resp);
    assert(r == MD_FIPS_CONTROL_DAEMON_UNAVAILABLE);
    assert(resp.result == MD_FIPS_CONTROL_DAEMON_UNAVAILABLE);
    assert(resp.message != NULL);
    assert(resp.socket_path != NULL);
    assert(strcmp(resp.socket_path, path) == 0);
    md_fips_control_response_free(&resp);
    PASS("daemon unavailable is distinct");
}

static void test_peer_ready_from_show_peers(void)
{
    char path[108];
    snprintf(path, sizeof(path), "/tmp/metadesk-fips-ready-%d.sock", (int)getpid());

    FakeServer srv = {
        .srv_fd = make_fake_listener(path),
        .response = "{\"status\":\"ok\",\"data\":{\"peers\":[{\"npub\":\"npub1peer\",\"connectivity\":\"connected\",\"link_id\":7}]}}\n",
        .request = {0},
        .request_len = 0,
    };

    pthread_t tid;
    assert(pthread_create(&tid, NULL, fake_server_thread, &srv) == 0);

    MdFipsPeerReadiness ready;
    MdFipsPeerReadinessState state = md_fips_control_wait_peer_ready(
        path, "npub1peer", 1000, 10, &ready);
    assert(state == MD_FIPS_PEER_READY);
    assert(ready.state == MD_FIPS_PEER_READY);
    assert(ready.control_result == MD_FIPS_CONTROL_OK);
    assert(strstr(ready.detail, "show_peers") != NULL);

    assert(pthread_join(tid, NULL) == 0);
    assert(strstr(srv.request, "\"command\":\"show_peers\"") != NULL);

    close(srv.srv_fd);
    unlink(path);
    PASS("peer readiness from show_peers");
}
#else
static void test_one_shot_request(void) { PASS("one-shot request skipped on Windows"); }
static void test_daemon_unavailable(void) { PASS("daemon unavailable skipped on Windows"); }
static void test_peer_ready_from_show_peers(void) { PASS("peer readiness skipped on Windows"); }
#endif

int main(void)
{
    printf("test_fips_control:\n");
    test_result_strings();
    test_path_override();
    test_default_path_order();
    test_parse_ok_response();
    test_parse_daemon_error_response();
    test_parse_invalid_response();
    test_response_reuse();
    test_params_must_be_object();
    test_one_shot_request();
    test_daemon_unavailable();
    test_peer_ready_from_show_peers();
    return 0;
}
