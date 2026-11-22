#!/usr/bin/env python3
"""md-ah9.6 FIPS binary-session reference (rootless Podman); see mesh-video-validation.md.

Two FIPS nodes on a private IPv4-only underlay. Each node is a capability-free
keep-ID namespace holder plus a fipsd sidecar, the only NET_ADMIN/TUN owner.
Its control socket reaches metadesk only through a dedicated per-node volume,
mounted read-only with the supplementary group fips. The host node runs the
Tier B Sway/xdpw video host in --npub mode, and the client node runs the
decode-only client (C1). Each FIPS identity is its role's Nostr key: the host
refuses a daemon whose show_status npub is not its signer.

Nostr negotiation reuses validate-client.py: one private relay that retains
history, NIP-46 bunkers with throwaway keys, and the host role's v1 video
grant, all reached over the underlay. The video stream itself travels fips0
between the npub-derived addresses; the underlay has no IPv6 at all.

Cells: mesh session; privilege receipts; mesh restart on each node between
sessions; repeat negotiations' history checks (md-6ns/md-817x); identity
negatives (a stranger client, a host whose daemon holds another identity);
fail closed when the peer's or the local daemon is down; mid-stream mesh
restart; host-container restart with client reconnect.
Never mounts a host display, session bus or runtime directory.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("validate_client", HERE / "validate-client.py")
vc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vc)
check, podman, results, Incomplete = vc.check, vc.podman, vc.results, vc.Incomplete

SUBNET4 = "10.89.91.0/24"
NODE_IP = {"host": "10.89.91.2", "client": "10.89.91.3"}
vc.FIXTURE_IP = "10.89.91.10"
FIPS_GID = "777"  # the mesh image's fips group (Dockerfile.mesh)
BASE = "docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8"
# Keep-ID mapping comes from the holder (--userns container:HOLDER), not --userns keep-id.
NODE_SANDBOX = ["--user", "1000:1000", "--read-only", "--cap-drop", "all",
                "--security-opt", "no-new-privileges", "--init", "--pids-limit", "256"]
# Mesh-path gate: a raw TCP listener in the host namespace (harness only).
PROBE_PORT = 7499
PROBE_SERVER = ("import socket, sys\n"
                "s = socket.socket(socket.AF_INET6)\n"
                "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n"
                f"s.bind((sys.argv[1], {PROBE_PORT})); s.listen(8); print('probe listening', flush=True)\n"
                "while True:\n"
                "    c, _ = s.accept(); c.sendall(b'fips-ok'); c.close()\n")
PROBE_CLIENT = ("import socket, sys, time\n"
                "t0, n = time.monotonic(), 0\n"
                "while time.monotonic() - t0 < 90:\n"
                "    n += 1\n"
                "    try:\n"
                f"        c = socket.create_connection((sys.argv[1], {PROBE_PORT}), timeout=5)\n"
                "        if c.recv(16) == b'fips-ok':\n"
                "            print(f'ok attempt={n} after={time.monotonic() - t0:.1f}s'); sys.exit(0)\n"
                "    except OSError as e:\n"
                "        err = e\n"
                "    time.sleep(0.5)  # bounded probe retry of the mesh path, not completion\n"
                "print(f'failed attempts={n}: {err!r}'); sys.exit(1)\n")
STATS = r"decoded=(\d+) displayed=(\d+) decode_avg=([\d.]+)ms rtt=(\d+)ms rx=([\d.]+)MB"


class Logs(vc.Logs):
    def wait_from(self, start, pattern, timeout, accept=lambda m: True):
        """Like wait(), but only lines at index >= start."""
        import re
        rx, deadline = re.compile(pattern), time.monotonic() + timeout
        with self.cond:
            i = start
            while True:
                for i in range(i, len(self.lines)):
                    m = rx.search(self.lines[i])
                    if m and accept(m):
                        return m
                i = max(start, len(self.lines))
                left = deadline - time.monotonic()
                if self.done or left <= 0:
                    return None
                self.cond.wait(left)

    def wait_nth(self, pattern, n, timeout):
        """The n-th (1-based) line matching pattern. A follower started after
        a container restart re-reads earlier runs' lines first."""
        seen = [0]

        def nth(m):
            seen[0] += 1
            return seen[0] >= n
        return self.wait(pattern, timeout, nth)


