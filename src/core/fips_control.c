/*
 * metadesk — fips_control.c
 * Thin FIPS daemon control socket client seam.
 */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "fips_control.h"

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static char *md_fc_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static int md_fc_copy_path(const char *src, char *out, size_t out_len)
{
    if (!src || !out || out_len == 0) return -1;
    int n = snprintf(out, out_len, "%s", src);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return 0;
}

static int md_fc_join_path(const char *dir, const char *suffix,
                           char *out, size_t out_len)
{
    if (!dir || !suffix || !out || out_len == 0) return -1;
    int n = snprintf(out, out_len, "%s%s", dir, suffix);
    if (n < 0 || (size_t)n >= out_len) return -1;
    return 0;
}

void md_fips_control_response_init(MdFipsControlResponse *resp)
{
    if (!resp) return;
    memset(resp, 0, sizeof(*resp));
    resp->result = MD_FIPS_CONTROL_INVALID_ARGUMENT;
}

void md_fips_control_response_free(MdFipsControlResponse *resp)
{
    if (!resp) return;
    free(resp->socket_path);
    resp->socket_path = NULL;
    if (resp->data) {
        cJSON_Delete(resp->data);
        resp->data = NULL;
    }
    free(resp->message);
    resp->message = NULL;
    resp->result = MD_FIPS_CONTROL_INVALID_ARGUMENT;
}

static void md_fc_prepare_response(MdFipsControlResponse *resp)
{
    md_fips_control_response_free(resp);
    md_fips_control_response_init(resp);
}

static MdFipsControlResult md_fc_set_result(MdFipsControlResponse *resp,
                                            MdFipsControlResult result,
                                            const char *socket_path,
                                            const char *fmt, ...)
{
    if (!resp) return result;
    md_fc_prepare_response(resp);
    resp->result = result;
    if (socket_path && socket_path[0])
        resp->socket_path = md_fc_strdup(socket_path);

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        va_list ap2;
        va_copy(ap2, ap);
        int needed = vsnprintf(NULL, 0, fmt, ap);
        va_end(ap);
        if (needed >= 0) {
            resp->message = malloc((size_t)needed + 1);
            if (resp->message)
                vsnprintf(resp->message, (size_t)needed + 1, fmt, ap2);
        }
        va_end(ap2);
    }

    return result;
}

const char *md_fips_control_result_string(MdFipsControlResult result)
{
    switch (result) {
    case MD_FIPS_CONTROL_OK: return "ok";
    case MD_FIPS_CONTROL_DAEMON_ERROR: return "daemon_error";
    case MD_FIPS_CONTROL_DAEMON_UNAVAILABLE: return "daemon_unavailable";
    case MD_FIPS_CONTROL_INVALID_RESPONSE: return "invalid_response";
    case MD_FIPS_CONTROL_INVALID_ARGUMENT: return "invalid_argument";
    case MD_FIPS_CONTROL_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}


const char *md_fips_peer_readiness_string(MdFipsPeerReadinessState state)
{
    switch (state) {
    case MD_FIPS_PEER_READY: return "ready";
    case MD_FIPS_PEER_NOT_FOUND: return "not_found";
    case MD_FIPS_PEER_CONVERGING: return "converging";
    case MD_FIPS_PEER_ERROR: return "error";
    default: return "unknown";
    }
}

static void md_fc_readiness_set(MdFipsPeerReadiness *out,
                                MdFipsPeerReadinessState state,
                                MdFipsControlResult control_result,
                                const char *socket_path,
                                const char *fmt, ...)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = state;
    out->control_result = control_result;
    if (socket_path && socket_path[0])
        snprintf(out->socket_path, sizeof(out->socket_path), "%s", socket_path);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(out->detail, sizeof(out->detail), fmt, ap);
        va_end(ap);
    }
}

static bool md_fc_json_string_equals(const cJSON *obj, const char *key,
                                     const char *expected)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    return cJSON_IsString(item) && item->valuestring && expected &&
           strcmp(item->valuestring, expected) == 0;
}

static bool md_fc_link_id_usable(const cJSON *obj)
{
    cJSON *link_id = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, "link_id");
    if (cJSON_IsNumber(link_id)) return true;
    if (cJSON_IsString(link_id) && link_id->valuestring && link_id->valuestring[0] != '\0')
        return true;
    return false;
}

