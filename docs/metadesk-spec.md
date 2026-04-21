# metadesk — Project Specification
**v0.2 — DRAFT**

FIPS-native remote desktop and agent control plane. Nostr keypairs as identity. Platform-native accessibility trees for agent clients. H.264 video for human clients. Zero central authority.

| Field | Value |
|---|---|
| Project | metadesk |
| Organisation | Soul Factory / OpenClaw |
| Status | Phase 2 — implementation in progress |
| Phase 1 target | T7610 host (Linux) → dev laptop client (macOS), LAN |
| Language | C17/C++17 (C++ only for Dear ImGui overlay; Objective-C for macOS backends) |
| Build system | Meson |
| Target platforms | Linux, macOS, Windows |

---

## 1. Purpose & Scope

metadesk is a FIPS-native remote desktop system with two parallel client modes: a human video client and an agent semantic client. It uses Nostr keypairs (secp256k1) as the sole identity and access control mechanism, with FIPS providing encrypted mesh transport. There are no accounts, no central servers, no shared secrets beyond the node nsec.

The codebase is cross-platform from day one. Platform-specific functionality (screen capture, accessibility trees, input injection) is isolated behind stable C interfaces with per-platform backend implementations selected at compile time.

### 1.1 What This Project Is

- A screen capture and streaming host daemon (Linux, macOS, Windows)
- A human video client (SDL2 + Dear ImGui, all platforms)
- An agent semantic client consuming native accessibility trees over FIPS
- A NAT traversal companion daemon (`fips-nat`) using STUN + Nostr signaling
- A Nostr-based session negotiation layer (NIP-44 encrypted DMs, NIP-51 allowlists)

### 1.2 What This Project Is Not

- A general-purpose VPN (FIPS handles that layer)
- A replacement for FIPS itself (metadesk runs on top of the FIPS TUN interface)
- A vision-model computer use system (native accessibility tree is the primary agent interface; screenshots are fallback only)

### 1.3 Non-Goals

- Audio streaming (deferred to a later phase)
- Multi-monitor selection (Phase 3)
- File transfer (Phase 3)

---

## 2. Architecture

### 2.1 Layer Diagram

```
┌────────────────────────────────────────────────────────────┐
│  Platform UI layer                                         │
│  Human: SDL2 window + Dear ImGui overlay (all platforms)   │
│  Agent: structured JSON / compact-markup API               │
├────────────────────────────────────────────────────────────┤
│  C17 core  (libmetadesk — no UI deps)                      │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────┐   │
│  │ Frame pipeline│  │ Agent channel│  │ Session / auth │   │
│  │ capture HAL  │  │ a11y HAL     │  │ NIP-44 DMs     │   │
│  │ libyuv        │  │ tree delta   │  │ NIP-51 allowlst│   │
│  │ FFmpeg        │  │ input HAL    │  │ libsecp256k1   │   │
│  └──────────────┘  └──────────────┘  └────────────────┘   │
├────────────────────────────────────────────────────────────┤
│  Platform HAL backends (compile-time selected)             │
│  Capture:  PipeWire (Linux) │ ScreenCaptureKit (macOS)     │
│            DXGI (Windows)                                   │
│  A11y:     AT-SPI2 (Linux) │ AXUIElement (macOS)           │
│            UI Automation (Windows)                          │
│  Input:    uinput (Linux)  │ CGEvent (macOS)                │
│            SendInput (Windows)                              │
├────────────────────────────────────────────────────────────┤
│  Transport                                                  │
│  BSD sockets → FIPS TUN (fd00::/8 npub-derived IPv6)       │
│  fips-nat companion: STUN + Nostr hole-punch signaling      │
├────────────────────────────────────────────────────────────┤
│  Identity                                                   │
│  secp256k1 keypair (Nostr nsec/npub)                        │
│  Stored in 1Password Connect — never in config files        │
└────────────────────────────────────────────────────────────┘
```

