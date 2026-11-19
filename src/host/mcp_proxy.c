/*
 * metadesk — mcp_proxy.c
 * `metadesk-host mcp-proxy`: stdio <-> Streamable-HTTP MCP shim that signs
 * every HTTP request with NIP-98 (md-ah9.14, container-runtime plan §7.7).
 *
 * No mainstream MCP client signs NIP-98. Configure any stdio MCP client (for
 * example Claude Code) to launch this command; it forwards each
 * newline-delimited JSON-RPC message from stdin as one POST /mcp and writes
 * the response message(s) to stdout. After the first successful POST it also
 * holds the server's GET /mcp SSE stream and relays each notification as one
 * stdout line.
 *
 * Every request carries a fresh kind-27235 event with exactly the u (the
 * URL given on the command line, byte for byte), method and, for POST,
 * payload = SHA-256(body) tags. Cost model: one MdSigner signature per POST
 * plus one per SSE (re)connect. With a local key, Grotto/NIP-55L or NIP-5F
 * that is a local operation; with NIP-46 each one is a relay round trip.
 *
 * Messages are forwarded one at a time, in stdin order. The host serializes
 * MCP dispatch anyway; this also keeps notifications ordered before later
 * requests (for example notifications/initialized before tools/list).
 */
#include "host_mcp.h"
#include "accounts.h"
#include "mcp_server.h"
#include "nostr.h"
#include "signer.h"

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <nostr/nip19/nip19.h>
#include <nostr/nip98/nip98.h>

/* A response may batch several messages; the UI tree is the largest. */
#define PROXY_MAX_RESPONSE (8 * MD_MCP_MAX_MESSAGE_SIZE)
#define PROXY_SSE_BACKOFF_MIN_MS 500
#define PROXY_SSE_BACKOFF_MAX_MS 30000

typedef struct {
    const char *url;
    MdSigner *signer;
    bool sse;

    pthread_mutex_t out_mu;     /* one writer per stdout line */
    pthread_mutex_t mu;         /* guards stop/sse_started/sse_multi */
    pthread_cond_t cv;
    bool stop;
    bool sse_started;
    CURLM *sse_multi;           /* non-NULL while a GET is in flight */
    pthread_t sse_thread;
} Proxy;

static int usage(void) {
    fprintf(stderr,
        "Usage: metadesk-host mcp-proxy [SIGNER] [--no-sse] URL\n"
        "  URL: the host's MCP endpoint, e.g. http://127.0.0.1:7710/mcp. It is\n"
        "       signed verbatim as NIP-98 'u', so it must be one the host accepts\n"
        "       (a loopback form of its port or one of its --mcp-public-url values).\n"
        "SIGNER (default: the saved client role, see 'metadesk-host account'):\n"
        "  --key-file PATH        local key: 64-hex or nsec1, regular file, no group/other access\n"
        "  --socket-signer [PATH] NIP-5F Unix socket signer\n"
        "  --dbus-signer          NIP-55L D-Bus signer (e.g. Grotto)\n"
        "  --bunker URI           NIP-46 remote signer (a relay round trip per request)\n"
        "  --no-sse               do not hold the GET /mcp notification stream\n"
        "Reads newline-delimited JSON-RPC on stdin, writes responses and notifications\n"
        "to stdout, diagnostics to stderr. One signature per HTTP request.\n");
    return 2;
}

/* ── Output ─────────────────────────────────────────────────── */

static void emit_line(Proxy *p, const char *json, size_t len) {
    pthread_mutex_lock(&p->out_mu);
    if (fwrite(json, 1, len, stdout) != len || fputc('\n', stdout) == EOF ||
        fflush(stdout) == EOF)
        fprintf(stderr, "mcp-proxy: stdout write failed: %s\n", strerror(errno));
    pthread_mutex_unlock(&p->out_mu);
}

/* Re-serialize so one message is always exactly one stdout line. */
static void emit_json(Proxy *p, const cJSON *msg) {
    char *line = cJSON_PrintUnformatted(msg);
    if (line) emit_line(p, line, strlen(line));
    free(line);
}

