# metadesk — Project Specification
**v0.1 — DRAFT**

FIPS-native remote desktop and agent control plane. Nostr keypairs as identity. AT-SPI2 semantic UI tree for agent clients. H.264/NVENC video for human clients. Zero central authority.

| Field | Value |
|---|---|
| Project | metadesk |
| Organisation | Soul Factory / OpenClaw |
| Status | Pre-implementation — specification phase |
| Phase 1 target | T7610 host → dev laptop client, LAN |
| Language | C/C++ (C++ with RAII, minimal templates) |
| Build system | Meson |

---

## 1. Purpose & Scope

metadesk is a FIPS-native remote desktop system with two parallel client modes: a human video client and an agent semantic client. It uses Nostr keypairs (secp256k1) as the sole identity and access control mechanism, with FIPS providing encrypted mesh transport. There are no accounts, no central servers, no shared secrets beyond the node nsec.

### 1.1 What This Project Is

- A screen capture and streaming host daemon for Linux
- A human video client (SDL2 + Dear ImGui, Linux-first)
- An agent semantic client consuming AT-SPI2 accessibility trees over FIPS
- A NAT traversal companion daemon (`fips-nat`) using STUN + Nostr signaling
- A Nostr-based session negotiation layer (NIP-44 encrypted DMs, NIP-51 allowlists)

### 1.2 What This Project Is Not

- A general-purpose VPN (FIPS handles that layer)
- A replacement for FIPS itself (metadesk runs on top of the FIPS TUN interface)
- A cross-platform product in Phase 1 or 2 (Linux only until dogfood gate)
- A vision-model computer use system (AT-SPI2 tree is the primary agent interface; screenshots are fallback only)

### 1.3 Non-Goals

- Audio streaming (Phase 1 and 2 scope excludes audio)
- Multi-monitor selection (Phase 3)
- File transfer (Phase 3)
- Windows or macOS support (Phase 3, contingent on dogfood outcome)

---

## 2. Architecture

### 2.1 Layer Diagram

```
┌────────────────────────────────────────────────────────────┐
│  Platform UI layer                                         │
│  Human: SDL2 window + Dear ImGui overlay                   │
│  Agent: structured JSON / compact-markup API               │
├────────────────────────────────────────────────────────────┤
│  C++ core  (libmetadesk — no UI, no platform deps)        │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────┐   │
│  │ Frame pipeline│  │ Agent channel│  │ Session / auth │   │
│  │ PipeWire      │  │ AT-SPI2 walk │  │ nostrc: NIP-44  │   │
│  │ libyuv        │  │ tree delta   │  │ nostrc: NIP-51  │   │
│  │ FFmpeg NVENC  │  │ uinput inject│  │ nostrc: NIP-17  │   │
│  └──────────────┘  └──────────────┘  └────────────────┘   │
├────────────────────────────────────────────────────────────┤
│  Transport                                                  │
│  BSD sockets → FIPS TUN fips0 (fd00::/8 ULA, SHA-256 of   │
│    pubkey → node_addr → IPv6 addr, see fips-ipv6-adapter)  │
│  DNS: npub1xxx.fips → fd00:: addr (primes identity cache)  │
│  fips-nat companion: STUN + Nostr hole-punch signaling      │
├────────────────────────────────────────────────────────────┤
│  Identity                                                   │
│  secp256k1 keypair (Nostr nsec/npub)                        │
│  FIPS addr: fd + SHA-256(pubkey)[0..15] → fd00::/8 ULA     │
│  Stored in 1Password Connect — never in config files        │
└────────────────────────────────────────────────────────────┘
```

### 2.2 Process Model

Three daemons run on the host machine. They are independent processes communicating via Unix domain sockets and shared configuration.

