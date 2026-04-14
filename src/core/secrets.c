/*
 * metadesk — secrets.c
 * 1Password Connect REST API integration.
 *
 * Uses POSIX sockets for HTTP requests to the local 1Password Connect
 * server. No external HTTP library dependency — the Connect server is
 * always on localhost or LAN, so a simple HTTP/1.1 client suffices.
 *
 * Security measures:
 *   - Token stored in mlock'd memory (no swap)
 *   - All secret buffers zeroed on free
 *   - No secrets written to disk, logs, or env vars
 *   - Connection over localhost (no TLS needed for Phase 1)
 *
 * The op:// reference format is parsed as: op://vault/item/field
 * Vault lookup: GET /v1/vaults → find vault by name → get vault ID
 * Item lookup:  GET /v1/vaults/{id}/items?filter=title eq "item" → get item ID
 * Field fetch:  GET /v1/vaults/{id}/items/{id} → find field by label → return value
 */
#include "secrets.h"
#include "platform.h"

#include <cjson/cJSON.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close(s) closesocket(s)
#  define sock_read(s,b,n) recv((s),(b),(int)(n),0)
#  define sock_write(s,b,n) send((s),(b),(int)(n),0)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <unistd.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close(s) close(s)
#  define sock_read(s,b,n) read((s),(b),(n))
#  define sock_write(s,b,n) write((s),(b),(n))
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ── Constants ───────────────────────────────────────────────── */

#define MD_SECRETS_MAX_RESPONSE  (256 * 1024)  /* 256 KB max response */
#define MD_SECRETS_TOKEN_MAX     512            /* max token length     */
#define MD_SECRETS_TIMEOUT_MS    5000

/* ── Structure ───────────────────────────────────────────────── */

struct MdSecrets {
    char   *connect_url;      /* e.g. "http://localhost:8080"  */
    char   *host;             /* parsed hostname               */
    uint16_t port;            /* parsed port                   */
    char    token[MD_SECRETS_TOKEN_MAX]; /* mlock'd bearer token */
    size_t  token_len;
};

/* ── URL parsing ─────────────────────────────────────────────── */

/* Parse "http://host:port" into host and port.
 * Returns 0 on success. host_out must be freed by caller. */
static int parse_url(const char *url, char **host_out, uint16_t *port_out) {
    if (!url || !host_out || !port_out)
        return -1;

    /* Skip "http://" */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0)
        p += 8;  /* Note: TLS not implemented, just skip prefix */

    /* Find port separator or end */
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    const char *end = slash ? slash : p + strlen(p);

    if (colon && colon < end) {
        /* Host:port */
        size_t host_len = (size_t)(colon - p);
        *host_out = strndup(p, host_len);
        *port_out = (uint16_t)atoi(colon + 1);
    } else {
        /* Host only, default port */
        size_t host_len = (size_t)(end - p);
        *host_out = strndup(p, host_len);
        *port_out = 8080;  /* 1Password Connect default */
    }

    return *host_out ? 0 : -1;
}

/* ── op:// reference parsing ─────────────────────────────────── */

/* Parse "op://vault/item/field" into components.
 * Returns 0 on success. Outputs must be freed by caller. */
static int parse_op_ref(const char *ref,
                        char **vault_out, char **item_out, char **field_out) {
    if (!ref || !vault_out || !item_out || !field_out)
        return -1;

    *vault_out = NULL;
    *item_out  = NULL;
    *field_out = NULL;

    /* Skip "op://" prefix */
    const char *p = ref;
    if (strncmp(p, "op://", 5) == 0)
        p += 5;

    /* Split by '/' */
    const char *slash1 = strchr(p, '/');
    if (!slash1) return -1;

    const char *slash2 = strchr(slash1 + 1, '/');
    if (!slash2) return -1;

    *vault_out = strndup(p, (size_t)(slash1 - p));
    *item_out  = strndup(slash1 + 1, (size_t)(slash2 - slash1 - 1));
    *field_out = strdup(slash2 + 1);

    if (!*vault_out || !*item_out || !*field_out) {
        free(*vault_out); free(*item_out); free(*field_out);
        *vault_out = *item_out = *field_out = NULL;
        return -1;
    }

    return 0;
}

/* ── HTTP client ─────────────────────────────────────────────── */

/*
 * Perform an HTTP GET request and return the response body.
 * Caller must free the returned buffer.
 * Returns NULL on error. body_len is set to the response length.
 */
static char *http_get(MdSecrets *s, const char *path, size_t *body_len) {
    if (!s || !path || !body_len)
        return NULL;

    *body_len = 0;

    /* Resolve host */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", s->port);

    if (getaddrinfo(s->host, port_str, &hints, &res) != 0)
        return NULL;

    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) {
        freeaddrinfo(res);
        return NULL;
    }

    /* Set socket timeout */
