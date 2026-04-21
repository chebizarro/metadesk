# metadesk — Agent API

MCP (Model Context Protocol) interface for AI agents to drive remote desktops.

## Overview

metadesk exposes a standard MCP server that lets AI agents interact with remote desktops through structured accessibility trees and input actions. Instead of parsing pixels, agents read a semantic UI tree and invoke named actions on UI element IDs.

**Transport options:**
- **stdio** (Phase 1) — `metadesk-host --mcp` communicates via stdin/stdout
- **HTTP+SSE** (Phase 2) — `metadesk-host --mcp-http` on port 7710

## Quick Start

```bash
# Start metadesk in MCP mode
metadesk-host --mcp

# Or pipe to your agent process
my-agent | metadesk-host --mcp
```

The host reads JSON-RPC 2.0 messages from stdin and writes responses to stdout, one JSON object per line.

## MCP Handshake

```json
→ {"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"my-agent","version":"1.0"}}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-03-26","serverInfo":{"name":"metadesk","version":"0.1.0"},"capabilities":{"tools":{},"resources":{"subscribe":true}}}}
→ {"jsonrpc":"2.0","method":"notifications/initialized"}
```

## Available Tools

All tools are prefixed with `metadesk_` and operate on accessibility tree node IDs.

| Tool | Description | Required Params |
|------|-------------|-----------------|
| `metadesk_click` | Click a UI element | `target_id` |
| `metadesk_dbl_click` | Double-click a UI element | `target_id` |
| `metadesk_right_click` | Right-click a UI element | `target_id` |
| `metadesk_type` | Type text into an element | `target_id`, `text` |
| `metadesk_key_combo` | Press a key combination | `keys` (array) |
| `metadesk_scroll` | Scroll at an element | `target_id`, optional `dx`/`dy` |
| `metadesk_focus` | Focus an element | `target_id` |
| `metadesk_set_value` | Set a value directly | `target_id`, `text` |
| `metadesk_screenshot` | Capture screen region | optional `region` [x,y,w,h] |

### Example: Click a button

```json
→ {"jsonrpc":"2.0","method":"tools/call","id":2,"params":{"name":"metadesk_click","arguments":{"target_id":"btn_save"}}}
← {"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"[{\"op\":\"update\",\"node\":{\"id\":\"btn_save\",\"state\":[\"pressed\"]}}]"}]}}
```

### Example: Type text

```json
→ {"jsonrpc":"2.0","method":"tools/call","id":3,"params":{"name":"metadesk_type","arguments":{"target_id":"input_search","text":"hello world"}}}
```

### Example: Key combo

```json
→ {"jsonrpc":"2.0","method":"tools/call","id":4,"params":{"name":"metadesk_key_combo","arguments":{"keys":["ctrl","s"]}}}
```

## Available Resources

| URI | Description | MIME Type |
|-----|-------------|-----------|
| `metadesk://ui-tree` | Current accessibility tree | `application/json` or `text/plain` |
| `metadesk://session-info` | Session state and metadata | `application/json` |

### Read the UI tree

```json
→ {"jsonrpc":"2.0","method":"resources/read","id":5,"params":{"uri":"metadesk://ui-tree"}}
← {"jsonrpc":"2.0","id":5,"result":{"contents":[{"uri":"metadesk://ui-tree","mimeType":"application/json","text":"{\"v\":1,\"ts\":1700000000000,\"root\":{\"id\":\"n1\",\"role\":\"frame\",\"label\":\"gedit - untitled\",\"state\":[\"visible\",\"active\"],\"bounds\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080},\"children\":[{\"id\":\"n42\",\"role\":\"button\",\"label\":\"Save\",\"state\":[\"enabled\"],\"bounds\":{\"x\":100,\"y\":50,\"w\":80,\"h\":30},\"children\":[]}]}}"}]}}
```

### Read session info

```json
→ {"jsonrpc":"2.0","method":"resources/read","id":6,"params":{"uri":"metadesk://session-info"}}
← {"jsonrpc":"2.0","id":6,"result":{"contents":[{"uri":"metadesk://session-info","mimeType":"application/json","text":"{\"session_id\":\"abc-123\",\"state\":\"active\",\"capabilities\":[\"agent\",\"input\"],\"tree_format\":\"json\",\"action_count\":5}"}]}}
```

## Resource Subscriptions

Subscribe to get notified when the UI tree changes:

```json
→ {"jsonrpc":"2.0","method":"resources/subscribe","id":7,"params":{"uri":"metadesk://ui-tree"}}
← {"jsonrpc":"2.0","id":7,"result":{}}

// When the tree changes, the server sends:
← {"jsonrpc":"2.0","method":"notifications/resources/updated","params":{"uri":"metadesk://ui-tree"}}

// The agent then reads the updated tree:
→ {"jsonrpc":"2.0","method":"resources/read","id":8,"params":{"uri":"metadesk://ui-tree"}}
```

## Agent Interaction Loop

```
1. Initialize MCP connection (handshake above)
2. Read metadesk://ui-tree to get the full accessibility tree
3. Reason about the UI tree, decide on an action
4. Call the appropriate tool (e.g. metadesk_click)
5. Read the tool result (UI tree delta showing what changed)
6. Repeat from step 3
```

The tool result contains the UI tree delta after the action was executed and the UI settled. This eliminates the need to re-read the full tree after each action.

For applications with partial accessibility coverage, use `metadesk_screenshot` to capture specific regions as a vision fallback.

## Error Handling

| Code | Meaning |
|------|---------|
| `-32700` | Parse error (malformed JSON) |
| `-32600` | Invalid request (missing jsonrpc/method) |
| `-32601` | Method not found / unknown tool |
| `-32602` | Invalid params (missing required arguments) |
| `-32603` | Internal error (server not initialized) |

Tool-level errors return `"isError": true` in the result:

```json
← {"jsonrpc":"2.0","id":9,"result":{"content":[{"type":"text","text":"Error: missing target_id"}],"isError":true}}
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  AI Agent (Claude, GPT, etc.)                           │
│  ├─ Reads UI tree (resources/read)                      │
│  └─ Sends actions (tools/call)                          │
├─────────────────────────────────────────────────────────┤
│  MCP Transport (stdio or HTTP+SSE)                      │
│  JSON-RPC 2.0 — one message per line                    │
├─────────────────────────────────────────────────────────┤
│  MCP Server Core                                        │
│  ├─ tools/list, tools/call → MdMcpTools                 │
│  └─ resources/read, subscribe → MdMcpResources          │
├─────────────────────────────────────────────────────────┤
│  MdAgent (action handler)                               │
│  ├─ Parse action → resolve target via a11y tree         │
│  ├─ Inject input via platform HAL                       │
│  ├─ Wait for UI settle (100ms)                          │
│  └─ Diff a11y tree → return delta                       │
├─────────────────────────────────────────────────────────┤
│  Platform HAL                                           │
│  ├─ A11y: AT-SPI2 (Linux), AXUIElement (macOS), UIA    │
│  └─ Input: uinput (Linux), CGEvent (macOS), SendInput   │
└─────────────────────────────────────────────────────────┘
```

## Accessibility Coverage Tiers

| Tier | Applications | Strategy |
|------|-------------|----------|
| **Full tree** | GTK4, Qt, Electron, browsers, most native apps | A11y tree only |
| **Partial tree** | Some Java apps, legacy toolkits | Tree + targeted screenshots |
| **No tree** | Games, custom GPU UIs | Full screenshot fallback |
