# Investigation: Is metadesk's accessibility data ingested as XML and rendered to the agent as XML?

## Summary
**The claim is false.** metadesk never ingests, stores, transports, or renders accessibility data as XML. Each platform backend reads typed native accessibility objects (libatspi C API on Linux, `AXUIElement` on macOS, `IUIAutomation` COM on Windows) into an in-memory `MdA11yNode` C struct tree; the shared core serialises that tree with **cJSON** (structured JSON) or a hand-built **compact plain-text list**, and deltas as a **JSON array**. Agents receive it either as `MD_PKT_UI_TREE`/`MD_PKT_UI_TREE_DELTA` payload bytes behind a 16-byte binary stream header, or as MCP JSON-RPC text content (`metadesk://ui-tree` resource with `application/json`/`text/plain`, and `{"type":"text"}` tool results). There is no XML library in metadesk's build (`libcjson` only) and zero XML references in `src/`, `tests/`, `tools/`, `docs/`.

Side-finding (tracked as **md-qqz**): compact-negotiated stream sessions receive JSON deltas that the client's JSON-only `md_a11y_tree_patch()` cannot apply to a compact snapshot — a real but currently dormant producer/consumer mismatch, and spec §3.3.3 misdescribes it.

## Claim Under Test
> the accessibility information received by `metadesk` is ingested as XML and rendered to the agent as such

## Symptoms / Initial Observations (Phase 1)
- `file_search` for `xml|XML` across `metadesk/src`, `tests`, `tools`, `docs`, `README.md`, `AGENTS.md` → **0 matches** (109 files searched).
- Accessibility code lives in `src/core/a11y.{c,h}` (platform-neutral) plus backends `a11y_atspi.c` (Linux AT-SPI2/D-Bus), `a11y_axui.m` (macOS AXUIElement), `a11y_uia.cpp` (Windows UI Automation).
- `src/core/a11y.h` declares only JSON/compact serializers: `md_a11y_to_json`, `md_a11y_to_compact`, `md_a11y_delta_to_json`, `md_a11y_tree_patch(tree_json, delta_json)`. No XML API.
- `src/client/main.c:760` calls `md_a11y_tree_patch(ctx.ui_tree_json, delta_str)` — field name says JSON.
- `src/host/main.c:740,877` create `MdA11yCtx` via `md_a11y_create()` for both the MCP path and the streaming/agent path.
- `src/core/agent.c:99-106` `serialize_tree()` picks `md_a11y_to_json` or `md_a11y_to_compact` by `MdTreeFormat`.
- `docs/metadesk-spec.md` §3.3.1 "Full tree (structured JSON) — MD_PKT_UI_TREE", §3.3.2 compact, §3.3.3 delta packets.

## Initial Hypotheses
- H1 (claim true): a11y backend emits XML, agent receives XML. **Unlikely** given zero XML references.
- H2: Backends read native platform object trees (D-Bus/AT-SPI, AXUIElement, UIA COM), the core serializes to JSON (or a compact text form), and the agent sees JSON — via MCP tool results and/or MD_PKT_UI_TREE packets.
- H3: Some intermediate (e.g., D-Bus introspection XML) is involved in ingestion even if output is JSON — need to check `a11y_atspi.c` for introspection usage.

## Background / Prior Research
_Not needed — question is fully answerable from the workspace._

## Investigator Findings
### Scope and evidence

Source-only investigation of the current checkout (`HEAD f3da051`), with separate read-only probes for platform ingestion, MCP delivery, and serialization/build dependencies. Load-bearing probe results were checked against source reads/searches. Examples below illustrate the implemented output shape; they are not captured runtime output. No native accessibility session, build, or test was run. Only this placeholder was replaced; the other report sections and pre-existing working-tree changes were left alone. Read targeting used Jev `find-lines` / `verify-claim` (parent-only totals: 75 requests, 384,011 input tokens, estimated $0.0161); absent, unclear, or mis-scoped judgments were resolved by focused queries/source reads rather than used as evidence.

### 1. INGESTION per platform — native objects → `MdA11yNode`

**Common boundary.** `src/core/a11y.h:30–39` defines the platform-neutral node as C strings, state strings, integer geometry, and child pointers—not a serialized document:

```c
typedef struct MdA11yNode {
    char *id; char *role; char *label;
    char **states; int state_count;
    int x, y, w, h;
    struct MdA11yNode **children;
    int child_count;
} MdA11yNode;
```

The backend contract is `int (*get_tree)(MdA11yCtx *ctx, MdA11yNode **out_root)` (`src/core/a11y.h:62–67`). `md_a11y_create()` selects/initializes that backend, and `md_a11y_walk()` calls its `get_tree()` and returns the resulting node pointer (`src/core/a11y.c:20–46`).

**Linux / AT-SPI2.** `walk_accessible(AtspiState *, AtspiAccessible *, int)` allocates an `MdA11yNode` (`src/core/a11y_atspi.c:103–113`), then reads libatspi's C object API:

