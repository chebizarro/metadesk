# MCP HTTP authentication and `mcp-proxy`

`metadesk-host --mcp-http` serves MCP over Streamable HTTP (`POST /mcp`,
SSE `GET /mcp`). Design: [container-runtime plan §7.7](plans/container-runtime-2026-11-14.md).

Every request, credentialed or not, gets the **always-on guards** first: any
`Origin` header gets 403, and a `Host` that is neither loopback nor the
authority of a `--mcp-public-url` gets 403. After that, a request must carry one of:

| Scheme | Header | Configure with |
|---|---|---|
| Bearer | `Authorization: Bearer <token>` | `--mcp-token-file PATH` (regular file, mode exactly 0600) |
| NIP-98 | `Authorization: Nostr <base64 kind-27235 event>` | one or more NIP-98 sources (below) |

With no credential configured, a loopback bind serves unauthenticated requests
with a warning, and a non-loopback `--mcp-http-bind` is refused at startup.

## NIP-98 validation

A NIP-98 event must pass every check below. Failing any check, or reusing an
event, gets 401. The checks:

- valid id and signature, kind 27235, `created_at` within ±60 s;
- `method` equals the HTTP method;
- `u` equals a `--mcp-public-url`, or a loopback form
  `http://{127.0.0.1,localhost,[::1]}:PORT/mcp`;
- for `POST`, a `payload` tag equal to the SHA-256 of the exact body (mandatory);
- an event id not seen within 2× the window (a bounded replay cache; when the
  cache is full, new events are rejected rather than evicting a live entry);
- the signer is **authorized** by a source below.

Sources are consulted only after the event itself validates.

## Authorization sources and how they combine (md-ah9.3)

| Source | Flag | Changes at runtime? | Works offline? |
|---|---|---|---|
| Static npubs | `--mcp-allow-npub NPUB` (repeatable) | no | yes |
| Static file | `--mcp-allow-file PATH` (one npub per line, `#` comments; an empty file is a startup error) | no (read once) | yes |
| Host allowlist | `--mcp-allow-host-list` [`--relay URL` …] | **yes, live** | no |

**Rule: the sources are a union.** A signer is accepted if it appears in the
static set **or** the live host allowlist grants it `agent`. The static set is
checked first and never consults the network. The live source can only add
access. It cannot remove a statically listed npub: statically listed keys are
operator deploy-time policy (for example, a break-glass key or a container secret).
To make the relay-backed list authoritative, configure only `--mcp-allow-host-list`.

**The host-allowlist source** uses the saved **host role** (see
[accounts](accounts.md)). It reads that role's NIP-51 kind-30000
`d=metadesk-allowlist`, which is the same list `metadesk-host access` edits and
the same list that gates video sessions. A signer is authorized only when its
entry has exactly one `p` membership and exactly one valid
`["metadesk-caps", <pubkey>, "v1:…"]` grant that **includes `agent`** (md-ikr
format, see [access](access.md)). `v1:video`, `v1:none`, legacy entries without
a grant, and malformed or duplicate records deny.

- **Fail-closed readiness.** Until at least one relay has delivered EOSE, and
  while every relay is disconnected or has sent CLOSED, the source denies
  everyone (401). An empty loaded list also denies. The host does not wait for
  history before serving; bearer and static-NIP-98 clients are unaffected.
- **Live, never polled.** One REQ
  `{kinds:[30000], authors:[host], #d:["metadesk-allowlist"], limit:1}` is held
  open. A newer replaceable event the relay pushes takes effect on the **next
  request**; nothing is restarted or re-subscribed. Older replacements lose
  under the newest-replaceable rule, and a same-second tie goes to the lower
  event id. Reconnection and backoff are the existing bridge's (the nostrc pool);
  readiness drops to deny while history is being re-delivered.
- **Open SSE streams** admitted by this source are rechecked before every
  broadcast. A revoked stream gets no further notifications and is closed.
- **Host role changes** (revocation of the binding) stop the bridge, after which
  this source denies.
- Readiness and every accepted list version are logged to stderr, for example
  `host[mcp]: host allowlist updated (v3): 2 member(s), 1 with agent`.

**Scope note.** An admitted MCP principal gets the host's whole MCP tool
surface. Action tools are present unless the host runs with `--no-input` or
has no input backend. Unlike video sessions, `agent` without `input` does not
yet hide action tools from MCP. Grant `agent` to MCP agents accordingly, or run
`--no-input`. Per-principal `input` gating is tracked as a follow-up.

### Cost of `--mcp-allow-host-list`

MCP mode normally starts no Nostr at all. With this flag it adds:

- restoring the saved host role, so its signer must be reachable. It is used
  for identity only: no signing request is made.
- one WebSocket per `--relay` (default: the built-in relay), NIP-11 probing,
  and NIP-42 AUTH if a relay asks for it;
- exactly one REQ with the filter above. There is no gift-wrap or DM
  subscription and no session log.
- per request, one lock-guarded lookup in the in-memory list. No relay round
  trip happens on the request path.

`--relay` in MCP mode is accepted only together with `--mcp-allow-host-list`,
which in turn requires `--mcp-http`.

```sh
metadesk-host access --relay "$RELAY" add "$AGENT_NPUB" agent
metadesk-host --mcp-http 7710 --mcp-allow-host-list --relay "$RELAY"
```

## `metadesk-host mcp-proxy` (md-ah9.14)