### 2.2 Process Model

Three daemons run on the host machine. They are independent processes communicating via Unix domain sockets (Linux/macOS) or named pipes (Windows) and shared configuration.

| Process | Role | Key Dependencies |
|---|---|---|
| `fipsd` | FIPS mesh daemon (upstream project) | Rust — run as system service |
| `metadesk-host` | Capture, encode, a11y walk, session auth | FFmpeg + platform capture/a11y/input backends |
| `fips-nat` | NAT traversal: STUN + Nostr signaling | libnice, libwebsockets, libsecp256k1 |

### 2.3 Platform HAL Interfaces

Three hardware abstraction interfaces isolate all platform-specific code. No platform headers appear outside their respective backend files.

#### 2.3.1 Capture HAL

```c
// src/core/capture.h
typedef struct MdCaptureBackend {
    int   (*init)(MdCaptureCtx *ctx, MdCaptureConfig *cfg);
    int   (*start)(MdCaptureCtx *ctx);
    int   (*get_frame)(MdCaptureCtx *ctx, MdFrame *out);
    void  (*release_frame)(MdCaptureCtx *ctx, MdFrame *frame);
    void  (*stop)(MdCaptureCtx *ctx);
    void  (*destroy)(MdCaptureCtx *ctx);
} MdCaptureBackend;

// Implemented by each backend:
MdCaptureBackend *md_capture_backend_create(void);
```

| Platform | Backend file | API used |
|---|---|---|
| Linux | `capture_pipewire.c` | PipeWire xdg-desktop-portal, DMA-BUF |
| macOS | `capture_screencapturekit.m` | ScreenCaptureKit (macOS 13+), Objective-C++ |
| Windows | `capture_dxgi.cpp` | DXGI Desktop Duplication, Direct3D 11 |

#### 2.3.2 Accessibility Tree HAL

```c
// src/core/a11y.h
typedef struct MdA11yBackend {
    int   (*init)(MdA11yCtx *ctx);
    int   (*get_tree)(MdA11yCtx *ctx, MdA11yTree *out);
    int   (*subscribe_changes)(MdA11yCtx *ctx, MdA11yChangeCb cb, void *userdata);
    void  (*destroy)(MdA11yCtx *ctx);
} MdA11yBackend;

MdA11yBackend *md_a11y_backend_create(void);
```

| Platform | Backend file | API used |
|---|---|---|
| Linux | `a11y_atspi.c` | AT-SPI2 (`libatspi-2.0`) |
| macOS | `a11y_axui.m` | `AXUIElement` / NSAccessibility (Objective-C++) |
| Windows | `a11y_uia.cpp` | UI Automation (`UIAutomationClient`) |

#### 2.3.3 Input Injection HAL

```c
// src/core/input.h
typedef struct MdInputBackend {
    int   (*init)(MdInputCtx *ctx);
    int   (*mouse_move)(MdInputCtx *ctx, int x, int y);
    int   (*mouse_button)(MdInputCtx *ctx, int button, int pressed);
    int   (*mouse_scroll)(MdInputCtx *ctx, int dx, int dy);
    int   (*key_event)(MdInputCtx *ctx, uint32_t keysym, int pressed);
    int   (*type_text)(MdInputCtx *ctx, const char *utf8);
    void  (*destroy)(MdInputCtx *ctx);
} MdInputBackend;

MdInputBackend *md_input_backend_create(void);
```

| Platform | Backend file | API used |
|---|---|---|
| Linux | `input_uinput.c` | uinput (`/dev/uinput`) via ioctl |
| macOS | `input_cgevent.m` | `CGEventCreateMouseEvent` / `CGEventCreateKeyboardEvent` |
| Windows | `input_sendinput.cpp` | `SendInput` |

### 2.4 Component Boundary

