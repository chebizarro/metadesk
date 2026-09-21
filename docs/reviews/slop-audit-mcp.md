# Slop Audit — MCP / JSON-RPC / Agent Layer

**Date:** 2026-09-20
**Auditor:** automated review session
**Scope (exclusive):** `src/core/mcp_server.{c,h}`, `mcp_stdio.{c,h}`, `mcp_http.{c,h}`, `mcp_tools.{c,h}`, `mcp_resources.{c,h}`, `mcp_bridge.{c,h}`, `jsonrpc.{c,h}`, `agent.{c,h}`, `ipc.h`, `ipc_unix.c`, `ipc_win32.c` (4,277 lines total).

**Premise:** the code works and tests pass. This audit targets code that is the *wrong shape* — workarounds standing in for a primitive that already exists, duplication that has already drifted, and ceremony that imposes ongoing maintenance cost.

---

## Answers to the Directed Questions

| Question | Answer |
|---|---|
| **Is `jsonrpc.c` actually shared by all transports?** | **Yes — this part is correct.** Only `mcp_server.c` parses/serializes JSON-RPC. `mcp_stdio.c` does newline framing, `mcp_http.c` does HTTP framing; neither parses JSON-RPC. **One exception:** `mcp_http.c:90-127` hand-assembles a JSON *array* with `memcpy`/pointer arithmetic instead of cJSON (F16). |
| **Does `mcp_bridge` own policy or is it a forwarder?** | **Both, badly.** It owns real policy (session lifecycle, UUID, capability bits, registration ordering) but `run/shutdown/get_state/get_server/is_degraded` are pure forwarders, and the HTTP path **bypasses the bridge entirely** — `src/host/main.c:841-852` reaches in with `get_server()` + `set_write_fn()` and `md_mcp_bridge_run()` returns `-1` in HTTP mode (F28). |
| **Does `agent.c` duplicate MCP tool logic?** | **Yes — the worst finding in the audit.** `md_agent_handle_action` (133-243) and `md_agent_handle_action_mcp` (246-347) are ~90 near-verbatim duplicated lines that have **already drifted in three observable ways** (F2). |
| **Does IPC duplicate between unix/win32 beyond platform necessity?** | **Mostly no** — the split is genuine (sockets vs named pipes). The real defects are *intra*-file: a pipe-flag drift inside `ipc_win32.c` itself (F5) and an EINTR/EOF conflation in `ipc_unix.c` (F6). |

---

## Findings Table