- Identity, role, label: `atspi_accessible_get_accessible_id`, `atspi_accessible_get_role_name`, `atspi_accessible_get_name` (`:115–141`).
- States and screen bounds: `atspi_accessible_get_state_set`, `atspi_accessible_get_component_iface`, `atspi_component_get_extents(..., ATSPI_COORD_TYPE_SCREEN, ...)` (`:143–164`).
- Hierarchy: `atspi_accessible_get_child_count`, `atspi_accessible_get_child_at_index`; defunct children are checked using `atspi_state_set_contains` (`:166–191`).
- `atspi_get_tree_unlocked()` starts from `atspi_get_desktop(0)`, creates a synthetic `desktop` node, walks applications, and assigns `*out_root = root`; `atspi_get_tree()` adds locking (`:313–374`).

Representative ingestion excerpt (`src/core/a11y_atspi.c:110–111, 133–140`):

```c
MdA11yNode *node = calloc(1, sizeof(MdA11yNode));
/* ... */
gchar *role_name = atspi_accessible_get_role_name(acc, &error);
/* ... */
gchar *name = atspi_accessible_get_name(acc, &error);
if (name && name[0] != '\0')
    node->label = strdup(name);
```

This is libatspi object/property access over the platform's AT-SPI service, **not metadesk reading introspection XML**. A backend search for `dbus|introspect|xml|expat|g_markup` returned zero matches; there are no direct GDBus calls or XML parser calls here. This establishes metadesk's API boundary, not the implementation of every linked system library.

**macOS / AXUIElement.** `walk_element(AXUIState *, AXUIElementRef, int)` allocates the same `MdA11yNode` (`src/core/a11y_axui.m:172–179`). The `ax_copy_multi()` helper invokes `AXUIElementCopyMultipleAttributeValues` (`:143–150`); the walker requests role, title, description, value, enabled/focused/selected, position, and size, then normalizes the role and chooses a label (`:194–233`). `AXValueGetValue` extracts geometry; `AXUIElementCopyAttributeValue(..., kAXChildrenAttribute, ...)` provides the child array for recursion (`:281–327`). `axui_get_tree_unlocked()` creates a synthetic desktop, enumerates GUI apps through `NSWorkspace`, and uses `AXUIElementCreateApplication(pid)` before walking each app (`:591–647`).

```c
AXError err = AXUIElementCopyMultipleAttributeValues(elem, attrs,
                  0 /* options */, &values);  /* a11y_axui.m:148–149 */
/* ... */
AXError err = AXUIElementCopyAttributeValue(elem, kAXChildrenAttribute,
                                             &childrenRef); /* :302–303 */
```

These are typed Core Foundation / AX values, converted to node fields—not XML ingestion.

**Windows / UI Automation.** `uia_init()` initializes COM and obtains `IUIAutomation` and its content-view walker (`src/core/a11y_uia.cpp:260–302`). `walk_element(UIAState *, IUIAutomationTreeWalker *, IUIAutomationElement *, int)` allocates an `MdA11yNode`, obtains a runtime ID, reads `CurrentControlType`, `CurrentName`, states and `CurrentBoundingRectangle`, and recursively uses `GetFirstChildElement` / `GetNextSiblingElement` (`:153–255`). `uia_get_tree_with_unlocked()` calls `GetRootElement`, constructs a synthetic desktop, walks its children and returns `*out_root`; `uia_get_tree()` supplies locking (`:307–386`).

```cpp
MdA11yNode *node = (MdA11yNode *)calloc(1, sizeof(MdA11yNode)); /* :158 */
/* ... */
elem->get_CurrentControlType(&ctid);                        /* :190 */
node->role = _strdup(control_type_to_role(ctid));             /* :191 */
/* ... */
HRESULT hr = walker->GetFirstChildElement(elem, &child);     /* :214 */
```

Thus **all three backends produce `MdA11yNode` trees from native accessibility object APIs**. None takes XML as the accessibility payload.

### 2. SERIALIZATION — cJSON documents or manually formatted compact text

`src/core/a11y.c:11` includes `<cjson/cJSON.h>`. JSON encoding and patching use **cJSON**, not a hand-written JSON/XML encoder. Only the compact text formatter is hand-built (`StrBuf`, `vsnprintf`, `:497–500`).

The format choices match `docs/metadesk-spec.md:267–302` (§3.3): structured JSON or a compact interactable list. The implementation below determines the exact wire shape, including JSON-only deltas.

**`md_a11y_to_json()`.** The top-level envelope is exactly `v`, wall-clock-millisecond `ts`, and `root` (`src/core/a11y.c:323–345`):

```c
cJSON_AddNumberToObject(doc, "v", 1);
cJSON_AddNumberToObject(doc, "ts", (double)now_wall_ms());
cJSON_AddItemToObject(doc, "root", node_to_json(root));
char *str = cJSON_PrintUnformatted(doc);                    /* :339–343 */
```