The C++ core (`libmetadesk`) exposes a callback interface upward to the UI layer and calls downward only through the HAL interfaces. No platform headers appear in core headers. This allows headless operation for agent-only deployments and future UI layer replacement without touching the pipeline.

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

Two formats are defined. The host sends whichever the client negotiated during session setup. Both formats are platform-agnostic — the HAL normalises all platform accessibility APIs into a common node model before serialisation.

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

```
Client                                    Host
──────                                    ────
1. Query Nostr relay for host npub
   kind:30078 tag:d=fips-transport   →
                                     ←   Current FIPS transport address

2. Send NIP-44 DM to host npub:
   { "type": "session_request",
     "v": 1,
     "capabilities": ["video","agent","input"],
     "tree_format": "compact",
     "fips_addr": "npub1client..." }  →

                                     ←   Check NIP-51 allowlist
                                     ←   If not listed: emit approval event
                                         (human or bunker approves)

                                     ←   { "type": "session_accept",
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

- Host maintains a NIP-51 list (kind:30000) of authorised client npubs
- List entries carry a `caps` tag limiting granted capabilities per npub
- Unlisted npubs trigger an approval event; host daemon emits a kind:30078 event that the UI or bunker listens for
- Session tokens are NIP-44 encrypted; relay cannot read session content
- Revocation is immediate: remove from NIP-51 list, active sessions receive a disconnect packet within one keepalive interval

### 4.3 Ports

| Port | Protocol | Purpose |
|---|---|---|
| 7700 | TCP over FIPS | Frame channel (video + agent tree + actions) |
| 7701 | UDP over FIPS | Reserved for future low-latency action channel |
| 2121 | UDP | FIPS daemon transport (upstream default) |

---

## 5. Dependencies

Dependencies are grouped by scope: cross-platform (required on all targets) and platform-specific (compiled only on the relevant platform).

> **Version policy:** All versions below are minimum versions tested against the Phase 1 target environments (T7610 Ubuntu 24.04, macOS 13 Ventura). Vendor or pin only if a system package is unavailable or outdated.

### 5.1 Cross-Platform Dependencies

Required on Linux, macOS, and Windows.

| Library | Min Version | Role | Notes |
|---|---|---|---|
| FFmpeg (`libavcodec`, `libavutil`) | ≥ 6.0 | H.264 encode and decode | Homebrew / vcpkg on non-Linux |
| `libyuv` | ≥ r1845 | Colorspace conversion | vendor from chromium/libyuv if no package |
| `nostrc` | — | Nostr C library: events, keys, relay pool, NIP-44/17/51 | github.com/chebizarro/nostrc |
| `libgo` | — | Go-style concurrency runtime (channels, goroutines, waitgroups) | bundled with nostrc |
| `libsecp256k1` | ≥ 0.3.2 | Nostr keypair ops, NIP-44 ECDH | Transitive via nostrc |
| `libnice` | ≥ 0.1.21 | STUN/TURN/ICE for fips-nat | Homebrew: `libnice` |
| `SDL2` | ≥ 2.28 | Human client frame display | Homebrew: `sdl2` |
| Dear ImGui | ≥ 1.90 | Human client overlay UI | vendor as submodule |
| `cJSON` | ≥ 1.7.17 | JSON encode/decode for wire formats | Homebrew: `cjson` |
| `libb2` / blake2 | ≥ 0.98 | Session ID generation | Homebrew: `b2-sum` or vendor |

### 5.2 Platform-Specific Dependencies

#### Linux

| Library | Min Version | Role | Acquire |
|---|---|---|---|
| `libpipewire-0.3` | ≥ 0.3.65 | Screen capture (capture HAL backend) | `apt: libpipewire-0.3-dev` |
| `libatspi-2.0` | ≥ 2.48 | Accessibility tree (a11y HAL backend) | `apt: libatspi2.0-dev` |
| `libudev` | ≥ 252 | uinput device creation (input HAL backend) | `apt: libudev-dev` |

#### macOS

| Framework | Min OS | Role | Notes |
|---|---|---|---|
| ScreenCaptureKit | macOS 13.0 | Screen capture (capture HAL backend) | System framework, no install needed |
| NSAccessibility / AXUIElement | macOS 10.10 | Accessibility tree (a11y HAL backend) | System framework, no install needed |
| CGEvent / CoreGraphics | macOS 10.6 | Input injection (input HAL backend) | System framework, no install needed |

macOS backend files are Objective-C++ (`.m` / `.mm`). Meson must enable `objcpp` for these targets.

> **macOS permission requirements:** ScreenCaptureKit requires Screen Recording permission. AXUIElement requires Accessibility permission. Both must be granted to `metadesk-host` in System Settings. For headless daemon use, see OQ-6 and OQ-7.

#### Windows

| API | Min OS | Role | Notes |
|---|---|---|---|
| DXGI Desktop Duplication | Windows 8 | Screen capture (capture HAL backend) | System API, no install needed |
| UI Automation | Windows 7 | Accessibility tree (a11y HAL backend) | System API, no install needed |
| `SendInput` (Win32) | Windows XP | Input injection (input HAL backend) | System API, no install needed |

### 5.3 Meson Platform Selection

```meson
# Platform HAL backend selection
if host_machine.system() == 'linux'
  capture_src  = 'src/core/capture_pipewire.c'
  capture_deps = [dependency('libpipewire-0.3')]
  a11y_src     = 'src/core/a11y_atspi.c'
  a11y_deps    = [dependency('atspi-2')]
  input_src    = 'src/core/input_uinput.c'
  input_deps   = [dependency('libudev')]

