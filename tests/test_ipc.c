/*
 * metadesk — tests/test_ipc.c
 * Unit tests for the Unix domain socket IPC layer.
 *
 * Tests listen/accept/connect, send/recv, and lifecycle management.
 * Runs on POSIX systems only (Linux, macOS).
 */
#include "ipc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define PASS(name) printf("  PASS  %s\n", name)

/* Unique IPC name per test to avoid collisions */
static int g_name_counter = 0;
static void make_name(char *buf, size_t len)
{
    snprintf(buf, len, "test-ipc-%d-%d", (int)getpid(), g_name_counter++);
}

/* ── Test: server create and destroy ─────────────────────────── */

static void test_server_lifecycle(void)
{
    char name[MD_IPC_NAME_MAX];
    make_name(name, sizeof(name));

    MdIpcServer *srv = md_ipc_listen(name);
    assert(srv != NULL);

    const char *path = md_ipc_server_path(srv);
    assert(path != NULL);
    assert(strlen(path) > 0);

    md_ipc_server_destroy(srv);

    PASS("server lifecycle");
}

/* ── Test: connect without server should fail ────────────────── */

static void test_connect_no_server(void)
{
    MdIpcConn *conn = md_ipc_connect("nonexistent-ipc-test", 100);
    assert(conn == NULL);

    PASS("connect no server");
}

/* ── Background accept thread ────────────────────────────────── */

typedef struct {
    MdIpcServer *srv;
    MdIpcConn   *accepted;
} AcceptArgs;

static void *accept_thread(void *arg)
{
    AcceptArgs *a = arg;
    a->accepted = md_ipc_accept(a->srv, 3000); /* 3s timeout */
    return NULL;
}

/* ── Test: connect + accept round-trip ───────────────────────── */

static void test_connect_accept(void)
{
    char name[MD_IPC_NAME_MAX];
    make_name(name, sizeof(name));

    MdIpcServer *srv = md_ipc_listen(name);
    assert(srv != NULL);

    AcceptArgs args = { .srv = srv, .accepted = NULL };
    pthread_t tid;
    pthread_create(&tid, NULL, accept_thread, &args);

    /* Small delay so accept is waiting before we connect */
    usleep(50000);

    MdIpcConn *client = md_ipc_connect(name, 2000);
    assert(client != NULL);

    pthread_join(tid, NULL);
    assert(args.accepted != NULL);

    /* Both ends should report connected */
    assert(md_ipc_is_connected(client) == true);
    assert(md_ipc_is_connected(args.accepted) == true);

    md_ipc_close(client);
    md_ipc_close(args.accepted);
    md_ipc_server_destroy(srv);

    PASS("connect + accept");
}

/* ── Test: send/recv round-trip ──────────────────────────────── */

static void test_send_recv(void)
{
    char name[MD_IPC_NAME_MAX];
    make_name(name, sizeof(name));

    MdIpcServer *srv = md_ipc_listen(name);
    assert(srv != NULL);

    AcceptArgs args = { .srv = srv, .accepted = NULL };
    pthread_t tid;
    pthread_create(&tid, NULL, accept_thread, &args);
    usleep(50000);

    MdIpcConn *client = md_ipc_connect(name, 2000);
    assert(client != NULL);
    pthread_join(tid, NULL);
    MdIpcConn *server_conn = args.accepted;
    assert(server_conn != NULL);

    /* Client → Server */
    const char *msg1 = "hello from client";
    assert(md_ipc_send(client, msg1, strlen(msg1)) == 0);

    char buf[256];
    int n = md_ipc_recv(server_conn, buf, sizeof(buf), 2000);
    assert(n == (int)strlen(msg1));
    assert(memcmp(buf, msg1, (size_t)n) == 0);

    /* Server → Client */
    const char *msg2 = "hello from server";
    assert(md_ipc_send(server_conn, msg2, strlen(msg2)) == 0);

    n = md_ipc_recv(client, buf, sizeof(buf), 2000);
    assert(n == (int)strlen(msg2));
    assert(memcmp(buf, msg2, (size_t)n) == 0);

    md_ipc_close(client);
    md_ipc_close(server_conn);
    md_ipc_server_destroy(srv);

    PASS("send/recv round-trip");
}

/* ── Test: larger message ────────────────────────────────────── */