| # | Sev | Code | Location | Pattern | Correct Shape |
|---|---|---|---|---|---|
| F1 | structural | S7/S9 | `mcp_server.c:545`, `mcp_http.c:63,413-415`, `mcp_bridge.c:33-41` | Single global `write_fn` forces a `_Thread_local` response-capture hack, a mutable `set_write_fn` escape hatch, and a trampoline | Add a response-sink param to `md_mcp_server_handle_message()` |
| F2 | structural | S4 | `agent.c:133-243` vs `246-347` | ~90 duplicated lines; 3 live drifts | Extract `agent_execute_locked()`; two thin emitters |
| F3 | structural | S5 | `mcp_server.c:192-208`; 9 sites in `mcp_tools.c` | `error_msg` allocated by every handler, then `free()`d unread | Emit into content array, or delete the out-param from the fn type |
| F4 | structural | S5 | `mcp_http.c:139-142` | Every POST response broadcast to **all** SSE clients | `if (!tls_capture) md_mcp_http_send_sse(...)` |
| F5 | structural | S4 | `ipc_win32.c:161` vs `220-222` | Same pipe created two ways; first `accept()` timeout silently ignored | One flag set: `PIPE_ACCESS_DUPLEX \| FILE_FLAG_OVERLAPPED` at both sites |
| F6 | structural | S5/C4 | `ipc_unix.c:301` (+ `ipc.h:117`) | `EINTR` returns `0` == documented "peer disconnect" | `continue` retry loop, as `write_all` already does at line 101 |
| F7 | structural | S5 | `agent.c:178`, `287` | Target resolution fails → log and continue → click at (0,0) | Return error for `CLICK/DBL_CLICK/RIGHT_CLICK/FOCUS` |
| F8 | structural | S9/S5 | `mcp_http.c:333`, `md_mcp_http_destroy` | Blocking `read()` with no timeout; destroy waits on the wait group forever | `setsockopt(SO_RCVTIMEO)` on the accepted fd |
| F9 | structural | S7 | `mcp_bridge.c:140-152`, `host/main.c:841-852` | Bridge owns stdio only; HTTP hand-wired by the caller | `MdMcpBridgeConfig.http_port/bind_addr`; `bridge_run()` selects transport |
| F10 | drift | S4/C4 | `mcp_server.c:182` vs `256`; `:364` | "Not found" answered with 2 different codes; init error uses `INTERNAL_ERROR` | One `lookup_or_error()` helper; `MD_JSONRPC_INVALID_REQUEST` |
| F11 | drift | S6 | `jsonrpc.h:22` | `MD_JSONRPC_INVALID_REQUEST` defined, **zero** uses; all parse failures → `-32700` | Out-param error code from `md_jsonrpc_parse_request()` |
| F12 | drift | S4 | `mcp_tools.c:170-190` vs `schema_*` | Hand-rolled validator duplicates the schema's `required` array | One field table drives both |
| F13 | drift | C4 | `mcp_tools.c:122`, `140` | `strncpy` silently truncates `target_id`(64)/`text`(1024) | `strlen` check → existing `is_error` path |
| F14 | drift | S4 | `mcp_stdio.c:105-112` vs `mcp_http.c:30` | HTTP caps at 1 MB; stdio buffer doubles unboundedly | Shared `MD_MCP_MAX_MESSAGE_SIZE` in `mcp_server.h` |
| F15 | drift | S9 | `mcp_http.c:204,304-305` | `Mcp-Session-Id` parsed, stored, **never read** | Delete the field, or implement session binding |
| F16 | drift | C4/S8 | `mcp_http.c:90-127` | JSON array built by `memcpy`/pointer arithmetic while cJSON is linked | `cJSON_CreateArray` + `cJSON_PrintUnformatted` |
| F17 | drift | C4 | `mcp_resources.c:30` vs `:141` | Registration advertises `application/json`; compact format returns `text/plain` | Derive registered `mime_type` from `tree_format` |
| F18 | drift | C4 | `ipc_win32.c:367` | `DisconnectNamedPipe` called on **client** handles | `bool is_server` in `MdIpcConn` |
| F19 | load | S1 | `mcp_server.c:33,193-196` | `tool_mu` re-guards what `agent->mu` already guards; held across 100 ms sleep + full tree walk | Delete `tool_mu` |
| F20 | load | S4 | `mcp_bridge.c:76-134` (5×), `mcp_http.c:596-668` (4×) | Teardown ladder copy-pasted | `goto fail:` + single cleanup reusing `*_destroy()` |
| F21 | load | S4 | `mcp_tools.c:70-80, 224-290` | 5 near-identical schema builders | Declarative `FieldDef[]` table + one builder |
| F22 | load | C4 | `mcp_tools.c:95-103` vs `214-221`; `mcp_resources.c:14-51,56-110` | 3 hand-rolled "text content" builders | Shared `md_mcp_text_content()` in `mcp_server.c` |
| F23 | load | S7 | `mcp_stdio.c:82-91` | `get_write_fn` ignores its arg; `get_write_userdata` is identity | Delete both (follows F1) |
| F24 | load | S9/S5 | `mcp_bridge.c:54,170-173`; `mcp_bridge.h:35` | `degraded` has **0 non-test callers**; header says a11y "required" but NULL accepted | Reject NULL a11y in `create()`; delete flag + accessor |
| F25 | load | S9/S6 | `mcp_tools.h:22`, `mcp_tools.c:10`, `mcp_bridge.c:97` | `MdMcpToolCtx.a11y` written once, never read; `#include "a11y.h"` unused | Delete field + include |
| F26 | load | C5 | 17 sites across `mcp_server.c`, `agent.c`, `ipc_unix.c`, `ipc_win32.c` | `fprintf(stderr, "agent[mcp]: ...")` as logging in library code | One `MD_LOG_*` macro in a shared core header |
| F27 | cosmetic | S1 | `agent.c:105-112` | Unreachable `default` fallback after exhaustive switch | Delete the trailing return |
| F28 | cosmetic | S3 | `mcp_tools.c:86-88`, `agent.c:341-342` | Comments describe an approach the code doesn't take | Delete |
| F29 | cosmetic | C4 | `mcp_http.c:347,358,386,397,408,423,452` | Hand-counted literal body lengths (`23`, `24`, `11`…) | `send_http_text()` helper using `strlen` |
| F30 | cosmetic | S4 | `mcp_resources.c` (5 literals) | `"metadesk://ui-tree"` ×3, `"metadesk://session-info"` ×2 | `#define MD_MCP_URI_UI_TREE` in `mcp_resources.h` |
| F31 | cosmetic | S6 | `ipc.h:58` | `MD_IPC_MAX_MSG` documented as send/recv max, never enforced | Enforce in `md_ipc_send/recv`, or rename `MD_IPC_BUF_SIZE` |
| F32 | confidence | C4 | `ipc_win32.c:327` | `GetOverlappedResult` return ignored; `bytesRead` may be stale | Check return; `return -1` on failure |