elif host_machine.system() == 'darwin'
  add_languages('objcpp', required: true)
  capture_src  = 'src/core/capture_screencapturekit.m'
  capture_deps = []
  a11y_src     = 'src/core/a11y_axui.m'
  a11y_deps    = []
  input_src    = 'src/core/input_cgevent.m'
  input_deps   = []
  add_project_link_arguments(
    '-framework', 'ScreenCaptureKit',
    '-framework', 'CoreGraphics',
    '-framework', 'AppKit',
    language: ['c', 'cpp', 'objcpp']
  )

elif host_machine.system() == 'windows'
  capture_src  = 'src/core/capture_dxgi.cpp'
  capture_deps = []
  a11y_src     = 'src/core/a11y_uia.cpp'
  a11y_deps    = []
  input_src    = 'src/core/input_sendinput.cpp'
  input_deps   = []
endif
```

### 5.4 Build Invocation

```bash
# Linux
meson setup build --buildtype=debugoptimized
ninja -C build

# macOS (Homebrew deps)
PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig" \
  meson setup build --buildtype=debugoptimized
ninja -C build

# Produces (all platforms):
#   build/metadesk-host    — host daemon
#   build/metadesk-client  — human video client
#   build/fips-nat         — NAT traversal daemon
#   build/libmetadesk.so   — shared core (.dylib on macOS, .dll on Windows)
```

---

## 6. Directory Structure

```
metadesk/
├── meson.build
├── meson_options.txt
├── src/
│   ├── core/                           # libmetadesk (no UI, no platform headers in .h files)
│   │   ├── capture.c/h                 # capture HAL interface
│   │   ├── capture_pipewire.c          # Linux backend (PipeWire)
│   │   ├── capture_screencapturekit.m  # macOS backend (ScreenCaptureKit)
│   │   ├── capture_dxgi.cpp            # Windows backend (DXGI)
│   │   ├── a11y.c/h                    # accessibility tree HAL + compact/JSON serializers
│   │   ├── a11y_atspi.c               # Linux backend (AT-SPI2)
│   │   ├── a11y_axui.m                # macOS backend (AXUIElement)
│   │   ├── a11y_uia.cpp              # Windows backend (UI Automation)
│   │   ├── input.c/h                   # input injection HAL interface
│   │   ├── input_uinput.c             # Linux backend (uinput)
│   │   ├── input_cgevent.m            # macOS backend (CGEvent)
│   │   ├── input_sendinput.cpp        # Windows backend (SendInput)
│   │   ├── encode.c/h                 # FFmpeg encode pipeline (cross-platform)
│   │   ├── decode.c/h                 # FFmpeg decode (cross-platform)
│   │   ├── bitrate_ctrl.c/h           # AIMD adaptive bitrate controller
│   │   ├── session.c/h               # session state machine + JSON payloads
│   │   ├── session_log.c/h           # signed Nostr session event log (M2.4)
│   │   ├── nostr.c/h                 # NIP-17 DMs, NIP-51 allowlist, relay pool
│   │   ├── signer.c/h                # pluggable signer abstraction (direct-key, NIP-46, NIP-55L, NIP-5F)
│   │   ├── agent.c/h                 # agent action handler + tree serializer
│   │   ├── action.c/h                # action parse/encode (click, type, scroll, key)
│   │   ├── packet.c/h                # wire format encode/decode
│   │   ├── stream.c/h                # TCP stream transport + keepalive
│   │   ├── ipc.h + ipc_unix.c/ipc_win32.c  # Unix/Win32 domain socket IPC
│   │   ├── jsonrpc.c/h               # JSON-RPC 2.0 message layer
│   │   ├── mcp_server.c/h            # MCP server core (tool/resource registry)
│   │   ├── mcp_tools.c/h             # MCP tool implementations
│   │   ├── mcp_resources.c/h         # MCP resource implementations
│   │   ├── mcp_bridge.c/h            # MCP bridge (a11y + input → MCP server)
│   │   ├── mcp_http.c/h              # MCP HTTP+SSE transport
│   │   ├── mcp_stdio.c/h             # MCP stdio transport
│   │   ├── fips_addr.c/h             # FIPS fd00::/8 address derivation
│   │   ├── secrets.c/h               # 1Password Connect integration
│   │   └── platform.h                # platform detection macros
│   ├── host/                          # metadesk-host daemon
│   │   └── main.c
│   ├── client/                        # metadesk-client (human video client)
│   │   ├── main.c
│   │   ├── render.c/h                # SDL2 frame display + HiDPI scaling
│   │   └── ui.cpp/h                  # Dear ImGui overlay (peer list, allowlist, approval)
│   └── fips-nat/                      # NAT traversal daemon
│       ├── main.c
│       ├── stun.c/h                  # RFC 5389 STUN binding discovery
│       ├── punch.c/h                 # UDP hole punch coordinator
│       ├── turn.c/h                  # RFC 5766 TURN relay client
│       ├── publish.c/h              # Nostr NAT endpoint publication
│       └── fipsnat_ipc.c/h          # IPC protocol for host ↔ fips-nat
├── subprojects/
│   └── imgui/                         # Dear ImGui vendored
├── tests/                             # 25+ unit test executables
│   ├── test_packet.c                  # wire format round-trip
│   ├── test_atspi.c                   # a11y tree serialisation + compact format + delta patching
│   ├── test_nostr.c                   # NIP-44 encrypt/decrypt + allowlist
│   ├── test_session.c                 # session JSON + state machine + tree format negotiation
│   ├── test_session_log.c             # signed session log ring buffer
│   ├── test_signer.c                  # signer abstraction (+ NIP-46/55L/5F integration tests)
│   ├── test_agent.c                   # agent action handler
│   ├── test_action.c                  # action parse/encode
│   ├── test_stream.c                  # TCP stream transport
│   ├── test_encode.c                  # encode/decode round-trip
│   ├── test_input.c                   # input injection
│   ├── test_capture.c                 # capture convenience API
│   ├── test_stun.c                    # STUN binding discovery
│   ├── test_punch.c                   # UDP hole punch
│   ├── test_turn.c                    # TURN relay client
│   ├── test_publish.c                 # NAT endpoint publication
│   ├── test_bitrate_ctrl.c            # AIMD bitrate controller
│   ├── test_mcp.c                     # MCP server core
│   ├── test_mcp_stdio.c              # MCP stdio transport
│   ├── test_mcp_http.c               # MCP HTTP+SSE transport
│   ├── test_jsonrpc.c                # JSON-RPC 2.0 messages
│   ├── test_ipc.c                    # Unix domain sockets
│   ├── test_fipsnat_ipc.c            # fips-nat IPC protocol
│   ├── test_fips_addr.c              # FIPS address derivation
│   └── test_secrets.c                # 1Password Connect secrets
├── tools/
│   ├── capture_frame.c               # single-frame capture utility
│   ├── encode_roundtrip.c            # encode/decode benchmark
│   └── atspi_dump.c                  # a11y tree dump utility
└── docs/
    ├── metadesk-spec.md              # this document
    └── metadesk-spec-phase-2.1.md    # Phase 2.1 implementation spec (archived)
