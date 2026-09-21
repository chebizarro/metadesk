# Slop Audit — Nostr Layer

**Date:** 2026-09-20
**Scope:** `src/core/nostr.c`, `nostr.h`, `signer.c`, `signer.h`, `session_log.c`, `session_log.h`, `secrets.c`, `secrets.h` — *only*.
**Question asked of every workaround:** what primitive is this standing in for, does that primitive already exist, and what would the code look like using it?

## Context

The Nostr layer is documented as a "thin bridge to nostrc" (`nostr.h:2-8`: *"metadesk does NOT implement Nostr primitives"*). In practice it re-implements four things nostrc already ships — NIP-17 gift-wrapping, NIP-42 AUTH event construction, event de-duplication, and hex codecs — and one thing libcurl already ships (an HTTP client). `subprojects/nostrc` is a symlink to the working nostrc tree, and `meson.build:230-250` already links `dep_nip17`, `dep_nip11`, `dep_nip51`, `dep_curl` into `core_deps`, so every "correct shape" below is a call away: no new dependency, no new build flag.

Every claim in this report was verified against the actual nostrc sources at `/Users/bizarro/Documents/Projects/nostrc` (headers *and* implementations), not against grep output alone. Relay URL defaults live in `src/host/main.c` / `src/client/main.c` and are out of scope; no hardcoded `wss://` exists in the audited files.

---

## Findings table

| # | Severity | Code | Location | Observed pattern | Correct shape |
|---|----------|------|----------|------------------|---------------|
| F1 | structural | C1, S4 | `nostr.c:866-985` `md_nostr_send_dm` | Hand-rolled NIP-17 3-layer chain; gift-wrap and rumor carry **no `p` tag** | `nostr_nip17_create_rumor()` + `nostr_nip17_create_gift_wrap()` (`nips/nip17`, already linked) |
| F2 | structural | S7, S1 | `nostr.c:263-330`, `423-431` | 70-line pool→ctx instance registry with a 16-instance cap, because "event_middleware has no user_data" | `nostr_simple_pool_set_event_middleware_ex(pool, cb, n)` (`nostr-simple-pool.h:241`) |
| F3 | structural | C4 | `nostr.c:434` | `nostr_event_get_id()` result assigned to `const char *`, never freed | It is `(transfer full)`: `char *id = ...; free(id);` — or delete the dedup ring (F5) |
| F4 | structural | C1, S5 | `nostr.c:466-485` | kind:30000 handler accepts an allowlist from **any author**, unsigned, trusting the relay to honour the REQ filter | `nostr_event_get_pubkey(ev)` vs `n->pk_hex` + `nostr_event_validate()` |
| F5 | structural | C4 | `nostr.c:1036-1073` vs `706-736` | Second `nostr_simple_pool_subscribe()` frees `pool->filters_shared` that the live kind:1059 sub still points at | One `NostrFilters` with both filters, one subscribe call |
| F6 | structural | C4 | `nostr.c:78-80`, `466-485`, `1008-1215` | `n->allowlist` written on the pool worker thread, read/mutated on caller threads, with no mutex — while the *dedup ring in the same struct* has one | `pthread_mutex_t allowlist_mu`, same pattern as `dedup_mu` |
| F7 | structural | C4, S5 | `signer.c:515-545` `nip55l_sign_event` | Stamps `sig` onto an event whose `id` was never computed → serialized event has no `id` field | `nostr_nip55l_sign_event_json()` (same header, returns full signed JSON incl. `id`) |
| F8 | structural | S4, C4 | `secrets.c:250-380` | Hand-rolled HTTP/1.1 client + winsock shims + "TLS not implemented" | `curl_easy_*` — libcurl is already required (`meson.build:133`) and linked (`:239`) |
| F9 | confidence | S5 | `nostr.c:151-172` `publish_all` | Always returns `0`; `nostr_relay_publish()` is fire-and-forget `void` | `nostr_relay_publish_and_wait(relay, ev, timeout, &err)`; fail when zero relays OK'd |
| F10 | confidence | S5 | `secrets.c:250-380`, `410-560` | Every failure (DNS, connect, 401, 404, parse) collapses to `NULL` → "vault not found" for an expired token | Status out-param / `CURLINFO_RESPONSE_CODE` |
| F11 | confidence | S3 | `secrets.h:41` vs `secrets.c:562-600` | Doc: "the original is zeroed" — `md_secrets_create` takes `const char *` and never touches it | Fix the doc, or take `char *token` and `md_secure_zero()` it |
| F12 | drift | C1 | 16 sites: `nostr.c:198,440,466,719,875,897,957,1054,1096,1166`; `session_log.h:60` | Numeric kind literals `22242 / 1059 / 30000 / 14 / 13 / 1078` | `#include <nostr-kinds.h>`: `NOSTR_KIND_CLIENT_AUTHENTICATION`, `_GIFT_WRAP`, `_CATEGORIZED_PEOPLE_LIST`, `_DIRECT_MESSAGE`, `_SEAL` |
| F13 | drift | S4, C4 | `nostr.c:184-250` `md_nostr_auth_handler` | Hand-builds the kind-22242 AUTH event with the same two tags nostrc builds | `nostr_relay_auth(relay, sign_cb, &err)` (`libnostr/src/relay.c`) |
| F14 | drift | S6 | `nostr.h:36`, `meson.build:237` | `nip17.h` included and `dep_nip17` linked; **zero** `nostr_nip17_*` calls in the repo | Use it (F1) or drop the include and the dep |
| F15 | drift | S4 | `nostr.c:43-52`, `signer.c:35-43`, `signer.c:469-482` | Three hand-rolled hex codecs (two decoders, one encoder) | `nostr_hex2bin()` / `nostr_bin2hex()` (`nostr-utils.h:91,100`) |
| F16 | drift | C4 | `signer.c:87-88`, `signer.c:528-529` | `free(ev->pubkey); ev->pubkey = strdup(...)` and `free(ev->sig); ev->sig = sig;` | `nostr_event_set_pubkey()` / `nostr_event_set_sig()` (`nostr-event.h:170,242`) |
| F17 | drift | S4, C5 | `session_log.c:116-168`, `195-225` | Second Nostr-event builder (cJSON) + second copy of `sign_event_via_signer` | Export `md_nostr_sign_event_json()`; build with `nostr_event_new`/`nostr_tags_new` |
| F18 | drift | C1 | `session_log.h:60`, `session_log.c:124-140` | `MD_SESSION_LOG_KIND 1078` (unassigned kind) + a `d` tag on a non-addressable event, with a comment admitting it | `NOSTR_KIND_APPLICATION_SPECIFIC_DATA` (30078, NIP-78) for addressable state, or a `t` topic tag on a regular kind |
| F19 | drift | C4 | `secrets.c:320`, `secrets.c:336` | Raw `close(fd)` on a `sock_t` on two exit paths; every other path uses `sock_close(fd)` | `sock_close(fd)` — the macro exists at `secrets.c:36-44` for exactly this |
| F20 | drift | S1 | `signer.c:862-875` `md_signer_is_ready` | Predicate-shaped name performing a blocking D-Bus/socket/relay RPC and discarding the error | Return `s->pubkey_hex != NULL`, or rename to `md_signer_probe()` returning `MdSignerError` |
| F21 | load | S1, S3 | `nostr.c:68-133`, `434-436`, `777-779` | App-level 64-entry dedup ring + mutex duplicating the pool's dedup, with a comment claiming it "guarantees idempotent processing" | Delete; `nostr_simple_pool_subscribe(..., unique=true)` is already passed (`nostr.c:728`) |
| F22 | load | S9, C5 | `nostr.c:488-600`, `654-670` | ~110 lines + thread fan-out of NIP-11 probing whose only output is `fprintf` text; blocks `md_nostr_create()` | Delete, or feed `limitation->max_content_length` / NIP-17 support into publish routing |
| F23 | load | S4 | `nostr.c:1092-1125` vs `1162-1190` | Two byte-identical 25-line allowlist-event builders; removal pokes `NostrList` internals | One `static NostrEvent *build_allowlist_event(MdNostr *)`; rebuild the list instead of shifting `entries[]` |
| F24 | load | C5 | 59 sites (`nostr.c` 29, `signer.c` 20, `secrets.c` 7, `session_log.c` 3) | `fprintf(stderr, ...)` as the logging system in library code linked into a GUI client | A `md_log.h` with levels + a host-installed sink; there is no logging module in the repo today |
| F25 | cosmetic | S3 | `nostr.c:739-741` | "bridge ready (… relays=%d)" prints `cfg->relay_count`, not `n->relay_count` | Print `n->relay_count`; fail `md_nostr_create()` when it is 0 |