`node_to_json()` conditionally adds `id`, `role`, and `label` when their pointers are non-null; adds `state` when the state array/count is present; always emits `bounds` with `x/y/w/h` and a `children` array, recursively serialized (`:280–321`). Illustrative valid output, with a leaf root for brevity:

```json
{"v":1,"ts":1700000000000,"root":{"id":"n42","role":"button","label":"Save","state":["enabled"],"bounds":{"x":10,"y":20,"w":80,"h":30},"children":[]}}
```

**`md_a11y_to_compact()`.** Starts with `v1 ts:<wall-clock-ms>\n`, then emits indented `ROLE[id]` lines, labels and state annotations (`src/core/a11y.c:503–563`). The role table maps containers/controls such as `desktop→DSK`, `frame/window→WIN`, `button/push button→BTN`, `text/entry→TXT`; decorative or unknown roles are omitted while their children are still visited (`:380–449, 523–551`). Text roles quote the label; other roles append it unquoted. This is **plain text**, not XML:

```text
v1 ts:1700000000000
WIN[n1] Editor
  BTN[n42] Save *enabled*
  TXT[n43] <focused> 'Hello' *enabled*
```

Implementation excerpt (`src/core/a11y.c:531–539, 554–560`):

```c
sb_appendf(sb, "%s[%s]", role_abbrev(node->role),
           node->id ? node->id : "?");
/* ... */
if (has_state(node, "focused")) sb_append(sb, " <focused>");
sb_appendf(sb, " '%s'", node->label ? node->label : "");
/* ... */
sb_appendf(&sb, "v1 ts:%lu\n", (unsigned long)now_wall_ms());
```

`<focused>` is a literal status marker within this custom line format—not an XML element/document. Labels are inserted as strings, not parsed as markup.

**`md_a11y_delta_to_json()`.** Emits a **bare JSON array**, not the full-tree envelope. Each entry has `op` (`add`, `remove`, `update`; `unknown` for an unrecognized enum), an optional `node` using the same node serializer, and optional `parent_id` (`src/core/a11y.c:567–597`):

```c
cJSON_AddStringToObject(entry, "op", op_str(deltas[i].op));
/* node_to_json(deltas[i].node), when present */
if (deltas[i].parent_id)
    cJSON_AddStringToObject(entry, "parent_id", deltas[i].parent_id);
/* ... */
char *str = cJSON_PrintUnformatted(arr);
```

Illustrative shape:

```json
[{"op":"update","node":{"id":"n42","role":"button","label":"Saved","state":["enabled"],"bounds":{"x":10,"y":20,"w":80,"h":30},"children":[]}}]
```

**`md_a11y_tree_patch()`.** Parses the cached tree with `cJSON_Parse(tree_json)`, requires its `root`, parses the delta as a JSON array, applies `add` under `parent_id`, `remove` by `node.id`, or `update` by `node.id`, refreshes `ts`, and returns `cJSON_PrintUnformatted(doc)` (`src/core/a11y.c:695–773`). It is a JSON-to-JSON patcher, **not a compact parser or XML parser**.

### 3. DELIVERY PATH A (stream) — negotiated text in binary-framed packets

**Format selection and negotiation.** `MdTreeFormat` has only `MD_TREE_FORMAT_JSON` and `MD_TREE_FORMAT_COMPACT` (`src/core/session.h:32–36`). `md_session_request()` records the preference (`src/core/session.c:18–26`). Request JSON encodes `"tree_format":"compact"` or `"json"`; parsing selects compact only for the exact `compact` string, otherwise JSON (`:191–194, 230–235`). The acceptance JSON confirms the selected format, with the same JSON default for missing/unrecognized values (`:281–284, 308–313`).

```c
cJSON_AddStringToObject(root, "tree_format",
    req->tree_format == MD_TREE_FORMAT_COMPACT ? "compact" : "json");
                                                     /* session.c:192–193 */
```

The npub client actually requests **compact** (`src/client/main.c:217–233`). `md_nostr_send_session_request()` / `md_nostr_send_session_accept()` pass that session JSON to the NIP-17 DM sender (`src/core/nostr.c:738–745`); the latter builds/encrypts rumor/seal/gift-wrap events rather than converting the payload to XML (`:682–735`). On receive, the Nostr code decrypts/parses event JSON and forwards the DM content to its callback (`:237–288`). The host parses the session request (`src/host/main.c:108–136`), confirms/copies the requested format into its session (`:682–708`); the client parses the acceptance and records/uses its format (`src/client/main.c:104–113, 322–327`). **Nostr carries session-control JSON here, not the UI tree.**