vc.Logs = Logs


def remove_resource(kind, name):
    """Remove only a resource this run created; absence must be confirmed.

    Keep attempting the remaining cleanup even if one command fails.
    """
    args = ["rm", "-f", "-t", "5", name] if kind == "container" else [kind, "rm", "-f", name]
    try:
        removed = podman(*args, check_rc=False)
        absent = podman(kind, "exists", name, check_rc=False)
        ok = absent.returncode == 1
        detail = f"rm exit={removed.returncode}, exists exit={absent.returncode}"
    except (OSError, subprocess.SubprocessError) as error:
        ok, detail = False, str(error)
    return check(f"cleanup: {kind} {name} absent", ok, detail)


def ns(holder):
    return ["--network", f"container:{holder}", "--userns", f"container:{holder}"]


def sh(name, script, check_rc=False):
    return podman("exec", name, "sh", "-c", script, check_rc=check_rc).stdout.strip()


class MeshRun(vc.Run):
    def __init__(self, a, w, ids):
        super().__init__(a, w, ids)
        self.net = a.prefix + "-underlay"
        self.relay = f"ws://{vc.FIXTURE_IP}:7777"
        self.control_fixture = False
        # This run outlives the tests/ relay's 256 stored events with NIP-46 stored too.
        self.fixture_extra = ["--no-store-ephemeral"]
        self.volumes = []
        self.holder = {r: f"{a.prefix}-{r}-netns" for r in NODE_IP}
        self.mesh = {r: f"{a.prefix}-{r}-mesh" for r in NODE_IP}
        self.vol = {r: f"{a.prefix}-{r}-fips" for r in NODE_IP}

    # ── FIPS nodes ─────────────────────────────────────────────────────────
    def fips_config(self, role, identity_role=None):
        """Daemon YAML: explicit identity (the role's Nostr key), control socket
        on the shared volume, UDP to the other node, no rendezvous/DNS."""
        other = "client" if role == "host" else "host"
        key_role = identity_role or role
        nsec = (self.w / "keys" / f"{key_role}.key").read_text().strip()
        path = self.w / "secrets" / f"{role}-{key_role}.yaml"
        path.unlink(missing_ok=True)
        path.write_text(
            "node:\n"
            f"  identity: {{nsec: {nsec}, persistent: false}}\n"
            "  control: {enabled: true, socket_path: /run/fips/control.sock}\n"
            "  rendezvous: {nostr: {enabled: false}}\n"
            "tun: {enabled: true, name: fips0, mtu: 1280}\n"
            "dns: {enabled: false}\n"
            "transports:\n"
            "  udp: {bind_addr: \"0.0.0.0:2121\"}\n"
            "peers:\n"
            f"  - npub: {self.ids[other]['npub']}\n"
            "    addresses:\n"
            f"      - {{transport: udp, addr: \"{NODE_IP[other]}:2121\"}}\n")
        # The daemon is a mapped non-1000 UID without DAC override; the 0700
        # parent stays private and only this file is mounted, into its daemon.
        path.chmod(0o444)
        return path

    def start_node(self, role):
        holder, logs = self.start(f"{role}-netns", ["--network", self.net, "--ip", NODE_IP[role],
                                                     "--userns", "keep-id", *NODE_SANDBOX, "--stop-timeout", "5"],
                                  BASE, ["sleep", "infinity"])
        podman("volume", "create", self.vol[role])
        self.volumes.append(self.vol[role])
        self.start_mesh(role)
        if role == "host":
            name, logs = self.start("host-probe", [*ns(self.holder["host"]), *NODE_SANDBOX, "--memory", "64m"],
                                    self.a.mesh_image, ["python3", "-c", PROBE_SERVER, self.ids["host"]["address"]])
            # Binding the npub address needs fips0 up; on-failure is not used, so check.
            check("host node: mesh-path probe listener", bool(logs.wait(r"probe listening", 30)))

    def mesh_ready(self, cell):
        """Gate: metadesk's readiness is link-level and it dials once, but FIPS
        links flap after any daemon (re)start (crossing dials, Restart
        reconcile), dropping the npub route. Wait until raw TCP over fips0 works."""
        out = podman("exec", self.mesh["client"], "python3", "-c", PROBE_CLIENT, self.ids["host"]["address"],
                     check_rc=False, timeout=120)
        ok = out.returncode == 0
        check(f"{cell}: mesh path ready (raw TCP client node -> host npub address over fips0)", ok,
              (out.stdout or out.stderr).strip())
        if not ok:
            raise Incomplete

    def start_mesh(self, role, identity_role=None):
        cfg = self.fips_config(role, identity_role)
        name, logs = self.start(f"{role}-mesh", [
            *ns(self.holder[role]), "--user", f"0:{FIPS_GID}", "--read-only", "--cap-drop", "all",
            "--cap-add", "NET_ADMIN", "--device", "/dev/net/tun", "--security-opt", "no-new-privileges",
            "--init", "--pids-limit", "256", "--memory", "256m", "--stop-timeout", "5",
            "-v", f"{self.vol[role]}:/run/fips:U", "-v", f"{cfg}:/etc/fips/fips.yaml:ro"],
            self.a.mesh_image, [])
        check(f"{role} node: fipsd control socket listening",
              bool(logs.wait(r"control socket listening at /run/fips/control\.sock", 30)))
        return name, logs

    def retire_logs(self, name):
        """Keep a finished follower's lines for evidence under a unique key."""
        old = self.logs.pop(name)
        old.close()
        k = sum(1 for n in self.logs if n.startswith(name + "#"))
        self.logs[f"{name}#{k}"] = old
        return old

    def remove(self, name):
        if not remove_resource("container", name):
            raise Incomplete  # retain ownership so finally tries cleanup again
        if name in self.created:
            self.created.remove(name)
        self.retire_logs(name)

    def retire_fixture(self):
        name, self.fixture = self.fixture, None
        podman("stop", "-t", "5", name, check_rc=False)
        logs = self.logs[name]
        m = logs.wait(r"^fixture counters (.*)$", 10)
        self.counters[name] = json.loads(m[1]) if m else None
        check("fixture: final counters and logs drained", bool(m) and logs.drain(30))
        self.remove(name)

    def restart(self, name, grace, pattern, timeout):
        """podman restart; returns (start time, new follower, n-th pattern match)."""
        n = len(self.logs[name].findall(pattern)) + 1
        t0 = time.monotonic()
        podman("restart", "-t", str(grace), name, timeout=60)
        self.retire_logs(name)
        logs = self.logs[name] = Logs(name)
        return t0, logs, logs.wait_nth(pattern, n, timeout)

    def restart_mesh(self, role, gate=True):
        t0, logs, up = self.restart(self.mesh[role], 5, r"control socket listening", 30)
        check(f"{role} node: fipsd restarted, control socket back",
              bool(up), f"{time.monotonic() - t0:.1f}s after podman restart")
        if gate:
            self.mesh_ready(f"restart-{role}-mesh")
            results.append({"check": f"restart-{role}-mesh: daemon restart to mesh path ready", "result": "INFO",
                            "detail": f"{time.monotonic() - t0:.1f}s"})
        return t0

    def holder_state(self, role):
        h = self.holder[role]
        info = podman("inspect", "-f", "{{.Id}} {{.RestartCount}} {{.State.StartedAt}}", h).stdout.strip()
        return info, sh(h, "readlink /proc/1/ns/net")

    def links_up(self, roles=tuple(NODE_IP)):
        """Fresh daemons only: each waits for its first link."""
        for role in roles:
            m = self.logs[self.mesh[role]].wait(r"link established with .*", 60)
            check(f"{role} node: FIPS link established with the other node", bool(m), m[0] if m else "")

    # ── metadesk roles in the node namespaces ──────────────────────────────
    def host(self, cell, expect_ready=True):
        cnpub, ula = self.ids["client"]["npub"], self.ids["host"]["address"]
        script = ("( while :; do for c in 204060 603020 206040 402060; do "
                  "swaymsg -q output HEADLESS-1 bg \"#$c\" solid_color || exit 0; sleep 0.1; done; done ) & "
                  f"exec /opt/metadesk/bin/metadesk-host --npub {cnpub} --fips-control /run/fips/control.sock "
                  f"--bind {ula} --port {vc.PORT} --relay {self.relay} --no-input --capture-unattended \"$@\"")
        r = self.a.render_node
        # Private tmpfs runtime: a restarted container gets a clean Sway/PipeWire dir.
        args = [*ns(self.holder["host"]), *NODE_SANDBOX, "--group-add", FIPS_GID, "--memory", "768m",
                "--tmpfs", "/tmp:rw,nosuid,nodev,mode=1777",
                "--tmpfs", "/run/metadesk:rw,nosuid,nodev,mode=0700,U",
                "-v", f"{self.w}/home-host:/home/metadesk:rw", "-v", f"{self.vol['host']}:/run/fips:ro",
                "-e", "MD_VIDEO_CONSENT=allow-isolated-output",
                "--device", r, "-e", "WLR_RENDERER=gles2", "-e", "WLR_RENDERER_ALLOW_SOFTWARE=1",
                "-e", f"WLR_RENDER_DRM_DEVICE={r}", "-e", "LIBGL_ALWAYS_SOFTWARE=1"]
        name, logs = self.start(f"host-{cell}", args, self.a.video_image,
                                ["sh", "-c", script, "host", *self.bunker("host")])
        if expect_ready:
            self.host_ready(cell, logs)
        return name, logs

    def host_ready(self, cell, logs, start=0):
        hnpub = self.ids["host"]["npub"]
        fips = logs.wait_from(start, r"host: FIPS daemon ready.*npub=(\S+)\.\.\.", 90)
        check(f"{cell}: host checked its FIPS daemon and identity",
              bool(fips) and hnpub.startswith(fips[1]), fips[0] if fips else "no FIPS daemon ready line")
        ready = logs.wait_from(start, r"nostr bridge ready, waiting for session requests", 90)
        check(f"{cell}: host negotiation ready", bool(ready))
        return ready

    def client(self, cell, role, image, extra, client_args, negotiates=True):
        args = [*ns(self.holder["client"]), *NODE_SANDBOX, "--group-add", FIPS_GID, "--memory", "768m",
                "--tmpfs", "/tmp:rw,nosuid,nodev,mode=1777",
                "--tmpfs", "/run/user/1000:rw,nosuid,nodev,mode=0700,U",
                "-v", f"{self.vol['client']}:/run/fips:ro", *extra,
                "--entrypoint", "/opt/metadesk/bin/metadesk-client"]
        if role:
            client_args = [*client_args, *self.bunker(role)]
        return self.start(f"client-{cell}", args, image, client_args)

    def mesh_path(self, cell, hlogs, clogs):
        """The stream crossed fips0: the client resolved the host's npub, the
        host saw the client's npub-derived address, and that is a fips0 route."""
        haddr, caddr = self.ids["host"]["address"], self.ids["client"]["address"]
        ready = clogs.wait(r"client: FIPS peer ready: (.*)", 5)
        check(f"{cell}: client waited for real FIPS peer readiness", bool(ready), ready[1] if ready else "")
        resolved = clogs.wait(r"FIPS resolved \S+ → (\S+)", 5)
        accepted = hlogs.wait(r"accepted connection from (\S+)", 5)
        check(f"{cell}: client dialled the host's npub address; host saw the client's npub address",
              bool(resolved and accepted) and resolved[1] == haddr and caddr in accepted[1],
              f"dialled {resolved[1] if resolved else '?'}; host accepted from {accepted[1] if accepted else '?'}")
        peer = hlogs.wait(r"host: FIPS peer ready: (.*)", 5)
        check(f"{cell}: host waited for real FIPS peer readiness", bool(peer), peer[1] if peer else "")


