#!/usr/bin/env python3
"""Opt-in Linux loopback session: real binaries, NIP-46, private relay/keyring.

Requires sudo -n ip, dbus-run-session and gnome-keyring-daemon. The FIPS control
fixture reports a connected peer; an isolated network namespace routes its ULA
on loopback. This tests session authorization/binding, not FIPS encryption.
"""
import json
import os
from pathlib import Path
import socket
import socketserver
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode = True
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, PrivateUnixServer
from test_access_cli import HOST_PK, CLIENT_PK, HOST_NPUB, CLIENT_NPUB


class Control(socketserver.StreamRequestHandler):
    def handle(self):
        rpc = json.loads(self.rfile.readline(65536))
        if rpc["command"] == "show_status":
            data = {"npub": HOST_NPUB, "state": "running"}
        elif rpc["command"] == "show_peers":
            data = {"peers": [{"npub": npub, "connectivity": "connected", "link_id": i + 1}
                              for i, npub in enumerate((HOST_NPUB, CLIENT_NPUB))]}
        else:
            assert rpc["command"] == "ping", rpc
            data = {}
        self.wfile.write(json.dumps({"status": "ok", "data": data}).encode() + b"\n")


def checked(args, env, **kwargs):
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30, **kwargs)
    assert p.returncode == 0, (args, p.stdout, p.stderr)
    return p