```

---

## 7. Secret Storage & Signer Abstraction

> **Hard rule:** No secrets in config files, environment variables, or on-disk key files. Cryptographic material is managed through a pluggable signer abstraction (`signer.h`) that supports multiple backends without exposing keys to metadesk.

### 7.1 Signer Backends

| Backend | Key Location | Transport | Use Case |
|---|---|---|---|
| `direct-key` | In-process memory | None (local secp256k1) | 1Password Connect retrieval, testing |
| `NIP-46` | Remote bunker | Relay (kind:24133 RPC) | Signing delegation to remote device |
| `NIP-55L` | Local daemon | D-Bus IPC | Linux desktop signer daemon |
| `NIP-5F` | Local daemon | Unix domain socket | Cross-platform local signer |

All signer operations (get_pubkey, sign_event, nip44_encrypt, nip44_decrypt) dispatch through a vtable. Remote backends never expose the key to metadesk. Auto-detection tries NIP-5F → NIP-55L in order.

### 7.2 1Password Connect (Legacy Fallback)

When no signer backend is available, the direct-key backend retrieves the nsec from 1Password Connect at startup and holds it in locked memory.

| Secret | 1Password Item | Used By |
|---|---|---|
| FIPS node nsec | `op://metadesk/fips-node/nsec` | metadesk-host, fips-nat |
| 1Password Connect token | `op://metadesk/1pc/token` | secrets.c bootstrap only |