---

## Detailed Findings

### F1 — `structural` · S7/S9 · The one-global-`write_fn` workaround and its three dependents

**Observed.** `MdMcpServer` holds one `write_fn`/`write_userdata` pair set at create time. Request/response is inherently per-connection, so three separate workarounds were bolted on:

1. `mcp_http.c:63` — `static _Thread_local HttpResponseCapture *tls_capture`, set at `:413`, cleared at `:415`, to route a response back to the right socket.
2. `mcp_server.c:545` — `md_mcp_server_set_write_fn()`, a mutate-after-create escape hatch whose only production caller is `src/host/main.c:850`, existing solely because the transport is created after the server.
3. `mcp_bridge.c:33-41` — `bridge_stdio_write()`, a trampoline that re-resolves `md_mcp_stdio_get_write_fn()` (a constant) and `md_mcp_stdio_get_write_userdata()` (identity) **on every single write**, because the bridge registers the server at step 3 but creates the stdio transport at step 7.

The `capture_take_body()` "return a JSON array if more than one message was written" branch (`:99-127`) is a fourth symptom: without per-request routing, an out-of-band notification emitted during a request gets glued into that request's HTTP body.

**What primitive is this standing in for.** A per-request response sink. It does not exist; every transport is instead forced to smuggle routing through thread-local state or a mutable global.

**Correct shape.**
```c
/* mcp_server.h */
int md_mcp_server_handle_message(MdMcpServer *server, const char *json, size_t len,
                                 MdMcpWriteFn sink, void *sink_ud);
```
`mcp_stdio.c` passes `stdio_write, ctx`. `mcp_http.c` passes a local capture closure — no `_Thread_local`, no array-glue branch. `write_fn` stays on the server for out-of-band notifications only. Deletes: `tls_capture`, `md_mcp_server_set_write_fn`, `md_mcp_stdio_get_write_fn/userdata`, `md_mcp_http_get_write_fn/userdata`, `bridge_stdio_write` (F23 falls out for free).

**Cost imposed.** Four coupled workarounds; a documented-as-thread-safe API that is only safe because `set_write_fn` happens to be called before `run()`; every stdio write pays two indirect calls to recover a constant.

---

### F2 — `structural` · S4 · `md_agent_handle_action` vs `..._mcp`: 90 duplicated lines, already drifted

**Observed.** `agent.c:133-243` and `agent.c:246-347` are the same function. Steps 1–5 (parse JSON → log → lock → resolve target → the `CLICK/DBL_CLICK/RIGHT_CLICK` coord assignment and `FOCUS`→`CLICK` rewrite → inject → cleanup → `sleep_ms` → invalidate cache) are character-for-character identical apart from the `"agent:"` / `"agent[mcp]:"` log prefixes. Only step 6 differs: one calls `md_stream_send`, the other returns the string.

**The copies have already drifted — three confirmed divergences:**

| Behaviour | `md_agent_handle_action` | `md_agent_handle_action_mcp` |
|---|---|---|
| `action_count` | incremented unconditionally after injection (`:196`) | incremented **only if** serialization returned non-NULL (`:323`, `:334`) |
| Oversized delta | falls back to full tree above `MD_AGENT_MAX_DELTA_SIZE` (`:218`) | **no cap** — unbounded delta returned to MCP clients |
| No-delta path | calls `md_agent_send_tree_locked()` | re-implements the walk/serialize inline (`:329-337`) |

The `action_count` drift is observable: `metadesk://session-info` (`mcp_resources.c:96`) reports a different number over MCP than the stream path would for the same actions.

**Correct shape.**
```c
static int agent_execute_locked(MdAgent *a, const uint8_t *payload, uint32_t len);
/* steps 1-5, once */

char *md_agent_handle_action_mcp(MdAgent *a, const uint8_t *p, uint32_t n) {
    pthread_mutex_lock(&a->mu);
    if (agent_execute_locked(a, p, n) < 0) { unlock; return NULL; }
    char *out = agent_render_delta_locked(a);   /* shared with the stream path */
    pthread_mutex_unlock(&a->mu);
    return out;
}
```
The stream variant becomes `agent_execute_locked()` + `md_stream_send()` on the same rendered buffer, restoring the size cap for both.

**Cost imposed.** Every agent-pipeline change must be made twice, and the three existing drifts prove that already fails in practice.

---

### F3 — `structural` · S5 · Tool error messages are allocated, then thrown away

