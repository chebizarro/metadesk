#!/usr/bin/env python3
"""Real host CLI, private NIP-5F crypto signer + NIP-01 relay; no user state."""
import json
import os
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode = True
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, Peer, PrivateUnixServer

HOST_PK = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
CLIENT_PK = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
HOST_NPUB = "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d"
CLIENT_NPUB = "npub1ccz8l9zpa47k6vz9gphftsrumpw80rjt3nhnefat4symjhrsnmjs38mnyd"


class Signer(socketserver.StreamRequestHandler):
    def handle(self):
        def send(value):
            data = json.dumps(value).encode()
            self.wfile.write(struct.pack("!I", len(data)) + data)
            self.wfile.flush()
        send({"name": "private-md-pss", "version": 1})
        while True:
            header = self.rfile.read(4)
            if len(header) != 4:
                return
            length = struct.unpack("!I", header)[0]
            assert length <= 131072
            rpc = json.loads(self.rfile.read(length))
            if "name" in rpc:
                continue
            result = subprocess.run([self.server.crypto, self.server.role], input=json.dumps(rpc),
                                    capture_output=True, text=True, timeout=10, check=True)
            send({"id": rpc["id"], "error": None, "result": json.loads(result.stdout)})


class AccessPeer(Peer):
    def dispatch(self, msg):
        if msg[0] == "REQ":
            for filt in msg[2:]:
                assert filt.get("kinds") == [30000], filt
                assert filt.get("authors") == [HOST_PK], filt
                assert filt.get("#d") == ["metadesk-allowlist"], filt
                assert filt.get("limit") == 1, filt
            self.server.history_seen.set()
            assert self.server.history_release.wait(15), "history gate not released"
            if self.server.closed:
                self.send(["CLOSED", msg[1], "blocked: private history refused"])
                return
        if msg[0] == "EVENT":
            ev = msg[1]
            assert ev["kind"] == 30000 and ev["pubkey"] == HOST_PK
            assert ["d", "metadesk-allowlist"] in ev["tags"]
            assert len(ev["sig"]) == 128
            self.server.publish_seen.set()
            assert self.server.publish_release.wait(15), "publish gate not released"
            if self.server.reject:
                self.send(["OK", ev["id"], False, "blocked: private publish refused"])
                return
        super().dispatch(msg)