**Snapshot/update sender.** The stream host creates a separate a11y context and passes it, plus `session.tree_format`, into `MdAgentConfig` (`src/host/main.c:876–891`). On connection it calls `md_agent_send_tree()` (`:947–950`). That function walks the native tree, serializes it, and sends its string bytes (`src/core/agent.c:299–325`). `serialize_tree()` selects exactly these two serializers (`:99–109`):

```c
switch (fmt) {
case MD_TREE_FORMAT_JSON:    return md_a11y_to_json(root);
case MD_TREE_FORMAT_COMPACT: return md_a11y_to_compact(root);
}
```

`MD_PKT_UI_TREE = 0x03` and `MD_PKT_UI_TREE_DELTA = 0x04` are binary packet type codes; the header is 16 bytes and includes `payload_len` (`src/core/packet.h:19–41`). They do not denote XML content. `md_stream_send()` writes the encoded header and then the supplied payload unchanged; `md_stream_recv()` reads the corresponding payload bytes (`src/core/stream.c:576–658`).

Incoming `MD_PKT_ACTION` dispatches to `md_agent_handle_action()` (`src/host/main.c:284–287`). Its update builder takes `md_a11y_diff()` → `md_a11y_delta_to_json()` when a non-empty delta fits the size limit, otherwise walks/serializes a full tree (`src/core/agent.c:220–244`). It sends `MD_PKT_UI_TREE_DELTA` for the JSON delta and `MD_PKT_UI_TREE` for the full JSON/compact string (`:259–269`). Deltas are therefore **JSON regardless of negotiated full-tree format**. The separate `md_agent_send_delta()` likewise emits JSON (`:338–362`); source search found its declaration/definition but no production caller. The stream host is wired for initial and post-action tree sends, not the MCP resource-change notification path.

**Client consumption versus rendering.** `MD_PKT_UI_TREE` copies payload bytes into NUL-terminated `ctx.ui_tree_json`; `MD_PKT_UI_TREE_DELTA` passes that buffer and the received delta string to `md_a11y_tree_patch()` (`src/client/main.c:732–773`):

```c
memcpy(ctx.ui_tree_json, payload, hdr.payload_len);          /* :739 */
ctx.ui_tree_json[hdr.payload_len] = '\0';                  /* :740 */
/* ... */
char *patched = md_a11y_tree_patch(ctx.ui_tree_json, delta_str); /* :760 */
```

The field name/comment is not proof that every full payload is JSON: compact negotiation also reaches this raw-byte cache, while the patcher accepts only JSON. There is no compact-to-JSON or XML conversion at this receive site.

Crucially, **the inspected human client does not actually render this cached tree to its ImGui UI or feed it to an LLM**. All `ui_tree_json` references in `src/client/` are the cache, patch, and cleanup code (`src/client/main.c:55–56, 736–766, 822`). `render_client_overlay()` passes only status/performance fields (`:343–376`); `MdOverlayStats` has no tree field (`src/client/ui.h:28–41`). `ui.cpp` renders connection/status text, latency, FPS/bitrate, and Disconnect (`src/client/ui.cpp:174–249`). The receive comment's “agent interaction / overlay display” describes intent, not an implemented tree renderer. Agent-facing tree text is demonstrably exposed through MCP below.

### 4. DELIVERY PATH B (MCP) — JSON-RPC text content, not XML

**Host/bridge wiring.** At `src/host/main.c:740`, `mcp_a11y = md_a11y_create()`. The host config passes `.a11y = mcp_a11y` and explicitly selects `.tree_format = MD_TREE_FORMAT_JSON`, then creates/runs the bridge (`:759–784`). This fixes the current host entry point to JSON even though the bridge supports compact. The bridge passes those fields to the shared `MdAgent`, registers tools, assigns the same a11y context/format to resources, and subscribes to changes (`src/core/mcp_bridge.c:55–95`).

```c
MdMcpBridgeConfig mcp_cfg = {
    .a11y = mcp_a11y,
    .input = mcp_input,
    .tree_format = MD_TREE_FORMAT_JSON,
    /* ... */
};                                      /* src/host/main.c:759–767 */
```

**Resources.** Canonical URIs are defined at `src/core/mcp_resources.h:20–22`:

| Resource | Accessibility payload | Content MIME type |
| --- | --- | --- |
| `metadesk://ui-tree` | `md_a11y_walk()` → `md_a11y_to_json()` or `md_a11y_to_compact()` | `application/json` or `text/plain`, respectively |
| `metadesk://session-info` | Session metadata/action count; **not tree or node data** | `application/json` |

`read_ui_tree()` performs the walk/format switch (`src/core/mcp_resources.c:26–56`); registration exposes the same MIME selection (`:137–160`). The helper places the serialized tree in a **string** named `text`, alongside `uri` and `mimeType` (`:10–21`):

```c
cJSON_AddStringToObject(item, "uri", uri);
cJSON_AddStringToObject(item, "mimeType", mime);
cJSON_AddStringToObject(item, "text", text);
```