static bool md_fc_peer_connectivity_usable(const cJSON *obj)
{
    cJSON *conn = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, "connectivity");
    if (!cJSON_IsString(conn) || !conn->valuestring) return false;

    /* FIPS marks both connected and stale peers as usable for sending traffic. */
    return strcmp(conn->valuestring, "connected") == 0 ||
           strcmp(conn->valuestring, "active") == 0 ||
           strcmp(conn->valuestring, "stale") == 0;
}

static MdFipsPeerReadinessState
md_fc_inspect_peers(const cJSON *data, const char *peer_npub,
                    char *detail, size_t detail_len)
{
    cJSON *peers = cJSON_GetObjectItemCaseSensitive((cJSON *)data, "peers");
    if (!cJSON_IsArray(peers)) {
        snprintf(detail, detail_len, "show_peers response missing peers[]");
        return MD_FIPS_PEER_ERROR;
    }

    cJSON *peer = NULL;
    cJSON_ArrayForEach(peer, peers) {
        if (!cJSON_IsObject(peer)) continue;
        if (!md_fc_json_string_equals(peer, "npub", peer_npub)) continue;

        cJSON *conn = cJSON_GetObjectItemCaseSensitive(peer, "connectivity");
        const char *conn_s = cJSON_IsString(conn) && conn->valuestring
                           ? conn->valuestring : "unknown";
        if (md_fc_peer_connectivity_usable(peer) && md_fc_link_id_usable(peer)) {
            snprintf(detail, detail_len,
                     "peer present in show_peers with connectivity=%s and link_id",
                     conn_s);
            return MD_FIPS_PEER_READY;
        }

        snprintf(detail, detail_len,
                 "peer present in show_peers but route still converging (connectivity=%s, link_id=%s)",
                 conn_s, md_fc_link_id_usable(peer) ? "present" : "missing");
        return MD_FIPS_PEER_CONVERGING;
    }

    snprintf(detail, detail_len,
             "peer not present in show_peers");
    return MD_FIPS_PEER_NOT_FOUND;
}

static MdFipsPeerReadinessState
md_fc_inspect_sessions(const cJSON *data, const char *peer_npub,
                       MdFipsPeerReadinessState prior_state,
                       char *detail, size_t detail_len)
{
    cJSON *sessions = cJSON_GetObjectItemCaseSensitive((cJSON *)data, "sessions");
    if (!cJSON_IsArray(sessions)) {
        if (prior_state == MD_FIPS_PEER_NOT_FOUND)
            snprintf(detail, detail_len, "show_sessions response missing sessions[]");
        return prior_state == MD_FIPS_PEER_NOT_FOUND
             ? MD_FIPS_PEER_ERROR : prior_state;
    }

    cJSON *session = NULL;
    cJSON_ArrayForEach(session, sessions) {
        if (!cJSON_IsObject(session)) continue;
        if (!md_fc_json_string_equals(session, "npub", peer_npub)) continue;

        cJSON *state = cJSON_GetObjectItemCaseSensitive(session, "state");
        const char *state_s = cJSON_IsString(state) && state->valuestring
                            ? state->valuestring : "unknown";
        if (strcmp(state_s, "established") == 0) {
            snprintf(detail, detail_len,
                     "peer present in show_sessions with state=established");
            return MD_FIPS_PEER_READY;
        }

        snprintf(detail, detail_len,
                 "peer present in show_sessions but route still converging (state=%s)",
                 state_s);
        return MD_FIPS_PEER_CONVERGING;
    }

    if (prior_state == MD_FIPS_PEER_NOT_FOUND)
        snprintf(detail, detail_len,
                 "peer not present in show_peers or show_sessions");
    return prior_state;
}

static uint64_t md_fc_time_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)time(NULL) * 1000u;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
    return (uint64_t)time(NULL) * 1000u;
#else
    return (uint64_t)time(NULL) * 1000u;
#endif
}

static void md_fc_sleep_ms(uint32_t ms)
{
    if (ms == 0) return;
#if defined(_WIN32)
    (void)ms;
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
#endif
}