No mainstream MCP client signs NIP-98. `mcp-proxy` is a stdio↔HTTP shim: any
stdio MCP client launches it and talks to a NIP-98-protected host through it.

```sh
metadesk-host mcp-proxy [--key-file PATH | --socket-signer [PATH] | --dbus-signer | --bunker URI] \
    [--no-sse] http://127.0.0.1:7710/mcp
```

- **Signer** (an `MdSigner`). The options, in order of preference:
  - `--key-file`: a local key, as 64-hex or `nsec1`, in a regular file with no
    group or other access.
  - `--dbus-signer`: NIP-55L, for example Grotto.
  - `--socket-signer`: NIP-5F.
  - default: the saved **client** role.
  - `--bunker`: NIP-46. Works, but see the cost model.
- **Each message is one request.** Each newline-delimited JSON-RPC message from
  stdin becomes one `POST` with a fresh kind-27235 event:
  - `u` is the URL argument, byte for byte, so pass a URL the host accepts:
    a loopback form of its port or a `--mcp-public-url`.
  - `method` is `POST`.
  - `payload` is the SHA-256 of the body.
  - `content` is a random nonce, so two identical bodies signed in the same
    second do not collide in the host's replay cache.
- **Responses.** JSON responses, or responses in an SSE-framed body, are written
  to stdout, one message per line. Redirects are not followed, because they
  would change the signed `u`.
- **Failures.** If a request is refused or cannot be delivered (401, 403, a
  connection error, and so on), the proxy writes a JSON-RPC error with the
  request's `id`, for example `HTTP 401`, so the client never hangs.
  Undeliverable notifications are reported on stderr.
- **Notifications.** After the first successful POST, the proxy holds
  `GET /mcp` (SSE) and relays each notification as a stdout line. It reconnects
  with capped exponential backoff (0.5–30 s), signing each attempt afresh, and
  also retries after a 401, so a re-grant recovers. `--no-sse` disables this.
- **Ordering.** Messages are forwarded one at a time in stdin order. The host
  serializes MCP dispatch anyway, and this keeps `notifications/initialized`
  ahead of later requests.
- **Exit.** On stdin EOF the proxy stops promptly, even with an open SSE
  stream, and exits 0. Usage errors exit 2 and signer failures exit 1.

**Cost model: one signature per HTTP request.** That means one per POST (that
is, per tool call, read or notification) plus one per SSE (re)connect. With a
local key, Grotto/NIP-55L or NIP-5F, that is a local IPC or CPU operation.
With NIP-46, every request waits for a relay round trip to the remote signer,
so prefer a local or delegated agent key. The host's replay cache holds 1024
ids for about 121 s, which caps sustained throughput at roughly 8 signed
requests per second per host. A higher rate returns 401.

Example Claude Code configuration (`.mcp.json`):

```json
{
  "mcpServers": {
    "metadesk": {
      "command": "metadesk-host",
      "args": ["mcp-proxy", "--key-file", "/home/me/.config/metadesk/agent.key",
               "http://127.0.0.1:7710/mcp"]
    }
  }
}
```

**Why a subcommand and not a separate tool.** `mcp-proxy` is dispatched before
any capture, accessibility, input or relay startup, just like `account` and
`access`. It therefore costs nothing extra at runtime. It reuses the account
and signer stack that `metadesk-host` already ships (including saved-role
restore), and needs no new install target or packaging.

## Tests

- `MCP HTTP+SSE transport` (`tests/test_mcp_http.c`) covers:
  - the live authorizer fails closed before it is ready;
  - grants and revocations apply on the next request;
  - invalid events never reach the authorizer;
  - a revoked SSE stream is dropped at the next broadcast;
  - the union with the static list, with static SSE streams unaffected;
  - the live source alone counts as a credential for non-loopback binds.
- `mcp-proxy NIP-98 e2e private relay` (`tests/test_mcp_proxy.py`, needs
  `-Dsigner_nip5f=true`) runs the real `metadesk-host mcp-proxy` binary against
  the production MCP HTTP server and the production host-allowlist source.
  The saved host role uses a private NIP-5F signer, with a private loopback
  relay and throwaway keys. The fixture host omits the a11y and input backends
  that a desktop host needs. The test proves:
  - 401 while history is held before EOSE;
  - an `agent`-granted npub succeeds (initialize, tools, ping, SSE notification);
  - unlisted and `video`-only npubs get 401;
  - live revocation and re-grant work without a restart;
  - prompt exit on EOF;
  - the NIP-5F socket-signer path;
  - exactly one allowlist-only REQ on the relay.
- `MCP server core` covers the new CLI rejections.

## Diff-scoped Nostr protocol review

Reviewed against `nostr-protocol-smells` A–J; findings are recorded in the
md-ah9.3 and md-ah9.14 close notes. The relay-backed path adds:

- no polling or refresh timer, and no timeout-driven close;
- no publish (MCP never edits the list);
- no new frame handling: EOSE, CLOSED, AUTH and OK stay in the unchanged core.

Readiness and updates come from EOSE/CLOSED/connection boundaries and accepted
events, delivered through a condition variable. The filter is tightly scoped:
host author, kind 30000, exact `#d`, `limit:1`. The proxy's SSE reconnect is
HTTP, not Nostr; it backs off and re-signs. Test waits are failure bounds,
never completion signals: the barriers are the logged EOSE and version lines.