| Process | Role | Key Dependencies |
|---|---|---|
| `fipsd` | FIPS mesh daemon (reference impl in `fips/` workspace dir). Manages TUN `fips0`, peer auth (Noise IK), mesh routing (spanning tree + bloom filters), end-to-end sessions (Noise XK). See `fips/docs/design/` for canonical protocol docs. | Rust — run as system service |
| `metadesk-host` | Capture, encode, AT-SPI2 walk, session auth | PipeWire, FFmpeg, AT-SPI2, libsecp256k1 |
| `fips-nat` | NAT traversal: STUN + Nostr signaling | libnice, libwebsockets, libsecp256k1 |

### 2.3 Component Boundary

The C++ core (`libmetadesk`) exposes a callback interface upward to the UI layer and a command interface downward to the OS. No UI framework headers are included in the core. This boundary allows headless operation for agent-only deployments and enables future UI layer replacement without touching the pipeline.

```c
// Core → UI callbacks (host side)
on_session_request(npub, capabilities)  // → allow/deny
on_frame_encoded(buf, len, pts)

// Core → UI callbacks (client side)
on_frame_decoded(buf, width, height, pts)
on_ui_tree_update(tree_delta_json)
on_session_state_change(state)

// UI → Core commands
cmd_connect(npub)
cmd_send_action(action_type, target_id, payload)
cmd_request_screenshot(region)
cmd_disconnect()
```

---

## 3. Wire Formats

> **Stability note:** Wire formats defined here are versioned. The `version` field is mandatory in all packet types. Breaking changes require a version increment. Do not change formats between Phase 1 and Phase 2 without incrementing.

### 3.1 Packet Structure (Frame Channel)

All packets on the frame channel use a simple TLV header followed by payload. Sent over a FIPS-addressed TCP stream (FIPS provides encryption; no additional encryption at this layer).

```c
// Frame channel packet header (16 bytes, little-endian)
struct MdPacketHeader {
    uint8_t  version;      // protocol version, currently 1
    uint8_t  type;         // MdPacketType enum
    uint16_t flags;        // reserved, set to 0
    uint32_t payload_len;  // bytes following this header
    uint32_t sequence;     // monotonic sequence number
    uint32_t timestamp_ms; // capture timestamp
};

typedef enum {
    MD_PKT_VIDEO_FRAME   = 0x01,
    MD_PKT_ACTION        = 0x02,
    MD_PKT_UI_TREE       = 0x03,
    MD_PKT_UI_TREE_DELTA = 0x04,
    MD_PKT_SCREENSHOT    = 0x05,
    MD_PKT_PING          = 0x10,
    MD_PKT_PONG          = 0x11,
    MD_PKT_SESSION_INFO  = 0x20,
} MdPacketType;
```

### 3.2 Action Format (Agent → Host)

Actions are JSON-encoded, sent as `MD_PKT_ACTION` payload. JSON prioritises debuggability and LLM readability over throughput — actions are low-frequency.

```json
{
  "v": 1,
  "action": "click",
  "target_id": "node_42",
  "payload": {
    "text": "hello",
    "keys": ["ctrl", "s"],
    "dx": 0,
    "dy": 3,
    "region": [x, y, w, h]
  }
}
```

Valid action values: `click`, `dbl_click`, `right_click`, `type`, `key_combo`, `scroll`, `focus`, `set_value`, `screenshot`.

`target_id` is omitted for `key_combo`. `payload` fields are action-specific.

### 3.3 UI Tree Formats

Two formats are defined. The host sends whichever the client negotiated during session setup.

#### 3.3.1 Full tree (structured JSON) — `MD_PKT_UI_TREE`

```json
{
  "v": 1,
  "ts": 1700000000000,
  "root": {
    "id": "node_1",
    "role": "frame",
    "label": "gedit - untitled",
    "state": ["visible", "active"],
    "bounds": { "x": 0, "y": 0, "w": 1920, "h": 1080 },
    "children": []
  }
}
```

#### 3.3.2 Compact interactable list — token-efficient

```
v1 ts:1700000000000
WIN[1] gedit - untitled
  BTN[42] Save *enabled*
  BTN[43] Undo *enabled*
  TXT[44] <focused> 'Hello world...'
  MNU[45] File
  MNU[46] Edit
```