**Observed.** `MdMcpToolHandlerFn` (`mcp_server.h:38-42`) documents: *"returns NULL on error (sets `*is_error = true` and `*error_msg`)"*. `mcp_tools.c` honours it at **nine** sites (`:132,174,180,186,197,203,217`, …), each `strdup`-ing a precise message. `mcp_server.c:208` then does:
```c
free(error_msg);
```
The string is never placed in the response. Because it would be invisible, `mcp_tools.c` also returns a **second, vaguer** copy of the same message through `make_text_content()` — `"Missing required 'text' for type action"` is discarded while `"Error: missing text"` is what the client sees.

**Correct shape.** One channel. Either delete `error_msg` from the function-pointer type and keep the content array as the sole error surface, or use it in `handle_tools_call`:
```c
if (is_error && error_msg)
    cJSON_AddItemToObject(result, "content", make_text_content(error_msg));
```

**Cost imposed.** Nine `strdup`/`free` pairs of pure waste; two parallel error vocabularies for the same failures, guaranteed to drift; an API contract in the header that the implementation silently violates.

---

### F4 — `structural` · S5 · POST responses are broadcast to every SSE client

**Observed.** `mcp_http.c:133-145`:
```c
static int http_write_fn(const char *json, size_t len, void *userdata) {
    int rc = capture_append(tls_capture, json, len);
    if (md_mcp_http_send_sse(h, "message", json, len) < 0)   /* unconditional */
        rc = -1;
    return rc;
}
```
Every response to every POST is captured for the HTTP body **and** pushed to all connected SSE listeners. Under MCP Streamable HTTP, a response belongs to its POST; only server-initiated notifications belong on the SSE stream. With `DEFAULT_MAX_SSE_CLIENTS = 4`, any listener sees every other client's tool results — including screenshots and UI trees.

**Correct shape.** `tls_capture` already distinguishes the two cases exactly:
```c
if (tls_capture)
    return capture_append(tls_capture, json, len);     /* request response */
return md_mcp_http_send_sse(h, "message", json, len);  /* out-of-band notification */
```

**Cost imposed.** Cross-client response leakage and duplicate delivery; also makes `md_mcp_http_send_sse`'s always-0 return (`:471`) the reason the `rc = -1` branch is effectively dead.

---

### F5 — `structural` · S4 · Named pipe created two different ways in the same file

**Observed.** `ipc_win32.c:161` (`md_ipc_listen`):
```c
srv->pipe = ipc_create_named_pipe(srv->path, PIPE_ACCESS_DUPLEX);
```
`ipc_win32.c:220-222` (`md_ipc_accept`, re-arming for the next client):
```c
srv->pipe = ipc_create_named_pipe(srv->path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED);
```
`md_ipc_accept`'s timeout path (`:176-198`) issues `ConnectNamedPipe(srv->pipe, &ov)` with an `OVERLAPPED`. That requires the handle to have been opened with `FILE_FLAG_OVERLAPPED`. On the **first** accept it was not, so `ERROR_IO_PENDING` never occurs, the call blocks synchronously, and the caller's `timeout_ms` is silently ignored. Every subsequent accept honours it.

**Correct shape.** One flag expression, used at both sites — or better, hoist it into `ipc_create_named_pipe` since both callers want identical semantics:
```c
static HANDLE ipc_create_named_pipe(const char *path) {
    ... CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, ...);
}
```

**Cost imposed.** `md_ipc_accept(srv, 500)` behaves differently on the first call than on all later calls — a bug shape that is invisible in a single-connection test.

---

### F6 — `structural` · S5/C4 · `EINTR` is reported as peer disconnect

**Observed.** `ipc.h:117` documents `md_ipc_recv`: *"Returns bytes read on success, **0 on peer disconnect**, -1 on error."* `ipc_unix.c:299-303`:
```c
ssize_t n = read(conn->fd, buf, buf_len);
if (n < 0) {
    if (errno == EINTR) return 0;   /* ← indistinguishable from EOF */
    ...
}
```
Any signal delivered mid-`read` tells the caller the peer hung up. The same file already handles this correctly eleven lines earlier — `write_all` at `:101` does `if (errno == EINTR) continue;`.

**Correct shape.** Wrap the read in the same retry loop `write_all` uses:
```c
ssize_t n;
do { n = read(conn->fd, buf, buf_len); } while (n < 0 && errno == EINTR);
```

**Cost imposed.** Spurious session teardown under any signal traffic (`SIGWINCH`, profiling timers, `SIGCHLD`), and a header contract that the implementation contradicts.

---

### F7 — `structural` · S5 · Unresolvable target falls through to a click at (0,0)

**Observed.** `agent.c:170-182` (and the duplicate at `:279-291`):
```c
} else {
    fprintf(stderr, "agent: could not resolve target '%s'\n", action.target_id);
    /* Continue anyway — some actions (key_combo, type) don't need coords */
}
```
The comment's justification only covers `KEY_COMBO`/`TYPE`, but the code continues for **all** action types. `action.region[]` was zeroed by the `memset` at `:139`, so a `CLICK` on a stale or bogus `target_id` injects a real click at screen coordinate (0,0) and then reports success with a tree delta.

