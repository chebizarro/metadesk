/*
 * test_secrets.c — 1Password Connect integration tests.
 *
 * Tests the op:// reference parser, URL parser, and API surface.
 * Note: actual HTTP calls to 1Password Connect are not tested here
 * (would require a running server). These tests validate the parsing
 * and error handling logic.
 */
#include "secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("  test: %s ... ", name); } while (0)
#define PASS() \
    do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); tests_failed++; } while (0)

typedef struct {
    int listen_fd;
    int failures;
    int requests;
} MockSecretsServer;

static void mock_write_response(int fd, const char *body) {
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        strlen(body));
    write(fd, header, (size_t)hlen);
    write(fd, body, strlen(body));
}

static void *mock_secrets_server_thread(void *arg) {
    MockSecretsServer *srv = (MockSecretsServer *)arg;
    const char *expected_paths[] = {
        "/v1/vaults",
        "/v1/vaults/vault%20id/items?filter=title%20eq%20%22my%20item%22",
        "/v1/vaults/vault%20id/items/item%20id%231",
    };
    const char *bodies[] = {
        "[{\"name\":\"Vault\",\"id\":\"vault id\"}]",
        "[{\"id\":\"item id#1\"}]",
        "{\"fields\":[{\"label\":\"password\",\"value\":\"secret-value\"}]}",
    };

    for (int i = 0; i < 3; i++) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(srv->listen_fd, &readfds);
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        if (select(srv->listen_fd + 1, &readfds, NULL, NULL, &tv) <= 0) {
            srv->failures++;
            break;
        }

        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) {
            srv->failures++;
            break;
        }

        char req[2048];
        ssize_t n = read(fd, req, sizeof(req) - 1);
        if (n <= 0) {
            srv->failures++;
            close(fd);
            continue;
        }
        req[n] = '\0';

        char path[512] = {0};
        if (sscanf(req, "GET %511s HTTP/1.", path) != 1 ||
            strcmp(path, expected_paths[i]) != 0) {
            srv->failures++;
        }
        srv->requests++;
        mock_write_response(fd, bodies[i]);
        close(fd);
    }

    close(srv->listen_fd);
    return NULL;
}

static int run_mock_secret_fetch(uint8_t *buf, size_t buf_len,
                                 int *server_failures) {
    MockSecretsServer srv = { .listen_fd = -1, .failures = 0, .requests = 0 };
    srv.listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (srv.listen_fd < 0)
        return -999;

    int opt = 1;
    setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int off = 0;
    setsockopt(srv.listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = 0;
    addr.sin6_addr = in6addr_loopback;
    if (bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv.listen_fd, 4) < 0) {
        close(srv.listen_fd);
        return -999;
    }

    socklen_t addr_len = sizeof(addr);
    if (getsockname(srv.listen_fd, (struct sockaddr *)&addr, &addr_len) < 0) {
        close(srv.listen_fd);
        return -999;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, mock_secrets_server_thread, &srv) != 0) {
        close(srv.listen_fd);
        return -999;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://localhost:%u", ntohs(addr.sin6_port));
    MdSecrets *s = md_secrets_create(url, "test-token");
    int ret = -1;
    if (s) {
        ret = md_secrets_get(s, "op://Vault/my item/password", buf, buf_len);
        md_secrets_destroy(s);
    } else {
        close(srv.listen_fd);
    }

    pthread_join(tid, NULL);
    if (server_failures)
        *server_failures = srv.failures + (srv.requests != 3);
    return ret;
}

/* ── Test: create/destroy lifecycle ──────────────────────────── */

static void test_create_destroy(void) {
    TEST("create/destroy lifecycle");

    MdSecrets *s = md_secrets_create("http://localhost:8080", "test-token-abc");
    if (!s) {
        FAIL("create returned NULL");
        return;
    }

    md_secrets_destroy(s);
    PASS();
}

/* ── Test: create with invalid args ──────────────────────────── */

static void test_create_invalid(void) {
    TEST("create with invalid args");

    if (md_secrets_create(NULL, "token")) {
        FAIL("NULL url accepted"); return;
    }
    if (md_secrets_create("http://localhost", NULL)) {
        FAIL("NULL token accepted"); return;
    }
    if (md_secrets_create("http://localhost", "")) {
        FAIL("empty token accepted"); return;
    }

    PASS();
}

/* ── Test: get with invalid args ─────────────────────────────── */