When no tree is available it uses the literal `{"root":null}` (`:52–53`). `handle_resources_read()` wraps the returned array as `result.contents` (`src/core/mcp_server.c:252–287`). Illustrative current-host response shape:

```json
{"jsonrpc":"2.0","id":1,"result":{"contents":[{"uri":"metadesk://ui-tree","mimeType":"application/json","text":"{\"v\":1,\"ts\":1700000000000,\"root\":{\"bounds\":{\"x\":0,\"y\":0,\"w\":0,\"h\":0},\"children\":[]}}"}]}}
```

The nested JSON is escaped because it is carried as a JSON string; there is no XML wrapping.

**Tools.** All nine registered tools share the same handler: `metadesk_click`, `metadesk_dbl_click`, `metadesk_right_click`, `metadesk_type`, `metadesk_key_combo`, `metadesk_scroll`, `metadesk_focus`, `metadesk_set_value`, and `metadesk_screenshot` (`src/core/mcp_tools.c:315–333`). No dedicated tree/node-read tool is registered. After successful execution, each shared handler returns the post-action accessibility update from `md_agent_handle_action_mcp()` (`:249–300`), not necessarily a screenshot/image even for the screenshot-named tool.

The MCP action variant uses the same delta-or-full-tree builder as the stream path, returning its string without exposing `is_delta` separately (`src/core/agent.c:278–296`). `make_text_content()` wraps it as a text item, **without a MIME field** (`src/core/mcp_tools.c:239–247`):

```c
cJSON_AddStringToObject(item, "type", "text");
cJSON_AddStringToObject(item, "text", text);
```

The server attaches this array as `result.content`, adds `isError:true` on tool errors, and creates the JSON-RPC response (`src/core/mcp_server.c:194–228`). Thus successful tool text contains a JSON delta array or full-tree JSON object in current host wiring; a differently configured bridge can return compact text for full-tree fallbacks. It is not embedded as a structured JSON subtree and is not XML. For example:

```json
{"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"[{\"op\":\"remove\",\"node\":{\"id\":\"n42\",\"bounds\":{\"x\":0,\"y\":0,\"w\":0,\"h\":0},\"children\":[]}}]"}]}}
```

**Notifications and transport.** `md_mcp_a11y_change_cb()` discards the supplied delta array/count and notifies the tree resource URI (`src/core/mcp_resources.c:116–129`). For an initialized client subscribed to that URI, the server sends `notifications/resources/updated` with **only `params.uri`**, not a tree/delta payload (`src/core/mcp_server.c:459–483`); the client must read the resource again.

The outer envelope is constructed with cJSON as `{"jsonrpc":"2.0","id":...,"result":...}` (`src/core/jsonrpc.c:145–180`). The bridge wires stdio and/or HTTP+SSE transports (`src/core/mcp_bridge.c:99–124`). Stdio dispatches complete input lines to the server and writes JSON plus newline (`src/core/mcp_stdio.c:33–60, 119–130`). HTTP returns JSON responses with `Content-Type: application/json`; notifications carry JSON in SSE (`src/core/mcp_http.c:143–150, 197–205, 424–426`). These transports do not turn accessibility text into XML. How an external MCP client or model UI subsequently displays that text is outside this repository's evidence.

### 5. Other accessibility transformations and adjacent paths

- **Shared snapshot/diff engine:** `md_a11y_diff()` fetches a fresh native-node tree, retains the first snapshot, flattens/sorts old/current nodes by ID, compares them, produces `MdA11yDelta` add/remove/update entries, then replaces the prior snapshot (`src/core/a11y.c:144–231`). The intermediate type is still C structs, not XML. Representative excerpt (`:152–163`):

  ```c
  MdA11yNode *current = NULL;
  if (ctx->vtable->get_tree(ctx, &current) != 0 || !current) { /* ... */ }
  MdA11yNode *prev = ctx->last_snapshot;
  ```

- **Native change events:** AT-SPI events, AXObserver notifications, and UIA COM event handlers call that shared diff engine and deliver `MdA11yDelta` callbacks (`src/core/a11y_atspi.c:228–255`; `src/core/a11y_axui.m:384–417`; `src/core/a11y_uia.cpp:458–480`). These are native callbacks → node snapshots/deltas → the MCP URI notification described above, not a hidden XML intermediate.
- **Diagnostic dump:** `tools/atspi_dump.c:67–76` walks the same API. Its default pretty printer emits `[id] role: label`, states and bounds (`:18–42`); `--json` and `--compact` call the same serializers (`:92–111`). No alternate XML exporter.
- **Compatibility names:** `src/core/atspi.h:19–36` aliases old `MdAtspi*` types/functions to `MdA11y*`, including the JSON/compact serializers. It is not a second ingestion implementation.
- **Session logging:** this is lifecycle/action audit metadata, not a tree serializer. Event types are connect/disconnect/action/request/accept/deny (`src/core/session_log.h:31–38`). `md_session_log_build_content()` uses cJSON for `type`, `event`, `ts`, optional `session_id`, `peer`, and string `detail` (`src/core/session_log.c:92–114`):

  ```c
  cJSON_AddStringToObject(root, "type", "session_log");
  cJSON_AddStringToObject(root, "event", md_session_log_event_name(type));
  /* ... */
  cJSON_AddStringToObject(root, "detail", detail);
  ```

  That content can be signed as a Nostr event and published (`:146–199`); the inspected host callers supply lifecycle descriptions, not serialized accessibility trees (`src/host/main.c:142–146, 697–701, 930–935, 1046–1051`).