**Correct shape.** Gate on the action class the resolution was needed for:
```c
if (resolve_target(agent, action.target_id, &tx, &ty) != 0 &&
    md_action_needs_coords(action.type)) {          /* CLICK/DBL/RIGHT/FOCUS */
    md_action_cleanup(&action);
    pthread_mutex_unlock(&agent->mu);
    return -1;    /* NULL in the _mcp variant */
}
```

**Cost imposed.** A stale node ID produces a silent misfire on whatever is at the top-left of the screen, reported to the LLM as success.

---

### F8 — `structural` · S9/S5 · No read timeout; shutdown can hang forever

**Observed.** `mcp_http.c:333` reads from the accepted socket in a loop with no `SO_RCVTIMEO`, no shutdown-flag check, and no deadline. A client that connects and sends nothing parks a `go()` handler indefinitely. `md_mcp_http_shutdown` (`:690`) closes SSE clients but not in-flight POST sockets, and `md_mcp_http_destroy` (`:699`) then calls `go_wait_group_wait(&http->handler_wg)` — which never returns.

**Correct shape.** Set a receive deadline on the accepted fd immediately after `accept()` in `md_mcp_http_run`:
```c
struct timeval rcv = { .tv_sec = 10 };
setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
```

**Cost imposed.** One idle connection wedges process shutdown; `DEFAULT_MAX_SSE_CLIENTS` bounds SSE but nothing bounds parked handlers.

---

### F9 — `structural` · S7 · The bridge abstracts stdio only; HTTP is hand-wired around it

**Observed.** `MdMcpBridgeConfig` (`mcp_bridge.h:34-44`) has a comment reading *"Transport: use one of these"* followed by exactly one transport (`stdio_in_fd`/`stdio_out_fd`). `md_mcp_bridge_run` (`:140-152`) returns `-1` when no stdio fd was given. So `src/host/main.c:841-852` must reach through the abstraction:
```c
MdMcpHttpConfig http_cfg = { .server = md_mcp_bridge_get_server(mcp_bridge), ... };
md_mcp_server_set_write_fn(md_mcp_bridge_get_server(mcp_bridge),
                           md_mcp_http_get_write_fn(http),
                           md_mcp_http_get_write_userdata(http));
```
The bridge advertises itself as *"the one-call setup for hosting an MCP agent connection"* (`mcp_bridge.h:46`) while half its transports require the caller to do the wiring manually.

**Correct shape.** Finish the abstraction the config comment already promises:
```c
typedef struct {
    ...
    int         stdio_in_fd, stdio_out_fd;  /* -1 to skip */
    uint16_t    http_port;                  /* 0 to skip  */
    const char *http_bind_addr;
} MdMcpBridgeConfig;
```
`md_mcp_bridge_create` builds whichever transport is configured and wires it internally; `md_mcp_bridge_run` dispatches. `md_mcp_bridge_get_server` and `md_mcp_server_set_write_fn` both become deletable.

**Cost imposed.** Transport setup logic lives in `main.c` for HTTP and in the bridge for stdio; the bridge's two escape-hatch accessors exist only to support the bypass.

---

### F10 / F11 — `drift` · S4/S6 · Error-code policy is ad hoc; `INVALID_REQUEST` is a ghost

**Observed.** Three inconsistencies in one dispatcher:
- Unknown **tool** → `MD_JSONRPC_METHOD_NOT_FOUND` (`mcp_server.c:182`).
- Unknown **resource URI** → `MD_JSONRPC_INVALID_PARAMS` (`:256`). Same question ("named thing not registered"), different answer.
- "Server not initialized" → `MD_JSONRPC_INTERNAL_ERROR` (`:364`). A client protocol violation reported as a server fault.
- `MD_JSONRPC_INVALID_REQUEST` (`jsonrpc.h:22`) has **zero** references repo-wide. `md_jsonrpc_parse_request` returns a bare `-1` for both "not JSON" and "valid JSON, wrong shape", so `mcp_server.c:341-346` maps every failure to `-32700 Parse error` — including a well-formed object missing `"jsonrpc":"2.0"`, which the spec defines as `-32600`.

**Correct shape.** Give the parser a reason code and use the constant that already exists:
```c
typedef enum { MD_JSONRPC_OK, MD_JSONRPC_ERR_PARSE, MD_JSONRPC_ERR_INVALID } MdJsonRpcParseResult;
MdJsonRpcParseResult md_jsonrpc_parse_request(MdJsonRpcRequest *req, const char *json, size_t len);
```
and a single lookup helper so tools and resources answer "not found" identically.

