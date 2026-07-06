# metadesk

FIPS-native remote desktop with native accessibility trees for AI agents and H.264 video for humans. Nostr keypairs as identity. Zero central authority.

## What is this?

metadesk lets AI agents and humans control remote desktops over the [FIPS](https://github.com/nickolasgogo/fips) encrypted mesh network. Unlike traditional remote desktop tools, it gives AI agents direct access to the platform's native accessibility tree — the same semantic structure used by screen readers — so agents can read UI elements and interact by ID instead of parsing pixels.

**Two client modes, one host:**

| Mode | Transport | Interface |
|------|-----------|-----------|
| **Agent** | MCP (JSON-RPC 2.0) | Accessibility tree + named actions |
| **Human** | H.264 video stream | SDL2 window + Dear ImGui overlay |

Both modes connect to the same host daemon and can run simultaneously.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  Clients                                        │
│  AI Agent (MCP stdio/HTTP)  │  Human (SDL2+ImGui)│
├─────────────────────────────────────────────────┤
│  libmetadesk (C17, cross-platform core)         │
│  ┌────────────┐ ┌────────────┐ ┌─────────────┐ │
│  │ Encode/    │ │ Agent      │ │ Session     │ │
│  │ Decode     │ │ A11y tree  │ │ Nostr auth  │ │
│  │ FFmpeg     │ │ Input HAL  │ │ NIP-44/51   │ │
│  └────────────┘ └────────────┘ └─────────────┘ │
├─────────────────────────────────────────────────┤
│  Platform HAL (compile-time selected)           │
│  Capture:  PipeWire │ ScreenCaptureKit │ DXGI   │
│  A11y:     AT-SPI2  │ AXUIElement      │ UIA    │
│  Input:    uinput   │ CGEvent          │ SendInput│
├─────────────────────────────────────────────────┤
│  Transport: TCP over FIPS TUN (fd00::/8)        │
│  Identity:  secp256k1 keypair (Nostr nsec/npub) │
└─────────────────────────────────────────────────┘
```

## Components

| Binary | Description |
|--------|-------------|
| `metadesk-host` | Host daemon — captures screen, walks a11y tree, handles input injection |
| `metadesk-client` | Human video client — SDL2 display with ImGui overlay |
| `libmetadesk` | Shared library — all core logic, no UI dependencies |
| `fips-nat` | Legacy/deprecated NAT sidecar (compile-time opt-in; off by default) |

## Building

### Prerequisites

**All platforms:** FFmpeg (≥6.0), libyuv, libsecp256k1, libwebsockets, cJSON, libb2, [nostrc](https://github.com/chebizarro/nostrc)

**FIPS runtime:** A current external FIPS daemon, tested against FIPS v0.3.x and v0.4.0, with TUN enabled, `.fips` DNS/address derivation available, the control socket enabled, and peer discovery/reachability owned by FIPS.

**Linux additionally:** PipeWire (≥0.3.65), AT-SPI2 (≥2.48), libudev (≥252), D-Bus  
**macOS additionally:** ScreenCaptureKit (macOS 13+), Xcode command line tools

### Build

```bash
# Linux
meson setup build -Dnostrc_root=/path/to/nostrc
ninja -C build

# macOS (Homebrew)
meson setup build -Dnostrc_root=/path/to/nostrc
ninja -C build

# Run tests
meson test -C build
```

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `nvenc` | `true` | NVENC hardware encoding (requires NVIDIA GPU) |
| `client` | `true` | Build the human video client (requires SDL2) |
| `fips_nat` | `false` | Build legacy fips-nat daemon (deprecated; requires libnice) |
| `signer_nip46` | `false` | NIP-46 Nostr Connect remote signer |
| `signer_nip55l` | `false` | NIP-55L D-Bus local signer |
| `signer_nip5f` | `false` | NIP-5F Unix socket signer |
| `nostrc_root` | `""` | Path to nostrc source tree (for uninstalled components) |

## FIPS Runtime Setup

metadesk does not run or manage FIPS. Start and configure the upstream FIPS daemon before starting `metadesk-host` or connecting with `metadesk-client`.

Required daemon capabilities for the current metadesk path:

- TUN adapter routing for `fd00::/8` and `.fips` address resolution.
- Control socket enabled for `show_status`, `show_peers`, and `show_sessions` readiness checks.
- Peer reachability configured in FIPS, either through static peers or Nostr overlay discovery (`node.discovery.nostr.enabled`, relay lists, `peers[].via_nostr`, and advertised UDP/TCP/Tor transports as appropriate).
- STUN, traversal signaling, relay adverts, retry/cooldown, ACLs, and mesh route maintenance handled by FIPS, not metadesk.

On Linux and macOS, metadesk looks for the daemon control socket in the FIPS client order unless overridden by metadesk configuration: `/run/fips/control.sock`, then `$XDG_RUNTIME_DIR/fips/control.sock`, then `/tmp/fips-control.sock`. The socket is normally mode `0770` and group `fips`; add the user running metadesk to that group and re-login if needed.

Readiness failures are reported before the TCP stream is opened:

- daemon unavailable or permission denied on the control socket;
- daemon error or non-ready TUN/control state;
- peer not configured or not discovered by the local FIPS daemon;
- peer present but route/session still converging before the bounded retry expires.

`fips-nat` is deprecated and off by default. It remains in-tree as a compile-time opt-in only for legacy experiments; the FIPS daemon control socket is the supported integration path.

### Optional `fips-gateway`

`fips-gateway` is optional FIPS-owned infrastructure for Linux LAN bridge deployments. It lets non-FIPS LAN hosts reach mesh destinations through a DNS proxy, virtual IPv6 pool, and nftables/proxy-NDP NAT rules managed by the FIPS gateway service. It is Linux-only, runs outside metadesk, reads FIPS `gateway.*` configuration, and has its own gateway control socket. metadesk does not start, stop, configure, or supervise `fips-gateway`, and does not assume ownership of its nftables or proxy-NDP lifecycle.

For normal metadesk host/client sessions, run FIPS on each participating Linux or macOS machine and let metadesk open TCP over the daemon-managed FIPS route. Use `fips-gateway` only when an operator intentionally wants a LAN segment bridged into the mesh.

## Usage

### Host (MCP agent mode)

```bash
# Start with MCP stdio transport (pipe to your AI agent)
metadesk-host --mcp

# Or with HTTP+SSE transport on port 7710
metadesk-host --mcp-http
```

### Host (video streaming)

```bash
# Start the host daemon (listens on port 7700)
metadesk-host
```

### Client (human)

```bash
# Connect to a host by Nostr public key
metadesk-client npub1<host-pubkey>
```

## Agent API (MCP)

metadesk exposes a [Model Context Protocol](https://modelcontextprotocol.io) server with 9 tools and 2 resources. Agents read the UI tree, invoke actions on element IDs, and receive deltas showing what changed.

### Tools

| Tool | Description | Key Params |
|------|-------------|------------|
| `metadesk_click` | Click a UI element | `target_id` |
| `metadesk_dbl_click` | Double-click | `target_id` |
| `metadesk_right_click` | Right-click | `target_id` |
| `metadesk_type` | Type text into an element | `target_id`, `text` |
| `metadesk_key_combo` | Press a key combination | `keys` (array) |
| `metadesk_scroll` | Scroll | `target_id`, `dx`, `dy` |
| `metadesk_focus` | Focus an element | `target_id` |
| `metadesk_set_value` | Set a value directly | `target_id`, `text` |
| `metadesk_screenshot` | Capture screen region | `region` [x,y,w,h] |

### Resources

| URI | Description |
|-----|-------------|
| `metadesk://ui-tree` | Current accessibility tree (subscribable) |
| `metadesk://session-info` | Session state, capabilities, action count |

### Example Interaction

```jsonc
// 1. Initialize
→ {"jsonrpc":"2.0","method":"initialize","id":1,"params":{
     "protocolVersion":"2025-03-26",
     "clientInfo":{"name":"my-agent","version":"1.0"}}}

// 2. Read the UI tree
→ {"jsonrpc":"2.0","method":"resources/read","id":2,
   "params":{"uri":"metadesk://ui-tree"}}

// 3. Click a button by its accessibility node ID
→ {"jsonrpc":"2.0","method":"tools/call","id":3,
   "params":{"name":"metadesk_click","arguments":{"target_id":"btn_save"}}}
// ← Response includes the UI tree delta showing what changed
```

See [docs/AGENT_API.md](docs/AGENT_API.md) for the complete integration guide.

## Session Negotiation

Sessions are established via Nostr DMs encrypted with NIP-44:

1. Client verifies the local FIPS daemon is reachable and ready via the control socket
2. Client verifies the host npub is configured/discovered and has a usable or converging FIPS route
3. Client sends a session request (NIP-44 encrypted DM) to the host's npub
4. Host checks the NIP-51 allowlist — approved clients connect immediately; unknown clients require explicit approval
5. Host replies with a session accept containing the granted capabilities
6. Client opens a TCP connection over the FIPS TUN route to the host on port `7700`

Access control is managed entirely through Nostr identity — no accounts, no passwords, no central servers.

## Secret Storage

All cryptographic material is retrieved at startup from [1Password Connect](https://developer.1password.com/docs/connect/) and held in locked memory. No secrets are stored in config files or environment variables.

```toml
# config/metadesk.toml — only references, never secrets
[secrets]
connect_url = "http://localhost:8080"
nsec_ref = "op://metadesk/fips-node/nsec"
token_ref = "op://metadesk/1pc/token"
```

## Testing

The test suite covers all core modules:

```bash
meson test -C build
```

```
 1/26 packet round-trip               OK
 2/26 JSON-RPC 2.0 message layer      OK
 3/26 session JSON + state machine    OK
 4/26 FIPS control socket client seam  OK
 5/26 FIPS address derivation         OK
 6/26 signer abstraction              OK
 7/26 1Password Connect secrets       OK
 8/26 STUN binding discovery          OK
 9/26 UDP hole punch                  OK
10/26 capture convenience API         OK
11/26 action parse/encode             OK
12/26 TURN relay client               OK
13/26 bitrate controller AIMD         OK
14/26 MCP stdio transport             OK
15/26 a11y tree serialisation         OK
16/26 TCP stream transport            OK
17/26 agent action handler            OK
18/26 MCP server core                 OK
19/26 input injection                 OK
20/26 NAT endpoint publication        OK
21/26 fips-nat IPC protocol           OK
22/26 signed session log              OK
23/26 nostr NIP-44                    OK
24/26 IPC Unix domain sockets         OK
25/26 encode/decode round-trip        OK
26/26 MCP HTTP+SSE transport          OK
```

## Project Structure

```
metadesk/
├── src/
│   ├── core/           # libmetadesk — cross-platform core
│   │   ├── a11y.h      # accessibility tree HAL
│   │   ├── action.c/h    # action parse/encode
│   │   ├── bitrate_ctrl.c/h # AIMD adaptive bitrate controller
│   │   ├── capture.h   # screen capture HAL
│   │   ├── decode.c/h  # FFmpeg H.264 decode
│   │   ├── encode.c/h  # FFmpeg H.264 encode
│   │   ├── fips_addr.c/h  # FIPS address derivation and DNS
│   │   ├── fips_control.c/h # FIPS daemon control socket client
│   │   ├── input.h     # input injection HAL
│   │   ├── jsonrpc.c/h   # JSON-RPC 2.0 message layer
│   │   ├── mcp_*.c/h   # MCP server, tools, resources, transports
│   │   ├── nostr.c/h   # Nostr relay client (NIP-44/51)
│   │   ├── session.c/h # session state machine
│   │   ├── session_log.c/h # signed Nostr session event log
│   │   ├── signer.c/h  # signing backend abstraction
│   │   └── stream.c/h  # TCP framed transport
│   ├── host/           # metadesk-host daemon
│   ├── client/         # metadesk-client (SDL2 + ImGui)
│   └── fips-nat/       # legacy deprecated NAT traversal daemon
├── tests/              # 26 test suites
├── tools/              # Diagnostic utilities
├── config/             # Example configuration
└── docs/               # Specification and API docs
```

## Documentation

- [Full Specification](docs/metadesk-spec.md) — architecture, wire formats, session negotiation, roadmap
- [Agent API Guide](docs/AGENT_API.md) — MCP integration with examples
- [Example Config](config/metadesk.toml.example) — annotated configuration template

## Status

**Pre-release — active development.** The core library and MCP agent interface are functional. Platform backends are implemented for Linux and macOS. Windows support is planned for Phase 3.

## License

See repository root for license terms.
