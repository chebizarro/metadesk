# Critique: md-ah9 Container Runtime Plan

**Target:** `docs/plans/container-runtime-2026-11-14.md` (the "plan")
**Baseline:** the generated plan in `prompt-exports/oracle-plan-2026-11-14-144116-build-run-metadesk-c-1cac.md`, from `# 1. Summary` (export L100) to the end (the "export"). The export's composed prompt and file selection were read only as context.
**Date:** 2026-11-14 · **Scope:** md-ah9 only. OCI publishing (md-kb7) and E2E cells (md-5zi) are discussed only where the plan depends on them.

## Context / Scope

I diffed the plan against the export (`diff` of export L100–935 against the plan). The plan is the export **verbatim**, with three changes:

1. A new title and spike header (plan L1–3).
2. The export's "Delivery status" paragraph is replaced by a shorter one (plan L9).
3. A new **§1.1 Execution index** (plan L11–26, work items P0 and W1–W9).

So most findings below apply equally to the export. Category 1 is short for that reason. Spot checks focused on the load-bearing seams in `src/host/main.c` (MCP branch ordering) and `src/core/mcp_bridge.c` (capability from the input pointer, a11y subscription). They then followed the call chain into `agent.c`, `a11y_atspi.c`, `mcp_http.c`, `mcp_stdio.c` and `mcp_server.c`. Several plan claims depend on those files, so the checks covered more than two seams. `capture_pipewire.c` was **not** re-checked.

Line numbers below refer to the current working tree. `docs/plans/` is untracked (`git status`: `?? docs/plans/`), so there is no history showing what the pre-spike Background looked like. Whether the Background was "preserved verbatim in substance" cannot be verified from git.

---

## 1. Export content missing, weakened, or generalized in the plan

| # | Item | Detail | Correction |
|---|---|---|---|
| 1.1 | Delivery status weakened | The export said "No files were changed, builds run, deployments validated, **or issue updates submitted**." The plan (L9) drops the issue-status clause. | Restore one line saying spin-offs W1–W9 are **not yet filed** in bd, so readers don't assume they exist. |
| 1.2 | Export-session artifacts kept as if they were plan text | The plan keeps wording that only made sense inside the context_builder run: "The revised document should distinguish" (L32); "*Retain the existing section below*" (L44); "the supplied material omits `a11y.h` … stdio implementation" (L314); "neither **supplied** executable enables them" (L338); "not fetched or revalidated during this text-only pass" (L711); §4 row "Replace open questions with this architecture" (L741); Phase 0 step 1 "Update the plan" (L787). | Rewrite as statements about the repo. L314 in particular is now answerable from the code (see 3.4). The plan *is* the updated doc, so Phase 0 step 1 is done. |
| 1.3 | Evidence conventions declared but never applied | L30–40 defines five evidence labels and says "historical anchors must remain intact". Yet none of the nine "Additional code findings" rows carries a file:line anchor or a label. | Add anchors. Verified ones: signer/Nostr setup `main.c:635–760` runs before the MCP branch `main.c:857–916`; 60 s wait via `go_select_timeout(…, 60000)` (~L744); `printf` diagnostics `main.c:860–915`; capability from pointer `mcp_bridge.c:47`; subscribe with no unsubscribe `mcp_bridge.c:91–96` vs destroy `mcp_bridge.c:168–187`; `http_bind_addr` pass-through `mcp_bridge.c:114`; `::1` default `mcp_http.c:611`; `g_running`-only handler `main.c:44–58`. |

Nothing implementation-bearing from the export was dropped.

---

## 2. Under-specified seams, contradictions, wrong references, missing dependencies

### 2.1 Execution index contradicts the body