**Cost imposed.** Clients cannot distinguish transport corruption from protocol violation; a defined constant misleads readers into thinking the distinction is handled.

---

### F12 / F13 — `drift` · S4/C4 · Schema and validator are two sources of truth; inputs truncate silently

**Observed.** `schema_type_text()` (`:224`) declares `required: [target_id, text]`; `tool_handler` re-encodes the same rule by hand at `:170-190` as an `if` chain keyed on `MdActionType`. No JSON Schema validator is linked, so the schema is documentation for the LLM and the `if` chain is the enforcement — two copies that already disagree:
- `schema_key_combo()` sets `additionalProperties: false` and omits `target_id`, yet `tool_handler:120` still reads `target_id` for key-combo calls.
- `schema_screenshot()` declares `minItems/maxItems: 4` for `region`, but `:158` silently ignores any array whose size isn't exactly 4 — a 3-element region becomes a full-screen capture with no error.

Separately, `:122` and `:140`:
```c
strncpy(action.target_id, tid->valuestring, sizeof(action.target_id) - 1);  /* 64 */
strncpy(action.text,     text->valuestring, sizeof(action.text) - 1);       /* 1024 */
```
Over-long values are truncated with no diagnostic: a 70-char node ID becomes a different, non-existent ID, which then trips F7 and clicks (0,0); a 2 KB `type` string types half of it and reports success.

**Correct shape.** One table drives both:
```c
typedef struct { const char *name; FieldKind kind; bool required; const char *desc; size_t max_len; } FieldDef;
static const FieldDef fields_type_text[] = {
    { "target_id", FIELD_STRING, true, "Accessibility node ID", sizeof(((MdAction*)0)->target_id) },
    { "text",      FIELD_STRING, true, "Text to type",          sizeof(((MdAction*)0)->text) },
};
```
`build_schema(fields, n)` emits the JSON Schema; `extract_args(fields, n, arguments, &action, error_msg)` performs presence **and length** checks, routing over-length values into the existing `is_error` path.

**Cost imposed.** Adding a tool field means editing two places that no test cross-checks; over-long inputs fail silently in a way that surfaces as a mysterious misclick.

---

### F14 — `drift` · S4 · Two transports, two answers on message size limits

**Observed.** `mcp_http.c:30` defines `MAX_REQUEST_SIZE (1024*1024)` and enforces it at `:346-353` with a 413. `mcp_stdio.c:105-112` has no equivalent: the line buffer simply doubles (`buf_cap *= 2`) for as long as bytes arrive without a newline. A peer that never sends `\n` grows the buffer until `realloc` fails.

**Correct shape.** One constant in `mcp_server.h`, since it is a property of the protocol, not of a transport:
```c
#define MD_MCP_MAX_MESSAGE_SIZE (1024 * 1024)
```
`mcp_stdio.c` aborts the line (and the connection) when `buf_len` exceeds it; `mcp_http.c` uses it in place of `MAX_REQUEST_SIZE`.

**Cost imposed.** The same threat is defended on one transport and not the other; a reader cannot tell which behaviour is intended.

---

### F15 — `drift` · S9 · `Mcp-Session-Id` is parsed into a field nothing reads

**Observed.** `mcp_http.c:204` reserves `char session_id[128]`, and `:300-306` carefully case-insensitively locates the header, trims it, bounds-checks the length, and `memcpy`s it in. Grep over the whole repo: the field is written at `:305` and **never read**. Nothing validates, echoes, or issues a session ID; the server never sends `Mcp-Session-Id` on the initialize response.

**What it stands in for.** MCP Streamable HTTP session binding. It does not exist — this is the vestige of an implementation that was started and abandoned.

**Correct shape.** Either delete `session_id` and its 7 lines of parsing, or finish it: issue a UUID in `handle_initialize` (the bridge already generates one at `mcp_bridge.c:63-65` via `uuid_generate`), return it as an `Mcp-Session-Id` response header, and reject POSTs whose header doesn't match.

**Cost imposed.** A reader reasonably assumes sessions are enforced; combined with no auth, no Origin check, and `PIPE`/socket bound to loopback as the only control, this is a false signal of protection.

---

### F16 — `drift` · C4/S8 · JSON array assembled with pointer arithmetic

**Observed.** `mcp_http.c:99-127` builds a JSON array by hand:
```c
size_t total = 3; /* '[' + ']' + NUL */
for (...) total += strlen(cap->items[i]) + (i > 0 ? 1 : 0);
char *body = malloc(total);
char *p = body; *p++ = '[';
for (...) { if (i > 0) *p++ = ','; memcpy(p, cap->items[i], len); p += len; }
*p++ = ']'; *p = '\0';
```
This is a JSON serializer written in a file that already links cJSON and sits one layer above a module whose entire purpose is JSON-RPC serialization.

