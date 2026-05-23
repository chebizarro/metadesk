# metadesk — Phase 2.1 Implementation Spec

**v0.1 DRAFT — Session Signaling & Relay Wiring**

> Legacy note (2026-05-23): this phase spec predates the current FIPS daemon discovery/traversal path. Its kind `30078` / `d=fips-transport` and `fips-nat` signaling details are retained for historical context only, not as recommended implementation guidance. Current metadesk deployments should use an external FIPS v0.3.x-era daemon for TUN, `.fips` DNS/addressing, control-socket readiness, overlay discovery, STUN/NAT traversal, and route maintenance. Optional `fips-gateway` deployments are Linux-only, FIPS-owned infrastructure outside metadesk lifecycle control.

|Field|Value|
|---|---|
|Builds on|Phase 1 complete (macOS green, T7610 pending)|
|Goal|First fully signaled FIPS session: OpenClaw agent connects to host by npub, no raw IP|
|Scope|`nostr.c`, `host/main.c`, `client/main.c`, `render.c`|
|Out of scope|`fips-nat`, multi-relay, adaptive bitrate, Windows|

---

## 1. What needs to happen

Seven TODOs block a real session. They fall into three groups:

**Group A — Relay publish/subscribe (nostr.c)** Four stubs that need nostrc API calls wired in. The primitives exist — `nostr_relay_publish()`, `nostr_tag_new()`, `nostr_tags_new()`, `nostr_simple_pool_subscribe()` — they just aren't called yet.

**Group B — Session negotiation (host/main.c, client/main.c)** The host echoes `MD_PKT_SESSION_INFO` back without reading it. The client sends nothing. The NIP-17 DM handshake in `nostr.c` is implemented but never wired into either daemon's startup sequence.

**Group C — Input forwarding (render.c)** Keyboard and mouse events from the client SDL2 window are captured but dropped. Without this the human client is display-only.

---

## 2. Group A — nostr.c relay wiring

### 2.1 `md_nostr_publish_transport()`

Currently: builds event, signs it, frees it without sending.

Fix: add d-tag `["d", "fips-transport"]` and call `nostr_relay_publish()` on every relay in the pool.

```c
// After nostr_event_set_created_at():

// Add d-tag
NostrTag *d_tag = nostr_tag_new("d", "fips-transport", NULL);
NostrTags *tags = nostr_tags_new(1, d_tag);
nostr_event_set_tags(event, tags);

// Sign
NostrEvent *signed_event = sign_event_via_signer(n->signer, event);
nostr_event_free(event);
if (!signed_event) return -1;

// Publish to all relays
for (size_t i = 0; i < n->pool->relay_count; i++) {
    nostr_relay_publish(n->pool->relays[i], signed_event);
}
nostr_event_free(signed_event);
return 0;
```

**Test:** After calling this, query your sharegap.net relay for `{"kinds":[30078],"authors":["<host_pk_hex>"]}` and confirm the event appears.

### 2.2 `md_nostr_subscribe_transport()`

Currently: stub returning 0.

Fix: build a REQ filter for kind:30078 from the target host pubkey and register it on the pool. Incoming events route to `md_nostr_event_handler()` via the existing middleware, which already handles kind:30078 and calls `cbs.on_transport`.

```c
NostrFilter *f = nostr_filter_new();
// set kinds = [30078], authors = [host_pubkey_hex]
// set #d tag = ["fips-transport"]

NostrFilters filters = { .filters = f, .count = 1 };
const char *urls[/* relay count */];
for (size_t i = 0; i < n->pool->relay_count; i++)
    urls[i] = n->pool->relays[i]->url;

nostr_simple_pool_subscribe(n->pool, urls, n->pool->relay_count,
                            filters, /* unique= */ true);
```

Check `nostr-filter.h` for the exact field names on `NostrFilter` — authors, kinds, and tag filters. The `#d` tag filter field may be `tags` or a separate field; read the header before writing this.

**Test:** Run `./metadesk-client --npub <host_npub>` and confirm `on_transport` fires with the host's FIPS address before the TCP connect attempt.

### 2.3 `md_nostr_refresh_allowlist()`

Currently: stub.

Fix: subscribe to `{"kinds":[30000],"authors":["<our_pk>"],"#d":["metadesk-allowlist"]}`. On receipt, call `nostr_nip51_parse_list()` and store in `n->allowlist`. This can be fire-and-forget on startup — the callback already handles kind:30000 events if you add a case to `md_nostr_event_handler()`.

### 2.4 `md_nostr_allowlist_add()`

Currently: builds event, signs it, frees signed event without publishing.

Two fixes needed:

1. Serialize the allowlist entries as NIP-51 `["p", "<pubkey>", "<caps>"]` tags using `nostr_tags_new()` / `nostr_tag_new()` before signing
2. Publish via `nostr_relay_publish()` after signing, same pattern as 2.1

---

## 3. Group B — Session negotiation

### 3.1 Host startup sequence

Current `host/main.c` accepts a TCP connection immediately and starts streaming. The new sequence:

```
1. md_nostr_create() — connect to relays, load allowlist
2. md_nostr_publish_transport() — publish FIPS addr as kind:30078
3. Listen for NIP-17 DM session requests via on_dm callback
4. On receipt: validate JSON payload, check md_nostr_is_allowed()
5. If allowed: send NIP-17 DM accept, then accept TCP on port 7700
6. If not allowed: emit approval event (kind:30078 with type=approval_request)
```