static uint32_t md_fc_remaining_request_timeout(uint64_t deadline_ms)
{
    uint64_t now = md_fc_time_ms();
    if (now >= deadline_ms) return 0;
    uint64_t remaining = deadline_ms - now;
    if (remaining > MD_FIPS_CONTROL_DEFAULT_TIMEOUT_MS)
        remaining = MD_FIPS_CONTROL_DEFAULT_TIMEOUT_MS;
    if (remaining == 0) remaining = 1;
    return (uint32_t)remaining;
}

MdFipsPeerReadinessState
md_fips_control_wait_peer_ready(const char *socket_override,
                                const char *peer_npub,
                                uint32_t total_timeout_ms,
                                uint32_t poll_interval_ms,
                                MdFipsPeerReadiness *out)
{
    if (!peer_npub || peer_npub[0] == '\0' || !out) {
        md_fc_readiness_set(out, MD_FIPS_PEER_ERROR,
                            MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                            "missing peer npub");
        return MD_FIPS_PEER_ERROR;
    }

    if (total_timeout_ms == 0)
        total_timeout_ms = MD_FIPS_CONTROL_DEFAULT_TIMEOUT_MS;
    if (poll_interval_ms == 0)
        poll_interval_ms = MD_FIPS_CONTROL_DEFAULT_PEER_POLL_MS;

    uint64_t deadline = md_fc_time_ms() + total_timeout_ms;
    MdFipsPeerReadinessState last_state = MD_FIPS_PEER_NOT_FOUND;
    MdFipsControlResult last_result = MD_FIPS_CONTROL_OK;
    char last_socket[MD_FIPS_CONTROL_PATH_MAX] = {0};
    char detail[256] = {0};

    for (;;) {
        uint32_t request_timeout_ms = md_fc_remaining_request_timeout(deadline);
        if (request_timeout_ms == 0) break;

        MdFipsControlResponse peers_resp;
        md_fips_control_response_init(&peers_resp);
        MdFipsControlResult r = md_fips_control_request(socket_override,
                                                        "show_peers", NULL,
                                                        request_timeout_ms,
                                                        &peers_resp);
        last_result = r;
        if (peers_resp.socket_path)
            snprintf(last_socket, sizeof(last_socket), "%s", peers_resp.socket_path);

        if (r != MD_FIPS_CONTROL_OK) {
            snprintf(detail, sizeof(detail), "show_peers failed: %s%s%s",
                     md_fips_control_result_string(r),
                     peers_resp.message ? ": " : "",
                     peers_resp.message ? peers_resp.message : "");
            md_fips_control_response_free(&peers_resp);
            md_fc_readiness_set(out, MD_FIPS_PEER_ERROR, r, last_socket,
                                "%s", detail);
            return MD_FIPS_PEER_ERROR;
        }

        last_state = md_fc_inspect_peers(peers_resp.data, peer_npub,
                                         detail, sizeof(detail));
        md_fips_control_response_free(&peers_resp);

        if (last_state == MD_FIPS_PEER_READY) {
            md_fc_readiness_set(out, last_state, MD_FIPS_CONTROL_OK,
                                last_socket, "%s", detail);
            return last_state;
        }
        if (last_state == MD_FIPS_PEER_ERROR) {
            md_fc_readiness_set(out, last_state, MD_FIPS_CONTROL_INVALID_RESPONSE,
                                last_socket, "%s", detail);
            return last_state;
        }

        request_timeout_ms = md_fc_remaining_request_timeout(deadline);
        if (request_timeout_ms == 0) break;

        MdFipsControlResponse sessions_resp;
        md_fips_control_response_init(&sessions_resp);
        r = md_fips_control_request(socket_override, "show_sessions", NULL,
                                    request_timeout_ms,
                                    &sessions_resp);
        last_result = r;
        if (sessions_resp.socket_path)
            snprintf(last_socket, sizeof(last_socket), "%s", sessions_resp.socket_path);

        if (r != MD_FIPS_CONTROL_OK) {
            snprintf(detail, sizeof(detail), "show_sessions failed: %s%s%s",
                     md_fips_control_result_string(r),
                     sessions_resp.message ? ": " : "",
                     sessions_resp.message ? sessions_resp.message : "");
            md_fips_control_response_free(&sessions_resp);
            md_fc_readiness_set(out, MD_FIPS_PEER_ERROR, r, last_socket,
                                "%s", detail);
            return MD_FIPS_PEER_ERROR;
        }

        last_state = md_fc_inspect_sessions(sessions_resp.data, peer_npub,
                                            last_state, detail, sizeof(detail));
        md_fips_control_response_free(&sessions_resp);

        if (last_state == MD_FIPS_PEER_READY) {
            md_fc_readiness_set(out, last_state, MD_FIPS_CONTROL_OK,
                                last_socket, "%s", detail);
            return last_state;
        }
        if (last_state == MD_FIPS_PEER_ERROR) {
            md_fc_readiness_set(out, last_state, MD_FIPS_CONTROL_INVALID_RESPONSE,
                                last_socket, "%s", detail);
            return last_state;
        }

        uint64_t now = md_fc_time_ms();
        if (now >= deadline) break;
        uint64_t remaining = deadline - now;
        md_fc_sleep_ms((uint32_t)(remaining < poll_interval_ms ? remaining : poll_interval_ms));
    }

    md_fc_readiness_set(out, last_state, last_result, last_socket,
                        "%s", detail[0] ? detail : "peer readiness timed out");
    return last_state;
}