**Correct shape.**
```c
cJSON *arr = cJSON_CreateArray();
for (size_t i = 0; i < cap->count; i++)
    cJSON_AddItemToArray(arr, cJSON_Parse(cap->items[i]));
char *body = cJSON_PrintUnformatted(arr);
cJSON_Delete(arr);
```
Under F1 this branch disappears entirely — the multi-message case only exists because notifications leak into request captures.

**Cost imposed.** Hand-maintained length arithmetic (`total = 3` accounting for three characters in a comment) in the one place where a single off-by-one produces a heap overflow.

---

### F19 — `load` · S1 · `tool_mu` re-guards state that `agent->mu` already guards

**Observed.** `mcp_server.c:193-196` takes `tool_mu` around every tool handler call, commented *"registered tools may share agent/input/a11y state."* The only registered tools are the nine in `mcp_tools.c`, all of which funnel into `md_agent_handle_action_mcp`, which takes `agent->mu` at `agent.c:259` around exactly that state. The outer lock therefore guards nothing extra — but it is held across the inner lock **plus** `sleep_ms(settle_ms)` (100 ms default, `agent.h:44`) **plus** a full `md_a11y_walk` and `md_a11y_diff`.

**Correct shape.** Delete `tool_mu` (`mcp_server.c:33,193,196,448,467`). The agent already owns its concurrency policy; the server's job is dispatch. If a future tool has unsynchronized state, that tool owns its lock.

**Cost imposed.** A mutex held across a call-out into arbitrary user code for 100 ms+, serializing the entire HTTP transport that `go()`-per-connection was specifically added to parallelize. Two locks to reason about where one suffices.

---

### F20 — `load` · S4 · The teardown ladder, copy-pasted nine times

**Observed.** `mcp_bridge.c:76-134` repeats this block, growing by one line each step, **five** times:
```c
md_mcp_tools_cleanup(&b->tool_ctx);
md_mcp_server_destroy(b->server);
md_agent_destroy(b->agent);
free(b);
return NULL;
```
`mcp_http.c:596-668` repeats its own 5-call variant (`go_wait_group_destroy`, `pthread_mutex_destroy`, `free(sse_clients)`, `free(h)`, sometimes `close(listen_fd)`) **four** times, with the `close(h->listen_fd)` present in three of them and absent in the first.

**Correct shape.** The canonical C idiom, reusing the destructors that already exist:
```c
if (!b->server) goto fail;
...
fail:
    md_mcp_bridge_destroy(b);   /* already null-safe at every step */
    return NULL;
```

**Cost imposed.** Nine hand-maintained teardown sequences; adding one field to either struct means editing nine sites, and the existing asymmetry in `mcp_http_create` shows a step was already missed once.

---

### F24 / F25 — `load` · S9 · Dead state: `degraded` and `MdMcpToolCtx.a11y`

**Observed.**
- `mcp_bridge.c:54` computes `b->degraded = (config->a11y == NULL || config->input == NULL)`. The only reader is `md_mcp_bridge_is_degraded` (`:170`), whose only callers are `tests/test_mcp.c:483` and `:508`. **Zero production callers.** Meanwhile `mcp_bridge.h:36` documents `a11y` as *"(required)"*, yet `create()` accepts NULL and returns a bridge whose every tool fails at runtime — a universal fallback that converts a startup misconfiguration into nine confusing per-call errors.
- `MdMcpToolCtx.a11y` (`mcp_tools.h:22`, *"shared a11y context (for tree reads)"*) is assigned once at `mcp_bridge.c:97` and read **nowhere**. `mcp_tools.c:10` includes `"a11y.h"` although the file references no a11y symbol.

**Correct shape.** Enforce the documented contract and delete the rest:
```c
MdMcpBridge *md_mcp_bridge_create(const MdMcpBridgeConfig *config) {
    if (!config || !config->a11y) return NULL;   /* input stays optional */
    ...
}
```
Then delete `degraded`, `md_mcp_bridge_is_degraded`, `MdMcpToolCtx.a11y`, and the `a11y.h` include. The two tests asserting `is_degraded` become one test asserting `create()` returns NULL.

**Cost imposed.** Two pieces of state and a public accessor kept alive solely by their own tests — the "unwired implementation" pattern. Readers must determine for themselves that "degraded" has no behavioural consequence anywhere.

---

### F26 — `load` · C5 · `fprintf(stderr)` as the logging facility

