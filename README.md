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
| `fips-nat` | NAT traversal — STUN discovery + Nostr signaling + UDP hole punch |
| `libmetadesk` | Shared library — all core logic, no UI dependencies |

## Building

### Prerequisites

**All platforms:** FFmpeg (≥6.0), libyuv, libsecp256k1, libwebsockets, cJSON, libb2, [nostrc](https://github.com/chebizarro/nostrc)

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
| `fips_nat` | `true` | Build the fips-nat daemon (requires libnice) |
| `signer_nip46` | `false` | NIP-46 Nostr Connect remote signer |
| `signer_nip55l` | `false` | NIP-55L D-Bus local signer |
| `signer_nip5f` | `false` | NIP-5F Unix socket signer |
| `nostrc_root` | `""` | Path to nostrc source tree (for uninstalled components) |

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

1. Client discovers the host's FIPS transport address via Nostr relay
2. Client sends a session request (NIP-44 encrypted DM) to the host's npub
3. Host checks the NIP-51 allowlist — approved clients connect immediately; unknown clients require explicit approval
4. Host replies with a session accept containing the granted capabilities
5. Client opens a TCP connection over FIPS TUN to `fd00::<host-npub>:7700`

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
│   └── fips-nat/       # NAT traversal daemon
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