| Index row | Conflict | Fix |
|---|---|---|
| **W9** key file `deploy/containers/bundle-contract.md` (L26) | The artifact tree and §4 both say `deploy/containers/reference/bundle-contract.md` (L536, L749). | Use one path. `reference/` matches the tree. |
| **W8** depends on "W1–W4" (L25) | §3.9 and Phase 0 step 3 say the cells are *proposed* to md-5zi at P0 review time. The §6 spin-off table says the dependency is "Runtime prerequisites", not W4. Gating on W4 (video) delays the md-5zi handoff by the whole video experiment. | W8 depends on **P0** (propose cells). Mark individual cells "pending W1/W4" inside md-5zi. |
| **W4** key files "`deploy/containers/reference/*` (Sway session)" (L21) | §3.8's artifact set has **no** Sway/PipeWire/xdpw files. `Dockerfile.agent` is MCP-only, and no Tier B image contract exists anywhere. | Either add `Dockerfile.video` + `reference/sway-session.sh` to the artifact set with a contract table, or state that W4 creates them and P0 does not. |
| **W5** key files (L22) | §3.6 lists "Host CLI and input tests" (an input-backend selection flag in `src/host/main.c`). The index omits the host CLI. | Add `src/host/main.c`. |
| **W1** key files (L18) | These omit `src/core/agent.c` (needed for 3.1), `a11y_axui.m` / `a11y_uia.cpp` (needed for 3.4), and `mcp_http.c`'s Origin/Host handling (needed for 4.1). | Extend the list, and resize W1 (see 3.2–3.4: some work shrinks, some grows). |
| **W3** (L20) | The goal is "FIPS sidecar", but Phase 3 also validates the agent-only Kubernetes Pod (step 12). md-5zi's "Kubernetes / pinned desktop node" cell has no owning work item. | Split out a W3b "K8s agent-only Pod", or fold it explicitly into W2. |
| **W5/W6** depend on W4 | The virtual-input backend can be built against any nested wlroots compositor, and the portal-required policy is testable against a host xdpw. Neither needs the containerized Sway reference. | Make these soft dependencies so W5 and W6 can run in parallel with W4. That changes the critical path. |

### 2.2 Runtime-bundle ownership is circular

W2 depends on the "md-kb7 bundle" (L19), and Phase 2 step 7 says "Produce a local runtime bundle through the **md-kb7-agreed** path" (L803). Meanwhile W9 has md-kb7 *consuming* md-ah9's contract (L26). Nobody is assigned to produce the first bundle W2 needs. **Decision needed:** either md-ah9 ships a throwaway local bundling procedure (`meson install --destdir` + DT_NEEDED closure check) in `bundle-contract.md`, or W2 is blocked on md-kb7 tooling. These lead to different implementation orders.

### 2.3 Other under-specified seams

- **`::1:7710` notation** (L274) isn't a valid address/port form. Use `[::1]:7710`, as L260 already does.
- **Phase 0 exit criteria** (L791) say "every open architectural question has a decision". The plan still has open questions: bundle ownership (2.2), the FIPS adapter (deliberately operator-supplied, §3.4), IPv6 behaviour (only a negative axis), and the recovery policy (4.4). Either the exit criterion is wrong or Phase 0 isn't done.
- **`XDG_RUNTIME_DIR` on a read-only root** (mount table, §3.2): "image-created … mode 0700" on `read_only: true` leaves nothing writable at that path for GLib/dconf. Specify a tmpfs (Docker) or memory `emptyDir` (K8s) at `/run/user/1000`, with the bus socket and AT-SPI directory bind-mounted *inside* it.
- **Missing process dependency:** W1 reorders Nostr negotiation in `main.c`. Per `AGENTS.md`, Nostr-touching changes need a **Nostr Protocol Review** before merge. Add this to W1's acceptance criteria.

---

## 3. Claims disproved by code, unneeded, or replaced by a simpler design

### 3.1 "Existing tool errors" for unavailable input do not exist — **disproved**

Plan L199: "operations requiring unavailable input must return existing tool errors rather than report success."
`agent.c:192–200`, in `agent_execute_locked`: when `agent->input == NULL` the injection block is skipped and execution falls through to `action_count++`, the settle sleep, and `return 0`. `mcp_tools.c:286–298` reports a non-NULL result as success. Under the planned `--no-input`, `metadesk_click`, `metadesk_type`, `metadesk_set_value`, `metadesk_screenshot` and the rest all **report success while doing nothing**. That is the fake-success pattern the 2026-07 audit targeted.
**Correction:** add a W1 row: "With no input, input-requiring actions return an MCP tool error (`is_error=true`) naming the missing capability." Add a `test_mcp` case. Decide whether these tools stay listed (see Q5).

### 3.2 "HTTP concurrency" row is already satisfied — **not required as code work**

Plan L304 lists serialization as required behavior. `mcp_server.c:35` has `dispatch_mu` ("serializes request dispatch"), held across dispatch at `mcp_server.c:357–365`. The agent also has its own `mu` (`agent.c:38, 252–293`).
**Correction:** downgrade this to "add a concurrent-POST regression test in `test_mcp_http.c`". Remove it from W1's code scope. §4 already hedges for `mcp_server.c` with "change only if existing dispatch lacks it". The two places should agree.

