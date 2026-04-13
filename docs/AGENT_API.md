# Agent Client API

> See `metadesk-spec.md` §10 for the full specification.

## Overview

An agent client connects to a metadesk host to interact with a remote desktop via the AT-SPI2 accessibility tree. No video stream is required — a pure agent session carries only the UI tree and action channels.

## Connection

1. Negotiate a session with `capabilities: ["agent"]` and `tree_format: "compact"`
2. Connect TCP to host on port 7700 (over FIPS TUN)
3. Exchange `MD_PKT_SESSION_INFO` packets

## Interaction Loop

```
1. Receive MD_PKT_UI_TREE (full tree on connect)
2. Reason about tree, emit MD_PKT_ACTION
3. Host injects action, waits for AT-SPI2 change
4. Receive MD_PKT_UI_TREE_DELTA
5. Update local tree model, repeat
```

## Action Types

| Action | Description | Required Payload Fields |
|---|---|---|
| `click` | Single left-click on target | `target_id` |
| `dbl_click` | Double-click on target | `target_id` |
| `right_click` | Right-click on target | `target_id` |
| `type` | Type text into focused element | `target_id`, `payload.text` |
| `key_combo` | Press key combination | `payload.keys` |
| `scroll` | Scroll by delta | `target_id`, `payload.dx`, `payload.dy` |
| `focus` | Move focus to target | `target_id` |
| `set_value` | Set value of target element | `target_id`, `payload.text` |
| `screenshot` | Request screenshot of region | `payload.region` |

## Tree Formats

### Compact (recommended for agents)

```
v1 ts:1700000000000
WIN[1] gedit - untitled
  BTN[42] Save *enabled*
  BTN[43] Undo *enabled*
  TXT[44] <focused> 'Hello world...'
```

### Full JSON

See spec §3.3.1 for the complete JSON schema.

## Screenshot Fallback

When the accessibility tree is insufficient (Tier 2/3 applications), request a targeted screenshot:

```json
{"v": 1, "action": "screenshot", "payload": {"region": [100, 100, 400, 300]}}
```

The host responds with `MD_PKT_SCREENSHOT` containing an annotated JPEG with node IDs overlaid.
