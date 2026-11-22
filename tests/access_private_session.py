#!/usr/bin/env python3
"""Real host/client processes over private Nostr + authenticated loopback TCP.

No desktop capture/input, FIPS daemon/routing, public relay, or user credentials.
Run: python3 tests/access_private_session.py BUILD/metadesk-host BUILD/metadesk-client BUILD/access_signer_fixture
"""
import json
import os
from pathlib import Path
import pty
import socket
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, PrivateUnixServer
from test_access_cli import Signer, HOST_PK, CLIENT_PK, HOST_NPUB, CLIENT_NPUB


def main():
    clipboard_native = "--clipboard-native" in sys.argv
    clipboard_denied = "--clipboard-denied" in sys.argv
    live_monitor = "--monitor-capture" in sys.argv
    live_quality = "--quality-capture" in sys.argv or live_monitor
    host, client, crypto = map(os.path.abspath, [a for a in sys.argv[1:] if a not in ("--quality-capture","--monitor-capture","--clipboard-native","--clipboard-denied")])

    with tempfile.TemporaryDirectory(prefix="md-w15-session-") as tmp:
        root = Path(tmp).resolve()
        relay = Relay()
        servers, threads, children, readers = [relay], [], [], []
        logs = {}
        clipboard_events = {role: threading.Event() for role in ("host", "client")}
        clipboard_helper = None
        try:
            if clipboard_native or clipboard_denied:
                clipboard_helper = subprocess.Popen([str(Path(crypto).parent / "clipboard_native_fixture")],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                assert clipboard_helper.stdout.readline().strip() == "READY"
                clipboard_helper.stdin.write("set md-w15-round-trip\n"); clipboard_helper.stdin.flush()
                assert clipboard_helper.stdout.readline().strip() == "SET"
            envs = {}
            for role, pk in (("host", HOST_PK), ("client", CLIENT_PK)):
                home = root / role
                home.mkdir(mode=0o700)
                env = {k: v for k, v in os.environ.items()
                       if not k.startswith(("NOSTR_", "GNOSTR_", "MD_", "XDG_"))
                       and k not in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY")}
                env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / "config"),
                           NOSTR_SIGNER_SOCK=str(root / (role + ".sock")),
                           DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-bus"))
                envs[role] = env
                signer = PrivateUnixServer(env["NOSTR_SIGNER_SOCK"], Signer)
                signer.crypto, signer.role = crypto, role
                servers.append(signer)
            for server in servers:
                t = threading.Thread(target=server.serve_forever, daemon=True)
                t.start()
                threads.append(t)
            def checked(args, role):
                p = subprocess.run(args, env=envs[role], capture_output=True, text=True, timeout=20)
                assert p.returncode == 0, (args, p.stdout, p.stderr)
                return p
            for role, pk in (("host", HOST_PK), ("client", CLIENT_PK)):
                checked([host, "account", "add", "nip5f", pk, "Private session"], role)
                checked([host, "account", "bind-role", role, "nip5f", pk], role)
            url = f"ws://127.0.0.1:{relay.server_address[1]}"
            checked([host, "access", "--relay", url, "add", CLIENT_NPUB, "video,clipboard" if clipboard_native else "video,monitor" if live_monitor else "video"], "host")
            listed = checked([host, "access", "--relay", url, "list"], "host")
            assert listed.stdout.strip() == CLIENT_PK + ("\tvideo,clipboard" if clipboard_native else "\tvideo,monitor" if live_monitor else "\tvideo"), listed.stdout
            with relay.lock:
                policy = next(e for e in relay.events.values() if e["kind"] == 30000)
                assert policy["tags"] == [["d", "metadesk-allowlist"]] and policy["content"]
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = str(sock.getsockname()[1])
            def launch(role, args, target):
                master, slave = pty.openpty()
                p = subprocess.Popen(args, env=envs[role], stdout=slave, stderr=slave, close_fds=True)
                os.close(slave)
                children.append(p)
                logs[role] = []
                ready = threading.Event()
                def read():
                    try:
                        with os.fdopen(master, "r", errors="replace") as stream:
                            for line in stream:
                                logs[role].append(line)
                                if "clipboard received" in line: clipboard_events[role].set()
                                if target in line:
                                    ready.set()
                    except OSError:  # PTY EOF on Linux
                        pass
                t = threading.Thread(target=read, daemon=True)
                t.start()
                readers.append(t)
                return p, ready
            hp, ready = launch("host", [host, "--bind", "127.0.0.1", "--port", port,
                                        "--relay", url, "--no-input", *(["--fps", "30", "--bitrate", "2000000",
                                        "--capture-max-size", "1024x768"] if live_quality else ["--no-capture"])],
                               "nostr bridge ready, waiting for session requests")
            assert ready.wait(20), logs
            cp, connected = launch("client", [client, "--host", "127.0.0.1", "--npub", HOST_NPUB,
                                               "--relay", url, "--port", port, "--no-display", "--timeout", "15000",
                                               *(["--clipboard","both"] if clipboard_native or clipboard_denied else []),
                                               *(["--max-size", "640x480", "--bitrate", "1000000", "--fps", "15", *(["--output","id:1"] if live_monitor else [])] if live_quality else [])],
                                   "decoded frame size" if live_quality else "receiving stream...")
            assert connected.wait(25), logs
            assert hp.poll() is None and cp.poll() is None, logs
            if clipboard_native:
                assert all(e.wait(15) for e in clipboard_events.values()), logs
                clipboard_helper.stdin.write("check md-w15-round-trip\n"); clipboard_helper.stdin.flush()
                assert clipboard_helper.stdout.readline().strip() == "MATCH"
                print("PASS: native general NSPasteboard bidirectional text over real granted bound host/client stream")
            if clipboard_denied:
                assert any("clipboard not granted" in s for s in logs["client"]), logs
                assert not any("clipboard sent" in s or "clipboard received" in s for lines in logs.values() for s in lines), logs
                print("PASS: real host/client denies ungranted clipboard; unit backend counters prove zero accesses")
            if live_quality:
                assert any("cap=640x480 bitrate=1000000 fps=15" in s for s in logs["host"]), logs
                sizes = [s.split("decoded frame size ")[1].strip() for s in logs["client"] if "decoded frame size " in s]
                w, h = map(int, sizes[0].split("x"))
                assert w <= 640 and h <= 480, sizes
                print(f"PASS: real SCK -> encoder -> pinned TLS -> decoder, {w}x{h}; 1Mbps/15fps quality policy applied")
            print("PASS: real host/client processes; private-only allowlist via external NIP-5F; NIP-17 authorization; loopback TCP session_bind ACK")
            print("Scope: " + ("native quality capture; no input/FIPS;" if live_quality else "no capture/input/FIPS qualification;") + " no public relay, desktop devices, user keys, or routing changes")
        finally:
            for p in reversed(children):
                if p.poll() is None:
                    p.terminate()
                try:
                    p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
            if clipboard_helper:
                clipboard_helper.stdin.write("restore\n"); clipboard_helper.stdin.flush()
                assert clipboard_helper.stdout.readline().strip() == "RESTORED", "clipboard restoration failed"
                assert clipboard_helper.wait(timeout=10) == 0
                clipboard_helper.stdin.close(); clipboard_helper.stdout.close()
            for t in readers:
                t.join(timeout=10)
                assert not t.is_alive(), "process output reader leaked"
            for server in servers:
                server.shutdown()
                server.server_close()
            for t in threads:
                t.join()


if __name__ == "__main__":
    main()