### 3.3 Most HTTP cancellation already exists; the real gaps are narrower — **partially disproved**

Plan L302 says shutdown must "stop HTTP accepts, interrupt idle connection I/O, drain workers". The code already does most of this:

- The accept loop re-checks `shutdown` every second (`select` with a 1 s timeout, `mcp_http.c:639–652`).
- `md_mcp_http_shutdown` closes SSE clients (`mcp_http.c:676–681`).
- Idle reads are bounded by `SO_RCVTIMEO` (`mcp_http.c:322–326`).
- `destroy` waits on the handler wait-group (`mcp_http.c:683–688`).

The actual gaps:

1. **No production caller of shutdown.** `md_mcp_bridge_shutdown` / `md_mcp_http_shutdown` are called only from `tests/test_mcp_http.c`. The host never calls them, so SIGTERM only flips `g_running` (`main.c:44–48`), which the MCP loops ignore.
2. **stdio read can't be woken.** `mcp_stdio.c:93–114` is a blocking `read()` that retries on `EINTR` and checks only `ctx->shutdown`.
3. **The timeout budget contradicts the acceptance criteria.** `CLIENT_READ_TIMEOUT_SEC` is **30** (`mcp_http.c:35`). An idle keep-alive POST holds a handler, so `destroy`'s wait-group wait can take up to 30 s. That breaks "SIGTERM exits within 5 s" (L322) and equals the K8s "30-second Pod grace period" (L631), so the process races SIGKILL.

**Correction:** narrow W1 "Transport cancellation" to:

- (a) a host shutdown path that calls `md_mcp_bridge_shutdown`;
- (b) a wakeable stdio loop (`poll` on `in_fd` plus a wake fd);
- (c) on shutdown, `shutdown(fd, SHUT_RDWR)` on tracked in-flight client fds, or cut the read timeout below the 5 s budget;
- (d) one explicit budget table: read timeout < SIGTERM target < `terminationGracePeriodSeconds`.

### 3.4 Callback quiescence: the "validation required" caveat is answerable now, and the work is bigger than implied

Plan L314 defers the decision because `a11y.h` was "omitted". The code shows:

- `a11y.h:65,108` has `subscribe_changes` and **no unsubscribe**, in either the public API or the backend vtable.
- `atspi_subscribe_changes` refuses a NULL callback (`a11y_atspi.c:378`), so "subscribe(NULL)" can't act as an unsubscribe.
- The event path copies `change_cb` under `st->lock` and **invokes it after unlocking** (`a11y_atspi.c:238–246`). Clearing the pointer under the lock therefore does *not* quiesce an in-flight callback.
- A second subscribe silently replaces the callback and userdata (`a11y_atspi.c:381–386`).
- The vtable has three implementations: `a11y_atspi.c`, `a11y_axui.m:665`, `a11y_uia.cpp`.

**Correction:** specify `md_a11y_unsubscribe_changes(ctx, cb, userdata)` as a *blocking* detach. It clears the callback under the lock, then waits on an in-flight counter plus condition variable before returning. Implement it for all three backends, or provide a vtable default that returns an error. Call it in `md_mcp_bridge_destroy` before `md_mcp_server_destroy` (`mcp_bridge.c:183`). This also covers the construction-failure path: steps 7 and 8 can `goto fail` after the step-6 subscribe. This cross-platform HAL change belongs in W1's file list and sizing.

### 3.5 HTTP bind: "do not assume 127.0.0.1 works" is only true of the default

The listen socket clears `IPV6_V6ONLY` (`mcp_http.c:587`), and an IPv4 `bind_addr` is mapped to `::ffff:a.b.c.d` (`mcp_http.c:593–600`). So with the planned `--mcp-http-bind 127.0.0.1`, IPv4 loopback works with no new code.
**Correction:** reword that code-findings row. Consider making the reference manifests use `--mcp-http-bind 127.0.0.1`. Docker can disable IPv6 inside containers on non-IPv6 networks, which removes `::1` and makes the default bind fail, and some CRI port-forward implementations dial IPv4 loopback first. The plan currently lists "IPv6 disabled" only as a negative test axis (§3.9) without saying what the references should do. Both of those runtime behaviours are external and should be validated. See Q3.

### 3.6 Probe "verify an HTTP response from `[::1]:7710`" is unsafe as written