#### 3.3.3 Delta packets — `MD_PKT_UI_TREE_DELTA`

Delta packets carry only changed nodes using the same formats above, with an added `"op"` field: `"add"` | `"remove"` | `"update"`.

---

## 4. Session Negotiation

### 4.1 Flow

> **Nostr protocol model:** Nostr is an event-driven pub/sub protocol over persistent
> WebSocket connections (see NIP-01). Relay connections are stateful and last for the
> application's lifetime. Clients send `["REQ", sub_id, filter...]` to subscribe and
> `["EVENT", event]` to publish. Relays push matching events asynchronously, signal
> end-of-stored-events with `["EOSE", sub_id]`, and may close subscriptions with
> `["CLOSED", sub_id, reason]`. metadesk uses the nostrc `NostrSimplePool` which
> manages these connections, subscriptions, and message routing internally.

```
Client                                    Host
──────                                    ────

0. Both client and host maintain persistent WebSocket connections
   to their configured Nostr relays (via nostrc NostrSimplePool).
   Subscriptions are live for the app lifetime.

1. Client subscribes to kind:30078 from host pubkey:
   REQ [kinds:[30078], authors:[host_pk], #d:["fips-transport"]]
   Relay sends matching events; EOSE signals end of stored events.
   Client extracts FIPS transport address from event content.

2. Client sends NIP-17 gift-wrapped DM to host npub:
   (rumor kind:14 → seal kind:13 → gift-wrap kind:1059)
   { "type": "session_request",
     "v": 1,
     "capabilities": ["video","agent","input"],
     "tree_format": "compact",
     "fips_addr": "npub1client..." }  →

   Host has a live subscription for kind:1059 addressed to its pubkey.
   On receiving the gift-wrap, host unwraps via NIP-17 + NIP-44.

                                     ←   Check NIP-51 allowlist (kind:30000)
                                     ←   If not listed: emit approval event
                                         (human or bunker approves)

                                     ←   Host sends NIP-17 gift-wrapped DM:
                                         { "type": "session_accept",
                                           "session_id": "<uuid>",
                                           "fips_addr": "npub1host...",
                                           "granted": ["video","agent"] }

3. Open TCP connection to host
   fd00::npub1host...:7700           →
   Send MD_PKT_SESSION_INFO          →
                                     ←   MD_PKT_SESSION_INFO (ack)
   Streaming begins
```

### 4.2 Access Control

- Host maintains a NIP-51 list (kind:30000, addressable) of authorised client npubs. As an addressable event, relays retain only the latest version for the (pubkey, kind, d-tag) tuple.
- List entries carry a `caps` tag limiting granted capabilities per npub
- Unlisted npubs trigger an approval event; host daemon emits a kind:30078 event that the UI or bunker listens for
- Session tokens are NIP-44 encrypted via NIP-17 gift-wrapping; relay cannot read session content
- Revocation is immediate: host publishes updated NIP-51 list (replacing the old one on relays), active sessions receive a disconnect packet within one keepalive interval
- The host subscribes to its own allowlist (REQ kind:30000) to stay in sync with edits from other clients (e.g. bunker approval)

### 4.3 Ports and Transport

> **FIPS transport model:** metadesk runs on top of the FIPS TUN interface (`fips0`).
> Applications use standard BSD sockets to `fd00::/8` addresses — the FIPS IPv6 adapter
> (FSP port 256) handles address derivation, header compression, and MTU enforcement
> transparently. DNS resolution via `npub1xxx.fips` primes the identity cache so FIPS
> can route packets. FIPS provides hop-by-hop (Noise IK) and end-to-end (Noise XK)
> encryption — metadesk does not add its own encryption layer. The effective IPv6 MTU
> is `transport_mtu - 77` (FIPS_IPV6_OVERHEAD). See `fips/docs/design/fips-ipv6-adapter.md`.