#ifdef _WIN32
    DWORD timeout_ms = MD_SECRETS_TIMEOUT_MS;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = MD_SECRETS_TIMEOUT_MS / 1000;
    tv.tv_usec = (MD_SECRETS_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(fd, res->ai_addr, (int)res->ai_addrlen) < 0) {
        sock_close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);

    /* Build HTTP request */
    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Authorization: Bearer %.*s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, s->host, s->port,
        (int)s->token_len, s->token);

    if (req_len <= 0 || (size_t)req_len >= sizeof(request)) {
        close(fd);
        return NULL;
    }

    /* Send request */
    if (sock_write(fd, request, (size_t)req_len) != req_len) {
        sock_close(fd);
        return NULL;
    }

    /* Zero the request buffer (contained the token) */
    md_secure_zero(request, sizeof(request));

    /* Read response */
    char *response = calloc(1, MD_SECRETS_MAX_RESPONSE + 1);
    if (!response) {
        close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < MD_SECRETS_MAX_RESPONSE) {
        int n = (int)sock_read(fd, response + total,
                               MD_SECRETS_MAX_RESPONSE - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    sock_close(fd);

    if (total == 0) {
        free(response);
        return NULL;
    }

    /* Parse HTTP response — find body after \r\n\r\n */
    char *body = strstr(response, "\r\n\r\n");
    if (!body) {
        free(response);
        return NULL;
    }
    body += 4;

    /* Check status code (first line: "HTTP/1.1 200 OK\r\n") */
    if (strncmp(response, "HTTP/1.", 7) != 0) {
        free(response);
        return NULL;
    }
    int status = atoi(response + 9);  /* skip "HTTP/1.x " */
    if (status < 200 || status >= 300) {
        free(response);
        return NULL;
    }

    /* Copy body to new buffer */
    size_t body_size = total - (size_t)(body - response);
    char *result = calloc(1, body_size + 1);
    if (!result) {
        free(response);
        return NULL;
    }
    memcpy(result, body, body_size);
    *body_len = body_size;

    free(response);
    return result;
}

/* ── Vault and item resolution ───────────────────────────────── */

/* Find vault ID by name. Returns malloced string or NULL. */
static char *find_vault_id(MdSecrets *s, const char *vault_name) {
    size_t body_len;
    char *body = http_get(s, "/v1/vaults", &body_len);
    if (!body) return NULL;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return NULL;

    char *vault_id = NULL;
    if (cJSON_IsArray(root)) {
        int count = cJSON_GetArraySize(root);
        for (int i = 0; i < count; i++) {
            cJSON *vault = cJSON_GetArrayItem(root, i);
            cJSON *name = cJSON_GetObjectItemCaseSensitive(vault, "name");
            cJSON *id   = cJSON_GetObjectItemCaseSensitive(vault, "id");
            if (cJSON_IsString(name) && cJSON_IsString(id) &&
                strcmp(name->valuestring, vault_name) == 0) {
                vault_id = strdup(id->valuestring);
                break;
            }
        }
    }

    cJSON_Delete(root);
    return vault_id;
}

/* Find item ID by title within a vault. Returns malloced string or NULL. */
static char *find_item_id(MdSecrets *s, const char *vault_id,
                          const char *item_name) {
    /* URL-encode the filter query */
    char path[512];
    snprintf(path, sizeof(path),
             "/v1/vaults/%s/items?filter=title%%20eq%%20\"%s\"",
             vault_id, item_name);

    size_t body_len;
    char *body = http_get(s, path, &body_len);
    if (!body) return NULL;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return NULL;

    char *item_id = NULL;
    if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
        cJSON *item = cJSON_GetArrayItem(root, 0);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(id))
            item_id = strdup(id->valuestring);
    }

    cJSON_Delete(root);
    return item_id;
}