The only routes are `POST /mcp` and `GET /mcp`, and the GET opens an SSE stream. Every other path returns 404 (`mcp_http.c:405–450`). SSE is capped at **4 clients** (`mcp_http.c:29`, `440`), and SSE fds are pruned only on write failure (`mcp_http.c:507–528`). A periodic probe doing `GET /mcp` would fill those slots and lock out real clients. A POST requires `initialize`, which the plan forbids (L261).
**Correction:** the probe must use a TCP connect or a request to a non-`/mcp` path (a 404 shows the listener is up), and must never `GET /mcp`. Alternatively, add a `/healthz` route to W1.

### 3.7 FIPS sidecar is paired with the wrong tier

The Compose `agent` runs `--mcp-http 7710 --no-input`, binds the default `::1`, and gets the FIPS control directory mounted (L591, L598–601). But the MCP path never touches FIPS: the Background says "Default MCP flags perform no FIPS check", and the FIPS check sits only under `--npub` (`main.c:660–683`). MCP on `::1` also isn't reachable over the mesh. As specified, the mesh reference gives the agent routes it can't use and a socket it never opens.
**Correction:** pick one of these:

- (a) Pair the FIPS reference with the binary-session host or client, where `--npub` and `fips_control` actually run (Tier B / C1).
- (b) Keep Tier A but add the authenticated proxy as a third container, listening on the mesh address and forwarding to loopback. Drop the control-directory mount from the agent.

Without one of these, W3's "route + control-socket restart recovery" tests something the agent doesn't depend on.

### 3.8 Simpler: pin the a11y bus via the environment instead of path mirroring (verify upstream)

§3.2 mirrors the host's AT-SPI directory "at the same absolute path" so that `org.a11y.Bus.GetAddress` results resolve inside the container. libatspi checks `AT_SPI_BUS_ADDRESS` before asking the session bus. Setting `AT_SPI_BUS_ADDRESS=unix:path=<container mount>/bus` would:

- remove the same-path constraint;
- remove the session-bus round-trip from a11y discovery;
- let K8s use a fixed container path.

Tradeoff: an a11y-bus restart that changes the socket *name* goes unnoticed until restart. That risk already exists for the session bus (L238). *External behavior; confirm on the pinned at-spi2-core before adopting.*

---

## 4. Gaps absent from both export and plan

### 4.1 MCP HTTP has no in-process authentication or Origin/Host check, and "loopback" is shared (security, ownership)

`mcp_http.c` has no Origin, Host, or Authorization handling. A search of `src/` finds none for MCP. The plan assumes "localhost-only" means "agent-only", but:

- **Shared netns:** with Compose `network_mode: service:mesh` and in any K8s Pod, `[::1]:7710` is reachable from the FIPS container and every other container in the namespace. A compromised mesh sidecar gets desktop control.
- **Port-forward lands on a workstation:** `kubectl port-forward` exposes the unauthenticated endpoint on the operator's loopback, where browsers run. Without Host/Origin validation, a web page can POST to it (cross-origin simple requests, DNS rebinding).

L287 ("Reject unsolicited browser origins") is not assigned to any work item.
**Add to W1:** reject requests whose `Host` isn't a loopback literal or configured name, and reject any non-allowlisted `Origin`. Decide whether a bearer token read from a file (mode 0600, mounted secret) is in scope (Q2).

### 4.2 Signal delivery in a multithreaded host (cancellation)

`install_signal_handlers()` runs at `main.c:555`, but nothing blocks signals in threads created later (GLib a11y loop, libgo workers, Nostr). The kernel can deliver SIGTERM to any thread, so the main thread's `read()` or `select()` may never see `EINTR`. The plan's "set state and wake a self-pipe" (L301) doesn't say who reads the pipe. Also, `close_sse_clients` takes a mutex, so it can't run in a signal handler.
**Specify:** block SIGINT/SIGTERM before the first thread is created. A dedicated `sigwait`/`signalfd` thread then calls `md_mcp_bridge_shutdown`. stdio polls a wake fd. This is the concrete form of L301.

### 4.3 PID 1 during the entrypoint wait (lifecycle)

During `entrypoint.sh`'s 30 s readiness wait, the shell is PID 1. With no `trap`, PID 1 ignores SIGTERM, so `docker stop` and Pod deletion wait out the full grace period. After `exec`, the host is PID 1 and handles signals, but nothing reaps zombies.
**Specify:** `trap 'exit 143' TERM INT` in the entrypoint, or `init: true` / `--init` (tini) in the references.
**stdio corollary:** the MCP client that launches the container waits through that 30 s with no `initialize` response, which can exceed client startup timeouts. Document the bound, or use a shorter one for stdio.