- **Nostr DMs:** the request/accept wrappers transport the session JSON described in section 3 (`src/core/nostr.c:738–745`). A complete search for `a11y|ui_tree|tree_format|delta` across `nostr.c` and `session_log.{c,h}` returned zero matches. They do not introduce a separate a11y-to-XML transformation. Source-wide serializer-consumer searches located the agent sender, MCP resource reader, client JSON patcher, and diagnostic dump covered above.

### 6. XML and dependency checks — result and limits

**No XML representation/parser was found in metadesk's traced a11y ingestion, in-memory model, serializers, stream delivery, MCP delivery, or client consumption.** This conclusion rests on the positive API/serializer trace above, not only an absence-of-keyword search.

The renewed case-insensitive search `xml|expat|g_markup|g_dbus_node_info` over RepoPrompt-visible `src/`, `tests/`, `tools/`, `meson.build`, `meson_options.txt`, and `subprojects/` returned **0 matches across 102 searched files**. This search does **not** cover ignored subproject contents; those were checked separately below. A separate docs/README/SPEC/AGENTS search excluding this investigation report also returned zero matches. Broader `introspect` matches were only unrelated signer-test wording, not a D-Bus introspection implementation.

Build evidence:

- `meson.build:27–33` directly declares **libcjson**, not libxml/expat:

  ```meson
  dep_cjson = dependency('libcjson', version : '>=1.7.17')
  ```

- Linux selects `atspi-2` plus `dbus-1` and compiles `a11y_atspi.c` (`meson.build:176–188`); macOS selects `a11y_axui.m` and system frameworks including `ApplicationServices` (`:190–211`); Windows selects `a11y_uia.cpp` and COM/UIA libraries (`:213–230`). D-Bus linkage is not evidence of XML accessibility ingestion.
- `core_deps` includes `dep_cjson` and platform dependencies (`meson.build:245–265`); the shared library compiles `a11y.c` plus the selected backend (`:319–355`). No declared libxml/libxml2/expat dependency was found in these build definitions.
- **Ignored subprojects were checked directly, not assumed empty.** `.gitignore:5–8` excludes subproject directories and `.wraplock`, explaining the empty RepoPrompt tree. A direct directory check found `.wraplock`, `imgui/`, and a `nostrc` symlink resolving to `/Users/bizarro/Documents/Projects/nostrc`. A read-only scan of 4,151 build/dependency manifests in those local trees (pruning build/cache/VCS directories; not following further directory symlinks) found **libxml2 dependencies in unrelated nostrc targets**, and no expat matches:
  - `subprojects/nostrc/nostr-gtk/CMakeLists.txt:50–51` explicitly identifies its libxml2 use as a **shared Open Graph `<head>` parser**; `subprojects/nostrc/nostr-gtk/meson.build:32` declares the same dependency.
  - `subprojects/nostrc/gnome/nostr-dav/CMakeLists.txt:3–5, 18–23` declares libxml2 for its **CalDAV/CardDAV/WebDAV bridge**.
  - `subprojects/nostrc/gnome/groundhog/CMakeLists.txt:3543–3549` declares libxml2 for **Groundhog web content** and links it to `groundhog-conversation-ui`.

  ```cmake
  # Shared Open Graph <head> parser (gn-og-metadata.c, nostrc-8xfib.3).
  pkg_check_modules(NOSTR_GTK_LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
  # subprojects/nostrc/nostr-gtk/CMakeLists.txt:50–51
  ```

  These are real XML-library dependencies in the local ignored dependency checkout, **not evidence of XML in metadesk's a11y flow**. Metadesk's `core_deps` does not include those targets (`meson.build:245–265`); its Nostr integration selects installed/build-tree core/NIP/runtime components (`:65–99`), and a complete root-build search found no `subproject()` call or `nostr-gtk`/`nostr-dav`/`groundhog` reference. Its “Vendored subprojects” block actually compiles explicit `imgui_vendor` C++/SDL backend sources (`:295–314`), not those XML consumers. The `subprojects/nostrc/...` citations resolve through the local symlink and describe that dependency checkout, not files recorded in metadesk's `f3da051` commit.

