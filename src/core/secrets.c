/*
 * metadesk — secrets.c
 * 1Password Connect REST API integration.
 *
 * HTTP is handled by libcurl (already a required dependency of the
 * core library), which gives us TLS, chunked transfer encoding, and
 * real status codes.
 *
 * Security measures:
 *   - Token stored in mlock'd memory (no swap)
 *   - All secret buffers zeroed on free
 *   - No secrets written to disk, logs, or env vars
 *   - Plaintext HTTP is warned about outside loopback
 *
 * The op:// reference format is parsed as: op://vault/item/field
 * Vault lookup: GET /v1/vaults → find vault by name → get vault ID
 * Item lookup:  GET /v1/vaults/{id}/items?filter=title eq "item" → get item ID
 * Field fetch:  GET /v1/vaults/{id}/items/{id} → find field by label → return value
 */
#include "secrets.h"
#include "platform.h"

#include <cjson/cJSON.h>
#include <curl/curl.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "log.h"
#define MD_LOG_TAG "secrets"

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

static int parse_port(const char *p, const char *end, uint16_t *port_out) {
    if (!p || !end || !port_out || p >= end)
        return -1;

    unsigned long port = 0;
    for (const char *q = p; q < end; q++) {
        if (*q < '0' || *q > '9')
            return -1;
        port = port * 10 + (unsigned long)(*q - '0');
        if (port > 65535)
            return -1;
    }
    if (port == 0)
        return -1;

    *port_out = (uint16_t)port;
    return 0;
}

/* Parse "http(s)://host:port" into host and port.
 * Returns 0 on success. host_out must be freed by caller. */
static int parse_url(const char *url, char **host_out, uint16_t *port_out) {
    if (!url || !host_out || !port_out)
        return -1;

    *host_out = NULL;
    *port_out = 8080;  /* 1Password Connect default */

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0)
        p += 8;
    else if (strncmp(p, "http://", 7) == 0)
        p += 7;

    const char *slash = strchr(p, '/');
    const char *end = slash ? slash : p + strlen(p);
    if (p >= end)
        return -1;

    if (*p == '[') {
        /* Bracketed IPv6 literal, e.g. http://[::1]:8080 */
        const char *close = memchr(p + 1, ']', (size_t)(end - p - 1));
        if (!close)
            return -1;
        *host_out = strndup(p + 1, (size_t)(close - p - 1));
        if (!*host_out)
            return -1;
        if (close + 1 < end) {
            if (close[1] != ':' || parse_port(close + 2, end, port_out) < 0) {
                free(*host_out);
                *host_out = NULL;
                return -1;
            }
        }
        return 0;
    }

    /* Accept the common unbracketed ::1 loopback shorthand too. */
    if ((size_t)(end - p) >= 3 && strncmp(p, "::1", 3) == 0 &&
        (p + 3 == end || p[3] == ':')) {
        *host_out = strdup("::1");
        if (!*host_out)
            return -1;
        if (p + 3 < end && parse_port(p + 4, end, port_out) < 0) {
            free(*host_out);
            *host_out = NULL;
            return -1;
        }
        return 0;
    }

    const char *colon = memchr(p, ':', (size_t)(end - p));
    if (colon) {
        *host_out = strndup(p, (size_t)(colon - p));
        if (!*host_out)
            return -1;
        if (parse_port(colon + 1, end, port_out) < 0) {
            free(*host_out);
            *host_out = NULL;
            return -1;
        }
    } else {
        *host_out = strndup(p, (size_t)(end - p));
    }

    return *host_out ? 0 : -1;
}

static int is_loopback_host(const char *host) {
    return host &&
           (strcmp(host, "localhost") == 0 ||
            strcmp(host, "127.0.0.1") == 0 ||
            strcmp(host, "::1") == 0);
}

static int is_url_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static char *url_percent_encode(const char *in) {
    static const char hex[] = "0123456789ABCDEF";

    if (!in)
        return NULL;

    size_t len = strlen(in);
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        size_t add = is_url_unreserved((unsigned char)in[i]) ? 1 : 3;
        if (out_len > ((size_t)-1) - add)
            return NULL;
        out_len += add;
    }

    char *out = malloc(out_len + 1);
    if (!out)
        return NULL;

    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (is_url_unreserved(c)) {
            *p++ = (char)c;
        } else {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 0x0f];
        }
    }
    *p = '\0';
    return out;
}