---

## Detailed findings

### F1 — Hand-rolled NIP-17 chain omits the `p` tag the code's own subscription filters on
**structural · C1, S4 · `nostr.c:866-985`**

**Observed.** `md_nostr_send_dm()` builds rumor (kind 14, `:875`), seal (kind 13, `:897`) and gift-wrap (kind 1059, `:957`) by hand — 120 lines of `nostr_event_set_*`, ephemeral key generation, `hex_to_bytes`, `nostr_nip44_encrypt_v2`. Neither the rumor nor the gift-wrap gets any tags: the gift-wrap is `set_kind` / `set_content` / `set_pubkey` / `set_created_at` / `nostr_event_sign`, then published.

Meanwhile `md_nostr_create()` subscribes with `nostr_filter_builder_tag(..., "p", n->pk_hex)` (`nostr.c:713-722`), which nostrc serializes as `"#p":[...]` (`libnostr/src/filter.c:700`). A relay that honours the filter will never deliver a metadesk gift-wrap to a metadesk peer, because the gift-wrap carries no `p` tag to match. `created_at` is also the exact wall clock, where NIP-59 calls for a randomized timestamp.

**Correct shape.** `nips/nip17` (linked as `dep_nip17`, header already included at `nostr.h:36`) provides the builders, and `nostr_nip17_create_gift_wrap()` adds the `p` tag and uses `get_randomized_time()`:

```c
NostrEvent *rumor = nostr_nip17_create_rumor(n->pk_hex, recipient_pk, content, 0);
/* seal must stay local: it is signed through MdSigner, and nostrc's
   create_seal takes a raw sk_hex that remote signers do not expose */
NostrEvent *seal = md_seal_via_signer(n->signer, rumor, recipient_pk);
NostrEvent *gw   = nostr_nip17_create_gift_wrap(seal, recipient_pk);
```

Only the seal step genuinely needs a local variant (nostrc's `create_seal`/`wrap_dm` take `sender_sk_hex`, which NIP-46/55L/5F backends cannot supply); the rumor and gift-wrap steps have no such constraint and should not be hand-written. If the whole chain stays local, it must at minimum append `["p", recipient]` to both rumor and gift-wrap.

**Cost.** Session signalling — the entire request/accept handshake — is silently undeliverable against any relay that applies `#p`. Tests pass because they never assert on gift-wrap tags or round-trip through a filtering relay. Plus 120 lines of crypto plumbing that must track NIP-17/NIP-59 changes by hand.

---

### F2 — A pool→context registry standing in for a `user_data` parameter that upstream added two days later
**structural · S7, S1 · `nostr.c:263-330`, `423-431`**

**Observed.** `g_registry` is a 16-slot static table with a global mutex, `registry_add/remove/find_by_relay`, and this justification (`:266-270`): *"nostrc's event_middleware callback signature has no user_data parameter, so we identify the owning MdNostr by matching the incoming relay pointer against each registered pool's relay list."* Every incoming event takes the global lock and scans instances × relays. Overflow past 16 instances logs a warning and silently drops routing.

`nostr_simple_pool_set_event_middleware_ex(pool, void (*cb)(NostrIncomingEvent *, void *), void *user_data)` exists at `nostr-simple-pool.h:241-243`, is implemented at `libnostr/src/simplepool.c:426`, and the pool prefers it over the legacy hook (`simplepool.c:1275`, `:1534`). Git archaeology: metadesk's registry landed `33713a6` (2026-04-20, "Fix nostr.c global singleton g_nostr_ctx"); nostrc added `event_middleware_ex` in `6e37ef6b` (2026-04-22, *"eliminate global singleton relay pool"*) — the upstream fix for exactly this problem shipped two days later and was never adopted. The comment at `:325` ("Both have a user_data parameter (unlike event_middleware)") is now simply false.

**Correct shape.**
```c
static void md_nostr_event_handler(NostrIncomingEvent *incoming, void *user_data) {
    MdNostr *n = user_data;
    ...
}
nostr_simple_pool_set_event_middleware_ex(n->pool, md_nostr_event_handler, n);
```
and delete `g_registry`, `MD_NOSTR_MAX_INSTANCES`, `registry_add`, `registry_remove`, `registry_find_by_relay`, and the `registry_*` calls in create/destroy.

**Cost.** 70 lines, a process-global lock on the hot event path, a silent 16-instance ceiling, and a dependency on the relay-pointer snapshot (F5's fragility) purely for identity lookup.

---

### F3 — `nostr_event_get_id()` is `(transfer full)`; its result is leaked on every incoming event
**structural · C4 · `nostr.c:434-436`**

**Observed.**
```c
const char *ev_id = nostr_event_get_id(ev);
if (ev_id && dedup_is_seen(n, ev_id))
    return;
```
`nostr-event.h:114-120`: *"Recomputes the canonical NIP-01 id. It never trusts or caches @event->id. Returns: (transfer full) (nullable): newly allocated lowercase hex id."* Implementation (`libnostr/src/event.c`) is `compute_id()` + `strdup()`. The `const char *` receiver hides the ownership; nothing frees it.

**Correct shape.** Either delete the dedup ring (F21), or:
```c
char *ev_id = nostr_event_get_id(ev);
bool dup = ev_id && dedup_is_seen(n, ev_id);
free(ev_id);
if (dup) return;
```

**Cost.** A 65-byte leak *and* a SHA-256 recomputation for every event on every subscription, for the lifetime of the process.

---

### F4 — The kind:30000 handler treats the relay's filter as authorization
**structural · C1, S5 · `nostr.c:466-485`**

**Observed.** The handler accepts any kind:30000 event whose parsed NIP-51 identifier is `"metadesk-allowlist"` and installs it as *the* access-control list. It never compares `nostr_event_get_pubkey(ev)` with `n->pk_hex`, and never verifies the signature. The author constraint exists only in the REQ filter (`nostr.c:1046-1060`) — i.e. it is enforced by the remote relay. The pool does not verify signatures either: `pool->signature_checker = NULL` (`libnostr/src/simplepool.c:343`).

**Correct shape.**
```c
const char *author = nostr_event_get_pubkey(ev);
char canon[65];
if (!author || strcmp(author, n->pk_hex) != 0) return;
if (nostr_event_validate(ev, canon) != NOSTR_EVENT_VALIDATION_OK) return;
```
(`nostr_event_validate()` — `nostr-event.h:104-110` — binds id to the canonical hash and verifies the Schnorr signature.)

**Cost.** A malicious or compromised relay can push a forged allowlist and add itself (or anyone) to the host's authorized-clients set; the host then hands out desktop sessions. This defeats the purpose of the NIP-51 gate.

---

### F5 — Two `nostr_simple_pool_subscribe()` calls: the second frees the filters the first subscription still points at
**structural · C4 · `nostr.c:1036-1073` vs `706-736`**

**Observed.** metadesk subscribes twice against the same pool: kind:1059 at create (`:726`) and kind:30000 in `md_nostr_refresh_allowlist()` (`:1070`). In nostrc, `pool_subscribe_impl()` deep-copies the caller's filters into `owned`, then **frees the previous `pool->filters_shared`** and installs the new one (`libnostr/src/simplepool.c`, "Replace pool->filters_shared"). Live subscriptions hold a borrowed pointer: `nostr_subscription_new()` does `sub->filters = filters;` (`libnostr/src/subscription.c`) and the match path uses `sub->priv->match = nostr_filters_match` against it.

So the allowlist subscribe leaves the DM subscription matching against freed memory, and the pool's shared filter set no longer describes the DM REQ.

**Correct shape.** Compose one filter set and subscribe once:
```c
NostrFilters *filters = nostr_filters_new();
nostr_filters_add(filters, dm_filter);            /* kind:1059, #p = us      */
nostr_filters_add(filters, allowlist_filter);     /* kind:30000, authors=us  */
nostr_simple_pool_subscribe(n->pool, urls, n->relay_count, *filters, true);
```
If the allowlist REQ must be deferred, rebuild the full merged set on each subscribe call rather than issuing a second independent one — or drop to `nostr_relay_prepare_subscription()` + `nostr_subscription_fire()` per relay with filters this module owns.

**Cost.** Use-after-free in the pool worker thread, and the DM subscription's local filter matching becomes undefined the moment allowlist refresh runs — which the host does at startup.

---

### F6 — `n->allowlist` is cross-thread shared state with no lock, in a struct that locks its dedup ring
**structural · C4 · `nostr.c:78-80`, `466-485`, `1008-1215`**

**Observed.** The pool worker thread frees and replaces `n->allowlist` (`:470-478`). `md_nostr_is_allowed()`, `md_nostr_has_allowlist()`, `md_nostr_allowlist_add/remove/count/get_entry()` read and mutate the same pointer and its `entries[]` array from caller threads, unguarded. `md_nostr_allowlist_get_entry()` even hands out interior pointers (`out->pubkey_hex = entry->value`). The same struct already demonstrates the right pattern for the dedup ring (`dedup_mu`, `:96`), so this is inconsistency, not an unknown.

**Correct shape.** Add `pthread_mutex_t allowlist_mu`, take it in the handler around the replace and in every `md_nostr_allowlist_*` / `md_nostr_is_allowed` body; for the entry accessor, copy into the caller's `MdAllowlistEntry` storage rather than aliasing.

**Cost.** A relay-pushed refresh that lands while the host is authorizing a client frees the array under the iteration at `:1017-1024` — a use-after-free on the access-control path, i.e. a crash or a wrong allow decision.

---

### F7 — NIP-55L signing produces an event with no `id`
**structural · C4, S5 · `signer.c:515-545`**

**Observed.** `nip55l_sign_event()` calls `nostr_nip55l_sign_event()` (which returns *only* a signature), deserializes the caller's **unsigned** event JSON, does `free(ev->sig); ev->sig = sig;`, and re-serializes. The input never had an `id`, and nothing computes one; `nostr_event_serialize_compact()` emits the `id` field only `if (event->id && *event->id)` (`libnostr/src/event.c`). The result is a "signed" event JSON with a `sig` and no `id`, which every relay rejects.

The same nostrc header metadesk already includes documents the right call (`nips/nip55l/include/nostr/nip55l/signer_ops.h:24-28`): *"Sign an event and return the complete signed event JSON. The returned JSON includes id, pubkey, created_at, kind, tags, content, sig."*

**Correct shape.**
```c
char *signed_json = NULL;
int ret = nostr_nip55l_sign_event_json(event_json, "", "", &signed_json);
if (ret != 0 || !signed_json) return MD_SIGNER_ERR_CONNECT;
*out_signed_json = signed_json;
```
(or `nostr_nip55l_sign_event_full()` + `nostr_event_set_sig()` if the id/pubkey are needed separately.) This also deletes the deserialize/re-serialize round trip.

**Cost.** The entire NIP-55L backend — one of four advertised signer backends — cannot publish anything. Nothing catches it because the failure appears at the relay as an OK rejection, which `publish_all` ignores (F9).

---

### F8 — A hand-rolled HTTP client next to a required, already-linked libcurl
**structural · S4, C4 · `secrets.c:250-380`**

**Observed.** `http_get()` is ~130 lines of `getaddrinfo` + `socket` + `setsockopt(SO_RCVTIMEO)` + a `snprintf` request into a fixed 2 KB buffer + `strstr(response, "\r\n\r\n")` + `atoi(response + 9)`, with a `_WIN32` shim block at `:29-45`. It ignores `Content-Length`, does not handle `Transfer-Encoding: chunked`, caps responses at 256 KB, and `parse_url()` hard-refuses `https://` (`:98-103`) because "TLS is not implemented". The file header justifies this as *"No external HTTP library dependency"* — but `meson.build:133` declares `dependency('libcurl', version:'>=7.80', required: true)` and `:239` puts `dep_curl` in `core_deps`, the same target that compiles `secrets.c`. The dependency is there for NIP-11; secrets.c just doesn't use it.

**Correct shape.**
```c
CURL *c = curl_easy_init();
struct curl_slist *hdrs = curl_slist_append(NULL, auth_header);   /* Bearer */
curl_easy_setopt(c, CURLOPT_URL, url);
curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, MD_SECRETS_TIMEOUT_MS);
CURLcode rc = curl_easy_perform(c);
long status; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
```

**Cost.** ~200 lines of transport code (including a Windows socket shim) to maintain; an unhandled chunked response silently becomes a JSON parse failure reported as "vault not found"; and the "no TLS" limitation — which forces the 1Password Connect server onto loopback and produces the plaintext-token warning at `:585-590` — disappears for free.

---

### F9 — `publish_all()` cannot fail, so every publish API reports success it never observed
**confidence · S5 · `nostr.c:151-172`**

**Observed.** `publish_all()` fans out `nostr_relay_publish()` (a `void`, transport-level enqueue) across relays and returns `0` unless `relay_count == 0`. `md_nostr_send_session_request/accept`, `md_nostr_allowlist_add/remove` and `md_nostr_publish_signed_json` all propagate that, and `nostr.h:126` documents *"Returns 0 on publish success."* Rejections surface only as an `fprintf` in `md_nostr_ok_handler` plus an optional callback that has no correlation back to the caller (send_dm never returns the event id). `session_log.c:228-236` consequently logs a warning it can never reach.

**Correct shape.** `nostr_relay_publish_and_wait(relay, ev, timeout_ms, &err)` (`nostr-relay.h:139-152`: *"Returns: true only when the relay sends OK with ok=true"*) inside the existing per-relay threads; accumulate successes and return `-1` when none accepted. The existing `GoWaitGroup` fan-out is already the right structure for it.

**Cost.** A host reports a session accept as delivered when every relay dropped it; the audit log records "published" entries that no relay stored. This is the failure mode F7 hides behind.

---

### F10 — Every HTTP failure mode collapses to `NULL`, producing wrong diagnostics
**confidence · S5 · `secrets.c:250-380`, `410-560`**

**Observed.** `http_get()` returns `NULL` for DNS failure, connect failure, non-2xx status, missing header terminator, and allocation failure alike (`:290-370`). Callers then print specific, false causes: an expired bearer token (HTTP 401) surfaces as `"secrets: vault '%s' not found"` (`:620`), a 500 as `"item '%s' not found"`. `md_secrets_is_connected()` reports `false` for a 403 the same as for a dead server.

**Correct shape.** Return a status alongside the body — with libcurl (F8), `CURLcode` + `CURLINFO_RESPONSE_CODE`; without it, an `int *http_status` out-param — and branch the messages on `401/403` vs `404` vs transport error.

**Cost.** Operators chase vault/item naming for what is an auth or connectivity failure. This is the single most common bootstrap failure of the whole product and it reports the wrong cause.

---

### F11 — `secrets.h` documents a zeroing guarantee the implementation cannot provide
**confidence · S3 · `secrets.h:41` vs `secrets.c:562-600`**

**Observed.** Header: *"The token is copied into mlock'd memory and the original is zeroed."* The signature is `md_secrets_create(const char *connect_url, const char *token)`; the body copies into `s->token` and never writes to the caller's buffer. Callers reading the header will reasonably skip zeroing their own copy.

**Correct shape.** Either change the contract to `char *token` and `md_secure_zero(token, token_len)` after copying, or correct the doc to *"the caller remains responsible for zeroing its own copy."* Given Spec §7's "no secrets on disk or in env vars" posture, the first is preferable.

**Cost.** A bootstrap token left resident in caller memory (and possibly swapped) in the one module whose stated purpose is to prevent that.

---

### F12 — 16 numeric kind literals where nostrc ships the registry header
**drift · C1 · `nostr.c:198,440,466,719,875,897,957,1054,1096,1166`; `session_log.h:60`**

**Observed.** `22242` (`:198`), `1059` (`:440`, `:719`, `:957`), `30000` (`:466`, `:1054`, `:1096`, `:1166`), `14` (`:875`), `13` (`:897`) appear as bare integers in executable code (the comment occurrences are additional). `libnostr/include/nostr-kinds.h` defines `NOSTR_KIND_SEAL 13`, `NOSTR_KIND_DIRECT_MESSAGE 14`, `NOSTR_KIND_GIFT_WRAP 1059`, `NOSTR_KIND_CLIENT_AUTHENTICATION 22242`, `NOSTR_KIND_NOSTR_CONNECT 24133`, `NOSTR_KIND_CATEGORIZED_PEOPLE_LIST 30000`, plus `nostr_kind_is_addressable()` helpers. No metadesk file includes it. nostrc's own NIP-17 code uses the constants (`nips/nip17/src/nip17.c`: `nostr_event_set_kind(gift_wrap, NOSTR_KIND_GIFT_WRAP)`).

**Correct shape.** `#include <nostr-kinds.h>` in `nostr.c` and substitute the constants; the kind checks in `md_nostr_event_handler` become `case NOSTR_KIND_GIFT_WRAP:` / `case NOSTR_KIND_CATEGORIZED_PEOPLE_LIST:`.

**Cost.** Grepping for "which kinds does metadesk speak" requires reading numbers; a kind change (or a registry addition such as a dedicated metadesk family) has 16 independent edit sites, some inside comments that already drifted (`signer.h:23` names kind:24133 in prose only).

---

### F13 — The NIP-42 AUTH event is rebuilt by hand
**drift · S4, C4 · `nostr.c:184-250`**

**Observed.** `md_nostr_auth_handler()` allocates an event, sets kind `22242`, builds `challenge` and `relay` tags, handles four cleanup paths, signs through the signer and publishes — ~65 lines. `nostr_relay_auth()` (`libnostr/src/relay.c`) constructs the identical event (`kind = NOSTR_KIND_CLIENT_AUTHENTICATION`, `nostr_tags_new(2, challenge, relay)`, empty content), calls a caller-supplied `sign` callback, and publishes it, taking the challenge from the relay's own state.

**Correct shape.** `nostr_relay_auth(relay, md_sign_auth_event, &err)` from the auth callback. The one wrinkle is that the `sign` callback is `void (*)(NostrEvent *, Error **)` with no `user_data`, so the signer must reach it another way; if that is unacceptable, keep the local builder but use `NOSTR_KIND_CLIENT_AUTHENTICATION` and `nostr_tags_new` as upstream does — the current hand-built version is at minimum a kind-literal and tag-order fork of the canonical one.

**Cost.** metadesk now owns a second definition of the NIP-42 frame; any NIP-42 clarification (e.g. added tags) must be applied twice, and a divergence shows up only as relays refusing subscriptions.

---

### F14 — `dep_nip17` / `nip17.h`: linked, included, never called
**drift · S6 · `nostr.h:36`, `meson.build:237`**

**Observed.** `nostr.h:36` includes `<nostr/nip17/nip17.h>` and `meson.build:237` links `dep_nip17` into `core_deps`. `grep -rn "nostr_nip17_" src/ tests/` returns nothing — the only "nip17" hits are a local `has_nip17` boolean in the NIP-11 probe. The library whose absence the hand-rolled chain would justify is present and paid for.

**Correct shape.** Adopt it (F1). If the signer constraint genuinely rules out every entry point — it does not, `create_rumor` and `create_gift_wrap` take no secret key — then remove the include and the `dep_nip17` entry so the dependency list reflects reality.

**Cost.** A dependency that implies coverage it does not provide; readers of `nostr.h` reasonably assume the DM path is library-backed.

---

### F15 — Three hand-rolled hex codecs
**drift · S4 · `nostr.c:43-52`, `signer.c:35-43`, `signer.c:469-482`**

**Observed.** `nostr.c:hex_to_bytes()` (nibble table), `signer.c:hex_to_bin()` (`sscanf("%2x")` per byte), and `signer.c:npub_to_hex()`'s inline `hexd[]` encode loop — three implementations of two operations, in two files of the same module. `nostr-utils.h:91,100` exports `bool nostr_hex2bin(unsigned char *bin, const char *hex, size_t bin_len)` and `char *nostr_bin2hex(const unsigned char *bin, size_t len)`; nostrc's own NIP-17 code uses them.

**Correct shape.** Delete all three; `#include <nostr-utils.h>` and call `nostr_hex2bin` / `nostr_bin2hex`.

**Cost.** Three places to get hex validation wrong (the `sscanf` variant accepts leading whitespace and `+`/`-` forms that the nibble variant rejects — the two disagree on what a valid key is), for zero functionality.

---

### F16 — Writing `NostrEvent` fields directly past the setters
**drift · C4 · `signer.c:87-88`, `signer.c:528-529`**

**Observed.**
```c
free(ev->pubkey); ev->pubkey = strdup(dk->pk_hex);   /* direct_sign_event  */
free(ev->sig);    ev->sig    = sig;                  /* nip55l_sign_event  */
```
`nostr_event_set_pubkey()` (`nostr-event.h:170`) and `nostr_event_set_sig()` (`:242`) exist and are used everywhere else in this codebase (`nostr.c` uses the setters exclusively).

**Correct shape.** `nostr_event_set_pubkey(ev, dk->pk_hex);` and `nostr_event_set_sig(ev, sig); free(sig);`.

**Cost.** Bypasses whatever invariants the setters maintain (notably id invalidation — see F7, which is precisely this pattern's failure), and breaks the moment nostrc makes `NostrEvent` opaque, which its in-flight GObject work points at.

---

### F17 — A second Nostr-event builder and a second signer round-trip in `session_log.c`
**drift · S4, C5 · `session_log.c:116-168`, `195-225`**

**Observed.** `build_unsigned_event_json()` assembles a Nostr event with raw cJSON — `kind`, `content`, `pubkey`, `created_at`, and hand-built `["d",...]`, `["p",...]`, `["session",...]` tag arrays (50 lines, four nested null-checks per tag). `nostr.c` builds the same shape with `nostr_event_new()` + `nostr_tags_new()`. `md_session_log_event()` then re-implements `sign_event_via_signer()` (`nostr.c:806-830`): get pubkey → build unsigned JSON → `md_signer_sign_event` → keep JSON.

**Correct shape.** Promote `sign_event_via_signer()` to a shared helper — e.g. `md_nostr_sign_event(MdSigner *, NostrEvent *, char **out_json)` in `nostr.h` — and build the log event with `nostr_event_new` / `nostr_tags_new` / `nostr_event_serialize`. `session_log.c` then drops its cJSON event builder entirely (the `md_session_log_build_content` payload builder can stay; that is genuinely metadesk-specific).

**Cost.** Two event builders drift independently: this one already serializes `created_at` through `cJSON_AddNumberToObject((double))` while the other path uses int64, and it omits `tags` entirely if the array allocation fails (`:132`), producing an event whose signature covers a different shape than intended.

---

### F18 — kind 1078 with a `d` tag on a non-addressable event
**drift · C1 · `session_log.h:60`, `session_log.c:124-140`**

**Observed.** `#define MD_SESSION_LOG_KIND 1078` — an unassigned kind, absent from `nostr-kinds.h` — carrying `["d","metadesk-session-log"]` with the comment *"not an addressable event but useful for filtering"*, i.e. the author knew the tag's defined semantics do not apply here.

**Correct shape.** Two coherent options, both concrete: (a) if entries are meant to be an append-only audit stream, keep a regular kind and use `["t","metadesk-session-log"]` — the standard topic tag, filterable as `#t`, with no addressable-event implication; (b) if the intent is app state, use `NOSTR_KIND_APPLICATION_SPECIFIC_DATA` (30078, NIP-78), where the `d` tag is the defined identifier. Either way the kind belongs in a shared constants header, not as a private `#define` in `session_log.h`.

**Cost.** metadesk squats an unregistered kind and attaches a tag whose semantics it explicitly disclaims, so no third-party client or relay policy can reason about the audit stream.

---

### F19 — `close()` on a `sock_t` where the file defines `sock_close()` for exactly this
**drift · C4 · `secrets.c:320`, `secrets.c:336`**

**Observed.** The file defines `sock_close(s)` as `closesocket()`/`close()` per platform (`:33-44`) and uses it on five exit paths (`:301`, `:326`, `:347`). Two paths — the oversized-request bail (`:320`) and the response-buffer allocation failure (`:336`) — call bare `close(fd)`. On Windows `unistd.h` is not included and `fd` is a `SOCKET`, so these paths do not compile / leak the handle.

**Correct shape.** `sock_close(fd);` on both lines.

**Cost.** The declared cross-platform support of this module is untested on the branch that matters; each hit leaks a socket on POSIX only by luck of `close()` working there.

---

### F20 — A predicate that performs a blocking RPC and discards the reason
**drift · S1 · `signer.c:862-875`**

**Observed.** `md_signer_is_ready()` — described in `signer.h:219` as *"Check if the signer is ready (connected, key available)"* — casts away `const` and calls `ops->get_pubkey()`, which for NIP-46 is a relay round trip, for NIP-55L a D-Bus call, for NIP-5F a socket round trip. The result is reduced to a bool and the `MdSignerError` discarded. `signer.h:30-33` warns callers that signer ops may block; the name gives no such warning.

**Correct shape.** Make the predicate cheap — `return s && s->ops && s->pubkey_hex != NULL;` (the constructors already cache the pubkey on success) — and expose `MdSignerError md_signer_probe(MdSigner *)` for callers that want to pay for a live round trip and see why it failed.

**Cost.** UI or session code calling a bool-returning "is_ready" can block on network I/O and cannot distinguish "denied" from "timed out" from "no key".

---

### F21 — An application dedup ring duplicating the pool's dedup, with a guarantee it cannot make
**load · S1, S3 · `nostr.c:68-133`, `434-436`, `777-779`**

**Observed.** A 64-entry ring with a dedicated mutex and linear scan, documented as *"a small fixed-size ring that tracks recent event IDs to guarantee idempotent processing."* `NostrSimplePool` already carries `dedup_unique`, `dedup_hash`, `dedup_cap`, `dedup_evict` (`nostr-simple-pool.h:53-61`), and metadesk passes `unique = true` on both subscribes (`:728`, `:1071`). The stated cause — "reconnect replays and cross-relay overlap" — is the pool's dedup's job; and a 64-entry FIFO cannot *guarantee* anything, since 65 interleaved events evict the first.

**Correct shape.** Delete `MD_DEDUP_RING_CAP`, `dedup_ids/head/len`, `dedup_mu`, `dedup_is_seen()`, the call site, and the destroy-time teardown. If a stronger guarantee is genuinely required, raise `pool->dedup_cap` (one field) rather than adding a weaker second layer.

**Cost.** ~60 lines, a mutex acquisition per event, a SHA-256 per event (F3), a leak per event (F3), and a comment that will mislead the next reader into trusting an idempotency property the code does not have.

---

### F22 — 110 lines of NIP-11 probing that blocks startup and produces only log text
**load · S9, C5 · `nostr.c:488-600`, `654-670`**

**Observed.** `probe_relay_nip11_impl()` fetches each relay's info document, prints name/software/supported-NIPs/limitations, warns when NIP-17 is unadvertised, and frees the document. `probe_relay_nip11_thread` + a `GoWaitGroup` fan-out make `md_nostr_create()` block until the slowest relay's HTTPS fetch finishes. **No value escapes the function** — `auth_required`, `max_content_length`, `max_message_length`, `payment_required` and the NIP-17 flag are all discarded. The header advertises this as "capability detection" (`nostr.h:106-107`).

**Correct shape.** Either delete the block (the NIP-42 handler is installed unconditionally anyway, so `auth_required` changes nothing), or make it load-bearing: store per-relay `max_content_length` and NIP-17 support on the cached relay entries and use them — publish gift-wraps only to relays advertising NIP-17, reject oversized content before enqueue. `nostr_nip11_fetch_info()` results already contain exactly those fields.

**Cost.** Every process start pays a full HTTPS round trip per relay before the first byte of session traffic, plus a thread per relay, for stderr text nobody acts on.

---

### F23 — The allowlist event builder exists twice; removal reaches into nostrc's list internals
**load · S4 · `nostr.c:1092-1125` vs `1162-1190`**

**Observed.** `md_nostr_allowlist_add()` and `md_nostr_allowlist_remove()` contain the same 25-line block: new event, kind `30000`, pubkey, created_at, `nostr_tags_new(0)`, `d` tag, per-entry tag loop with the `e->extra ? 3-arg : 2-arg` branch, `set_tags`, `sign_event_via_signer`, `publish_all`, success log. The only difference is the log string. Additionally, `_remove` mutates `NostrList` internals directly — `nostr_nip51_entry_free(entry)` then shifting `n->allowlist->entries[j] = entries[j+1]` and decrementing `count` — because nip51 exposes `list_add_entry` but no remove.

**Correct shape.** One `static NostrEvent *build_allowlist_event(MdNostr *n)` called by both, halving the surface. For removal, rebuild the list through the public API (`nostr_nip51_list_new()` + `nostr_nip51_list_add_entry()` for survivors + `set_identifier`) instead of editing the array in place, or add `nostr_nip51_list_remove_entry()` upstream and use it.

**Cost.** Any tag-schema change (private entries, `title`, expiry) must be made twice or the two published variants of the same list diverge; the in-place shift depends on a struct layout nostrc does not promise.

---

### F24 — `fprintf(stderr)` is the logging system
**load · C5 · 59 sites across the four `.c` files**

**Observed.** 29 in `nostr.c`, 20 in `signer.c`, 7 in `secrets.c`, 3 in `session_log.c`. These are library modules linked into both the headless host and the ImGui client. Several are on hot or repeated paths: `md_nostr_ok_handler` per publish result, `warn_allowlist_startup_window()` on *every* authorization check (`:998-1006`), `session_log` on every event, the whole NIP-11 block. Levels are encoded as prose prefixes — `"WARNING:"`, `"ERROR:"`, `"NOTE:"`, `"WARNING —"` — with no consistency, and nothing can filter or redirect them. The repo has no logging module at all.

**Correct shape.** A ~30-line `src/core/md_log.h`: `MD_LOG_{ERROR,WARN,INFO,DEBUG}(cat, fmt, ...)` dispatching to a host-installed sink (default: stderr), so the client can route to its console and the host to syslog/journal. Mechanical substitution afterwards.

**Cost.** A GUI client that spews relay chatter to a terminal nobody reads; no way to raise the level in production or capture structured diagnostics; and in the allowlist case, unbounded log growth proportional to connection attempts.

---

### F25 — The readiness log reports configured relays as connected
**cosmetic · S3 · `nostr.c:739-741`**

**Observed.** `"nostr: bridge ready (signer=%s, pk=%.8s..., relays=%d)"` prints `cfg->relay_count`. The actual usable count is `n->relay_count`, populated from the pool snapshot at `:678-690` and possibly smaller (a URL that failed `ensure_relay`, a `strdup` failure, or the `calloc` failure path at `:676` that leaves it at 0 and continues). With `n->relay_count == 0`, `publish_all()` returns `-1` for every publish forever, and `md_nostr_create()` still returns a "ready" bridge.

**Correct shape.** Print `n->relay_count`, and fail creation (`goto fail`) when it is zero — a bridge with no relays cannot perform any of its advertised operations.

**Cost.** The startup line that operators read to confirm connectivity confirms only that they typed some URLs.

---

## Recommended sequencing

1. **F1, F7, F9** — correctness of the publish path. Without F9 the other two are invisible.
2. **F4, F6** — the allowlist is access control; it is currently relay-trusting and racy.
3. **F2, F3, F21, F5** — the event-ingest path: adopting `event_middleware_ex` deletes the registry, and deleting the dedup ring deletes the leak. These four are mostly subtraction (~150 lines removed).
4. **F8, F10, F11, F19** — the secrets module: one libcurl rewrite subsumes all four.
5. **F12, F15, F16, F13, F17, F23** — mechanical de-duplication against nostrc.
6. **F22, F24, F20, F25, F14, F18** — cleanup and honesty.

Net effect if applied in full: roughly 450 lines deleted, four re-implementations replaced by already-linked library calls, and three silent failure modes (undeliverable DMs, unsigned-id NIP-55L events, unacknowledged publishes) made visible.
