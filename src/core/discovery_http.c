#include "discovery_http.h"
#include "md_time.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif
#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static bool curl_ready, curl_async_dns;
static void init_curl(void) {
    curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    const curl_version_info_data *version = curl_ready ? curl_version_info(CURLVERSION_NOW) : NULL;
    curl_async_dns = version && (version->features & CURL_VERSION_ASYNCHDNS);
}
bool md_discovery_http_init(void) {
    pthread_once(&curl_once, init_curl);
    return curl_ready;
}

static bool public_v4(const unsigned char *ip) {
    return ip[0] != 0 && ip[0] != 10 && ip[0] != 127 && ip[0] < 224 &&
        !(ip[0] == 100 && (ip[1] & 192) == 64) &&
        !(ip[0] == 169 && ip[1] == 254) && !(ip[0] == 172 && (ip[1] & 240) == 16) &&
        !(ip[0] == 192 && (ip[1] == 168 || (ip[1] == 0 && (ip[2] == 0 || ip[2] == 2)))) &&
        !(ip[0] == 198 && (ip[1] == 18 || ip[1] == 19 || (ip[1] == 51 && ip[2] == 100))) &&
        !(ip[0] == 203 && ip[1] == 0 && ip[2] == 113);
}

static bool public_address(const struct sockaddr *addr, size_t len) {
    if (!addr || len < sizeof(*addr)) return false;
    if (addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in))
        return public_v4((const unsigned char *)&((const struct sockaddr_in *)addr)->sin_addr);
    if (addr->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)addr;
        const unsigned char *ip = v6->sin6_addr.s6_addr;
        static const unsigned char nat64[] = {0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
        if (v6->sin6_scope_id) return false;
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr) || !memcmp(ip, nat64, sizeof(nat64)))
            return public_v4(ip + 12);
        /* Global unicast only; exclude 6to4, Teredo, and documentation. */
        return (ip[0] & 0xe0) == 0x20 &&
            !(ip[0] == 0x3f && ip[1] == 0xff && (ip[2] & 0xf0) == 0) &&
            !(ip[0] == 0x20 && (ip[1] == 2 || (ip[1] == 1 &&
                ((ip[2] == 0 && ip[3] == 0) || (ip[2] == 0x0d && ip[3] == 0xb8)))));
    }
    return false;
}

static bool public_dns_answers(const char *host) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *addrs = NULL;
    if (getaddrinfo(host, NULL, &hints, &addrs) != 0) return false;
    bool safe = addrs != NULL;
    for (struct addrinfo *a = addrs; a && safe; a = a->ai_next)
        safe = public_address(a->ai_addr, a->ai_addrlen);
    freeaddrinfo(addrs);
    return safe;
}