**Evidence boundary:** this is a source-level trace of metadesk and its declared direct build dependencies, not a runtime capture or audit of every transitive/system-library implementation. It does not claim that libatspi or another system library can never use introspection metadata internally, nor that arbitrary application labels cannot contain XML-looking characters. Neither would establish that metadesk ingests its accessibility tree as XML. Likewise, external agents may choose their own presentation wrappers; metadesk does not generate an XML tree for them.

**Verdict:** The claim is **refuted for the inspected metadesk implementation**. Linux reads libatspi objects, macOS reads AXUIElement attributes, and Windows reads UI Automation COM objects; all become `MdA11yNode` trees. Metadesk serializes those trees as cJSON-generated JSON or a custom compact plain-text list, sends JSON deltas/full-tree text in stream packets, and exposes tree/update strings as MCP text content (JSON in current host wiring, with `application/json` on the tree resource). The human client caches/patches received tree data but has no implemented tree renderer in its ImGui overlay. No XML ingestion, intermediate conversion, or XML output was found in this metadesk-owned path.

## Investigation Log

### Phase 1 — Triage (agent)
**Hypothesis:** If a11y data were XML there would be an XML parser/serializer and build dependency.
**Findings:** `file_search "xml|XML"` over `src/`, `tests/`, `tools/`, `docs/`, `README.md`, `AGENTS.md` → 0 matches. `src/core/a11y.h:107-133` declares only `md_a11y_to_json`, `md_a11y_to_compact`, `md_a11y_delta_to_json`, `md_a11y_tree_patch(tree_json, delta_json)`.
**Conclusion:** H1 (XML) effectively eliminated at triage; proceed to trace actual formats.

### Phase 2 — context_builder + Oracle initial assessment
**Findings:** 28-file selection; Oracle independently traced native API → `MdA11yNode` → cJSON/compact → binary packets / MCP JSON-RPC, and flagged the compact-snapshot-vs-JSON-delta mismatch.

### Phase 3 — Pair investigator (Codex, `gpt-6.1-sol`)
Full trace recorded under *Investigator Findings* above (sections 1–6). Fanned out three explore probes (AT-SPI backend API inventory; MCP tool/resource formats; serializer + build deps).

### Phase 3b — Agent spot-checks of load-bearing claims
| Claim | Verified evidence |
|---|---|
| JSON via cJSON | `src/core/a11y.c:11` `#include <cjson/cJSON.h>`; `:343,594,769` `cJSON_PrintUnformatted`; `:699,709` `cJSON_Parse` |
| MCP resource MIME | `src/core/mcp_resources.c:38-45` compact→`text/plain`, JSON→`application/json`; `:143-144` registration |
| Host pins MCP to JSON | `src/host/main.c:762` `.tree_format = MD_TREE_FORMAT_JSON` |
| Only two formats exist | `src/core/session.h:33-36` `MD_TREE_FORMAT_JSON`, `MD_TREE_FORMAT_COMPACT` |
| No XML lib in build | `meson.build:30` `libcjson`; grep `xml|expat` over `meson.build`, `meson_options.txt`, `subprojects/` → hits only in vendored ImGui `.vcxproj`/`.natvis`/`.plist` files |
| `dbus-1` not used for a11y | consumers: `src/core/capture_pipewire.c`, `src/client/main.c`, `src/host/main.c`; `a11y_atspi.c` includes only `<atspi/atspi.h>` and has no `dbus|introspect|GVariant` symbols |
| Deltas always JSON | `src/core/agent.c:220-244` `agent_render_update_locked()` calls `md_a11y_delta_to_json` without consulting `tree_format`; `:352-361` same in `md_agent_send_delta` |
| Client requests compact, patches JSON | `src/client/main.c:205,220` compact; `:732-740` raw bytes cached; `:760` `md_a11y_tree_patch` |
| Client never sends actions | `src/client/main.c` `md_stream_send` only at `:268` (`MD_PKT_SESSION_INFO`) and `:725` (`MD_PKT_PONG`) |
| Spec wording | `docs/metadesk-spec.md:54` "structured JSON / compact-markup API"; `:300-302` §3.3.3 "same formats above" |

### Phase 4 — Oracle synthesis
Confirmed verdict; classified compact/delta mismatch as real-but-dormant (active only for a compact stream consumer that sends `MD_PKT_ACTION`; MCP unaffected because JSON is pinned); identified likely sources of the misconception (below).

## Root Cause / Answer

### Verdict
The statement *"the accessibility information received by metadesk is ingested as XML and rendered to the agent as such"* is **false at every layer** of the implemented pipeline:

```
Native a11y objects ──► MdA11yNode (C structs) ──► serialiser ──► envelope ──► agent
  libatspi C API          src/core/a11y.h:30-39     cJSON JSON          binary MD_PKT_* header (stream)
  AXUIElementRef                                    compact text        JSON-RPC text content (MCP)
  IUIAutomationElement                              JSON delta array
```

