#!/usr/bin/env python3
"""md-ah9.11 container client qualification (rootless Podman); see client-validation.md.
Updated by md-rk4 (no stdbuf: both binaries line-buffer stdout) and md-33e (GUI
mode without a granted display must refuse before any signer or session work).

Each cell starts a fresh Tier B Sway/xdpw host container (it captures only its
own isolated output; the host has a single startup negotiation) and a client
container. Negotiation is real: NIP-17 session DMs over a private relay, the
host role's NIP-51 allowlist with a v1 video grant (published by `access` with
a NIP-5F host account), explicit `--bunker` NIP-46 signers holding throwaway
keys at runtime, and the one-use stream binding. The FIPS control endpoint is a
fixture and TCP crosses a TUN-less IPv6 container bridge to the host's
npub-derived address. This is not a FIPS daemon or encrypted-transport receipt.

Cells: decode-only (headless) with stranger and legacy direct-IP negatives;
GUI into a private Sway display (authorized socket) and GUI without a granted
display (fail-closed refusal). Never mounts a host display, session bus or
runtime directory.
"""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

FIXTURE_IP = "10.89.97.10"
SUBNET4 = "10.89.97.0/24"
PORT = "7400"
SANDBOX = ["--userns", "keep-id", "--user", "1000:1000", "--read-only", "--cap-drop", "all",
           "--security-opt", "no-new-privileges", "--init", "--pids-limit", "256"]
results = []


class Incomplete(Exception):
    """A prerequisite failed (already recorded); later cells cannot run."""


def check(name, ok, detail=""):
    results.append({"check": name, "result": "PASS" if ok else "FAIL", "detail": detail})
    print(("PASS " if ok else "FAIL ") + name + (": " + detail if detail else ""), flush=True)
    return ok


def podman(*args, check_rc=True, timeout=120, **kw):
    p = subprocess.run(["podman", *args], capture_output=True, text=True, timeout=timeout, **kw)
    if check_rc and p.returncode:
        raise RuntimeError(f"podman {' '.join(args[:3])}... failed: {p.stderr.strip()}")
    return p


