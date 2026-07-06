/*
 * metadesk — ipc_win32.c
 * Windows named pipe IPC backend.
 *
 * Endpoints are created as named pipes under \\.\pipe\metadesk-<name>.
 * The server creates a pipe instance and waits for a client to connect.
 *
 * Named pipes provide byte-stream mode matching the Unix socket API.
 */
#include "ipc.h"

#ifdef _WIN32  /* entire file is Windows-only */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Path construction ───────────────────────────────────────── */

#define MD_IPC_PIPE_PREFIX "\\\\.\\pipe\\metadesk-"
#define MD_IPC_PATH_MAX    256

static bool ipc_build_path(const char *name, char *path, size_t path_len) {
    int ret = snprintf(path, path_len, "%s%s", MD_IPC_PIPE_PREFIX, name);
    return ret >= 0 && (size_t)ret < path_len;
}

/* ── Pipe security ───────────────────────────────────────────── */

typedef struct MdIpcSecurity {
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    PACL dacl;
    PTOKEN_USER token_user;
} MdIpcSecurity;

static void ipc_security_destroy(MdIpcSecurity *sec) {
    if (!sec) return;
    free(sec->dacl);
    free(sec->token_user);
    memset(sec, 0, sizeof(*sec));
}

static bool ipc_security_init(MdIpcSecurity *sec) {
    if (!sec) return false;
    memset(sec, 0, sizeof(*sec));

    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    DWORD token_len = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &token_len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_len == 0) {
        CloseHandle(token);
        return false;
    }

    sec->token_user = (PTOKEN_USER)malloc(token_len);
    if (!sec->token_user) {
        CloseHandle(token);
        return false;
    }

    if (!GetTokenInformation(token, TokenUser, sec->token_user, token_len, &token_len)) {
        CloseHandle(token);
        ipc_security_destroy(sec);
        return false;
    }
    CloseHandle(token);

    DWORD sid_len = GetLengthSid(sec->token_user->User.Sid);
    DWORD acl_size = (DWORD)(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_len);
    sec->dacl = (PACL)malloc(acl_size);
    if (!sec->dacl) {
        ipc_security_destroy(sec);
        return false;
    }

    if (!InitializeAcl(sec->dacl, acl_size, ACL_REVISION)) {
        ipc_security_destroy(sec);
        return false;
    }

    if (!AddAccessAllowedAce(sec->dacl, ACL_REVISION, GENERIC_ALL,
                             sec->token_user->User.Sid)) {
        ipc_security_destroy(sec);
        return false;
    }

    if (!InitializeSecurityDescriptor(&sec->sd, SECURITY_DESCRIPTOR_REVISION)) {
        ipc_security_destroy(sec);
        return false;
    }

    if (!SetSecurityDescriptorDacl(&sec->sd, TRUE, sec->dacl, FALSE)) {
        ipc_security_destroy(sec);
        return false;
    }

    sec->sa.nLength = sizeof(sec->sa);
    sec->sa.lpSecurityDescriptor = &sec->sd;
    sec->sa.bInheritHandle = FALSE;
    return true;
}

static HANDLE ipc_create_named_pipe(const char *path, DWORD open_mode) {
    MdIpcSecurity security;
    if (!ipc_security_init(&security))
        return INVALID_HANDLE_VALUE;

    HANDLE pipe = CreateNamedPipeA(
        path,
        open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        MD_IPC_MAX_MSG,    /* output buffer size */
        MD_IPC_MAX_MSG,    /* input buffer size  */
        0,                 /* default timeout    */
        &security.sa);     /* current-user DACL  */

    ipc_security_destroy(&security);
    return pipe;
}

/* ── Structures ──────────────────────────────────────────────── */

struct MdIpcServer {
    HANDLE pipe;
    char   path[MD_IPC_PATH_MAX];
};

struct MdIpcConn {
    HANDLE pipe;
    bool   connected;
};

/* ── Server API ──────────────────────────────────────────────── */

MdIpcServer *md_ipc_listen(const char *name) {
    if (!md_ipc_name_is_valid(name)) return NULL;

    MdIpcServer *srv = calloc(1, sizeof(MdIpcServer));
    if (!srv) return NULL;
    srv->pipe = INVALID_HANDLE_VALUE;

    if (!ipc_build_path(name, srv->path, sizeof(srv->path))) {
        free(srv);
        return NULL;
    }

    /* Create a named pipe instance.
     * PIPE_TYPE_BYTE | PIPE_READMODE_BYTE: byte-stream mode
     * PIPE_WAIT: blocking operations
     * Buffer sizes: 64 KB each direction */
    srv->pipe = ipc_create_named_pipe(srv->path, PIPE_ACCESS_DUPLEX);

    if (srv->pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ipc: CreateNamedPipe failed for '%s': %lu\n",
                srv->path, GetLastError());
        free(srv);
        return NULL;
    }

    return srv;
}