---

## 8. Roadmap

### 8.1 Phase Summary

| Phase | Goal | Timeline |
|---|---|---|
| 1 | PoC — T7610 Linux host, macOS laptop client, LAN | 3–6 weeks |
| 2 | Dogfood — OpenClaw agent fleet, NAT traversal, fips-nat | 2–3 months |
| 3 | Product — Windows support, external users (contingent) | TBD |

### 8.2 Phase 1 Milestones ✅

All Phase 1 milestones are complete.

- ✅ **1.1** Meson scaffold — builds cleanly on Linux and macOS, all cross-platform deps linked, HAL stub implementations compile on both platforms
- ✅ **1.2** Linux capture — PipeWire portal frame to disk, DMA-BUF path confirmed on P40
- ✅ **1.3** macOS capture — ScreenCaptureKit frame to disk on dev laptop
- ✅ **1.4** Encode/decode round-trip — FFmpeg H.264 encode → SDL2 display, latency measured on both platforms
- ✅ **1.5** Raw socket streaming — UDP packetizer, frame delivery Linux → macOS over LAN
- ✅ **1.6** FIPS integration — replace raw socket with `fd00::npub` IPv6 peer address
- ✅ **1.7** Linux input forwarding — uinput mouse + keyboard keysym injection
- ✅ **1.8** macOS input forwarding — CGEvent mouse + keyboard
- ✅ **1.9** Linux AT-SPI2 tree walker — serialize full tree, send as `MD_PKT_UI_TREE`
- ✅ **1.10** macOS AXUIElement tree walker — same output format as AT-SPI2 backend
- ✅ **1.11** Agent action handler — receive `MD_PKT_ACTION`, dispatch through input HAL
- ✅ **1.12** Dear ImGui overlay — latency display, connection status, disconnect button