class Logs:
    """Follows `podman logs -f`; waiters block on a condition, never on sleeps."""

    def __init__(self, name):
        self.lines, self.done, self.cond = [], False, threading.Condition()
        self.proc = subprocess.Popen(["podman", "logs", "-f", name], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            with self.cond:
                self.lines.append(line.rstrip("\n"))
                self.cond.notify_all()
        with self.cond:
            self.done = True
            self.cond.notify_all()

    def wait(self, pattern, timeout, accept=lambda m: True):
        """First line matching pattern (and accept) within timeout, else None."""
        rx, deadline, i = re.compile(pattern), time.monotonic() + timeout, 0
        with self.cond:
            while True:
                for i in range(i, len(self.lines)):
                    m = rx.search(self.lines[i])
                    if m and accept(m):
                        return m
                i = len(self.lines)
                left = deadline - time.monotonic()
                if self.done or left <= 0:
                    return None
                self.cond.wait(left)

    def has(self, pattern):
        with self.cond:
            return any(re.search(pattern, line) for line in self.lines)

    def close(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)


class Run:
    def __init__(self, a, w, ids):
        self.a, self.w, self.ids, self.created, self.logs = a, w, ids, [], {}
        self.net = a.prefix + "-net"
        self.relay = f"ws://{FIXTURE_IP}:7777"
        self.fixture, self.counters, self.npub_clients = None, {}, 0

    def retire_fixture(self):
        name, self.fixture = self.fixture, None
        podman("stop", "-t", "5", name, check_rc=False)
        m = self.logs[name].wait(r"^fixture counters (.*)$", 10)
        counters = self.counters[name] = json.loads(m[1]) if m else None
        podman("rm", "-f", name, check_rc=False)
        self.created.remove(name)
        # Every negotiating client must have asked the control endpoint for peer readiness.
        peers = ((counters or {}).get("control:fips") or {}).get("show_peers", 0)
        check(f"{name}: each --npub client queried FIPS peer readiness", peers >= self.npub_clients,
              f"show_peers={peers}, npub clients={self.npub_clients}")

    def fresh_relay(self, cell, first=False):
        """Each cell gets an empty relay. Retained history would replay an earlier
        negotiation's session_accept to the client (see client-validation.md).
        Raises Incomplete (after recording the failure) if setup fails."""
        if self.fixture:
            self.retire_fixture()
        self.npub_clients = 0
        for f in self.w.glob("sock/*/*"):
            if f.name in ("signer.sock", "control.sock", "bunker.uri"):
                f.unlink()
        w, ids = self.w, self.ids
        args = ["--network", self.net, "--ip", FIXTURE_IP, *SANDBOX, "--memory", "256m",
                "-v", f"{w}/tests:/fixture/tests:ro", "-v", f"{w}/bin/nip5f-crypto:/fixture/nip5f-crypto:ro",
                "-v", f"{w}/bin/nip46-bunker:/fixture/nip46-bunker:ro",
                "-v", f"{w}/client-fixture.py:/fixture/client-fixture.py:ro",
                "-v", f"{w}/keys:/fixture/keys:ro", "-v", f"{w}/sock:/fixture/sock:rw", "--entrypoint", "python3"]
        cmd = ["/fixture/client-fixture.py", "--tests", "/fixture/tests", "--crypto", "/fixture/nip5f-crypto",
               "--relay", f"{FIXTURE_IP}:7777", "--sockets", "/fixture/sock", "--control", "fips",
               "--local-npub", ids["client"]["npub"], "--peer-npub", ids["host"]["npub"],
               "--signer=host=/fixture/keys/host.key", "--bunker-bin", "/fixture/nip46-bunker",
               *[f"--bunker={r}=/fixture/keys/{r}.key" for r in ("host", "client", "stranger")]]
        self.fixture, logs = self.start(f"fixture-{cell}", args, self.a.video_image, cmd)
        if not check(f"{cell}: fresh private relay, bunkers and control listening",
                     bool(logs.wait(r"^fixture ready$", 30))):
            raise Incomplete
        # Host role + allowlist via Metadesk commands; bind-role verifies the live signer owner.
        hpk, cpk, cnpub = ids["host"]["pubkey"], ids["client"]["pubkey"], ids["client"]["npub"]
        steps = [("account", "add", "nip5f", hpk, "container host"),
                 ("account", "bind-role", "host", "nip5f", hpk)] if first else []
        for args in [*steps, ("access", "--relay", self.relay, "add", cnpub, "video")]:
            r = self.account(*args)
            if not check(f"{cell}: host {' '.join(args[:2])}", r.returncode == 0, r.stderr.strip()[-120:]):
                raise Incomplete
        listed = self.account("access", "--relay", self.relay, "list")
        if not check(f"{cell}: allowlist is exactly client=video", listed.stdout.strip() == f"{cpk}\tvideo",
                     listed.stdout.strip().replace(cpk, "<client>")):
            raise Incomplete

    def start(self, name, args, image, command):
        name = f"{self.a.prefix}-{name}"
        podman("run", "-d", "--name", name, *args, image, *command)
        self.created.append(name)
        self.logs[name] = Logs(name)
        return name, self.logs[name]

    def stop(self, name, grace=10):
        t0 = time.monotonic()
        podman("stop", "-t", str(grace), name)
        elapsed = time.monotonic() - t0
        code = int(podman("inspect", "-f", "{{.State.ExitCode}}", name).stdout)
        self.logs[name].wait(r"$^", 5)  # drain: returns once `logs -f` reaches EOF
        return code, elapsed

    def bunker(self, role):
        # Throwaway bunker URI (includes its connect secret) in argv: --bunker has no other input.
        return ["--bunker", (self.w / "sock" / role / "bunker.uri").read_text().strip()]

    def account(self, *args):
        """Host account/access CLI with the NIP-5F host signer (signing only)."""
        return podman("run", "--rm", "--network", self.net, *SANDBOX,
                      "-v", f"{self.w}/home-host:/home/metadesk:rw", "-v", f"{self.w}/sock/host:/run/signer:rw",
                      "-e", "NOSTR_SIGNER_SOCK=/run/signer/signer.sock",
                      "-e", "DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent",
                      "--entrypoint", "/opt/metadesk/bin/metadesk-host", self.a.decode_image, *args,
                      check_rc=False, timeout=60)

    def host(self, cell):
        run_dir = self.w / f"run-host-{cell}"
        run_dir.mkdir(mode=0o700)
        ula = self.ids["host"]["address"]
        # Content generator for the private output only, so capture has damage.
        script = ("( while :; do for c in 204060 603020 206040 402060; do "
                  "swaymsg -q output HEADLESS-1 bg \"#$c\" solid_color || exit 0; sleep 0.1; done; done ) & "
                  # No stdbuf: the host line-buffers its own stdout (md-rk4).
                  f"exec /opt/metadesk/bin/metadesk-host --bind {ula} --port {PORT} --relay {self.relay} "
                  "--no-input --capture-unattended \"$@\"")
        r = self.a.render_node
        args = ["--network", self.net, "--ip6", ula, *SANDBOX, "--memory", "768m",
                "--tmpfs", "/tmp:rw,nosuid,nodev,mode=1777", "-v", f"{run_dir}:/run/metadesk:rw",
                "-v", f"{self.w}/home-host:/home/metadesk:rw", "-e", "MD_VIDEO_CONSENT=allow-isolated-output",
                "--device", r, "-e", "WLR_RENDERER=gles2", "-e", "WLR_RENDERER_ALLOW_SOFTWARE=1",
                "-e", f"WLR_RENDER_DRM_DEVICE={r}", "-e", "LIBGL_ALWAYS_SOFTWARE=1"]
        name, logs = self.start(f"host-{cell}", args, self.a.video_image,
                                ["sh", "-c", script, "host", *self.bunker("host")])
        ready = logs.wait(r"nostr bridge ready, waiting for session requests", 90)
        # The line must arrive while the host waits, not when it exits (md-rk4).
        running = podman("inspect", "-f", "{{.State.Running}}", name, check_rc=False).stdout.strip()
        check(f"{cell}: host negotiation ready (logged while running, no stdbuf)",
              bool(ready) and running == "true", f"{name} running={running}")
        return name, logs

    def client(self, cell, role, image, extra, client_args, negotiates=True):
        """negotiates=False: the client is expected to stop before any
        signer, relay or FIPS control use (no show_peers query expected)."""
        args = ["--network", self.net, *SANDBOX, "--memory", "768m",
                "--tmpfs", "/tmp:rw,nosuid,nodev,mode=1777",
                "--tmpfs", "/run/user/1000:rw,nosuid,nodev,mode=0700,U", *extra,
                "--entrypoint", "/opt/metadesk/bin/metadesk-client"]  # no stdbuf (md-rk4)
        if role:
            args += ["-v", f"{self.w}/sock/fips:/run/fips:rw"]
            client_args = [*client_args, *self.bunker(role)]
            self.npub_clients += negotiates
        return self.start(f"client-{cell}", args, image, client_args)

    def npub_args(self, timeout=15000):
        return ["--npub", self.ids["host"]["npub"], "--fips-control", "/run/fips/control.sock",
                "--relay", self.relay, "--port", PORT, "--timeout", str(timeout)]

    def stream(self, cell, logs, display_expected):
        """Session accepted, stream bound, and >= N decoded frames observed."""
        frames = self.a.frames
        check(f"{cell}: session accepted over private relay",
              bool(logs.wait(r"client: session accepted", 60)))
        check(f"{cell}: stream bound", bool(logs.wait(r"client: receiving stream", 30)))
        stats = logs.wait(r"decoded=(\d+) displayed=(\d+) decode_avg=([\d.]+)ms rtt=(\d+)ms rx=([\d.]+)MB",
                          120, lambda m: int(m[1]) >= frames)
        detail = stats[0] if stats else "no stats line reached the frame threshold"
        check(f"{cell}: decoded >= {frames} frames", bool(stats), detail)
        if stats:
            shown = int(stats[2])
            check(f"{cell}: displayed {'> 0' if display_expected else '== 0'}",
                  shown > 0 if display_expected else shown == 0, f"displayed={shown}")
        return stats

    def host_media(self, cell, logs):
        cap = logs.wait(r"host: capture started \((\d+)x(\d+)\)", 30)
        enc = logs.wait(r"host: encoder ready \((\w+), (\d+)x(\d+)\)", 30)
        check(f"{cell}: host captured its isolated output", bool(cap), cap[0] if cap else "")
        check(f"{cell}: host encoder", bool(enc), enc[0] if enc else "")
        check(f"{cell}: host bound client #1", bool(logs.wait(r"host: streaming to client #1", 30)))

    def finish_client(self, cell, name, display_expected):
        code, elapsed = self.stop(name)
        done = self.logs[name].wait(r"client: done\. decoded (\d+) frames, displayed (\d+)", 5)
        check(f"{cell}: client SIGTERM exit 0, <5 s", code == 0 and elapsed < 5 and bool(done),
              f"exit={code} {elapsed:.2f}s; {done[0] if done else 'no summary'}")

    def finish_host(self, cell, name):
        code, elapsed = self.stop(name)
        sent = self.logs[name].wait(r"host: client #1 disconnected \(sent (\d+) frames\)", 5)
        check(f"{cell}: host recorded client #1 disconnect", bool(sent), sent[0] if sent else "")
        results.append({"check": f"{cell}: host stop", "result": "INFO",
                        "detail": f"exit={code} {elapsed:.2f}s"})


def sandbox_receipt(cell, name):
    caps = podman("exec", name, "sh", "-c", "grep CapEff /proc/1/status; ls /dev/dri 2>/dev/null | wc -l; "
                  "printf 'W=%s D=%s\\n' \"${WAYLAND_DISPLAY:-}\" \"${DISPLAY:-}\"; "
                  "ls -A /run/user/1000 | tr '\\n' ' '").stdout.split("\n")
    ro = podman("inspect", "-f", "{{.HostConfig.ReadonlyRootfs}} {{len .HostConfig.Devices}}", name).stdout.strip()
    return caps, ro


def identity(w, role):
    key = w / "keys" / f"{role}.key"
    subprocess.run([sys.executable, str(w / "mesh-peer.py"), "--generate-key", str(key)], check=True)
    out = subprocess.run([sys.executable, str(w / "mesh-peer.py"), "--identity", str(key)],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work", type=Path, required=True,
                   help="new task directory holding mesh-peer.py, client-fixture.py, tests/, bin/nip5f-crypto")
    p.add_argument("--prefix", default="cont-ah911")
    p.add_argument("--video-image", required=True)
    p.add_argument("--decode-image", required=True)
    p.add_argument("--gui-image", required=True)
    p.add_argument("--render-node", default="/dev/dri/renderD128")
    p.add_argument("--frames", type=int, default=60)
    a = p.parse_args()
    w = a.work.resolve()
    os.umask(0o077)
    for d in ["keys", "sock/host", "sock/client", "sock/stranger", "sock/fips", "run-display",
              *[f"home-{r}/.config" for r in ("host", "legacy", "display")]]:
        (w / d).mkdir(parents=True, exist_ok=True)
    ids = {role: identity(w, role) for role in ("host", "client", "stranger")}
    net6 = ipaddress.IPv6Network(ids["host"]["address"] + "/64", strict=False)
    run = Run(a, w, ids)
    net_created = False
    try:
        podman("network", "create", "--ipv6", "--subnet", SUBNET4, "--subnet", str(net6), run.net)
        net_created = True

        # ── Cell 1: decode-only (headless) ──────────────────────────────────
        run.fresh_relay("decode", first=True)
        host, hlogs = run.host("decode")
        stranger, slogs = run.client("stranger", "stranger", a.decode_image, [],
                                     [*run.npub_args(5000), "--no-display"])
        rejected = hlogs.wait(r"REJECTED session request from non-allowlisted pubkey", 40)
        timed_out = slogs.wait(r"timed out waiting for session accept", 40)
        code = int(podman("wait", stranger, timeout=60).stdout.strip() or -1)
        check("decode: non-allowlisted client refused before any stream", bool(rejected and timed_out) and code != 0
              and not hlogs.has(r"client #\d+ connected"), f"host rejected, client exit={code}")
        dec, dlogs = run.client("decode", "client", a.decode_image, [], [*run.npub_args(), "--no-display"])
        run.host_media("decode", hlogs)
        run.stream("decode", dlogs, display_expected=False)
        size = dlogs.wait(r"client: decoded frame size (\d+)x(\d+)", 5)
        check("decode: client logged decoded frame size", bool(size), size[0] if size else "")
        try:
            (caps, ro) = sandbox_receipt("decode", dec)
        except RuntimeError as e:
            caps, ro = [str(e), "", "", ""], ""
        check("decode: no display server, devices or capabilities",
              caps[0].split()[-1] == "0000000000000000" and caps[1] == "0" and caps[2] == "W= D=" and ro == "true 0",
              f"{caps[0].strip()}; dri={caps[1]}; {caps[2]}; runtime=[{caps[3].strip()}]; ro/devices={ro}")
        libs = podman("run", "--rm", "--entrypoint", "sh", a.decode_image, "-c",
                      "ldconfig -p | grep -oE '(libEGL|libGLESv2|libwayland-[a-z]+)\\.so\\.[0-9]+' | sort -u | tr '\\n' ' '"
                      ).stdout.strip()
        check("decode: image has no EGL/GLES runtime", "libEGL" not in libs and "libGLESv2" not in libs,
              f"display-related libraries present: [{libs}]")
        run.finish_client("decode", dec, display_expected=False)
        # Direct-IP mode needs no signer; it still needs a private (empty) config home.
        legacy, llogs = run.client("legacy", None, a.decode_image, ["-v", f"{w}/home-legacy:/home/metadesk:rw"],
                                   [ids["host"]["address"], "--port", PORT, "--no-display", "--timeout", "5000"])
        unproven = hlogs.wait(r"host: rejected unproven client #2", 30)
        lcode, _ = run.stop(legacy)
        # The drained final summary (after decoder flush) must exist and report zero frames.
        summary = llogs.wait(r"client: done\. decoded (\d+) frames", 5)
        check("decode: legacy direct-IP client fails closed (no session proof)",
              bool(unproven) and bool(summary) and summary[1] == "0" and not hlogs.has(r"streaming to client #2"),
              f"host: {unproven[0] if unproven else 'no rejection'}; "
              f"legacy: {summary[0] if summary else 'no summary'}, exit={lcode}")
        run.finish_host("decode", host)

        # ── Cell 2: GUI, display authorization ──────────────────────────────
        display, xlogs = run.start("display", ["--network", "none", *SANDBOX, "--memory", "384m",
                                               "--tmpfs", "/tmp:rw,nosuid,nodev,mode=1777",
                                               "-v", f"{w}/run-display:/run/metadesk:rw",
                                               "-v", f"{w}/home-display:/home/metadesk:rw",
                                               "-e", "MD_VIDEO_CONSENT=deny"],
                                   a.video_image, ["sleep", "infinity"])
        if not check("gui: private Sway display ready (pixman, no devices)",
                     bool(xlogs.wait(r"AvailableSourceTypes", 60))):
            raise Incomplete
        sock = (w / "run-display/session.env").read_text().split("WAYLAND_DISPLAY=")[1].split()[0]
        run.fresh_relay("gui")
        host, hlogs = run.host("gui")
        gui, glogs = run.client("gui", "client", a.gui_image,
                                ["-v", f"{w}/run-display/{sock}:/run/user/1000/{sock}:rw",
                                 "-e", f"WAYLAND_DISPLAY={sock}", "-e", "SDL_VIDEODRIVER=wayland",
                                 "-e", "LIBGL_ALWAYS_SOFTWARE=1"], run.npub_args())
        run.host_media("gui", hlogs)
        run.stream("gui", glogs, display_expected=True)
        check("gui: client logged wayland video driver, SDL renderer and frame size",
              glogs.has(r"client: receiving stream") and glogs.has(r"^render: SDL video driver 'wayland'$")
              and glogs.has(r"^render: SDL renderer '\w+'") and glogs.has(r"client: decoded frame size \d+x\d+")
              and not glogs.has(r"render: SDL_(Init|CreateWindow|CreateRenderer) failed|no visible output"),
              "; ".join(line for line in glogs.lines
                        if line.startswith("render:") or "frame size" in line)[:240])
        tree = json.loads(podman("exec", display, "sh", "-c",
                                 ". /run/metadesk/session.env; swaymsg -t get_tree").stdout)
        def views(node):
            yield node
            for child in node.get("nodes", []) + node.get("floating_nodes", []):
                yield from views(child)
        # pid is 0 here: the client lives in another container's PID namespace.
        win = [v for v in views(tree) if v.get("type") == "con" and v.get("name") == "metadesk"]
        check("gui: client window mapped on the private display", bool(win) and win[0].get("visible"),
              json.dumps({k: win[0].get(k) for k in ("app_id", "name", "visible", "shell")} | {
                  "rect": [win[0]["rect"]["width"], win[0]["rect"]["height"]]}) if win else "no metadesk view")
        run.finish_client("gui", gui, display_expected=True)
        run.finish_host("gui", host)

        # No display granted: GUI mode refuses before any signer, relay or
        # session work (md-33e); it never falls back to offscreen or decode-only.
        # Same image, sandbox and argv as the granted cell; only the grant differs.
        for cell, extra, refusal in (
                ("gui-nodisplay", [], r"render: SDL video driver '(\w+)' has no visible output|"
                                      r"render: SDL_Init failed: .*"),
                ("gui-strict", ["-e", "SDL_VIDEODRIVER=wayland"], r"render: SDL_Init failed: .*")):
            name, logs = run.client(cell, "client", a.gui_image, extra, run.npub_args(), negotiates=False)
            code = int(podman("wait", name, timeout=60).stdout.strip() or -1)
            logs.wait(r"$^", 5)  # drain: returns once `logs -f` reaches EOF
            why = logs.wait(refusal, 0)
            check(f"{cell}: no display granted -> GUI refused, exit 1",
                  code == 1 and bool(why) and logs.has(r"ERROR: no usable display for the GUI client"),
                  f"exit={code}; {why[0] if why else 'no refusal reason'}")
            check(f"{cell}: refused before signer, relay or session work",
                  not logs.has(r"NIP-46 bunker|signer ready|session request|connecting|"
                               r"receiving stream|decode-only mode|decoded="),
                  "; ".join(logs.lines[-3:])[:200])
    except Incomplete:
        check("qualification ran every cell", False, "stopped after a failed prerequisite")
    finally:
        if run.fixture:
            run.retire_fixture()
        for name in reversed(run.created):
            podman("stop", "-t", "5", name, check_rc=False)
            podman("rm", "-f", name, check_rc=False)
        for logs in run.logs.values():
            logs.close()
        if net_created:  # never remove a pre-existing network of the same name
            podman("network", "rm", run.net, check_rc=False)
        evidence = {"identities": {r: {"npub": v["npub"], "address": v["address"]} for r, v in ids.items()},
                    "fixture_counters": run.counters, "results": results,
                    "logs": {n: l.lines for n, l in run.logs.items()}}
        (w / "evidence.json").write_text(json.dumps(evidence, indent=1))
        print("fixture counters:", json.dumps(run.counters))
    failed = [r for r in results if r["result"] == "FAIL"]
    print(f"{len(results) - len(failed)} non-failing / {len(failed)} failing checks")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
