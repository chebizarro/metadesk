/*
 * metadesk — ipc.h
 * Cross-platform local IPC (inter-process communication).
 * See spec §2.2 — daemons communicate via local IPC.
 *
 * Backends:
 *   Linux/macOS:  Unix domain sockets  (ipc_unix.c)
 *   Windows:      Named pipes           (ipc_win32.c)
 *
 * Provides a simple bidirectional byte-stream channel between
 * local processes. The API mirrors a simplified socket pattern:
 * listen/accept (server) and connect (client), then send/recv.
 *
 * Paths:
 *   POSIX:    /tmp/metadesk-<name>.sock  (or XDG_RUNTIME_DIR)
 *   Windows:  \\.\pipe\metadesk-<name>
 */
#ifndef MD_IPC_H
#define MD_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum IPC endpoint name length */
#define MD_IPC_NAME_MAX 64

/* Maximum data per single send/recv */
#define MD_IPC_MAX_MSG  (64 * 1024)

/* Opaque types */
typedef struct MdIpcServer MdIpcServer;
typedef struct MdIpcConn   MdIpcConn;

/* ── Server API (listen / accept) ────────────────────────────── */

/*
 * Create an IPC server listening on the given name.
 *
 * name: Logical name (e.g. "host", "session"). The implementation
 *       maps this to a platform-appropriate path:
 *       - POSIX: $XDG_RUNTIME_DIR/metadesk-<name>.sock or /tmp/
 *       - Windows: \\.\pipe\metadesk-<name>
 *
 * Returns NULL on failure (permission, path conflict, etc.).
 */
MdIpcServer *md_ipc_listen(const char *name);

/*
 * Accept a single client connection (blocking).
 * timeout_ms: 0 = block forever, >0 = timeout in milliseconds.
 * Returns a connected MdIpcConn, or NULL on timeout/error.
 */
MdIpcConn *md_ipc_accept(MdIpcServer *srv, uint32_t timeout_ms);

/* Get the endpoint path for this server (for diagnostic logging). */
const char *md_ipc_server_path(const MdIpcServer *srv);

/* Destroy the server, removing the endpoint. */
void md_ipc_server_destroy(MdIpcServer *srv);

/* ── Client API (connect) ────────────────────────────────────── */

/*
 * Connect to an IPC server by name.
 * timeout_ms: 0 = OS default, >0 = connect timeout.
 * Returns a connected MdIpcConn, or NULL on failure.
 */
MdIpcConn *md_ipc_connect(const char *name, uint32_t timeout_ms);

/* ── Connection I/O ──────────────────────────────────────────── */

/*
 * Send data over the IPC connection.
 * Blocks until all bytes are written or an error occurs.
 * Returns 0 on success, -1 on error.
 */
int md_ipc_send(MdIpcConn *conn, const void *data, size_t len);

/*
 * Receive data from the IPC connection.
 * Blocks until data is available, timeout, or error.
 * buf: output buffer (must be at least buf_len bytes)
 * buf_len: maximum bytes to read
 * timeout_ms: 0 = block forever, >0 = timeout.
 * Returns bytes read on success, 0 on peer disconnect, -1 on error.
 */
int md_ipc_recv(MdIpcConn *conn, void *buf, size_t buf_len,
                uint32_t timeout_ms);

/* Check if the connection is still alive. */
bool md_ipc_is_connected(const MdIpcConn *conn);

/* Close and destroy an IPC connection. */
void md_ipc_close(MdIpcConn *conn);

#ifdef __cplusplus
}
#endif

#endif /* MD_IPC_H */