def receipts(run):
    """Least-privilege matrix (plan §3.5): only mesh roles have NET_ADMIN/TUN;
    metadesk sees the control directory read-only through group fips."""
    probe = ("grep CapEff /proc/1/status | cut -f2; [ -c /dev/net/tun ] && echo tun || echo no-tun; "
             "ls /dev/dri 2>/dev/null | tr '\\n' ' '; echo; "
             "awk 'BEGIN{m=\"none\"} $5==\"/run/fips\"{split($6,o,\",\"); m=o[1]} END{print m}' /proc/self/mountinfo; "
             "id -u; id -G")
    rows = {}
    for name in [n for n in run.created if not n.endswith("-fixture")]:
        out = sh(name, probe).split("\n") + [""] * 6
        ro = podman("inspect", "-f", "{{.HostConfig.ReadonlyRootfs}}", name).stdout.strip()
        rows[name] = {"caps": out[0], "tun": out[1], "dri": out[2].strip(), "fips": out[3], "uid": out[4],
                      "groups": out[5], "readonly": ro}
    p = run.a.prefix
    for name, r in rows.items():
        role = name[len(p) + 1:]
        if role.endswith("-netns") or role.endswith("-probe"):
            ok = r["caps"] == "0000000000000000" and r["tun"] == "no-tun" and r["fips"] == "none" and not r["dri"]
        elif role.endswith("-mesh"):
            ok = r["caps"] == "0000000000001000" and r["tun"] == "tun" and r["fips"] == "rw" and not r["dri"]
        elif role.startswith("host-"):
            ok = (r["caps"] == "0000000000000000" and r["tun"] == "no-tun" and r["fips"] == "ro"
                  and r["dri"] == Path(run.a.render_node).name and r["uid"] == "1000"
                  and FIPS_GID in r["groups"].split())
        else:
            ok = (r["caps"] == "0000000000000000" and r["tun"] == "no-tun" and r["fips"] == "ro"
                  and not r["dri"] and r["uid"] == "1000" and FIPS_GID in r["groups"].split())
        check(f"privilege: {role}", ok and r["readonly"] == "true", json.dumps(r))
    nets = {n[len(p) + 1:]: sh(n, "readlink /proc/1/ns/net") for n in rows}
    for role in NODE_IP:
        mine = {n for n, v in nets.items() if v == nets[f"{role}-netns"]}
        want = {n for n in nets if n.startswith(role + "-")}
        check(f"privilege: {role} node roles share exactly their holder's network namespace",
              mine == want and nets["host-netns"] != nets["client-netns"], json.dumps(sorted(mine)))