int md_fips_control_resolve_socket_path(const char *socket_override,
                                        char *out, size_t out_len)
{
    if (!out || out_len == 0) return -1;

    if (socket_override && socket_override[0] != '\0')
        return md_fc_copy_path(socket_override, out, out_len);

#ifndef _WIN32
    struct stat st;
    if (stat("/run/fips", &st) == 0 && S_ISDIR(st.st_mode))
        return md_fc_copy_path(MD_FIPS_CONTROL_RUN_SOCKET, out, out_len);

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0] != '\0')
        return md_fc_join_path(runtime_dir, MD_FIPS_CONTROL_XDG_SUFFIX,
                               out, out_len);

    return md_fc_copy_path(MD_FIPS_CONTROL_TMP_SOCKET, out, out_len);
#else
    return -1;
#endif
}

MdFipsControlResult md_fips_control_parse_response(const char *json,
                                                   size_t json_len,
                                                   MdFipsControlResponse *resp)
{
    if (!resp) return MD_FIPS_CONTROL_INVALID_ARGUMENT;
    md_fc_prepare_response(resp);

    if (!json || json_len == 0) {
        return md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_RESPONSE, NULL,
                                "empty response");
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root) {
        return md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_RESPONSE, NULL,
                                "response is not valid JSON");
    }

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_RESPONSE, NULL,
                                "response is not a JSON object");
    }

    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsString(status) || !status->valuestring) {
        cJSON_Delete(root);
        return md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_RESPONSE, NULL,
                                "response missing string status");
    }

    if (strcmp(status->valuestring, "ok") == 0) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        resp->result = MD_FIPS_CONTROL_OK;
        resp->data = data ? cJSON_Duplicate(data, true) : NULL;
        if (data && !resp->data) {
            cJSON_Delete(root);
            return md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_RESPONSE, NULL,
                                    "could not copy response data");
        }
        cJSON_Delete(root);
        return resp->result;
    }

    if (strcmp(status->valuestring, "error") == 0) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *msg = cJSON_IsString(message) && message->valuestring
                            ? message->valuestring
                            : "FIPS daemon returned an error";
        resp->result = MD_FIPS_CONTROL_DAEMON_ERROR;
        resp->message = md_fc_strdup(msg);
        cJSON_Delete(root);
        return resp->result;
    }

    cJSON_Delete(root);
    return md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_RESPONSE, NULL,
                            "unknown response status");
}