### 8.3 Phase 2 Milestones

- ✅ **2.1** Nostr session signaling — NIP-44 request/accept, NIP-51 allowlist, CLI connect tool. Pluggable signer abstraction with NIP-46, NIP-55L, and NIP-5F backends.
- ✅ **2.2** fips-nat daemon — STUN address discovery, Nostr transport publication, UDP hole punch, TURN fallback via sharegap.net relay node
- ✅ **2.3** MCP agent interface — JSON-RPC 2.0 tool/resource server, stdio + HTTP+SSE transports
- ✅ **2.4** Agent monitoring mode — headless host, auto-accept allowlisted npubs, signed Nostr session log (kind:1078)
- ✅ **2.5** Adaptive bitrate — AIMD RTT feedback loop to encoder bitrate target
- ✅ **2.6** UI session manager — peer list, allowlist management, approval popup, compact tree format + delta patching, tree format negotiation
- ⏳ **2.7** Dogfood gate — 30 days on OpenClaw fleet, all discovered bugs resolved

### 8.4 Phase 3 Milestones (contingent)

- **3.1** Windows host — DXGI capture + UI Automation + SendInput backends
- **3.2** Windows client — build and package metadesk-client for Windows
- **3.3** Multi-monitor and display selection
- **3.4** File transfer over FIPS session channel
- **3.5** Hardening — packet parser fuzzing, Debian/RPM/Homebrew packaging, release signing

---

## 9. Encoder Configuration

Hardware encoder availability varies by platform. The encoder module selects the best available backend at runtime, falling back to software if hardware is unavailable.

| Platform | Hardware path | Software fallback |
|---|---|---|
| Linux (P40 / 3090) | NVENC via FFmpeg CUDA | libx264 ultrafast |
| macOS | VideoToolbox H.264 | libx264 ultrafast |
| Windows (NVIDIA) | NVENC via FFmpeg CUDA | libx264 ultrafast |
| Windows (AMD) | AMF via FFmpeg | libx264 ultrafast |

### 9.1 NVENC Parameters (Linux / Windows — NVIDIA)

```
codec_id        = AV_CODEC_ID_H264
pix_fmt         = AV_PIX_FMT_CUDA    # stay on GPU, no copy (when CUDA surface available)
                  AV_PIX_FMT_NV12    # CPU copy fallback if no CUDA surface
bit_rate        = 8000000             # 8 Mbps initial, adaptive
gop_size        = 0                   # no keyframes; use intra refresh
max_b_frames    = 0                   # no B-frames (latency)
preset          = p1                  # lowest latency
tune            = ull                 # ultra-low latency
rc              = cbr
intra-refresh   = 1
b_adapt         = 0
zerolatency     = 1
```

### 9.2 VideoToolbox Parameters (macOS)

```
codec_id           = AV_CODEC_ID_H264
pix_fmt            = AV_PIX_FMT_VIDEOTOOLBOX
bit_rate           = 8000000
max_b_frames       = 0
realtime           = true
allow_sw           = false            # fail hard if HW unavailable; fall back to libx264
profile            = high
```

### 9.3 Latency Budget