def inside(host, client, bunker, root):
    env = os.environ.copy()
    daemon = subprocess.Popen(["gnome-keyring-daemon", "--foreground", "--unlock", "--components=secrets",
                               "--control-directory", str(root / "run/keyring")],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    daemon.stdin.write("private-md-pss-password\n")
    daemon.stdin.close()
    assert daemon.stdout.readline().startswith("GNOME_KEYRING_CONTROL=")
    relay = Relay()
    control = PrivateUnixServer(str(root / "control.sock"), Control)
    threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (relay, control)]
    for t in threads:
        t.start()
    children, readers = [], []
    logs = {}
    try:
        url = f"ws://127.0.0.1:{relay.server_address[1]}"
        credentials, addresses = {}, {}
        for role in ("host", "client"):
            p = subprocess.Popen([bunker, role, url], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, env=env)
            children.append(p)
            credentials[role] = p.stdout.readline().strip()
            addresses[role] = p.stdout.readline().strip()
            assert json.loads(credentials[role])["user_pubkey"] == (HOST_PK if role == "host" else CLIENT_PK)
        configs = {}
        for role, pk in (("host", HOST_PK), ("client", CLIENT_PK)):
            configs[role] = root / role
            configs[role].mkdir(mode=0o700)
            cmd = [host, "account", "--config-dir", str(configs[role])]
            checked([*cmd, "add", "nip46", pk, "Private session test"], env)
            checked([*cmd, "credentials-stdin", pk], env, input=credentials[role])
            checked([*cmd, "bind-role", role, "nip46", pk], env)
        cmd = [host, "access", "--config-dir", str(configs["host"]), "--relay", url]
        checked([*cmd, "add", CLIENT_NPUB], env)
        listed = checked([*cmd, "list"], env)
        assert listed.stdout.strip() == CLIENT_PK, listed.stdout
        # The caller placed this entire process tree in a disposable netns.
        # No interface, route or address in the VM's main namespace is changed.
        checked(["ip", "-6", "addr", "add", addresses["host"] + "/128", "dev", "lo", "nodad"], env)
        checked(["ip", "link", "set", "lo", "up"], env)
        with socket.socket(socket.AF_INET6) as port_socket:
            port_socket.bind(("::1", 0))
            port = str(port_socket.getsockname()[1])
        def launch(name, args, target):
            ready = threading.Event()
            logs[name] = []
            p = subprocess.Popen(["stdbuf", "-oL", "-eL", *args], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            children.append(p)
            def read():
                for line in p.stdout:
                    logs[name].append(line)
                    if target in line:
                        ready.set()
            t = threading.Thread(target=read, daemon=True)
            t.start()
            readers.append(t)
            return p, ready
        # Use standard default startup paths, with separate host/client HOME
        # roots and account files created solely by account commands above.
        def activate(role):
            # Startup supports XDG_CONFIG_HOME; account --config-dir used the
            # exact directory that becomes XDG_CONFIG_HOME/metadesk here.
            config_home = root / (role + "-config")
            config_home.mkdir(mode=0o700)
            configs[role].rename(config_home / "metadesk")
            env["XDG_CONFIG_HOME"] = str(config_home)
        activate("host")
        hp, host_ready = launch("host", [host, "--bind", addresses["host"], "--port", port,
                               "--relay", url, "--no-capture", "--no-input"],
                               "nostr bridge ready, waiting for session requests")
        assert host_ready.wait(30), "".join(logs["host"])
        activate("client")
        cp, connected = launch("client", [client, "--npub", HOST_NPUB, "--fips-control", str(root / "control.sock"),
                                 "--relay", url, "--port", port, "--no-display", "--timeout", "15000"],
                                 "receiving stream...")
        assert connected.wait(45), "".join(logs["host"] + logs["client"])
        assert hp.poll() is None and cp.poll() is None, logs
        assert any("client #1 connected" in line for line in logs["host"]), logs
        print("PASS: account add/credentials-stdin/bind-role -> access add/list -> real host/client NIP-17 negotiation -> one-use TCP session_bind ACK")
        print(f"Private relay: {relay.reqs} REQ, {relay.eoses} EOSE, {relay.oks} OK; no video/input/desktop or real FIPS daemon exercised")
    finally:
        for p in reversed(children):
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        for t in readers:
            t.join(timeout=5)
        for s in (relay, control):
            s.shutdown()
            s.server_close()
        for t in threads:
            t.join()
        daemon.terminate()
        daemon.wait(timeout=10)
        for name, lines in logs.items():
            # Session tokens are never emitted by the production processes.
            print(f"--- {name} ---\n{''.join(lines)}")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--namespace":
        root = Path(sys.argv[5])
        fixture = None
        try:
            for path in (root, *root.iterdir()):
                os.chown(path, 0, 0)
            subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
            fixture = subprocess.Popen(["dbus-run-session", "--", sys.executable,
                                        str(Path(__file__).resolve()), "--inside", *sys.argv[2:]],
                                       start_new_session=True)
            rc = fixture.wait(timeout=150)
            if rc:
                raise subprocess.CalledProcessError(rc, fixture.args)
        finally:
            if fixture is not None:
                for sig in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(fixture.pid, sig)
                    except ProcessLookupError:
                        pass
                    try:
                        fixture.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        if sig == signal.SIGKILL:
                            raise
            shutil.rmtree(root)
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--inside":
        inside(*sys.argv[2:5], Path(sys.argv[5]))
        return
    if os.environ.get("MD_ACCESS_SESSION_E2E") != "1":
        print("SKIP: opt in with MD_ACCESS_SESSION_E2E=1 on Linux with sudo -n, iproute2 and private-keyring dependencies")
        sys.exit(77)
    host, client, bunker = map(os.path.abspath, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="md-pss-session-") as tmp:
        root = Path(tmp).resolve()
        env = {k: v for k, v in os.environ.items() if k in
               ("PATH", "LANG", "LC_ALL", "LD_LIBRARY_PATH", "ASAN_OPTIONS", "UBSAN_OPTIONS")}
        for key, name in (("HOME", "home"), ("XDG_RUNTIME_DIR", "run"), ("XDG_CONFIG_HOME", "config"),
                          ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache")):
            path = root / name
            path.mkdir(mode=0o700)
            env[key] = str(path)
        # unshare creates a disposable namespace inherited by the entire fixture;
        # sudo preserves only this explicitly constructed non-user environment.
        try:
            subprocess.run(["sudo", "-n", "env", *[f"{k}={v}" for k, v in env.items()],
                            "unshare", "--net", "--pid", "--fork", "--kill-child=SIGKILL", sys.executable,
                            str(Path(__file__).resolve()), "--namespace", host, client, bunker, str(root)],
                           check=True, timeout=180)
        finally:
            if root.exists():
                # Fallback if the namespace supervisor itself was terminated.
                # The private PID namespace has killed its descendants first.
                subprocess.run(["sudo", "-n", sys.executable, "-c",
                                "import shutil,sys; shutil.rmtree(sys.argv[1])", str(root)],
                               check=True, timeout=10)


if __name__ == "__main__":
    main()