static char *md_fc_build_request_json(const char *command, const cJSON *params,
                                      MdFipsControlResponse *resp)
{
    if (!command || command[0] == '\0') {
        md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                         "missing FIPS control command");
        return NULL;
    }

    if (params && !cJSON_IsObject(params)) {
        md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                         "FIPS control params must be a JSON object");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                         "could not allocate FIPS control request");
        return NULL;
    }

    if (!cJSON_AddStringToObject(root, "command", command)) {
        cJSON_Delete(root);
        md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                         "could not add FIPS control command");
        return NULL;
    }

    if (params) {
        cJSON *params_copy = cJSON_Duplicate((cJSON *)params, true);
        if (!params_copy) {
            cJSON_Delete(root);
            md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                             "could not copy FIPS control params");
            return NULL;
        }
        if (!cJSON_AddItemToObject(root, "params", params_copy)) {
            cJSON_Delete(params_copy);
            cJSON_Delete(root);
            md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                             "could not add FIPS control params");
            return NULL;
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                         "could not serialize FIPS control request");
        return NULL;
    }

    size_t len = strlen(json);
    if (len + 1 > MD_FIPS_CONTROL_MAX_REQUEST) {
        cJSON_free(json);
        md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                         "FIPS control request exceeds %u bytes",
                         MD_FIPS_CONTROL_MAX_REQUEST);
        return NULL;
    }

    char *line = malloc(len + 2);
    if (!line) {
        cJSON_free(json);
        md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                         "could not allocate FIPS control request line");
        return NULL;
    }
    memcpy(line, json, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    cJSON_free(json);
    return line;
}

#ifndef _WIN32
static uint64_t md_fc_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return ((uint64_t)ts.tv_sec * 1000u) + ((uint64_t)ts.tv_nsec / 1000000u);
    return 0;
}

static uint64_t md_fc_deadline_ms(uint32_t timeout_ms)
{
    if (timeout_ms == 0) timeout_ms = MD_FIPS_CONTROL_DEFAULT_TIMEOUT_MS;
    return md_fc_now_ms() + timeout_ms;
}

static int md_fc_remaining_poll_ms(uint64_t deadline_ms)
{
    uint64_t now = md_fc_now_ms();
    if (now >= deadline_ms) return 0;
    uint64_t remaining = deadline_ms - now;
    if (remaining > 2147483647u) return 2147483647;
    return (int)remaining;
}

static int md_fc_make_socket_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static int md_fc_connect_unix(const char *path, uint64_t deadline_ms,
                              char *errbuf, size_t errbuf_len)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(errbuf, errbuf_len, "socket: %s", strerror(errno));
        return -1;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);