| Port | Protocol | Purpose |
|---|---|---|
| 7700 | TCP over FIPS TUN (`fips0`) | Frame channel (video + agent tree + actions) |
| 7701 | UDP over FIPS TUN (`fips0`) | Reserved for future low-latency action channel |
| 2121 | UDP | FIPS daemon transport (default, configurable) |

---

## 5. Dependencies

> **Version policy:** All versions below are minimum versions tested against the T7610 Ubuntu 24.04 environment. Vendor or pin only if a system package is unavailable or outdated.

| Library | Min Version | Role | Acquire |
|---|---|---|---|
| `libpipewire-0.3` | ≥ 0.3.65 | Screen capture via PipeWire portal (DMA-BUF) | `apt: libpipewire-0.3-dev` |
| `libavcodec` / FFmpeg | ≥ 6.0 | H.264 encode (NVENC) and decode | `apt: libavcodec-dev libavutil-dev` |
| `libyuv` | ≥ r1845 | Colorspace conversion (I420, NV12, RGBA) | apt or vendor from chromium/libyuv |
| `libnostr` (nostrc) | HEAD | Nostr event types, relay connections, key management | github.com/chebizarro/nostrc — CMake, links libwebsockets + libsecp256k1 |
| `nip44` (nostrc) | HEAD | NIP-44 v2 encryption/decryption for session DMs | Part of nostrc nips/ — links libsodium |
| `nip51` (nostrc) | HEAD | NIP-51 categorized people lists (allowlists) | Part of nostrc nips/ |
| `nip17` (nostrc) | HEAD | NIP-17 gift-wrapped private DMs | Part of nostrc nips/ |
| `libnice` | ≥ 0.1.21 | STUN/TURN/ICE for fips-nat | `apt: libnice-dev` |
| `libatspi-2.0` | ≥ 2.48 | AT-SPI2 accessibility tree walking | `apt: libatspi2.0-dev` |
| `libudev` | ≥ 252 | uinput device creation for input injection | `apt: libudev-dev` |
| `SDL2` | ≥ 2.28 | Human client frame display window | `apt: libsdl2-dev` |
| Dear ImGui | ≥ 1.90 | Human client overlay UI | vendor as submodule (header-only) |
| `cJSON` | ≥ 1.7.17 | JSON encode/decode for wire formats | `apt: libcjson-dev` |
| `libb2` / blake2 | ≥ 0.98 | Session ID generation | `apt: libb2-dev` |

> **nostrc dependency:** The `nostrc` library (github.com/chebizarro/nostrc) provides all Nostr protocol primitives. metadesk does NOT implement its own Nostr event handling, key generation, encryption, relay connections, or NIP logic. Instead, metadesk links against `libnostr`, `nip44`, `nip51`, and `nip17` from the nostrc workspace. nostrc is a CMake project; metadesk integrates it via Meson subproject or system install.

### 5.1 Build System

Meson with a single top-level `meson.build`. Subprojects for vendored dependencies (Dear ImGui). pkg-config for system libraries. The build must succeed with only system packages on Ubuntu 24.04 LTS; vendoring is a fallback, not default.

```bash
# Target build invocation
meson setup build --buildtype=debugoptimized
ninja -C build

# Produces:
#   build/metadesk-host    — host daemon
#   build/metadesk-client  — human video client
#   build/fips-nat         — NAT traversal daemon
#   build/libmetadesk.so   — shared core library
```

---

## 6. Directory Structure