static bool emit_payload(Proxy *p, const char *text, size_t len) {
    cJSON *msg = cJSON_ParseWithLength(text, len);
    if (!msg) {
        fprintf(stderr, "mcp-proxy: host sent a non-JSON message; dropped\n");
        return false;
    }
    if (cJSON_IsArray(msg)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, msg) emit_json(p, item);
    } else {
        emit_json(p, msg);
    }
    cJSON_Delete(msg);
    return true;
}

/* Synthesize a JSON-RPC error for each request (id-bearing message) that the
 * host never answered, so the stdio client does not wait forever. */
static void emit_errors(Proxy *p, const cJSON *sent, const char *why) {
    if (cJSON_IsArray(sent)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, sent) emit_errors(p, item, why);
        return;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(sent, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(sent, "method");
    if (!cJSON_IsObject(sent) || !id || cJSON_IsNull(id) || !cJSON_IsString(method)) {
        fprintf(stderr, "mcp-proxy: notification not delivered: %s\n", why);
        return;
    }
    cJSON *err = cJSON_CreateObject();
    cJSON *detail = cJSON_AddObjectToObject(err, "error");
    cJSON_AddStringToObject(err, "jsonrpc", "2.0");
    cJSON_AddItemToObject(err, "id", cJSON_Duplicate(id, true));
    cJSON_AddNumberToObject(detail, "code", -32001);
    char message[256];
    snprintf(message, sizeof(message), "metadesk mcp-proxy: %s", why);
    cJSON_AddStringToObject(detail, "message", message);
    emit_json(p, err);
    cJSON_Delete(err);
}

/* ── NIP-98 signing ─────────────────────────────────────────── */

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i] = digits[in[i] >> 4];
        out[2*i + 1] = digits[in[i] & 15];
    }
    out[2*n] = 0;
}

/* Returns a malloc'd "Authorization: Nostr <base64 event>" header line. */
static char *sign_authorization(Proxy *p, const char *method,
                                const char *body, size_t body_len) {
    char payload[65];
    if (body) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char *)body, body_len, hash);
        hex_encode(hash, sizeof(hash), payload);
    }
    NostrEvent *ev = nostr_nip98_create_auth_event(p->url, method, body ? payload : NULL);
    char *pk = NULL, *signed_json = NULL, *line = NULL;
    if (!ev || md_signer_get_pubkey(p->signer, &pk) != MD_SIGNER_OK || !pk) goto done;
    nostr_event_set_pubkey(ev, pk);
    /* The host rejects a reused event id for 2x the time window. Two
     * identical bodies signed in the same second would otherwise hash to
     * the same id, so content carries a random nonce (NIP-98 content is
     * free-form; validators ignore it). */
    unsigned char nonce[16];
    char nonce_hex[33];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) goto done;
    hex_encode(nonce, sizeof(nonce), nonce_hex);
    nostr_event_set_content(ev, nonce_hex);
    if (md_nostr_sign_event_json(p->signer, ev, &signed_json) != 0 || !signed_json) goto done;
    size_t json_len = strlen(signed_json);
    size_t b64_len = 4 * ((json_len + 2) / 3);
    const char prefix[] = "Authorization: Nostr ";
    line = malloc(sizeof(prefix) + b64_len + 1);
    if (!line) goto done;
    memcpy(line, prefix, sizeof(prefix) - 1);
    EVP_EncodeBlock((unsigned char *)line + sizeof(prefix) - 1,
                    (const unsigned char *)signed_json, (int)json_len);
done:
    if (!line) fprintf(stderr, "mcp-proxy: NIP-98 signing failed\n");
    free(signed_json);
    free(pk);
    if (ev) nostr_event_free(ev);
    return line;
}

/* ── HTTP plumbing ──────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} Buffer;

/* Geometric growth: the SSE parser appends one byte at a time. */
static size_t buffer_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n > PROXY_MAX_RESPONSE) { b->overflow = true; return 0; }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->data, cap);
        if (!grown) return 0;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

/* Incremental text/event-stream parser: emits each event's data. Data lines
 * are joined with a space (JSON-insignificant) to keep one line per message. */
typedef struct {
    Proxy *proxy;
    Buffer line;
    Buffer data;
    bool has_data;
} SseParser;

static void sse_dispatch(SseParser *s) {
    if (s->has_data && s->data.len) emit_payload(s->proxy, s->data.data, s->data.len);
    s->data.len = 0;
    s->has_data = false;
}