/* Get a field value from a full item. Returns malloced string or NULL. */
static char *get_field_value(MdSecrets *s, const char *vault_id,
                             const char *item_id, const char *field_name) {
    char path[512];
    snprintf(path, sizeof(path),
             "/v1/vaults/%s/items/%s", vault_id, item_id);

    size_t body_len;
    char *body = http_get(s, path, &body_len);
    if (!body) return NULL;

    cJSON *root = cJSON_Parse(body);

    /* Securely zero the body — it contained the secret field values */
    md_secure_zero(body, body_len);
    free(body);

    if (!root) return NULL;

    char *value = NULL;

    /* Search in "fields" array */
    cJSON *fields = cJSON_GetObjectItemCaseSensitive(root, "fields");
    if (cJSON_IsArray(fields)) {
        int count = cJSON_GetArraySize(fields);
        for (int i = 0; i < count; i++) {
            cJSON *field = cJSON_GetArrayItem(fields, i);
            cJSON *label = cJSON_GetObjectItemCaseSensitive(field, "label");
            cJSON *val   = cJSON_GetObjectItemCaseSensitive(field, "value");
            if (cJSON_IsString(label) && cJSON_IsString(val) &&
                strcmp(label->valuestring, field_name) == 0) {
                value = strdup(val->valuestring);
                break;
            }
        }
    }

    /* Also check "sections[].fields[]" for section-scoped fields */
    if (!value) {
        cJSON *sections = cJSON_GetObjectItemCaseSensitive(root, "sections");
        if (cJSON_IsArray(sections)) {
            int sec_count = cJSON_GetArraySize(sections);
            for (int si = 0; si < sec_count && !value; si++) {
                cJSON *sec = cJSON_GetArrayItem(sections, si);
                cJSON *sec_fields = cJSON_GetObjectItemCaseSensitive(sec, "fields");
                if (!cJSON_IsArray(sec_fields)) continue;

                int f_count = cJSON_GetArraySize(sec_fields);
                for (int fi = 0; fi < f_count; fi++) {
                    cJSON *field = cJSON_GetArrayItem(sec_fields, fi);
                    cJSON *label = cJSON_GetObjectItemCaseSensitive(field, "label");
                    cJSON *val   = cJSON_GetObjectItemCaseSensitive(field, "value");
                    if (cJSON_IsString(label) && cJSON_IsString(val) &&
                        strcmp(label->valuestring, field_name) == 0) {
                        value = strdup(val->valuestring);
                        break;
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    return value;
}

/* ── Public API ──────────────────────────────────────────────── */

MdSecrets *md_secrets_create(const char *connect_url, const char *token) {
    if (!connect_url || !token)
        return NULL;

    size_t token_len = strlen(token);
    if (token_len == 0 || token_len >= MD_SECRETS_TOKEN_MAX)
        return NULL;

    MdSecrets *s = calloc(1, sizeof(MdSecrets));
    if (!s) return NULL;

    s->connect_url = strdup(connect_url);
    if (!s->connect_url) {
        free(s);
        return NULL;
    }

    /* Parse URL into host:port */
    if (parse_url(connect_url, &s->host, &s->port) < 0) {
        free(s->connect_url);
        free(s);
        return NULL;
    }

    /* Copy token into mlock'd region */
    memcpy(s->token, token, token_len);
    s->token_len = token_len;

    /* Lock the token in memory to prevent swapping (best-effort) */
    if (md_mem_lock(s->token, sizeof(s->token)) < 0) {
        fprintf(stderr, "secrets: WARNING — memory lock failed (secrets may swap)\n");
    }

    return s;
}

int md_secrets_get(MdSecrets *s, const char *item_ref,
                   uint8_t *buf, size_t buf_len) {
    if (!s || !item_ref || !buf || buf_len == 0)
        return -1;

    /* Parse op://vault/item/field */
    char *vault_name = NULL, *item_name = NULL, *field_name = NULL;
    if (parse_op_ref(item_ref, &vault_name, &item_name, &field_name) < 0)
        return -1;

    int result = -1;

    /* Step 1: Find vault ID */
    char *vault_id = find_vault_id(s, vault_name);
    if (!vault_id) {
        fprintf(stderr, "secrets: vault '%s' not found\n", vault_name);
        goto cleanup;
    }

    /* Step 2: Find item ID */
    char *item_id = find_item_id(s, vault_id, item_name);
    if (!item_id) {
        fprintf(stderr, "secrets: item '%s' not found in vault '%s'\n",
                item_name, vault_name);
        free(vault_id);
        goto cleanup;
    }

    /* Step 3: Get field value */
    char *value = get_field_value(s, vault_id, item_id, field_name);
    free(vault_id);
    free(item_id);

    if (!value) {
        fprintf(stderr, "secrets: field '%s' not found in item '%s'\n",
                field_name, item_name);
        goto cleanup;
    }

    /* Copy value to caller's buffer */
    size_t value_len = strlen(value);
    if (value_len > buf_len) {
        /* Truncate — caller's buffer is too small */
        memcpy(buf, value, buf_len);
        result = (int)buf_len;
    } else {
        memcpy(buf, value, value_len);
        if (value_len < buf_len)
            buf[value_len] = '\0';
        result = (int)value_len;
    }

    /* Securely zero and free the value */
    md_secure_zero(value, value_len);
    free(value);

cleanup:
    free(vault_name);
    free(item_name);
    free(field_name);
    return result;
}

bool md_secrets_is_connected(MdSecrets *s) {
    if (!s) return false;

    /* GET /v1/activity — lightweight health check */
    size_t body_len;
    char *body = http_get(s, "/v1/activity", &body_len);
    if (!body) return false;
    free(body);
    return true;
}

void md_secrets_destroy(MdSecrets *s) {
    if (!s) return;

    /* Securely zero sensitive data */
    md_secure_zero(s->token, sizeof(s->token));
    md_mem_unlock(s->token, sizeof(s->token));

    free(s->connect_url);
    free(s->host);

    md_secure_zero(s, sizeof(MdSecrets));
    free(s);
}