def routes(run, cell):
    haddr, caddr = run.ids["host"]["address"], run.ids["client"]["address"]
    route = sh(run.mesh["client"], f"ip -6 route get {haddr}")
    addr = sh(run.mesh["host"], "ip -6 -o addr show scope global | awk '{print $2, $4}'")
    under = sh(run.mesh["client"], "ip -6 -o addr show dev eth0 scope global | wc -l")
    dev, _, cidr = addr.partition(" ")
    check(f"{cell}: client route to the host address is fips0; host address on fips0; no underlay IPv6",
          "dev fips0" in route and dev == "fips0" and cidr.split("/")[0] == haddr and under == "0",
          f"route: {route}; host: {addr}; underlay global IPv6 addrs: {under}")
    refused = podman("exec", run.fixture, "python3", "-c",
                     "import socket\ntry:\n socket.create_connection(('%s', %s), 3); print('open')\n"
                     "except OSError as e: print(type(e).__name__)" % (NODE_IP["host"], vc.PORT),
                     check_rc=False).stdout.strip()
    check(f"{cell}: host stream port not reachable on the underlay", refused == "ConnectionRefusedError",
          f"{NODE_IP['host']}:{vc.PORT} from the fixture: {refused}")
    for role in NODE_IP:
        for what in ("peers", "sessions"):
            out = sh(run.mesh[role], f"fipsctl show {what}")
            results.append({"check": f"{cell}: {role} fipsctl show {what}", "result": "INFO", "detail": out[-600:]})