### Ingestion (what is actually "received")
| Platform | Backend | Native API consumed | Produces |
|---|---|---|---|
| Linux | `src/core/a11y_atspi.c:103-191` | `atspi_accessible_get_{accessible_id,role_name,name,state_set,child_count,child_at_index}`, `atspi_component_get_extents` | `MdA11yNode` |
| macOS | `src/core/a11y_axui.m:143-150,172-327` | `AXUIElementCopyMultipleAttributeValues`, `AXUIElementCopyAttributeValue(kAXChildrenAttribute)`, `AXValueGetValue` | `MdA11yNode` |
| Windows | `src/core/a11y_uia.cpp:153-255,260-302` | `IUIAutomation::GetRootElement`, `IUIAutomationElement::get_Current{ControlType,Name,BoundingRectangle}`, `IUIAutomationTreeWalker` | `MdA11yNode` |

No backend requests or parses D-Bus introspection XML; libatspi is used purely through its typed C object API.

### Serialisation (`src/core/a11y.c`)
- `md_a11y_to_json()` `:333` — `{"v":1,"ts":<ms>,"root":{id,role,label,state[],bounds{x,y,w,h},children[]}}` via cJSON.
- `md_a11y_to_compact()` `:554` — plain text: `v1 ts:<ms>` then indented `ROLE[id] label *states*` lines; `<focused>` is a literal text marker, not an element.
- `md_a11y_delta_to_json()` `:576` — bare JSON array of `{op,node?,parent_id?}`.
- `md_a11y_tree_patch()` `:695` — JSON→JSON patcher (`cJSON_Parse` both inputs).

### Rendering to the agent
**Path A — stream** (`src/core/agent.c`): `serialize_tree()` `:99-109` picks JSON or compact per negotiated `MdTreeFormat`; sent as `MD_PKT_UI_TREE` (0x03) `:322`; post-action updates are JSON deltas as `MD_PKT_UI_TREE_DELTA` (0x04) when < 64 KiB else full tree `:220-244,265`. Envelope is a 16-byte binary header (`src/core/packet.h:19-41`). The shipped human client caches/patches the bytes (`src/client/main.c:732-773`) but never renders the tree in its ImGui overlay (`src/client/ui.cpp`).

**Path B — MCP** (the path an AI agent actually uses today): `read_ui_tree()` `src/core/mcp_resources.c:26-56` → resource `{uri,mimeType,text}` with `application/json` (JSON) or `text/plain` (compact); the nine `metadesk_*` tools return the post-action update as `{"type":"text","text":<JSON delta or full tree>}` (`src/core/mcp_tools.c:239-247,297`); `notifications/resources/updated` carries only `params.uri`. The host pins MCP to JSON (`src/host/main.c:762`), so today's agent sees **JSON inside JSON-RPC**.

### Likely sources of the misconception
1. `<focused>` marker in the compact format (`a11y.c:537`) looks like a tag.
2. Spec architecture diagram calls it a "compact-**markup** API" (`docs/metadesk-spec.md:54`) — it is plain text, not markup.
3. AT-SPI2's D-Bus heritage (interface introspection is XML) and the XML-ish output of common UIA/MSAA inspection tools — neither is what metadesk consumes.
4. MCP `type:"text"` envelopes hide the inner syntax; escaped JSON-in-JSON can look opaque.

### Eliminated hypotheses
- **H1 XML in/out** — no XML parser/serialiser/library anywhere in metadesk; all serialisers are cJSON or hand-built text.
- **H3 D-Bus introspection XML in ingestion** — `a11y_atspi.c` has no D-Bus/GDBus/introspection calls; `dbus-1` dependency is consumed by PipeWire capture and main.c only.

## Recommendations
1. **Fix compact/delta mismatch (md-qqz)** — `src/core/agent.c:220-244` (`agent_render_update_locked`) and `:338-362` (`md_agent_send_delta`): when `agent->tree_format == MD_TREE_FORMAT_COMPACT`, send a full compact snapshot as `MD_PKT_UI_TREE` instead of a JSON delta. Add a compact-negotiated post-action test in `tests/test_agent.c`.
2. **Correct spec §3.3.3** (`docs/metadesk-spec.md:300-302`): state that deltas are JSON for JSON sessions and that compact sessions receive full-snapshot replacement (or design a compact delta contract first).
3. **Rename "compact-markup"** (`docs/metadesk-spec.md:54`) to "compact text" to stop implying a markup language.
4. Optionally make the client's delta-apply log (`src/client/main.c:772`) conditional on `patched != NULL` so a failed patch is not reported as success.

## Preventive Measures
- Keep `docs/metadesk-spec.md` §3.3 as the single normative format description and add a doc-vs-code check to tests (e.g. golden JSON/compact fixtures in `tests/test_atspi.c` referenced from the spec).
- Any new tree format must ship with: a serialiser, a matching consumer/patcher, a `MdTreeFormat` enum value, and a test exercising the post-action update path for that format.