/* NSS resolution may not be cancellable. The caller's monotonic deadline
 * never waits for abandoned NSS calls, and their process-wide count is
 * bounded. Each worker owns its copy of the name until it returns. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t finished;
    bool done, safe, abandoned;
    char host[256];
} DnsCheck;
static atomic_uint dns_checks;

static void free_dns_check(DnsCheck *check) {
    pthread_cond_destroy(&check->finished);
    pthread_mutex_destroy(&check->mutex);
    free(check);
    atomic_fetch_sub(&dns_checks, 1);
}

static void *check_dns(void *data) {
    DnsCheck *check = data;
    bool safe = public_dns_answers(check->host);
    pthread_mutex_lock(&check->mutex);
    bool abandoned = check->abandoned;
    check->safe = safe;
    check->done = true;
    pthread_cond_signal(&check->finished);
    pthread_mutex_unlock(&check->mutex);
    if (abandoned) free_dns_check(check);
    return NULL;
}

bool md_discovery_public_host(const char *host, uint64_t deadline_ns) {
    if (!host || !*host || strlen(host) > 253 || !md_discovery_http_init() ||
        md_monotonic_ns() >= deadline_ns) return false;
    struct sockaddr_in v4 = { .sin_family = AF_INET };
    struct sockaddr_in6 v6 = { .sin6_family = AF_INET6 };
    if (inet_pton(AF_INET, host, &v4.sin_addr) == 1)
        return public_address((struct sockaddr *)&v4, sizeof(v4));
    if (inet_pton(AF_INET6, host, &v6.sin6_addr) == 1)
        return public_address((struct sockaddr *)&v6, sizeof(v6));
    if (atomic_fetch_add(&dns_checks, 1) >= 16) {
        atomic_fetch_sub(&dns_checks, 1);
        return false;
    }
    DnsCheck *check = calloc(1, sizeof(*check));
    if (!check) { atomic_fetch_sub(&dns_checks, 1); return false; }
    strcpy(check->host, host);
    if (pthread_mutex_init(&check->mutex, NULL)) {
        free(check); atomic_fetch_sub(&dns_checks, 1); return false;
    }
    if (md_cond_init_monotonic(&check->finished)) {
        pthread_mutex_destroy(&check->mutex);
        free(check); atomic_fetch_sub(&dns_checks, 1); return false;
    }
    pthread_t worker;
    if (pthread_create(&worker, NULL, check_dns, check)) { free_dns_check(check); return false; }
    pthread_mutex_lock(&check->mutex);
    while (!check->done && !md_cond_timedwait_until(&check->finished, &check->mutex, deadline_ns)) {}
    bool done = check->done, safe = check->safe && md_monotonic_ns() < deadline_ns;
    check->abandoned = !done;
    pthread_mutex_unlock(&check->mutex);
    if (!done) { pthread_detach(worker); return false; }
    pthread_join(worker, NULL);
    free_dns_check(check);
    return safe;
}

static curl_socket_t open_public_socket(void *data, curlsocktype purpose, struct curl_sockaddr *addr) {
    if (purpose != CURLSOCKTYPE_IPCXN || !public_address(&addr->addr, addr->addrlen)) {
        *(bool *)data = true;
        return CURL_SOCKET_BAD;
    }
    return socket(addr->family, addr->socktype, addr->protocol);
}

typedef struct { char *json; size_t len; } Body;
static size_t append_body(char *data, size_t size, size_t count, void *user) {
    Body *body = user;
    const size_t limit = 256 * 1024;
    if (size && count > (limit - body->len) / size) return 0;
    size_t bytes = size * count;
    if (memchr(data, '\0', bytes)) return 0;
    char *p = realloc(body->json, body->len + bytes + 1);
    if (!p) return 0;
    body->json = p;
    memcpy(p + body->len, data, bytes);
    body->len += bytes;
    p[body->len] = '\0';
    return bytes;
}

typedef struct { bool (*cancelled)(void *); void *data; } TransferCancel;
static int transfer_progress(void *ptr, curl_off_t dl, curl_off_t downloaded,
                             curl_off_t ul, curl_off_t uploaded) {
    (void)dl; (void)downloaded; (void)ul; (void)uploaded;
    TransferCancel *cancel = ptr;
    return cancel->cancelled(cancel->data);
}

char *md_discovery_http_get_cancellable(const char *url, const char *accept, bool public_only,
    long timeout_ms, bool *unsafe, bool (*cancelled)(void *), void *data) {
    if (unsafe) *unsafe = false;
    if (cancelled && cancelled(data)) return NULL;
    TransferCancel cancel = {cancelled, data};
    /* NOSIGNAL cannot bound a synchronous resolver. Do not start a transfer
     * unless the runtime libcurl can enforce the DNS portion of its timeout. */
    CURL *curl = md_discovery_http_init() && curl_async_dns ? curl_easy_init() : NULL;
    struct curl_slist *headers = NULL;
    Body body = {0};
    bool blocked = false, ok = false;
    long status = 0;
    if (!curl) return NULL;
    if (accept && !(headers = curl_slist_append(NULL, accept))) goto done;
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) goto done; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_HTTPHEADER, headers);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, public_only || strncmp(url, "http://", 7) ? "https" : "http");
#else
    SET(CURLOPT_PROTOCOLS, public_only || strncmp(url, "http://", 7) ? CURLPROTO_HTTPS : CURLPROTO_HTTP);
#endif
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_PROXY, ""); /* Proxy-side DNS would bypass our socket policy. */
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TIMEOUT_MS, timeout_ms);
    SET(CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 3000L ? timeout_ms : 3000L);
    SET(CURLOPT_WRITEFUNCTION, append_body);
    SET(CURLOPT_WRITEDATA, &body);
    if (cancelled) {
        SET(CURLOPT_NOPROGRESS, 0L);
        SET(CURLOPT_XFERINFOFUNCTION, transfer_progress);
        SET(CURLOPT_XFERINFODATA, &cancel);
    }
    if (public_only) {
        /* Runs on the address curl will actually connect to, not a preflight DNS answer. */
        SET(CURLOPT_OPENSOCKETFUNCTION, open_public_socket);
        SET(CURLOPT_OPENSOCKETDATA, &blocked);
    }
    ok = curl_easy_perform(curl) == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200;
#undef SET
 done:
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (unsafe) *unsafe = !ok && blocked;
    if (ok && (!cancelled || !cancelled(data))) return body.json;
    free(body.json);
    return NULL;
}

char *md_discovery_http_get(const char *url, const char *accept, bool public_only,
                            long timeout_ms, bool *unsafe) {
    return md_discovery_http_get_cancellable(url, accept, public_only, timeout_ms, unsafe, NULL, NULL);
}