static void sse_line(SseParser *s, char *line, size_t len) {
    if (len && line[len - 1] == '\r') line[--len] = 0;
    if (!len) { sse_dispatch(s); return; }
    if (len < 5 || strncmp(line, "data:", 5)) return; /* event:, id:, comments */
    const char *value = line + 5;
    if (*value == ' ') value++;
    if (s->has_data) buffer_write(" ", 1, 1, &s->data);
    buffer_write((char *)value, 1, strlen(value), &s->data);
    s->has_data = true;
}

static size_t sse_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    SseParser *s = userdata;
    size_t n = size * nmemb;
    for (size_t i = 0; i < n; i++) {
        if (ptr[i] != '\n') {
            if (buffer_write(ptr + i, 1, 1, &s->line) != 1) return 0;
            continue;
        }
        sse_line(s, s->line.data ? s->line.data : (char *)"", s->line.len);
        s->line.len = 0;
        if (s->line.data) s->line.data[0] = 0;
        if (s->data.overflow) return 0;
    }
    return n;
}

/* Header callback: announce the stream once its 200 headers have arrived. */
static size_t sse_header(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    if ((n == 2 && ptr[0] == '\r' && ptr[1] == '\n') || (n == 1 && ptr[0] == '\n')) {
        long status = 0;
        curl_easy_getinfo(userdata, CURLINFO_RESPONSE_CODE, &status);
        if (status == 200) fprintf(stderr, "mcp-proxy: notification stream open\n");
    }
    return n;
}

static void sse_parser_free(SseParser *s) {
    free(s->line.data);
    free(s->data.data);
}

static CURL *new_request(Proxy *p, const char *auth, struct curl_slist **headers,
                         bool event_stream) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    *headers = curl_slist_append(NULL, auth);
    *headers = curl_slist_append(*headers, event_stream
        ? "Accept: text/event-stream" : "Accept: application/json, text/event-stream");
    curl_easy_setopt(curl, CURLOPT_URL, p->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    /* A redirect would change the URL the signature is bound to. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    return curl;
}

/* ── SSE stream ─────────────────────────────────────────────── */

static bool proxy_stopping(Proxy *p) {
    pthread_mutex_lock(&p->mu);
    bool stop = p->stop;
    pthread_mutex_unlock(&p->mu);
    return stop;
}

/* One GET /mcp until the host closes it, it fails, or the proxy stops.
 * curl_multi_poll() blocks on the socket; stop wakes it (no busy loop). */
static long sse_once(Proxy *p) {
    char *auth = sign_authorization(p, "GET", NULL, 0);
    if (!auth) return -1;
    struct curl_slist *headers = NULL;
    CURL *curl = new_request(p, auth, &headers, true);
    OPENSSL_cleanse(auth, strlen(auth));
    free(auth);
    CURLM *multi = curl ? curl_multi_init() : NULL;
    SseParser parser = { .proxy = p };
    long status = -1;
    if (!multi) goto done;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, sse_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, curl);
    curl_multi_add_handle(multi, curl);
    pthread_mutex_lock(&p->mu);
    bool stop = p->stop;
    if (!stop) p->sse_multi = multi;
    pthread_mutex_unlock(&p->mu);
    int running = !stop;
    while (running) {
        if (curl_multi_perform(multi, &running) != CURLM_OK) break;
        if (!running || proxy_stopping(p)) break;
        if (curl_multi_poll(multi, NULL, 0, 60000, NULL) != CURLM_OK) break;
    }
    pthread_mutex_lock(&p->mu);
    p->sse_multi = NULL;
    pthread_mutex_unlock(&p->mu);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_multi_remove_handle(multi, curl);
done:
    if (multi) curl_multi_cleanup(multi);
    if (curl) curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    sse_parser_free(&parser);
    return status;
}

/* Reconnect with capped exponential backoff; each attempt signs afresh.
 * A 401 (e.g. a revoked grant) is retried too, so a re-grant recovers. */