#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        snprintf(errbuf, errbuf_len, "socket path too long: %s", path);
        close(fd);
        return -1;
    }

    if (md_fc_make_socket_nonblocking(fd) < 0) {
        snprintf(errbuf, errbuf_len, "fcntl: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) return fd;
    if (errno != EINPROGRESS) {
        snprintf(errbuf, errbuf_len, "connect %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    ret = poll(&pfd, 1, md_fc_remaining_poll_ms(deadline_ms));
    if (ret == 0) {
        snprintf(errbuf, errbuf_len, "connect %s: timeout", path);
        close(fd);
        return -1;
    }
    if (ret < 0) {
        snprintf(errbuf, errbuf_len, "connect %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    int err = 0;
    socklen_t err_len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0 || err != 0) {
        if (err == 0) err = errno;
        snprintf(errbuf, errbuf_len, "connect %s: %s", path, strerror(err));
        close(fd);
        return -1;
    }

    return fd;
}

static ssize_t md_fc_send(int fd, const char *buf, size_t len)
{
#ifdef MSG_NOSIGNAL
    return send(fd, buf, len, MSG_NOSIGNAL);
#else
    return write(fd, buf, len);
#endif
}

static int md_fc_write_all(int fd, const char *buf, size_t len,
                           uint64_t deadline_ms, char *errbuf, size_t errbuf_len)
{
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int ret = poll(&pfd, 1, md_fc_remaining_poll_ms(deadline_ms));
        if (ret == 0) {
            snprintf(errbuf, errbuf_len, "write timeout");
            return -1;
        }
        if (ret < 0) {
            if (errno == EINTR) continue;
            snprintf(errbuf, errbuf_len, "write poll: %s", strerror(errno));
            return -1;
        }

        ssize_t n = md_fc_send(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            snprintf(errbuf, errbuf_len, "write: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            snprintf(errbuf, errbuf_len, "write: connection closed");
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static char *md_fc_read_line(int fd, uint64_t deadline_ms,
                             char *errbuf, size_t errbuf_len)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        snprintf(errbuf, errbuf_len, "read: out of memory");
        return NULL;
    }

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, md_fc_remaining_poll_ms(deadline_ms));
        if (ret == 0) {
            snprintf(errbuf, errbuf_len, "read timeout");
            free(buf);
            return NULL;
        }
        if (ret < 0) {
            if (errno == EINTR) continue;
            snprintf(errbuf, errbuf_len, "read poll: %s", strerror(errno));
            free(buf);
            return NULL;
        }

        char tmp[512];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            snprintf(errbuf, errbuf_len, "read: %s", strerror(errno));
            free(buf);
            return NULL;
        }
        if (n == 0) {
            if (len == 0) {
                snprintf(errbuf, errbuf_len, "daemon closed without response");
                free(buf);
                return NULL;
            }
            snprintf(errbuf, errbuf_len, "daemon response missing newline");
            free(buf);
            return NULL;
        }

        for (ssize_t i = 0; i < n; i++) {
            if (len + 1 >= MD_FIPS_CONTROL_MAX_RESPONSE) {
                snprintf(errbuf, errbuf_len, "response exceeds %u bytes",
                         MD_FIPS_CONTROL_MAX_RESPONSE);
                free(buf);
                return NULL;
            }
            if (len + 1 >= cap) {
                size_t next = cap * 2;
                if (next > MD_FIPS_CONTROL_MAX_RESPONSE)
                    next = MD_FIPS_CONTROL_MAX_RESPONSE;
                char *grown = realloc(buf, next);
                if (!grown) {
                    snprintf(errbuf, errbuf_len, "read: out of memory");
                    free(buf);
                    return NULL;
                }
                buf = grown;
                cap = next;
            }

            if (tmp[i] == '\n') {
                if (len > 0 && buf[len - 1] == '\r') len--;
                buf[len] = '\0';
                return buf;
            }

            buf[len++] = tmp[i];
        }
    }
}
#endif /* !_WIN32 */

MdFipsControlResult md_fips_control_request(const char *socket_override,
                                            const char *command,
                                            const cJSON *params,
                                            uint32_t timeout_ms,
                                            MdFipsControlResponse *resp)
{
    if (!resp) return MD_FIPS_CONTROL_INVALID_ARGUMENT;
    md_fc_prepare_response(resp);

    char path[MD_FIPS_CONTROL_PATH_MAX];
    if (md_fips_control_resolve_socket_path(socket_override, path, sizeof(path)) < 0) {
        return md_fc_set_result(resp, MD_FIPS_CONTROL_INVALID_ARGUMENT, NULL,
                                "could not resolve FIPS control socket path");
    }

    char *request = md_fc_build_request_json(command, params, resp);
    if (!request) return resp->result;

#ifdef _WIN32
    free(request);
    return md_fc_set_result(resp, MD_FIPS_CONTROL_UNSUPPORTED, path,
                            "FIPS control socket client is only implemented for Linux/macOS");
#else
    char errbuf[256] = {0};
    uint64_t deadline_ms = md_fc_deadline_ms(timeout_ms);
    int fd = md_fc_connect_unix(path, deadline_ms, errbuf, sizeof(errbuf));
    if (fd < 0) {
        free(request);
        return md_fc_set_result(resp, MD_FIPS_CONTROL_DAEMON_UNAVAILABLE, path,
                                "%s", errbuf[0] ? errbuf : "daemon unavailable");
    }

    MdFipsControlResult result = MD_FIPS_CONTROL_OK;
    if (md_fc_write_all(fd, request, strlen(request), deadline_ms,
                        errbuf, sizeof(errbuf)) < 0) {
        result = md_fc_set_result(resp, MD_FIPS_CONTROL_DAEMON_UNAVAILABLE, path,
                                  "%s", errbuf[0] ? errbuf : "write failed");
        close(fd);
        free(request);
        return result;
    }
    free(request);

    char *line = md_fc_read_line(fd, deadline_ms, errbuf, sizeof(errbuf));
    close(fd);
    if (!line) {
        return md_fc_set_result(resp, MD_FIPS_CONTROL_DAEMON_UNAVAILABLE, path,
                                "%s", errbuf[0] ? errbuf : "read failed");
    }

    result = md_fips_control_parse_response(line, strlen(line), resp);
    resp->socket_path = md_fc_strdup(path);
    free(line);
    return result;
#endif
}