**Observed.** 17 sites in scope hand-roll their own prefix: `mcp_server.c` ×3 (`"mcp: "`), `agent.c` ×8 (`"agent: "` / `"agent[mcp]: "` — the prefix is itself a symptom of F2), `ipc_unix.c` ×4 (`"ipc: "`), `ipc_win32.c` ×2. There is no level, no timestamp, no way to silence them, and no `MD_LOG` macro anywhere in `src/core/*.h`. `agent.c:151` logs every single action unconditionally at what is effectively debug level.

**Correct shape.** One macro in a shared core header (the module boundary already exists — `session_log.{c,h}` handles *structured session events*, which is a different concern and not a substitute):
```c
/* src/core/log.h */
#define MD_LOG(level, fmt, ...) md_log_emit(level, "%s: " fmt, MD_LOG_TAG, ##__VA_ARGS__)
```
with `#define MD_LOG_TAG "agent"` at the top of each .c file, so the prefix is declared once per module rather than retyped 17 times.

**Cost imposed.** No runtime verbosity control; a library that writes to the host process's stderr unbidden; prefixes that drift (`"agent:"` vs `"agent[mcp]:"` for identical events).

---

### Remaining findings (brief)

- **F17** `mcp_resources.c:30` sets `mime = "text/plain"` for compact trees (emitted at `:42`), but the resource is registered with `.mime_type = "application/json"` at `:141`. → Derive the registered value from `res_ctx->tree_format`.
- **F18** `ipc_win32.c:367` calls `DisconnectNamedPipe` in `md_ipc_close` for **all** connections; client handles from `md_ipc_connect` are `CreateFileA` handles where the call is meaningless. → Add `bool is_server` to `MdIpcConn`; `Disconnect` only for server ends.
- **F21** `mcp_tools.c:70-80, 224-290`: five `schema_*` functions each repeat `schema_object()` → `properties` → `required` array → `additionalProperties:false`. → Subsumed by the `FieldDef[]` table in F12.
- **F22** Three hand-rolled MCP content builders: `make_text_content` (`mcp_tools.c:95`), an inline duplicate of it 120 lines later (`:214-221` — same six calls, ignoring the helper in the same file), and two more in `mcp_resources.c:14-51,56-110`. → One `cJSON *md_mcp_text_content(const char *uri, const char *mime, const char *text)` exported from `mcp_server.c`.
- **F27** `agent.c:105-112`: `serialize_tree` switches over both `MdTreeFormat` values and then has a `return md_a11y_to_json(root); /* default fallback */` that the type system makes unreachable. → Delete.
- **F28** `mcp_tools.c:86-88` describes a design the code doesn't use: *"The action type is encoded in the userdata via offset from the MdMcpToolCtx pointer. We use a small wrapper struct instead."* `agent.c:341-342` claims a caching rationale for a `cached_tree` that is invalidated after every action and so never survives to be reused. → Delete both.
- **F29** `mcp_http.c:347,358,386,397,408,423,452`: body lengths hand-counted as literals (`"Invalid Content-Length\n", 23`). All seven are currently correct; all seven must be recounted by hand on any edit. → `send_http_text(fd, status, status_text, msg)` computing `strlen` internally.
- **F30** `"metadesk://ui-tree"` appears at `mcp_resources.c:18,117,136` and `"metadesk://session-info"` at `:103,149`. → `#define MD_MCP_URI_UI_TREE "metadesk://ui-tree"` in `mcp_resources.h`.
- **F31** `ipc.h:58` documents `MD_IPC_MAX_MSG` as *"Maximum data per single send/recv"*; neither backend's `md_ipc_send`/`md_ipc_recv` checks `len` against it. It is used only as a Windows pipe buffer size and as a caller-side stack buffer size. → Enforce it, or rename to `MD_IPC_BUF_SIZE`.
- **F32** `ipc_win32.c:327`: `GetOverlappedResult(conn->pipe, &ov, &bytesRead, FALSE)` return value discarded; on failure `bytesRead` is returned as a byte count anyway. → Check the return, `return -1` on failure.

---

## Recommended Order of Work

1. **F1** (response sink) — unlocks deletion of F4's workaround surface, F16, F23, and half of F9.
2. **F2** (agent de-duplication) — highest ongoing cost, three live drifts.
3. **F3, F7, F6, F5** — swallowed errors and platform drift with observable wrong behaviour.
4. **F4, F8, F15** — HTTP transport correctness and dead session scaffolding.
5. **F19, F20, F24, F25** — pure subtraction; each is a net line-count reduction with no behaviour change.
6. **F12/F13/F21** — the tool-schema table; one refactor closes four findings.
7. Cosmetics (F27–F32) opportunistically alongside the above.

**Net effect if fully applied:** roughly 350 lines deleted, two mutexes removed, one thread-local removed, three public functions removed from headers, and the elimination of every "same logic, second copy" pair identified in this audit.