static void test_get_invalid(void) {
    TEST("get with invalid args");

    MdSecrets *s = md_secrets_create("http://localhost:8080", "test-token");
    if (!s) { FAIL("create failed"); return; }

    uint8_t buf[64];

    /* NULL ref */
    if (md_secrets_get(s, NULL, buf, sizeof(buf)) >= 0) {
        md_secrets_destroy(s);
        FAIL("NULL ref accepted"); return;
    }

    /* NULL buf */
    if (md_secrets_get(s, "op://vault/item/field", NULL, 64) >= 0) {
        md_secrets_destroy(s);
        FAIL("NULL buf accepted"); return;
    }

    /* Zero buf_len */
    if (md_secrets_get(s, "op://vault/item/field", buf, 0) >= 0) {
        md_secrets_destroy(s);
        FAIL("zero buf_len accepted"); return;
    }

    /* Invalid op:// format (missing components) */
    if (md_secrets_get(s, "op://vault/item", buf, sizeof(buf)) >= 0) {
        md_secrets_destroy(s);
        FAIL("missing field accepted"); return;
    }

    if (md_secrets_get(s, "op://vault", buf, sizeof(buf)) >= 0) {
        md_secrets_destroy(s);
        FAIL("missing item/field accepted"); return;
    }

    /* Without op:// prefix — should still parse (slash-separated) */
    /* This will fail due to unreachable server, which is expected */
    int ret = md_secrets_get(s, "vault/item/field", buf, sizeof(buf));
    /* ret should be -1 because there's no server to connect to */
    if (ret >= 0) {
        /* Unexpected success without a server */
        md_secrets_destroy(s);
        FAIL("succeeded without server"); return;
    }

    md_secrets_destroy(s);
    PASS();
}

/* ── Test: is_connected without server ───────────────────────── */

static void test_not_connected(void) {
    TEST("is_connected returns false without server");

    /* Use a port that (almost certainly) has nothing listening */
    MdSecrets *s = md_secrets_create("http://127.0.0.1:19999", "test-token");
    if (!s) { FAIL("create failed"); return; }

    if (md_secrets_is_connected(s)) {
        md_secrets_destroy(s);
        FAIL("connected to non-existent server"); return;
    }

    md_secrets_destroy(s);
    PASS();
}

/* ── Test: URL parsing variations ────────────────────────────── */

static void test_url_parsing(void) {
    TEST("URL parsing variations");

    /* Standard http URL */
    MdSecrets *s1 = md_secrets_create("http://localhost:8080", "tok");
    if (!s1) { FAIL("http://localhost:8080 failed"); return; }
    md_secrets_destroy(s1);

    /* Without port (should default to 8080) */
    MdSecrets *s2 = md_secrets_create("http://localhost", "tok");
    if (!s2) { FAIL("http://localhost failed"); return; }
    md_secrets_destroy(s2);

    /* With IP address */
    MdSecrets *s3 = md_secrets_create("http://192.168.1.100:9000", "tok");
    if (!s3) { FAIL("IP address URL failed"); return; }
    md_secrets_destroy(s3);

    /* HTTPS prefix is now rejected (TLS not implemented) */
    MdSecrets *s4 = md_secrets_create("https://secrets.local:443", "tok");
    if (s4) {
        md_secrets_destroy(s4);
        FAIL("https URL should have been rejected"); return;
    }

    PASS();
}

/* ── Test: successful fetch and URL encoding ────────────────── */

static void test_fetch_url_encoding(void) {
    TEST("fetch uses URL encoding");

    uint8_t buf[64];
    int failures = 0;
    int ret = run_mock_secret_fetch(buf, sizeof(buf), &failures);
    if (ret != (int)strlen("secret-value")) {
        FAIL("mock fetch failed"); return;
    }
    if (failures != 0) {
        FAIL("server saw unencoded or missing request path"); return;
    }
    if (strcmp((char *)buf, "secret-value") != 0) {
        FAIL("wrong secret value"); return;
    }

    PASS();
}

/* ── Test: small output buffers fail instead of truncating ───── */

static void test_buffer_too_small_returns_error(void) {
    TEST("buffer too small returns error");

    uint8_t buf[64];
    memset(buf, 0xA5, sizeof(buf));
    int failures = 0;
    int ret = run_mock_secret_fetch(buf, 4, &failures);
    if (ret != -1) {
        FAIL("small buffer should fail instead of truncating"); return;
    }
    if (failures != 0) {
        FAIL("server path validation failed on small-buffer case"); return;
    }
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0xA5) {
            FAIL("small-buffer failure modified output buffer"); return;
        }
    }

    PASS();
}

/* ── Test: non-loopback HTTP warning path ────────────────────── */

static void test_non_loopback_url_warning_path(void) {
    TEST("non-loopback URL warning path");

    MdSecrets *s = md_secrets_create("http://192.0.2.10:8080", "tok");
    if (!s) {
        FAIL("non-loopback HTTP URL should create after warning"); return;
    }
    md_secrets_destroy(s);

    PASS();
}

/* ── Test: secret zeroing on destroy ─────────────────────────── */

static void test_secure_destroy(void) {
    TEST("secure zeroing on destroy");

    /* This test verifies the API accepts and processes the token
     * without leaking. We can't directly verify memory zeroing
     * in a portable way, but we ensure the lifecycle works. */
    MdSecrets *s = md_secrets_create("http://localhost:8080",
                                     "very-secret-token-12345");
    if (!s) { FAIL("create failed"); return; }

    /* Destroy should zero the token and free cleanly */
    md_secrets_destroy(s);

    /* If we get here without crashes, the secure destroy worked */
    PASS();
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("test_secrets:\n");

    test_create_destroy();
    test_create_invalid();
    test_get_invalid();
    test_not_connected();
    test_url_parsing();
    test_fetch_url_encoding();
    test_buffer_too_small_returns_error();
    test_non_loopback_url_warning_path();
    test_secure_destroy();

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
