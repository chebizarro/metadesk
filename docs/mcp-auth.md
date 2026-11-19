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
  while no relay can serve history (every relay disconnected, rate-limited,
  awaiting NIP-42 AUTH or refusing), the source denies everyone (401). An
  empty loaded list also denies. The host does not wait for history before
  serving; bearer and static-NIP-98 clients are unaffected. Open SSE streams
  from this source are *held* while that is expected to pass (see
  [relay outages](#relay-outages-held-streams-md-554)), and closed once every
  relay has refused the subscription (see
  [refusing relays](#relays-that-refuse-the-subscription-md-ip2k)).
- **Live, never polled.** One REQ
  `{kinds:[30000], authors:[host], #d:["metadesk-allowlist"], limit:1}` is held
  open. A newer replaceable event the relay pushes takes effect on the **next
  request**; nothing is restarted or re-subscribed. Older replacements lose
  under the newest-replaceable rule, and a same-second tie goes to the lower
  event id. Reconnection and backoff are the existing bridge's (the nostrc pool);
  readiness drops to deny while history is being re-delivered.
- **Open SSE streams** admitted by this source are rechecked on every
  accepted list version, every readiness transition and before every
  broadcast. A stream whose grant lost `agent` is closed at once, with no
  further notifications; one whose visible tool set changed gets
  `notifications/tools/list_changed` (see below). While the list is not
  known, the stream is held and gets nothing.
- **Host role changes** (revocation of the binding) stop the bridge, after which
  this source denies.
- Readiness and every accepted list version are logged to stderr, for example
  `host[mcp]: host allowlist updated (v3): 2 member(s), 1 with agent (1 also input)`
  or `host[mcp]: host allowlist history pending; host-allowlist NIP-98 grants deny and their SSE streams are held until a relay delivers it`
  or `host[mcp]: host allowlist refused by every relay (ws://relay.example: restricted: not allowed); host-allowlist NIP-98 requests are refused (401) and their SSE streams closed until a relay delivers it`.
  The line is written after the SSE streams have been notified for that version.

## Per-principal tool capabilities (md-5km)

Admission is not the whole story: each request also carries its principal's
**effective capabilities**, computed when that request is authenticated, and
MCP dispatch gates tools on them. The rule matches video sessions: the
accessibility read tools (`metadesk_find`, `_subtree`, `_node`, `_focused`,
`_windows`) and the `metadesk://` resources need `agent`, and the nine action
tools (`metadesk_click` … `metadesk_screenshot`) need `agent` **and**
`input`. No grant implies another one.

| Principal | Effective capabilities |
|---|---|
| Host allowlist (NIP-98, live) | the entry's current `v1:` grant: `agent` is required to be admitted, and `input` adds actions |
| Static npub (`--mcp-allow-npub`, `--mcp-allow-file`) | all |
| Bearer token (`--mcp-token-file`) | all |
| No credential configured (loopback only) | all |
| stdio (`--mcp`) | all: host-local, with no network principal |

"All" still means **subject to host policy**. Under `--no-input`, or with no
ready input backend, action tools are not registered at all. If input is lost
at runtime they return `isError` (md-ah9.1). Capabilities only narrow what the
host offers; they never add tools.

For a principal without `input`:

- `tools/list` omits the action tools.
- `tools/call` of an action tool (clients may call tools that were not listed)
  returns a normal result with `isError: true` and the text
  `Error: tool 'metadesk_click' requires the 'input' grant, which this MCP principal lacks`.
  The tool handler never runs and no input is injected. An unknown tool name
  is still a JSON-RPC error.
- `metadesk://session-info` lists the effective capabilities: the host's,
  narrowed to the principal's grant.

**Grant changes apply to the next request.** Every POST is authenticated
afresh, and the grant is read from the live list at that moment. Nothing is
cached per connection or per MCP session. Granting `input` makes the actions
appear on the next `tools/list` and succeed on the next call. Removing it
refuses the very next call. An `input`-only or `video`-only grant is not
admitted (401). Open SSE streams carry notifications only, never tool
results. A stream is closed once its grant no longer includes `agent`.

### `notifications/tools/list_changed` (md-ht7)

With `--mcp-allow-host-list`, `initialize` advertises
`capabilities.tools.listChanged: true`, and a live principal with an open SSE
stream (`GET /mcp`) is told when its grant changes what it can see:

- **Trigger: events only.** The allowlist's watcher thread wakes on each
  accepted list version (and each readiness transition) via
  `md_nostr_allowlist_wait_change()`, the same wait that logs the version
  lines. It calls into the HTTP transport, which re-reads the grant of every
  stream the live list admitted. There is no timer and no poll.
- **Targeted.** Each stream records the grant its tool set was last checked
  against (initially the one it was admitted with). A stream gets exactly one
  `{"jsonrpc":"2.0","method":"notifications/tools/list_changed"}` when the set
  of tools visible under the new grant differs from that recorded set, and
  nothing otherwise. Narrowing `agent,input` → `agent` and widening
  `agent` → `agent,input` both notify. A change that leaves the tool set alone
  sends nothing: another principal's entry, `video` added or removed, a
  re-published identical list, or `input` changes on a host with no action
  tools registered (`--no-input`). Static-npub, bearer and credential-less
  streams never get one: their tool set does not depend on the list.
- **Revocation** (grant loses `agent`, or the host role changes) closes the
  stream on the change itself. The list becoming *temporarily unavailable*
  is not a revocation: the stream is held until the list is known again, then
  judged against its last known grant (next section). Every relay *refusing*
  the subscription is treated as a known deny and closes the stream (md-ip2k).
- **No gaps.** A stream that becomes ready just after a change is re-checked
  as it becomes ready. The record is updated under the SSE lock, so a change
  notifies once however many wake-ups observe it.
- **Bounded and teardown-safe.** Delivery uses the same single write budget
  (`MD_MCP_SSE_WRITE_TIMEOUT_MS`) as broadcasts. A stream that cannot be
  notified (a stall, a write error, an allocation failure) is closed, never
  silently skipped, so its client reconnects and lists again. Nothing is sent after shutdown starts or after the MCP server is
  quiesced. The transport unregisters from the watcher first thing in
  destroy, and unregistering waits for an in-flight call to return.

The notification only says "list again": clients re-run `tools/list`, and
every `tools/call` is still gated on the grant at that moment. A grant change
between a client's last `tools/list` and its opening the SSE stream is not
reported, so clients should list after the stream opens. Without the live
list (static, bearer or open callers only), `listChanged` is not advertised.

### Relay outages: held streams (md-554)

Readiness drops whenever no relay can serve history: every relay is
disconnected or in backoff, or has ended the subscription with a transient
CLOSED (`rate-limited:`, or `auth-required:` while our NIP-42 AUTH is
pending). If every relay has *refused* it, the answer is a known deny instead
(next section). Readiness also drops while *any* relay is re-delivering
history after it (re)connects,
until that relay's EOSE: a relay that is still connecting counts as pending.
So every relay reconnect causes a short window, whether one relay or several
are configured. In that window the source cannot say whether any live
principal is still granted, so:

- **Requests from live-list principals are refused** (401), as before. A new
  `GET /mcp` from one is refused too.
- **Their open SSE streams are held:** kept open, sent **nothing**. No
  `list_changed` and no broadcast (`notifications/resources/updated`,
  `notifications/message`, …) is written to them, and nothing is queued for
  later. Static-npub, bearer and credential-less streams are unaffected and
  keep receiving broadcasts.
- **Re-evaluation is event-driven.** The bridge's next readiness transition to
  loaded (EOSE after the reconnect, which the pool re-issues with `since` for
  catch-up) wakes the same watcher as list versions. Each held stream is then
  judged against the grant it last held while known:
  - grant no longer includes `agent`: the stream is closed;
  - visible tool set changed (for example `input` was removed while the relay
    was away): exactly one `notifications/tools/list_changed`;
  - otherwise: it resumes silently with the next broadcast.
- **No timer, no grace period.** A hold lasts as long as the outage. Its only
  bound is tied to an existing event: if the SSE client limit (`max_clients`,
  default 4) is reached, a `GET /mcp` from a principal the live list did not
  admit (static npub, bearer, open loopback) closes one held stream and takes
  its slot. So a long outage cannot lock operator credentials out of
  notifications, while held streams cost nothing else. A host role change
  (the bridge stops) is a revocation, not an outage: held streams close.

### Relays that refuse the subscription (md-ip2k)

A relay can answer the allowlist REQ with `["CLOSED", <sub>, <reason>]`
instead of history. No reconnect follows a CLOSED, so if it held streams the
way an outage does, they would stay held until shutdown, a host role change
or SSE-slot eviction. The bridge therefore classifies each CLOSED by the
reason's NIP-01 machine-readable prefix, per relay and per connection:

| CLOSED reason | Classification |
| --- | --- |
| `rate-limited:` | transient: the relay asks for a later retry |
| `auth-required:`, with an AUTH path | transient: awaiting AUTH |
| `auth-required:`, without an AUTH path | **refused** |
| `restricted:`, `blocked:`, `invalid:`, `pow:`, `mute:`, `duplicate:`, `error:`, any other or no prefix | **refused** |

An AUTH path exists while this connection has a NIP-42 challenge and our
answer to it has not failed (`OK false`, a signing failure or a transport
failure). An accepted answer still counts as a path: the pool's next REQ
then settles it with EOSE or a different CLOSED.

- **Every relay refused: a definite deny.** With no history and every
  configured relay refusing on its current connection, readiness is `FAILED`
  and the source answers a known 0, not "unknown". The watcher's wake-up on
  that transition closes held live-list streams, and new requests get 401. The
  host logs each relay and its reason once, for example
  `host[mcp]: host allowlist refused by every relay (ws://a: restricted: not allowed; ws://b: blocked: no); …`.
  The bridge also logs each relay's CLOSED with its classification, once per
  change rather than once per frame:
  `subscription CLOSED on ws://a: restricted: not allowed (refused: no history from this relay until it serves the REQ again)`.
  The nostrc pool re-issues the REQ after every CLOSED, about every 200 ms
  (md-odg). The bridge does not change that; repeated identical answers are
  idempotent.
- **Any other relay that might still deliver keeps it unknown.** A relay that
  is disconnected, in backoff, rate-limited or awaiting AUTH, beside refusing
  ones, holds streams as in an outage. One that has delivered EOSE keeps the
  list loaded: a refusing relay beside a live one changes nothing.
- **A refusal belongs to the connection that answered it.** A connection
  change (drop, reconnect) clears it, so the drop itself is a transient outage.
  A NIP-42 (re-)challenge turns an `auth-required:` refusal back into
  awaiting AUTH. A rejected AUTH (`OK false`) turns awaiting AUTH into a
  refusal.
- **Recovery is the normal path.** When a relay later serves history (the
  pool's re-REQ after a policy change, or after a re-challenge and accepted
  AUTH), its EOSE makes the list loaded again. New requests are admitted, and
  the clients whose streams were closed reconnect. Nothing is timed: every
  transition is a CLOSED, EOSE, AUTH, OK or connection-state event.

Classification lives in the bridge (`src/core/nostr.c`), so every allowlist
consumer sees the same states: `md_nostr_granted_capabilities_known()` is
*known* for `FAILED`, and `md_nostr_allowlist_refusals()` supplies the log
detail. Allowlist edits (`metadesk-host access`) still fail at once whenever
no history is loaded, without waiting for an outage or refusal to end.

**The trade-off.** No data flows while authorization is unknown, which is the
fail-closed rule applied to streams. The alternative (md-ht7's behaviour) was
to close every live stream on every outage, so each single-relay reconnect
churned every client. The cost of holding is that **notifications emitted
during the outage are lost for held streams, not replayed**: a client that
missed a `resources/updated` re-reads on the next one, or when it next lists or
reads. More relays do not avoid the window: a relay that is down
(disconnected or in backoff) does not hold readiness back while another has
delivered EOSE, but one that is reconnecting does, until its EOSE. Most holds
therefore last one handshake plus one history round trip.

**Why static and bearer principals keep all capabilities.** They are
deploy-time operator credentials: a 0600 token file, CLI flags, or a file the
operator wrote. They have no grant vocabulary, and they predate grants.
Narrowing them would silently remove actions from existing deployments.
The operator who can configure them can also configure the host's
`--no-input`, so a per-credential restriction would add no protection against
that operator. To give an agent read-only access, grant it `agent` without `input` in
the host allowlist and configure no static credential for it. A static
listing wins over the live list (as above), so a key that is both statically
listed and live-granted `agent` keeps all capabilities.

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
  - per-principal tool capabilities over real signed HTTP requests: a live
    `agent`-only grant lists no action tool and gets `isError` naming `input`.
    Adding `input` enables the actions on the next request, and removing it
    refuses the next call. `input`/`video` without `agent` get 401. Static
    npub, bearer and credential-less loopback callers keep all tools. A live
    SSE stream is dropped once its grant loses `agent`;
  - `tools/list_changed` (md-ht7), with two live principals and a bearer
    stream and a stand-in change feed: exactly one notification per tool-set
    change (narrowing and widening), only to that principal's stream; none
    for a repeated event, a `video`-only change, the other principal or the
    bearer stream; revocation closes the stream on the change; nothing after
    quiesce; `listChanged` advertised only while a feed is registered, and
    destroy unregisters;
  - relay outages (md-554), with the same stand-in source answering
    `MD_MCP_GRANT_UNKNOWN`: live streams stay open while a broadcast reaches
    only the bearer stream; live POST and GET get 401, bearer does not. On
    return with unchanged grants nothing is sent and the next frame is the
    next broadcast, so the outage frame was withheld rather than queued. An
    outage in which one principal loses `input` and the other loses `agent`
    ends with exactly one `list_changed` and one close. At the SSE limit a
    bearer newcomer takes a held stream's slot, and gets 503 while every
    grant is known;
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
  - an `agent`-only grant lists no action tool (`act` stands in for one) and
    gets `isError` naming `input` on call. Publishing `v1:agent,input` enables
    it on the next request, and publishing `v1:agent` again refuses it;
  - `tools/list_changed` (md-ht7): `initialize` advertises `listChanged`.
    Revoking `agent` closes the open stream at once. With a second live
    principal streaming, widening to `v1:agent,input` and narrowing back each
    send exactly one `list_changed` on the agent's stream and nothing on the
    other's. `v1:video,agent` → `v1:agent` and adding `video` send nothing.
    Revoking the second principal closes only its stream. The barrier is the
    host's version line (logged after notifying), then one `notify` broadcast
    that must arrive before anything else on every quiet stream;
  - a static `--allow-npub` principal keeps the full tool surface, even though
    its live grant is `video`-only;
  - prompt exit on EOF;
  - the NIP-5F socket-signer path;
  - relay outages (md-554), on the static-principal host with its single
    relay: the test disconnects the relay and holds reconnects at the
    WebSocket handshake. The live agent's stream is held, not closed, and its
    requests get 401. The static principal's `notify` broadcast during the
    outage never reaches it. On return with the same list it resumes with no
    `list_changed`. When `input` was removed during the outage it gets exactly
    one. When `agent` was revoked, the stream closes. The barriers are the
    host's readiness and `loaded` lines;
  - refusing relays (md-ip2k): after an outage the only relay comes back
    but answers the REQ with `CLOSED restricted:`. The held stream closes, the
    next request gets 401, and the static principal is unaffected. The host
    log names the relay and reason, and the bridge's CLOSED line appears once
    despite the pool's re-REQs. When the relay serves history again the list
    reloads and the same proxy's next request succeeds. A second host with one
    serving relay and one that always refuses stays loaded and its live stream
    stays open. When the serving relay drops, the stream is held (refusing +
    down is unknown) and resumes when that relay returns;
  - exactly one allowlist-only REQ per host connection on the relay. Each
    reconnect re-issues the same filter with `since`; the REQ the pool
    re-creates after a CLOSED has none. Refused REQs are not counted.
- `nostr ACL and AUTH private relays` (`tests/test_nostr.c` with
  `tests/nostr_acl_fixture.py`) covers the classification (md-ip2k):
  `rate-limited:` is unknown; `restricted:`/`blocked:` are `FAILED` and
  *known*, with `md_nostr_allowlist_refusals()` naming relay and reason, and
  edits fail rather than block; `auth-required:` with no challenge is refused,
  becomes awaiting AUTH (unknown) on a late challenge, is refused again on
  `OK false`, and recovers after a re-challenge, accepted AUTH and EOSE. With
  two relays, a refusal beside EOSE stays loaded, a refusal beside a dropped
  relay is unknown, and both refusing is `FAILED`. Every case recovers on the
  next EOSE. The real NIP-42 wire test checks that `auth-required:` is unknown
  while AUTH is pending and refused after rejection.
- `MCP server core` covers the new CLI rejections.
- `MCP accessibility queries` (`tests/test_mcp.c`) drives the real tool set
  through a bridge with mock input. Per caller grant, it checks the listed
  tools (5 for agent-only, 14 for agent+input or host-local stdio). It checks
  `isError` naming each missing grant, that no input reaches the backend on a
  refusal, and the caller-narrowed `session-info`.

## Diff-scoped Nostr protocol review

Reviewed against `nostr-protocol-smells` A–J; findings are recorded in the
md-ah9.3 and md-ah9.14 close notes. The relay-backed path adds:

- no polling or refresh timer, and no timeout-driven close;
- no publish (MCP never edits the list);
- no new frame handling: EOSE, CLOSED, AUTH and OK stay in the unchanged core;
- md-ht7: no new subscription or filter. `tools/list_changed` is driven by the
  same accepted-version wake-up, and repeat wake-ups are idempotent.
- md-554: no new subscription, filter, timer or frame handling. Held streams
  are released by the readiness wake-up that EOSE after the reconnect already
  produces. "Unknown" is read under the same lock as the grant, so a stopped
  bridge (a revocation) is never mistaken for an outage.
- md-ip2k: no new subscription, filter, timer or publish. CLOSED reasons are
  now classified by NIP-01 prefix instead of being treated as one case, and
  the AUTH challenge and the AUTH `OK` result feed the same readiness
  recomputation. A held stream ends on the refusal event itself. The pool's
  re-REQ after CLOSED (md-odg, nostrc's) is unchanged, and repeated refusals
  are idempotent and logged once.

Readiness and updates come from EOSE/CLOSED/connection boundaries and accepted
events, delivered through a condition variable. The filter is tightly scoped:
host author, kind 30000, exact `#d`, `limit:1`. The proxy's SSE reconnect is
HTTP, not Nostr; it backs off and re-signs. Test waits are failure bounds,
never completion signals: the barriers are the logged EOSE and version lines.