```
metadesk/
├── meson.build
├── meson_options.txt
├── SPEC.md
├── src/
│   ├── core/                   # libmetadesk (no UI, no platform deps)
│   │   ├── capture.c/h         # PipeWire capture
│   │   ├── encode.c/h          # FFmpeg NVENC pipeline
│   │   ├── decode.c/h          # FFmpeg decode
│   │   ├── atspi.c/h           # AT-SPI2 tree walker + delta
│   │   ├── input.c/h           # uinput injection
│   │   ├── session.c/h         # session state machine
│   │   ├── nostr_bridge.c/h    # Thin bridge to nostrc (libnostr, nip44, nip51, nip17)
│   │   ├── packet.c/h          # wire format encode/decode
│   │   └── secrets.c/h         # 1Password Connect integration
│   ├── host/                   # metadesk-host daemon
│   │   └── main.c
│   ├── client/                 # metadesk-client (human)
│   │   ├── main.c
│   │   ├── render.c/h          # SDL2 frame display
│   │   └── ui.cpp/h            # Dear ImGui overlay
│   └── fips-nat/               # NAT traversal daemon
│       ├── main.c
│       ├── stun.c/h            # libnice integration
│       ├── publish.c/h         # Nostr address publication
│       └── punch.c/h           # hole punch coordinator
├── subprojects/
│   └── imgui/                  # Dear ImGui vendored
├── tests/
│   ├── test_packet.c           # wire format round-trip tests
│   ├── test_atspi.c            # tree serialisation tests
│   └── test_nostr.c            # NIP-44 encrypt/decrypt tests
├── config/
│   └── metadesk.toml.example   # example config (no secrets)
└── docs/
    └── AGENT_API.md            # agent client integration guide
```

---

## 7. Secret Storage

> **Hard rule:** No secrets in config files, environment variables, or on-disk key files. All cryptographic material is retrieved at startup from 1Password Connect and held in locked memory (`mlock`). The config file contains only the 1Password Connect URL and the item reference path.

| Secret | 1Password Item | Used By |
|---|---|---|
| FIPS node nsec | `op://metadesk/fips-node/nsec` | metadesk-host, fips-nat |
| 1Password Connect token | `op://metadesk/1pc/token` | secrets.c bootstrap only |

---

## 8. Roadmap

### 8.1 Phase Summary

| Phase | Goal | Timeline |
|---|---|---|
| 1 | PoC — T7610 host to dev laptop, LAN only | 3–6 weeks |
| 2 | Dogfood — OpenClaw agent fleet, NAT traversal, fips-nat | 2–3 months |
| 3 | Product — multi-platform, external users (contingent) | TBD |

### 8.2 Phase 1 Milestones

- **1.1** PipeWire capture — single frame to disk, DMA-BUF path confirmed on P40
- **1.2** Encode/decode round-trip — NVENC → H.264 → SDL2 display on localhost
- **1.3** Raw socket streaming — UDP packetizer, latency measurement
- **1.4** FIPS integration — replace raw socket with `fd00::npub` IPv6 peer address
- **1.5** Basic input forwarding — uinput mouse + keyboard keysym injection
- **1.6** AT-SPI2 tree walker — serialize full tree, send as `MD_PKT_UI_TREE`
- **1.7** Agent action handler — receive `MD_PKT_ACTION`, inject via uinput/AT-SPI2
- **1.8** Dear ImGui overlay — latency display, connection status, disconnect button

### 8.3 Phase 2 Milestones

- **2.1** Nostr session signaling — NIP-44 request/accept, NIP-51 allowlist, CLI connect tool
- **2.2** fips-nat daemon — STUN address discovery, Nostr transport publication, UDP hole punch, TURN fallback via sharegap.net relay node
- **2.3** Agent monitoring mode — headless host, auto-accept allowlisted npubs, signed Nostr session log
- **2.4** Adaptive bitrate — RTT feedback loop to NVENC bitrate target
- **2.5** UI session manager — peer list, allowlist management, approval popup
- **2.6** Dogfood gate — 30 days on OpenClaw fleet, all discovered bugs resolved

### 8.4 Phase 3 Milestones (contingent)

- **3.1** macOS client — ScreenCaptureKit + AppKit/Objective-C++ UI layer
- **3.2** Windows client — DXGI Desktop Duplication + Win32 UI layer
- **3.3** NIP-46 bunker integration — approval delegated to signing device
- **3.4** Multi-monitor and display selection
- **3.5** File transfer over FIPS session channel
- **3.6** Hardening — packet parser fuzzing, Debian/RPM packaging, release signing