static void secure_zero_cjson_string_values(cJSON *item) {
    for (cJSON *cur = item; cur; cur = cur->next) {
        if (cJSON_IsString(cur) && cur->valuestring)
            md_secure_zero(cur->valuestring, strlen(cur->valuestring));
        if (cur->child)
            secure_zero_cjson_string_values(cur->child);
    }
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

/* ── HTTP client (libcurl) ───────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} CurlBuf;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb,
                            void *userdata) {
    CurlBuf *buf = userdata;
    size_t n = size * nmemb;

    if (buf->len + n + 1 > MD_SECRETS_MAX_RESPONSE)
        return 0;  /* overflow — aborts the transfer */
    if (buf->len + n + 1 > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : 16384;
        while (new_cap < buf->len + n + 1)
            new_cap *= 2;
        char *nb = realloc(buf->data, new_cap);
        if (!nb) return 0;
        buf->data = nb;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

/*
 * Perform an HTTP GET request and return the response body.
 * Caller must free the returned buffer. Returns NULL on error.
 * body_len is set to the response length on success.
 * status_out (optional) receives the HTTP status code, or 0 for a
 * transport-level failure (DNS/connect/TLS), so callers can
 * distinguish auth failures (401/403) from missing items (404).
 */
static char *http_get(MdSecrets *s, const char *path, size_t *body_len,
                      long *status_out) {
    if (!s || !path || !body_len)
        return NULL;

    *body_len = 0;
    if (status_out) *status_out = 0;

    char url[1024];
    int url_len = snprintf(url, sizeof(url), "%s%s", s->connect_url, path);
    if (url_len <= 0 || (size_t)url_len >= sizeof(url))
        return NULL;

    char auth[MD_SECRETS_TOKEN_MAX + 32];
    int auth_len = snprintf(auth, sizeof(auth), "Authorization: Bearer %.*s",
                            (int)s->token_len, s->token);
    if (auth_len <= 0 || (size_t)auth_len >= sizeof(auth))
        return NULL;

    CURL *c = curl_easy_init();
    if (!c) {
        md_secure_zero(auth, sizeof(auth));
        return NULL;
    }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth);
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    md_secure_zero(auth, sizeof(auth));
    if (!hdrs) {
        curl_easy_cleanup(c);
        return NULL;
    }

    CurlBuf buf = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)MD_SECRETS_TIMEOUT_MS);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(c);

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status_out) *status_out = status;

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || status < 200 || status >= 300) {
        if (buf.data) {
            md_secure_zero(buf.data, buf.len);
            free(buf.data);
        }
        return NULL;
    }

    if (!buf.data)
        return NULL;

    *body_len = buf.len;
    return buf.data;
}

/* ── Vault and item resolution ───────────────────────────────── */

/* Find vault ID by name. Returns malloced string or NULL. */
static char *find_vault_id(MdSecrets *s, const char *vault_name) {
    size_t body_len;
    long status;
    char *body = http_get(s, "/v1/vaults", &body_len, &status);
    if (!body) {
        if (status == 401 || status == 403)
            MD_LOG_I("Connect auth failed (HTTP %ld) — check token", status);
        else if (status == 0)
            MD_LOG_I("Connect server unreachable at %s", s->connect_url);
        return NULL;
    }

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
    char *enc_vault_id = url_percent_encode(vault_id);
    char *enc_item_name = url_percent_encode(item_name);
    if (!enc_vault_id || !enc_item_name) {
        free(enc_vault_id);
        free(enc_item_name);
        return NULL;
    }

    char path[512];
    int path_len = snprintf(path, sizeof(path),
             "/v1/vaults/%s/items?filter=title%%20eq%%20%%22%s%%22",
             enc_vault_id, enc_item_name);
    free(enc_vault_id);
    free(enc_item_name);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path))
        return NULL;

    size_t body_len;
    long status;
    char *body = http_get(s, path, &body_len, &status);
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
    char *enc_vault_id = url_percent_encode(vault_id);
    char *enc_item_id = url_percent_encode(item_id);
    if (!enc_vault_id || !enc_item_id) {
        free(enc_vault_id);
        free(enc_item_id);
        return NULL;
    }

    char path[512];
    int path_len = snprintf(path, sizeof(path),
             "/v1/vaults/%s/items/%s", enc_vault_id, enc_item_id);
    free(enc_vault_id);
    free(enc_item_id);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path))
        return NULL;

    size_t body_len;
    long status;
    char *body = http_get(s, path, &body_len, &status);
    if (!body) return NULL;

    cJSON *root = cJSON_Parse(body);

    /* Securely zero the raw response — it contained secret field values. */
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

    secure_zero_cjson_string_values(root);
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

    if (!is_loopback_host(s->host) &&
        strncmp(connect_url, "http://", 7) == 0) {
        MD_LOG_W("WARNING — 1Password Connect URL '%s' is non-loopback HTTP; bearer token and secrets may traverse the network in plaintext", connect_url);
    }

    /* Copy token into mlock'd region */
    memcpy(s->token, token, token_len);
    s->token_len = token_len;

    /* Lock the token in memory to prevent swapping. */
    if (md_mem_lock(s->token, sizeof(s->token)) < 0) {
        MD_LOG_I("memory lock failed: %s", strerror(errno));
        md_secure_zero(s->token, sizeof(s->token));
        free(s->host);
        free(s->connect_url);
        free(s);
        return NULL;
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
        MD_LOG_I("vault '%s' not found", vault_name);
        goto cleanup;
    }

    /* Step 2: Find item ID */
    char *item_id = find_item_id(s, vault_id, item_name);
    if (!item_id) {
        MD_LOG_I("item '%s' not found in vault '%s'", item_name, vault_name);
        free(vault_id);
        goto cleanup;
    }

    /* Step 3: Get field value */
    char *value = get_field_value(s, vault_id, item_id, field_name);
    free(vault_id);
    free(item_id);

    if (!value) {
        MD_LOG_I("field '%s' not found in item '%s'", field_name, item_name);
        goto cleanup;
    }

    /* Copy value to caller's buffer. Refuse silent truncation. */
    size_t value_len = strlen(value);
    if (value_len > buf_len) {
        MD_LOG_I("output buffer too small for field '%s'", field_name);
        result = -1;
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
    long status;
    char *body = http_get(s, "/v1/activity", &body_len, &status);
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
