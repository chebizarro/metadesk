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
| `fips-nat` | Legacy/deprecated NAT sidecar — not in the recommended FIPS runtime path |

## Building

### Prerequisites

**All platforms:** FFmpeg (≥6.0), libyuv, libsecp256k1, libwebsockets, cJSON, libb2, [nostrc](https://github.com/chebizarro/nostrc)

**FIPS runtime:** A current external FIPS daemon, tested against the v0.3.x runtime surface, with TUN enabled, `.fips` DNS/address derivation available, the control socket enabled, and peer discovery/reachability owned by FIPS.

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

`fips-nat` is deprecated and off by default. It remains in-tree only for legacy experiments; do not use its kind `30078` NAT endpoint publication as the recommended bootstrap path.

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
 1/18 JSON-RPC 2.0 message layer     OK
 2/18 packet round-trip               OK
 3/18 session JSON + state machine    OK
 4/18 FIPS address derivation         OK
 5/18 signer abstraction              OK
 6/18 MCP server core                 OK
 7/18 1Password Connect secrets       OK
 8/18 action parse/encode             OK
 9/18 capture convenience API         OK
10/18 MCP stdio transport             OK
11/18 agent action handler            OK
12/18 input injection                 OK
13/18 encode/decode round-trip        OK
14/18 TCP stream transport            OK
15/18 IPC Unix domain sockets         OK
16/18 nostr NIP-44                    OK
17/18 MCP HTTP+SSE transport          OK
18/18 a11y tree serialisation         OK
```

## Project Structure

```
metadesk/
├── src/
│   ├── core/           # libmetadesk — cross-platform core
│   │   ├── capture.h   # screen capture HAL
│   │   ├── a11y.h      # accessibility tree HAL
│   │   ├── input.h     # input injection HAL
│   │   ├── encode.c/h  # FFmpeg H.264 encode
│   │   ├── decode.c/h  # FFmpeg H.264 decode
│   │   ├── mcp_*.c/h   # MCP server, tools, resources, transports
│   │   ├── session.c/h # session state machine
│   │   ├── nostr.c/h   # Nostr relay client (NIP-44/51)
│   │   ├── signer.c/h  # signing backend abstraction
│   │   └── stream.c/h  # TCP framed transport
│   ├── host/           # metadesk-host daemon
│   ├── client/         # metadesk-client (SDL2 + ImGui)
│   └── fips-nat/       # legacy deprecated NAT traversal daemon
├── tests/              # 18 test suites
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