### 4.4 Desktop loss has no recovery owner (failure behavior)

L238 says "Session-bus replacement still requires recreating the container." L267 says liveness is "process existence only". Nothing recreates the container, so after a logout or login the host stays alive, readiness stays false, and the container never recovers. **Decision needed (Q4):** have the host exit nonzero when the a11y/session bus disconnects, and let the restart policy recover it. A container restart re-resolves bind-mount sources, so *restart* is enough; *recreate* isn't needed. The alternative is an operator runbook. Whether the AT-SPI backend detects bus loss at all is unverified.

### 4.5 Compose shared-netns restart semantics (failure behavior)

With `network_mode: service:mesh`, restarting the mesh container gives it a new namespace, while `agent` stays in the old one, with no TUN and no routes. Podman Pods and K8s keep the namespace in an infra/pause container, so they don't have this problem.
**Simpler, robust design:** in Compose, add a minimal `netns` holder service (a pause image). Have both `mesh` and `agent` use `network_mode: service:netns`. This matches the Podman and K8s topology and makes W3's "restart recovery" achievable.

### 4.6 Teardown leaks in the MCP branch (ownership)

The MCP branch frees the signer, a11y and input (`main.c:909–914`) but never `nostr` or `session_log`, which were created at `main.c:725` / `main.c:694`. If bridge creation fails, it returns without freeing `mcp_input` or `signer` (`main.c:896–899`). Early dispatch (W1) fixes most of this. Add it to W1's acceptance so the reorder isn't credited without the cleanup.

### 4.7 Testability of the host-process test

W1's "new host process test" (§4) requires a non-NULL a11y (`mcp_bridge.c:36`). There is no mock a11y backend: `src/core` has only `a11y_atspi.c`, `a11y_axui.m` and `a11y_uia.cpp`, and live a11y tests are gated behind `-Dlive_tests` (`meson.build:439`). Without a fixture, CI can't assert stdout cleanliness or shutdown timing.
**Specify one:**

- (a) a test-only mock a11y backend selected at build time; or
- (b) a CI fixture running `dbus-run-session` + `at-spi-bus-launcher` + one accessible test app.

Option (b) also serves md-5zi.

---

## 5. Questions that would change the design or the order

1. **Who builds the first runtime bundle** for W2: md-ah9 (throwaway procedure) or md-kb7 (blocking)? This decides whether W2 can start right after W1.
2. **In-process MCP HTTP auth:** add Host/Origin validation (minimum) and/or a file-mounted bearer token in W1, or rely only on proxies? Given 4.1, proxy-only leaves shared-netns peers and port-forwarded workstations exposed.
3. **Reference HTTP bind:** `[::1]` (current default) or explicit `127.0.0.1` via `--mcp-http-bind` (works today through the v4-mapped path, 3.5)? This affects every Compose/K8s manifest and the IPv6 negative cell.
4. **Desktop-loss policy:** should the host exit on bus loss so the restart policy can recover (changes W1 and the liveness design), or stay up unready and rely on an operator runbook?
5. **`--no-input` tool surface:** unregister input-requiring tools from `tools/list`, or keep them listed and return errors (the plan's choice, which needs 3.1)? This affects agent UX and the md-5zi assertion.
6. **Which tier does the FIPS reference serve** (3.7)? If it's Tier B/C1, W3 moves after W4/W7. If it's Tier A, a proxy container joins the artifact set.
7. **Is the agent-only Kubernetes Pod in md-ah9 scope**, or deferred to md-5zi? It's the costliest reference: node pinning, hostPath sockets, port-forward IPv6 behavior. Nothing in the index owns it today (2.1, W3 row).

---

## Recommendations (priority order)

1. Fix the fake-success path (3.1) and add Host/Origin rejection (4.1) **inside W1**. Both are correctness/security preconditions for any Tier A claim.
2. Re-scope W1 against the code: drop the serialization work (3.2), narrow cancellation to the real gaps (3.3), and add the blocking a11y unsubscribe across three backends (3.4). Re-estimate the size.
3. Settle the FIPS tier pairing (3.7) and the Compose netns holder (4.5) before drafting `compose.agent.yaml`.
4. Fix the index inconsistencies (2.1) and the bundle-ownership circularity (2.2). Remove leftover export-session wording (1.2) and add anchors to the code-findings table (1.3).
5. Pin down the probe (3.6), the signal model (4.2), the entrypoint PID 1 behavior (4.3) and the timeout budget (3.3) as explicit contracts in §3.2/§3.3.