---

## 9. Encoder Configuration

Phase 1 and 2 assume NVENC is available on the host (T7610 with P40). Software fallback (libx264 with ultrafast preset) must compile and link but is not the performance target.

```
# Target NVENC parameters (FFmpeg AVCodecContext)
codec_id        = AV_CODEC_ID_H264
pix_fmt         = AV_PIX_FMT_CUDA    # stay on GPU, no copy
bit_rate        = 8000000             # 8 Mbps initial, adaptive
gop_size        = 0                   # no keyframes; use intra refresh
max_b_frames    = 0                   # no B-frames (latency)

# NVENC private options
preset          = p1                  # lowest latency
tune            = ull                 # ultra-low latency
rc              = cbr                 # constant bitrate
intra-refresh   = 1
b_adapt         = 0
zerolatency     = 1
```

### 9.1 Latency Budget

| Stage | Target | Notes |
|---|---|---|
| PipeWire capture → DMA-BUF | < 2ms | Portal latency on idle desktop |
| libyuv colorspace conversion | < 1ms | SIMD path, GPU frame stays on device |
| NVENC encode | < 5ms | P40, 1080p, ULL preset |
| Network (LAN) | < 1ms | Phase 1 target environment |
| FFmpeg decode (client) | < 5ms | Software decode on laptop acceptable |
| SDL2 present | < 2ms | vsync disabled in Phase 1 |
| **Total** | **< 16ms** | **One frame at 60fps — Phase 1 goal** |

---

## 10. Agent Client API

### 10.1 Connection

An agent connects by initiating a standard metadesk session with capability `agent` declared. The host responds with the negotiated tree format. Video capability is optional for agent clients — a pure agent session carries only the UI tree and action channels, no video stream.

### 10.2 Interaction Loop

```
1. Connect to host npub via fips-nat-resolved address
2. Negotiate: capabilities=["agent"], tree_format="compact"
3. Receive MD_PKT_UI_TREE (full tree on connect)
4. Agent reasons about tree, emits MD_PKT_ACTION
5. Host injects action, waits for AT-SPI2 change event
6. Host emits MD_PKT_UI_TREE_DELTA
7. Agent receives delta, updates local tree model, repeat

// Screenshot fallback (when tree is insufficient):
Agent emits: { "action": "screenshot", "region": [x, y, w, h] }
Host responds: MD_PKT_SCREENSHOT (annotated JPEG, node IDs overlaid)
```

### 10.3 Accessibility Coverage Tiers

| Tier | Applications | Agent Strategy |
|---|---|---|
| Full tree | GTK4, Qt, Electron, web browsers | AT-SPI2 tree only — no screenshots needed |
| Partial tree | Some Java apps, legacy GTK2 | Tree + targeted region screenshots |
| No tree | Games, custom GPU UIs | Full screenshot fallback, vision model |

---

## 11. Open Questions

> These questions are intentionally deferred until Phase 1 empirical results are available. Do not resolve by assumption.

- **OQ-1** AT-SPI2 change events vs polling — does subscribing to AT-SPI2 state-change events give sufficient fidelity, or is periodic full-tree polling required for some applications?

- **OQ-2** GPU frame path — can a DMA-BUF handle be passed directly to NVENC without a CPU copy on the T7610/P40 hardware combination? Requires empirical test in Milestone 1.1.

- **OQ-3** fips-nat integration point — should fips-nat write directly to the FIPS daemon config and signal reload, or does FIPS v0.1.x expose a dynamic peer API via `fipsctl`?

- **OQ-4** Nostr relay selection — which relays should be configured as defaults for session signaling? Should metadesk run its own relay at sharegap.net?

- **OQ-5** Tree format negotiation — is it worth supporting both compact and JSON formats simultaneously, or should compact be the only agent format?

---

*metadesk project specification — Soul Factory / OpenClaw — DRAFT — not for external distribution*