MdIpcConn *md_ipc_accept(MdIpcServer *srv, uint32_t timeout_ms) {
    if (!srv || srv->pipe == INVALID_HANDLE_VALUE)
        return NULL;

    /* For timeout support, use overlapped I/O */
    if (timeout_ms > 0) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) return NULL;

        BOOL connected = ConnectNamedPipe(srv->pipe, &ov);
        DWORD err = GetLastError();

        if (!connected) {
            if (err == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(ov.hEvent, timeout_ms);
                if (waitResult != WAIT_OBJECT_0) {
                    CancelIo(srv->pipe);
                    CloseHandle(ov.hEvent);
                    return NULL; /* timeout */
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(ov.hEvent);
                return NULL;
            }
        }
        CloseHandle(ov.hEvent);
    } else {
        /* Blocking: ConnectNamedPipe waits indefinitely */
        BOOL connected = ConnectNamedPipe(srv->pipe, NULL);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED)
            return NULL;
    }

    /* Transfer pipe handle to connection object.
     * Create a new pipe instance for the server to accept again. */
    MdIpcConn *conn = calloc(1, sizeof(MdIpcConn));
    if (!conn) {
        DisconnectNamedPipe(srv->pipe);
        return NULL;
    }

    conn->pipe = srv->pipe;
    conn->connected = true;

    /* Create a new pipe instance for the next accept */
    srv->pipe = ipc_create_named_pipe(
        srv->path,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED);

    return conn;
}

const char *md_ipc_server_path(const MdIpcServer *srv) {
    return srv ? srv->path : NULL;
}

void md_ipc_server_destroy(MdIpcServer *srv) {
    if (!srv) return;
    if (srv->pipe != INVALID_HANDLE_VALUE)
        CloseHandle(srv->pipe);
    free(srv);
}

/* ── Client API ──────────────────────────────────────────────── */

MdIpcConn *md_ipc_connect(const char *name, uint32_t timeout_ms) {
    if (!md_ipc_name_is_valid(name)) return NULL;

    char path[MD_IPC_PATH_MAX];
    if (!ipc_build_path(name, path, sizeof(path))) return NULL;

    /* Wait for the pipe to become available */
    DWORD wait_ms = (timeout_ms > 0) ? timeout_ms : NMPWAIT_WAIT_FOREVER;
    if (!WaitNamedPipeA(path, wait_ms)) {
        fprintf(stderr, "ipc: pipe '%s' not available: %lu\n", path, GetLastError());
        return NULL;
    }

    /* Open the pipe */
    HANDLE pipe = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        0,              /* no sharing */
        NULL,           /* default security */
        OPEN_EXISTING,
        0,              /* default attributes */
        NULL);

    if (pipe == INVALID_HANDLE_VALUE)
        return NULL;

    /* Set pipe to byte mode (should already be, but be explicit) */
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, NULL, NULL);

    MdIpcConn *conn = calloc(1, sizeof(MdIpcConn));
    if (!conn) {
        CloseHandle(pipe);
        return NULL;
    }

    conn->pipe = pipe;
    conn->connected = true;
    return conn;
}

/* ── Connection I/O ──────────────────────────────────────────── */

int md_ipc_send(MdIpcConn *conn, const void *data, size_t len) {
    if (!conn || !conn->connected || !data) return -1;

    const uint8_t *p = (const uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0) {
        DWORD written = 0;
        DWORD to_write = (DWORD)(remaining > MAXDWORD ? MAXDWORD : remaining);
        if (!WriteFile(conn->pipe, p, to_write, &written, NULL)) {
            conn->connected = false;
            return -1;
        }
        p += written;
        remaining -= written;
    }

    return 0;
}

int md_ipc_recv(MdIpcConn *conn, void *buf, size_t buf_len,
                uint32_t timeout_ms) {
    if (!conn || !conn->connected || !buf || buf_len == 0) return -1;

    /* Timeout handling via overlapped read */
    if (timeout_ms > 0) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) return -1;

        DWORD bytesRead = 0;
        DWORD to_read = (DWORD)(buf_len > MAXDWORD ? MAXDWORD : buf_len);
        BOOL success = ReadFile(conn->pipe, buf, to_read, &bytesRead, &ov);

        if (!success) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD waitResult = WaitForSingleObject(ov.hEvent, timeout_ms);
                if (waitResult != WAIT_OBJECT_0) {
                    CancelIo(conn->pipe);
                    CloseHandle(ov.hEvent);
                    return -1; /* timeout */
                }
                GetOverlappedResult(conn->pipe, &ov, &bytesRead, FALSE);
            } else if (err == ERROR_BROKEN_PIPE) {
                CloseHandle(ov.hEvent);
                conn->connected = false;
                return 0; /* peer disconnected */
            } else {
                CloseHandle(ov.hEvent);
                conn->connected = false;
                return -1;
            }
        }

        CloseHandle(ov.hEvent);
        return (int)bytesRead;
    }

    /* Blocking read */
    DWORD bytesRead = 0;
    DWORD to_read = (DWORD)(buf_len > MAXDWORD ? MAXDWORD : buf_len);
    if (!ReadFile(conn->pipe, buf, to_read, &bytesRead, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE) {
            conn->connected = false;
            return 0;
        }
        conn->connected = false;
        return -1;
    }

    return (int)bytesRead;
}

bool md_ipc_is_connected(const MdIpcConn *conn) {
    return conn ? conn->connected : false;
}

void md_ipc_close(MdIpcConn *conn) {
    if (!conn) return;
    if (conn->pipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(conn->pipe);
        DisconnectNamedPipe(conn->pipe);
        CloseHandle(conn->pipe);
    }
    free(conn);
}

#endif /* _WIN32 */