def session(run, cell, a, receipt=None):
    """One negotiation and stream; returns the correlation record."""
    host, hlogs = run.host(cell)
    cli, clogs = run.client(cell, "client", a.decode_image, [], [*run.npub_args(), "--no-display"])
    run.host_media(cell, hlogs)
    stats = run.stream(cell, clogs, display_expected=False)
    run.mesh_path(cell, hlogs, clogs)
    corr = run.correlated(cell, hlogs, clogs)
    if receipt:
        receipt()
    run.finish_client(cell, cli, display_expected=False)
    run.finish_host(cell, host)
    metrics = hlogs.wait(r"host: FIPS metrics forwarding .*", 0)
    if metrics:
        results.append({"check": f"{cell}: host FIPS metrics at startup", "result": "INFO", "detail": metrics[0]})
    run.correlated_final(corr)
    for name in (cli, host):
        run.remove(name)
    return stats


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work", type=Path, required=True,
                   help="new task directory holding mesh-peer.py, client-fixture.py, tests/, bin/")
    p.add_argument("--prefix", default="md-l3-ah96")
    p.add_argument("--video-image", required=True)
    p.add_argument("--decode-image", required=True)
    p.add_argument("--mesh-image", required=True, help="Dockerfile.mesh fipsd image")
    p.add_argument("--render-node", default="/dev/dri/renderD128")
    p.add_argument("--frames", type=int, default=60)
    a = p.parse_args()
    w = a.work.resolve()
    os.umask(0o077)
    for d in ["keys", "secrets", "sock/host", "sock/client", "sock/stranger", "home-host/.config"]:
        (w / d).mkdir(parents=True, exist_ok=True)
    ids = {role: vc.identity(w, role) for role in ("host", "client", "stranger")}
    run = MeshRun(a, w, ids)
    net_created = False
    t_start = time.monotonic()
    try:
        # IPv4 only: the npub-derived IPv6 addresses exist only on fips0.
        podman("network", "create", "--subnet", SUBNET4, run.net)
        net_created = True
        run.start_fixture()
        for role in NODE_IP:
            run.start_node(role)
        run.links_up()
        run.mesh_ready("startup")

        # ── Cell 1: mesh session + privilege receipts ───────────────────────
        def first_receipts():
            routes(run, "session")
            receipts(run)
        session(run, "session", a, first_receipts)

        # ── Cell 2: restart each node's fipsd between sessions ──────────────
        for role in ("host", "client"):
            cell = f"restart-{role}-mesh"
            before = run.holder_state(role)
            t0 = run.restart_mesh(role)
            stats = session(run, cell, a)
            after = run.holder_state(role)
            check(f"{cell}: holder unchanged (same container, start, namespace)", before == after,
                  f"{before} -> {after}")
            results.append({"check": f"{cell}: restart to {a.frames} decoded frames", "result": "INFO",
                            "detail": f"{time.monotonic() - t0:.1f}s whole cell" if stats else "no stream"})

        # ── Cell 3: identity and readiness negatives ───────────────────────
        # Host daemon down: require fail-closed refusal; record separately
        # whether readiness refused or a stale FIPS session allowed negotiation.
        mark = len(run.logs[run.mesh["client"]].lines)
        t0 = time.monotonic()
        podman("stop", "-t", "5", run.mesh["host"])
        # The stopping daemon sends Shutdown (close=7); readiness can only fail
        # once the client daemon closes its link to the host.
        gone = run.logs[run.mesh["client"]].wait_from(mark, r"closing link with \S+ \((\w+), close=\d+\)", 150)
        check("peer-down: client daemon closed its link to the stopped host daemon", bool(gone),
              f"{gone[0] if gone else 'no link close within 150 s'} ({time.monotonic() - t0:.1f}s after stop)")
        cli, clogs = run.client("peer-down", "client", a.decode_image, [],
                                [*run.npub_args(8000), "--no-display"])
        code = int(podman("wait", cli, timeout=90).stdout.strip() or -1)
        if not check("peer-down: client logs drained", clogs.drain(30)):
            raise Incomplete
        refused = clogs.wait(r"ERROR: (?:client: )?(FIPS route not ready|FIPS peer not configured).*", 0)
        ready = clogs.wait(r"client: FIPS peer ready: .*", 0)
        failed = clogs.wait(r"ERROR: .*", 0)
        check("peer-down: client fails closed with the host's daemon down (nonzero exit, no stream)",
              code != 0 and bool(refused or (clogs.has(r"client: sending session request DM")
                                             and clogs.has(r"ERROR: timed out waiting for session accept")))
              and not clogs.has(r"client: connected|receiving stream"),
              f"exit={code}; {failed[0] if failed else clogs.lines[-1:]}")
        # Where it stopped: FIPS readiness (no session request) or only at negotiation.
        results.append({"check": "peer-down: client stop point", "result": "INFO",
                        "detail": f"refused at readiness: {refused[0]}" if refused else
                        f"readiness passed ({ready[0] if ready else '?'}); "
                        f"session request sent={clogs.has('sending session request')}"})
        run.remove(cli)
        # Local daemon down: the host refuses before relay or listener work.
        host, hlogs = run.host("local-down", expect_ready=False)
        code = int(podman("wait", host, timeout=90).stdout.strip() or -1)
        if not check("local-down: host logs drained", hlogs.drain(30)):
            raise Incomplete
        why = hlogs.wait(r"ERROR: FIPS daemon health check failed.*", 0)
        check("local-down: host refuses without its FIPS daemon", code == 1 and bool(why)
              and not hlogs.has(r"nostr bridge ready|listening on port"), f"exit={code}; {why[0] if why else ''}")
        run.remove(host)
        # A daemon holding another identity: the host refuses (show_status npub != signer).
        run.remove(run.mesh["host"])
        run.start_mesh("host", identity_role="stranger")
        host, hlogs = run.host("wrong-identity", expect_ready=False)
        code = int(podman("wait", host, timeout=90).stdout.strip() or -1)
        if not check("wrong-identity: host logs drained", hlogs.drain(30)):
            raise Incomplete
        why = hlogs.wait(r"ERROR: FIPS host identity check refused.*", 0)
        check("wrong-identity: host refuses a FIPS daemon that is not its signer identity",
              code == 1 and bool(why) and not hlogs.has(r"nostr bridge ready"), f"exit={code}; {why[0] if why else ''}")
        run.remove(host)
        run.remove(run.mesh["host"])
        run.start_mesh("host")
        run.links_up(("host",))
        run.mesh_ready("host daemon replaced")

        # ── Cell 4: a stranger on the client node; the host pins --npub ─────
        host, hlogs = run.host("stranger")
        cli, slogs = run.client("stranger", "stranger", a.decode_image, [], [*run.npub_args(5000), "--no-display"])
        rejected = hlogs.wait(r"REJECTED session request from unexpected FIPS client", 40)
        code = int(podman("wait", cli, timeout=60).stdout.strip() or -1)
        run.stop(host)
        if not check("stranger: host and client logs drained", hlogs.drain(30) and slogs.drain(30)):
            raise Incomplete
        check("stranger: host rejects a client that is not its --npub peer; no stream", bool(rejected) and code != 0
              and not hlogs.has(r"client #\d+ connected") and not slogs.has(r"receiving stream"),
              f"host rejected, stranger exit={code}")
        run.remove(cli)
        run.remove(host)

        # ── Cell 5: mid-stream host mesh restart, then host restart ─────────
        cell = "midstream"
        host, hlogs = run.host(cell)
        cli, clogs = run.client(cell, "client", a.decode_image, [], [*run.npub_args(), "--no-display"])
        run.host_media(cell, hlogs)
        if run.stream(cell, clogs, display_expected=False):
            mark = len(clogs.lines)
            last = clogs.findall(STATS)
            base = int(last[-1][1]) if last else 0
            t0 = run.restart_mesh("host", gate=False)
            # Either the TCP stream rides out the TUN replacement (decoding
            # resumes past the count at daemon restart), or the client reports
            # the loss and enters its reconnect loop.
            outcome = clogs.wait_from(mark, r"client: reconnecting in|" + STATS, 120,
                                      lambda m: m[0].startswith("client: reconnecting") or int(m[1]) > base + 30)
            check(f"{cell}: stream progresses or client enters reconnect after daemon restart", bool(outcome))
            survived = bool(outcome) and not outcome[0].startswith("client: reconnecting")
            results.append({"check": f"{cell}: stream across host fipsd restart", "result": "INFO",
                            "detail": (("survived: " if survived else "lost: ") + outcome[0]
                                       if outcome else "neither frames nor reconnect within 120 s")
                            + f" (decoded {base} at restart; {time.monotonic() - t0:.1f}s)"})
            # Host restart (what a restart policy does): a fresh process
            # negotiates once, and the client's reconnect loop renegotiates.
            mark = len(clogs.lines)
            accepted_before = len(hlogs.findall(r"accepted connection from (\S+)"))
            t0, hlogs, ready = run.restart(host, 10, r"nostr bridge ready", 120)
            check(f"{cell}: restarted host renegotiation ready", bool(ready), f"{time.monotonic() - t0:.1f}s")
            back = clogs.wait_from(mark, r"client: reconnected", 150)
            frames = None
            if back:  # decoded is cumulative: require a.frames more after the reconnect
                mark = len(clogs.lines)
                last = clogs.findall(STATS)
                base = int(last[-1][1]) if last else 0
                frames = clogs.wait_from(mark, STATS, 60, lambda m: int(m[1]) >= base + a.frames)
            check(f"{cell}: client reconnected to the restarted host over the mesh and streams",
                  bool(back and frames), f"{time.monotonic() - t0:.1f}s; {frames[0] if frames else 'no stream'}")
            accepted = hlogs.wait_nth(r"accepted connection from (\S+)", accepted_before + 1, 5)
            check(f"{cell}: restarted host accepted the client's npub address",
                  bool(accepted) and ids["client"]["address"] in accepted[1],
                  accepted[1] if accepted else "no fresh acceptance after host restart")
        else:
            check(f"{cell}: restart recovery exercised", False, "stream prerequisite failed; disruption not run")
        run.finish_client(cell, cli, display_expected=False)
        run.finish_host(cell, host)
    except Incomplete:
        check("qualification ran every cell", False, "stopped after a failed prerequisite")
    finally:
        if run.fixture:
            try:
                run.retire_fixture()
            except (Incomplete, RuntimeError, OSError, subprocess.SubprocessError, ValueError) as error:
                check("fixture: retirement completed", False, str(error))
        for name in reversed(run.created):
            remove_resource("container", name)
        for logs in run.logs.values():
            try:
                logs.close()
            except (OSError, subprocess.SubprocessError) as error:
                check("cleanup: log follower stopped", False, str(error))
        for vol in run.volumes:
            remove_resource("volume", vol)
        if net_created:
            remove_resource("network", run.net)
        evidence = {"identities": {r: {"npub": v["npub"], "address": v["address"]} for r, v in ids.items()},
                    "fixture_counters": run.counters, "results": results, "seconds": time.monotonic() - t_start,
                    "logs": {n: l.lines for n, l in run.logs.items()}}
        (w / "evidence.json").write_text(json.dumps(evidence, indent=1))
        print("fixture counters:", json.dumps(run.counters))
    failed = [r for r in results if r["result"] == "FAIL"]
    print(f"{len(results) - len(failed)} non-failing / {len(failed)} failing checks")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