| Stage | Target | Notes |
|---|---|---|
| Platform capture → buffer | < 2ms | PipeWire / ScreenCaptureKit / DXGI on idle desktop |
| Colorspace conversion (libyuv) | < 1ms | SIMD path |
| Hardware encode | < 5ms | NVENC / VideoToolbox, 1080p |
| Network (LAN) | < 1ms | Phase 1 target environment |
| FFmpeg decode (client) | < 5ms | Software decode acceptable for Phase 1 |
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
5. Host injects action via input HAL, waits for a11y change event
6. Host emits MD_PKT_UI_TREE_DELTA
7. Agent receives delta, updates local tree model, repeat

// Screenshot fallback (when tree is insufficient):
Agent emits: { "action": "screenshot", "region": [x, y, w, h] }
Host responds: MD_PKT_SCREENSHOT (annotated JPEG, node IDs overlaid)
```

### 10.3 Accessibility Coverage Tiers

The a11y HAL normalises all platform accessibility APIs into the same tree format. Coverage quality varies by application type, not by platform.

| Tier | Applications | Agent Strategy |
|---|---|---|
| Full tree | GTK4, Qt, Electron, web browsers, most native apps | a11y tree only — no screenshots needed |
| Partial tree | Some Java apps, legacy toolkits, complex custom widgets | Tree + targeted region screenshots |
| No tree | Games, custom GPU UIs, fully custom-rendered apps | Full screenshot fallback, vision model |

### 10.4 MCP Interface (Model Context Protocol)

In addition to the binary packet protocol (§3.1), metadesk provides an MCP server for AI agent integration via standard JSON-RPC 2.0 tooling.

**Transport options:**
- `metadesk-host --mcp` — stdio transport (newline-delimited JSON on stdin/stdout)
- `metadesk-host --mcp-http [PORT]` — HTTP+SSE on port 7710 (Phase 2)

**Tools exposed** (9 total, matching the action types in §3.2):
`metadesk_click`, `metadesk_dbl_click`, `metadesk_right_click`, `metadesk_type`, `metadesk_key_combo`, `metadesk_scroll`, `metadesk_focus`, `metadesk_set_value`, `metadesk_screenshot`

**Resources exposed:**
- `metadesk://ui-tree` — full accessibility tree (JSON or compact format)
- `metadesk://session-info` — session state, capabilities, action count

**Resource subscriptions:** Agents can subscribe to `metadesk://ui-tree` to receive `notifications/resources/updated` when the UI changes.

See `docs/AGENT_API.md` for the full integration guide with examples.

---

## 11. Open Questions

> These questions are intentionally deferred until Phase 1 empirical results are available. Do not resolve by assumption.

- **OQ-1** A11y change events vs polling — do AT-SPI2 and AXUIElement change event subscriptions give sufficient fidelity across both platforms, or is periodic full-tree polling required for some applications?

- **OQ-2** GPU frame path — can a DMA-BUF handle be passed directly to NVENC without a CPU copy on the T7610/P40 combination? ScreenCaptureKit on macOS delivers frames via `CVPixelBuffer`; is a VideoToolbox-direct encode path feasible to avoid the CPU copy there?

- **OQ-3** fips-nat integration point — should fips-nat write directly to the FIPS daemon config and signal reload, or does FIPS v0.1.x expose a dynamic peer API via `fipsctl`?

- **OQ-4** Nostr relay selection — which relays should be configured as defaults for session signaling? Should metadesk run its own relay at sharegap.net?

- **OQ-5** Tree format negotiation — is it worth supporting both compact and JSON formats simultaneously, or should compact be the only agent format?

- **OQ-6** macOS accessibility permissions — `AXUIElement` requires Accessibility access granted in System Settings. What is the correct UX for a headless daemon to request or detect this permission, and is a privileged helper process required?

- **OQ-7** macOS screen recording permissions — ScreenCaptureKit requires Screen Recording permission for the capturing process. Same question as OQ-6 for the headless daemon context. Determine whether `CGRequestScreenCaptureAccess()` is callable from a daemon and what happens when it is denied.

---

*metadesk project specification v0.2 — Soul Factory / OpenClaw — DRAFT — not for external distribution*