For Phase 2.1, steps 3-6 can be simplified: the host accepts the first DM from any npub on the allowlist and proceeds. The approval flow for unknown npubs is Phase 2.3.

The `MD_PKT_SESSION_INFO` handler in `host/main.c` should:

- Parse the JSON payload (capabilities, tree_format, client npub)
- Set `session.tree_format` from the negotiated value
- Echo back with granted capabilities

### 3.2 Client startup sequence

Current `client/main.c` connects directly by IP or FIPS address with no session negotiation. The new `--npub` flow:

```
1. md_nostr_create()
2. md_nostr_subscribe_transport(host_npub) — wait for kind:30078
3. On on_transport callback: extract FIPS address
4. Send NIP-17 DM session request with capabilities + tree_format
5. Wait for NIP-17 DM session accept from host
6. Extract session_id from accept payload
7. md_stream_connect_fips(host_npub, port, timeout)
8. Send MD_PKT_SESSION_INFO with session_id + capabilities
9. Wait for MD_PKT_SESSION_INFO ack
10. Begin stream
```

The existing `--host IP` path stays unchanged — raw IP connect skips all of this.

### 3.3 Session JSON payloads

Session request (client → host, NIP-17 DM content):

```json
{
  "type": "session_request",
  "v": 1,
  "capabilities": ["video", "agent", "input"],
  "tree_format": "compact",
  "fips_addr": "npub1client..."
}
```

Session accept (host → client, NIP-17 DM content):

```json
{
  "type": "session_accept",
  "v": 1,
  "session_id": "<uuid>",
  "granted": ["video", "agent"]
}
```

Parse with cJSON — it's already a dependency.

---

## 4. Group C — Input forwarding (render.c)

### 4.1 Keyboard events

The `SDL_KEYDOWN` handler in `render.c` currently only checks for Escape. Add:

```c
case SDL_KEYDOWN:
case SDL_KEYUP: {
    if (event.key.keysym.sym == SDLK_ESCAPE && event.type == SDL_KEYDOWN) {
        r->open = false;
        return -1;
    }
    if (r->input_cb) {
        r->input_cb(MD_INPUT_KEY,
                    event.key.keysym.scancode,
                    event.type == SDL_KEYDOWN ? 1 : 0,
                    r->input_userdata);
    }
    break;
}
```

### 4.2 Mouse events

```c
case SDL_MOUSEMOTION:
    if (r->input_cb)
        r->input_cb(MD_INPUT_MOUSE_MOVE,
                    event.motion.x, event.motion.y,
                    r->input_userdata);
    break;
case SDL_MOUSEBUTTONDOWN:
case SDL_MOUSEBUTTONUP:
    if (r->input_cb)
        r->input_cb(MD_INPUT_MOUSE_BUTTON,
                    event.button.button,
                    event.type == SDL_MOUSEBUTTONDOWN ? 1 : 0,
                    r->input_userdata);
    break;
case SDL_MOUSEWHEEL:
    if (r->input_cb)
        r->input_cb(MD_INPUT_SCROLL,
                    event.wheel.x, event.wheel.y,
                    r->input_userdata);
    break;
```

### 4.3 Input callback wiring

Add to `MdRenderer`:

```c
typedef void (*MdInputCallback)(int type, int a, int b, void *userdata);
// type: MD_INPUT_KEY, MD_INPUT_MOUSE_MOVE, MD_INPUT_MOUSE_BUTTON, MD_INPUT_SCROLL

MdInputCallback input_cb;
void *input_userdata;
```

In `client/main.c`, register a callback that builds an `MdAction` and calls `md_stream_send(stream, MD_PKT_ACTION, ...)`. The host's existing `md_agent_handle_action()` then receives and injects it.

Note: mouse coordinates need to be scaled from client window pixels to host screen resolution. The host sends its screen dimensions in `MD_PKT_SESSION_INFO` — store them and apply the scale factor in the callback.

---

## 5. Open questions for this pass

**OQ-8** — `nostr_simple_pool_subscribe()` takes a `NostrFilters` struct by value. Confirm whether it copies the filter data or takes a reference before the call returns. If it takes a reference, the filter must outlive the subscription.

**OQ-9** — The `on_dm` callback in `nostr.c` currently decrypts gift-wrap using `md_signer_nip44_decrypt()`. Verify this works end-to-end against a real sharegap.net relay before wiring the session handshake — a broken DM decrypt will make the host appear to silently ignore session requests.

**OQ-10** — SDL2 mouse coordinates in HiDPI (Retina) mode are in points, not pixels. The backing pixel resolution may be 2x. Check `SDL_GetRendererOutputSize()` vs `SDL_GetWindowSize()` to get the actual scale factor before forwarding coordinates.

---

## 6. Test sequence for this milestone

Once all three groups are implemented:

```bash
# Terminal 1: host on T7610 (or localhost for initial test)
./metadesk-host --auto-signer --relay wss://relay.sharegap.net

# Terminal 2: client on MacBook
./metadesk-client --npub <T7610_npub> --relay wss://relay.sharegap.net

# Expected sequence visible in host logs:
# nostr: published transport addr fd00::xxxx
# nostr: received session request from npub1client...
# nostr: sent session accept
# host: client connected (session_id: xxxx)
# host: streaming...

# Expected sequence visible in client logs:
# nostr: found host transport addr fd00::xxxx
# nostr: sent session request
# nostr: received session accept (session_id: xxxx)
# client: connected via FIPS
# client: receiving stream...
```

The milestone is complete when this sequence runs end-to-end without raw IP, and mouse/keyboard input from the client window injects on the host machine.