static void *sse_main(void *arg) {
    Proxy *p = arg;
    int backoff = PROXY_SSE_BACKOFF_MIN_MS;
    long last = 0;
    while (!proxy_stopping(p)) {
        long status = sse_once(p);
        if (proxy_stopping(p)) break;
        if (status != last)
            fprintf(stderr, "mcp-proxy: notification stream ended (HTTP %ld); reconnecting\n",
                    status);
        if (status == 200) backoff = PROXY_SSE_BACKOFF_MIN_MS;
        last = status;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += backoff / 1000;
        until.tv_nsec += (long)(backoff % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        pthread_mutex_lock(&p->mu);
        while (!p->stop && pthread_cond_timedwait(&p->cv, &p->mu, &until) != ETIMEDOUT) {}
        pthread_mutex_unlock(&p->mu);
        if (backoff < PROXY_SSE_BACKOFF_MAX_MS) backoff *= 2;
    }
    return NULL;
}

static void start_sse(Proxy *p) {
    if (!p->sse || p->sse_started) return;
    p->sse_started = pthread_create(&p->sse_thread, NULL, sse_main, p) == 0;
    if (!p->sse_started) fprintf(stderr, "mcp-proxy: cannot start notification stream\n");
}

/* ── POST ───────────────────────────────────────────────────── */

static void forward(Proxy *p, const char *body, size_t len) {
    cJSON *sent = cJSON_ParseWithLength(body, len);
    if (!sent) {
        static const char parse_error[] =
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}";
        emit_line(p, parse_error, sizeof(parse_error) - 1);
        return;
    }
    if (len > MD_MCP_MAX_MESSAGE_SIZE) {
        emit_errors(p, sent, "message exceeds the 1 MiB MCP limit");
        cJSON_Delete(sent);
        return;
    }
    char *auth = sign_authorization(p, "POST", body, len);
    if (!auth) {
        emit_errors(p, sent, "NIP-98 signing failed");
        cJSON_Delete(sent);
        return;
    }
    struct curl_slist *headers = NULL;
    CURL *curl = new_request(p, auth, &headers, false);
    OPENSSL_cleanse(auth, strlen(auth));
    free(auth);
    Buffer response = {0};
    if (!curl) {
        emit_errors(p, sent, "HTTP client unavailable");
        goto done;
    }
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    char *type = NULL;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
    char why[160];
    if (rc != CURLE_OK) {
        snprintf(why, sizeof(why), "%s", response.overflow ? "response too large"
                                                          : curl_easy_strerror(rc));
        emit_errors(p, sent, why);
    } else if (status == 202 || (status == 200 && !response.len)) {
        start_sse(p); /* notification accepted */
    } else if (status == 200) {
        start_sse(p);
        if (type && strncasecmp(type, "text/event-stream", 17) == 0) {
            SseParser parser = { .proxy = p };
            sse_write(response.data, 1, response.len, &parser);
            sse_line(&parser, (char *)"", 0);
            sse_parser_free(&parser);
        } else if (!emit_payload(p, response.data, response.len)) {
            emit_errors(p, sent, "host answered with invalid JSON");
        }
    } else {
        snprintf(why, sizeof(why), "host answered HTTP %ld%s", status,
                 status == 401 ? " (signer not authorized by the host's NIP-98 allowlist?)" : "");
        fprintf(stderr, "mcp-proxy: %s\n", why);
        emit_errors(p, sent, why);
    }
done:
    if (curl) curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(response.data);
    cJSON_Delete(sent);
}

/* ── Signer selection ───────────────────────────────────────── */

static MdSigner *key_file_signer(const char *path) {
    /* Symlinks are followed (Kubernetes secret mounts are symlinks); the
     * opened file itself must be regular and private. */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "mcp-proxy: cannot open key file: %s\n", strerror(errno));
        return NULL;
    }
    struct stat st;
    char buf[160] = {0};
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && !(st.st_mode & 077) &&
        st.st_size > 0 && st.st_size < (off_t)sizeof(buf))
        n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    MdSigner *signer = NULL;
    char hex[65] = {0};
    if (n > 0) {
        buf[n] = 0;
        char *s = buf, *end = buf + n;
        while (*s && isspace((unsigned char)*s)) s++;
        while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
        uint8_t sk[32];
        if (strlen(s) == 64 && strspn(s, "0123456789abcdefABCDEF") == 64) {
            for (int i = 0; i < 64; i++) hex[i] = (char)tolower((unsigned char)s[i]);
        } else if (strncmp(s, "nsec1", 5) == 0 && nostr_nip19_decode_nsec(s, sk) == 0) {
            hex_encode(sk, sizeof(sk), hex);
        }
        OPENSSL_cleanse(sk, sizeof(sk));
        if (hex[0]) signer = md_signer_create_direct(hex);
    }
    OPENSSL_cleanse(buf, sizeof(buf));
    OPENSSL_cleanse(hex, sizeof(hex));
    if (!signer)
        fprintf(stderr, "mcp-proxy: key file must be a regular file without group/other "
                        "access holding one 64-hex or nsec1 key\n");
    return signer;
}