def run_contracts(host, crypto, root, env, relay):
    url = f"ws://127.0.0.1:{relay.server_address[1]}"
    config = root / "accounts"
    config.mkdir(mode=0o700)
    def account(*args, ok=True):
        p = subprocess.run([host, "account", "--config-dir", str(config), *args],
                           env=env, capture_output=True, text=True, timeout=15)
        assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
        return p
    def command(*args):
        return [host, "access", "--config-dir", str(config), "--relay", url, *args]
    def access(*args, ok=True):
        p = subprocess.run(command(*args), env=env, capture_output=True, text=True, timeout=20)
        assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
        return p
    access("list", ok=False)
    for args in [[], ["add"], ["remove"], ["add", "npub1bad"], ["add", CLIENT_PK],
                 ["add", CLIENT_NPUB, "video"], ["list", "extra"], ["--open", "list"]]:
        p = access(*args, ok=False)
        assert p.returncode == 2, p.stderr
    account("add", "nip5f", HOST_PK, "private host")
    account("bind-role", "client", "nip5f", HOST_PK)
    access("list", ok=False)  # Never borrow the client role.
    account("bind-role", "host", "nip5f", HOST_PK)

    relay.history_release.clear()
    relay.history_seen.clear()
    p = subprocess.Popen(command("list"), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert relay.history_seen.wait(10), "no allowlist REQ"
        assert p.poll() is None, "list returned before EOSE"
    finally:
        relay.history_release.set()
    stdout, stderr = p.communicate(timeout=20)
    assert p.returncode == 0 and stdout == "", (stdout, stderr)

    access("add", HOST_NPUB)
    relay.publish_release.clear()
    relay.publish_seen.clear()
    p = subprocess.Popen(command("add", CLIENT_NPUB), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert relay.publish_seen.wait(10), "no allowlist EVENT"
        assert p.poll() is None, "add returned before OK"
    finally:
        relay.publish_release.set()
    stdout, stderr = p.communicate(timeout=20)
    assert p.returncode == 0, (stdout, stderr)
    expected = {HOST_PK, CLIENT_PK}
    assert set(access("list").stdout.splitlines()) == expected
    count = relay.oks
    access("add", CLIENT_NPUB)
    assert relay.oks == count, "idempotent add republished"
    relay.reject = True
    access("remove", CLIENT_NPUB, ok=False)
    relay.reject = False
    assert set(access("list").stdout.splitlines()) == expected
    relay.closed = True
    access("list", ok=False)
    access("add", CLIENT_NPUB, ok=False)
    relay.closed = False
    access("remove", CLIENT_NPUB)
    assert access("list").stdout.strip() == HOST_PK
    count = relay.oks
    access("remove", CLIENT_NPUB)
    assert relay.oks == count
    access("remove", HOST_NPUB)
    assert access("list").stdout == ""
    # Saved host selection must be checked against the actual signer owner.
    relay.server_role = None
    metadata = config / "management.json"
    data = json.loads(metadata.read_text())
    for entry in data["accounts"]:
        if entry["backend"] == "nip5f":
            entry["pubkey"] = CLIENT_PK
    data["roles"]["host"]["pubkey"] = CLIENT_PK
    data["roles"]["client"]["pubkey"] = CLIENT_PK
    metadata.write_text(json.dumps(data))
    access("list", ok=False)


def main():
    syntax_only = "--syntax-only" in sys.argv
    host, crypto = map(os.path.abspath, [a for a in sys.argv[1:] if a != "--syntax-only"])
    with tempfile.TemporaryDirectory(prefix="md-pss-") as tmp:
        root = Path(tmp).resolve()
        home = root / "home"
        home.mkdir(mode=0o700)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("NOSTR_", "GNOSTR_", "MD_", "XDG_"))
               and k not in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY")}
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"),
                   NOSTR_SIGNER_SOCK=str(root / "signer.sock"),
                   DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-bus"))
        if syntax_only:
            base = [host, "access", "--config-dir", str(root / "syntax")]
            for args in [[], ["add"], ["remove"], ["add", "npub1bad"], ["add", CLIENT_PK],
                         ["add", CLIENT_NPUB, "video"], ["list", "extra"], ["--open", "list"]]:
                p = subprocess.run([*base, *args], env=env, capture_output=True, text=True, timeout=10)
                assert p.returncode == 2, (args, p.stdout, p.stderr)
            p = subprocess.run([*base, "list"], env=env, capture_output=True, text=True, timeout=10)
            assert p.returncode == 1 and "host account restore" in p.stderr, p.stderr
            print("access CLI syntax and absent-host-role contracts passed")
            return
        signer = PrivateUnixServer(env["NOSTR_SIGNER_SOCK"], Signer)
        signer.crypto, signer.role = crypto, "host"
        relay = Relay()
        relay.RequestHandlerClass = AccessPeer
        relay.history_seen, relay.history_release = threading.Event(), threading.Event()
        relay.publish_seen, relay.publish_release = threading.Event(), threading.Event()
        relay.history_release.set()
        relay.publish_release.set()
        relay.closed = relay.reject = False
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (signer, relay)]
        for t in threads:
            t.start()
        try:
            run_contracts(host, crypto, root, env, relay)
            print(f"access CLI: private relay history/OK gates, rejection, CLOSED, host-role ownership, membership and idempotency passed ({relay.oks} OK)")
        finally:
            for s in (signer, relay):
                s.shutdown()
                s.server_close()
            for t in threads:
                t.join()


if __name__ == "__main__":
    main()