static void test_send_large(void)
{
    char name[MD_IPC_NAME_MAX];
    make_name(name, sizeof(name));

    MdIpcServer *srv = md_ipc_listen(name);
    assert(srv != NULL);

    AcceptArgs args = { .srv = srv, .accepted = NULL };
    pthread_t tid;
    pthread_create(&tid, NULL, accept_thread, &args);
    usleep(50000);

    MdIpcConn *client = md_ipc_connect(name, 2000);
    assert(client != NULL);
    pthread_join(tid, NULL);
    MdIpcConn *server_conn = args.accepted;
    assert(server_conn != NULL);

    /* Send 4KB of data */
    size_t sz = 4096;
    uint8_t *data = malloc(sz);
    assert(data != NULL);
    for (size_t i = 0; i < sz; i++)
        data[i] = (uint8_t)(i & 0xFF);

    assert(md_ipc_send(client, data, sz) == 0);

    uint8_t *recv_buf = malloc(sz);
    assert(recv_buf != NULL);
    int n = md_ipc_recv(server_conn, recv_buf, sz, 2000);
    assert(n == (int)sz);
    assert(memcmp(data, recv_buf, sz) == 0);

    free(data);
    free(recv_buf);
    md_ipc_close(client);
    md_ipc_close(server_conn);
    md_ipc_server_destroy(srv);

    PASS("send large message (4KB)");
}

/* ── Test: recv timeout ──────────────────────────────────────── */

static void test_recv_timeout(void)
{
    char name[MD_IPC_NAME_MAX];
    make_name(name, sizeof(name));

    MdIpcServer *srv = md_ipc_listen(name);
    assert(srv != NULL);

    AcceptArgs args = { .srv = srv, .accepted = NULL };
    pthread_t tid;
    pthread_create(&tid, NULL, accept_thread, &args);
    usleep(50000);

    MdIpcConn *client = md_ipc_connect(name, 2000);
    assert(client != NULL);
    pthread_join(tid, NULL);
    MdIpcConn *server_conn = args.accepted;
    assert(server_conn != NULL);

    /* Recv with short timeout — no data sent, should timeout */
    char buf[64];
    int n = md_ipc_recv(server_conn, buf, sizeof(buf), 100);
    /* -1 on timeout/error, or 0 on disconnect */
    assert(n <= 0);

    md_ipc_close(client);
    md_ipc_close(server_conn);
    md_ipc_server_destroy(srv);

    PASS("recv timeout");
}

/* ── Test: close detection ───────────────────────────────────── */

static void test_close_detection(void)
{
    char name[MD_IPC_NAME_MAX];
    make_name(name, sizeof(name));

    MdIpcServer *srv = md_ipc_listen(name);
    assert(srv != NULL);

    AcceptArgs args = { .srv = srv, .accepted = NULL };
    pthread_t tid;
    pthread_create(&tid, NULL, accept_thread, &args);
    usleep(50000);

    MdIpcConn *client = md_ipc_connect(name, 2000);
    assert(client != NULL);
    pthread_join(tid, NULL);
    MdIpcConn *server_conn = args.accepted;
    assert(server_conn != NULL);

    /* Close client end */
    md_ipc_close(client);

    /* Server recv should detect disconnect (returns 0) */
    char buf[64];
    int n = md_ipc_recv(server_conn, buf, sizeof(buf), 1000);
    assert(n == 0); /* peer disconnected */

    md_ipc_close(server_conn);
    md_ipc_server_destroy(srv);

    PASS("close detection");
}

/* ── Test: accept timeout ────────────────────────────────────── */

static void test_accept_timeout(void)
{
    char name[MD_IPC_NAME_MAX];
    make_name(name, sizeof(name));

    MdIpcServer *srv = md_ipc_listen(name);
    assert(srv != NULL);

    /* Accept with short timeout — no client connecting */
    MdIpcConn *conn = md_ipc_accept(srv, 100);
    assert(conn == NULL); /* should timeout */

    md_ipc_server_destroy(srv);

    PASS("accept timeout");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(void)
{
    printf("test_ipc: Unix domain socket IPC tests\n");

    test_server_lifecycle();
    test_connect_no_server();
    test_connect_accept();
    test_send_recv();
    test_send_large();
    test_recv_timeout();
    test_close_detection();
    test_accept_timeout();

    printf("\nAll IPC tests passed.\n");
    return 0;
}