static bool valid_url(const char *url) {
    size_t len = strlen(url);
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) &&
        len > 12 && strcmp(url + len - 4, "/mcp") == 0 && !strpbrk(url, " \r\n\t?#");
}

int md_host_mcp_proxy_cli(int argc, char **argv) {
    const char *key_file = NULL, *socket_path = NULL, *bunker = NULL, *url = NULL;
    bool socket_signer = false, dbus_signer = false, sse = true;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--key-file") && i + 1 < argc) key_file = argv[++i];
        else if (!strcmp(argv[i], "--bunker") && i + 1 < argc) bunker = argv[++i];
        else if (!strcmp(argv[i], "--dbus-signer")) dbus_signer = true;
        else if (!strcmp(argv[i], "--socket-signer")) {
            socket_signer = true;
            if (i + 1 < argc && argv[i + 1][0] == '/') socket_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--no-sse")) sse = false;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] != '-' && !url) url = argv[i];
        else return usage();
    }
    int signer_flags = !!key_file + socket_signer + dbus_signer + !!bunker;
    if (!url || signer_flags > 1 || !valid_url(url)) return usage();

    signal(SIGPIPE, SIG_IGN);
    MdAccountStartup account = {0};
    MdSigner *signer = NULL;
    if (key_file) signer = key_file_signer(key_file);
    else if (socket_signer) signer = md_signer_create_nip5f(socket_path);
    else if (dbus_signer) signer = md_signer_create_nip55l();
    else if (bunker) signer = md_signer_create_nip46(bunker, 30000);
    else {
        int rc = md_account_startup_open(MD_ACCOUNT_CLIENT, &account);
        if (rc || !account.binding) {
            fprintf(stderr, "mcp-proxy: no signer: client account restore: %s "
                            "(pass --key-file/--socket-signer/--dbus-signer/--bunker)\n",
                    md_accounts_error_string(rc ? rc : MD_ACCOUNTS_NOT_FOUND));
            md_account_startup_clear(&account);
            return 1;
        }
        signer = md_signer_ref(md_account_binding_signer(account.binding));
    }
    char *pk = NULL;
    if (!signer || md_signer_get_pubkey(signer, &pk) != MD_SIGNER_OK || !pk) {
        fprintf(stderr, "mcp-proxy: requested signer backend not available\n");
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        return 1;
    }
    MdSignerType type = md_signer_get_type(signer);
    fprintf(stderr, "mcp-proxy: signing as %.*s... (%s) for %s\n", 12, pk,
            md_signer_type_name(type), url);
    if (type == MD_SIGNER_NIP46)
        fprintf(stderr, "mcp-proxy: NIP-46 signs over a relay: every request waits a round trip\n");
    free(pk);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        md_signer_destroy(signer);
        md_account_startup_clear(&account);
        return 1;
    }
    Proxy p = { .url = url, .signer = signer, .sse = sse };
    pthread_mutex_init(&p.out_mu, NULL);
    pthread_mutex_init(&p.mu, NULL);
    pthread_cond_init(&p.cv, NULL);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        size_t start = 0;
        while (start < (size_t)n && isspace((unsigned char)line[start])) start++;
        if (start < (size_t)n) forward(&p, line + start, (size_t)n - start);
    }
    free(line);

    pthread_mutex_lock(&p.mu);
    p.stop = true;
    if (p.sse_multi) curl_multi_wakeup(p.sse_multi);
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.mu);
    if (p.sse_started) pthread_join(p.sse_thread, NULL);

    pthread_cond_destroy(&p.cv);
    pthread_mutex_destroy(&p.mu);
    pthread_mutex_destroy(&p.out_mu);
    curl_global_cleanup();
    md_signer_destroy(signer);
    md_account_startup_clear(&account);
    return 0;
}
