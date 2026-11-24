#!/usr/bin/env python3
"""Real loopback NIP-01 REQ/EOSE/OK and external NIP-5F crypto."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from account_startup_fixture import Peer, PrivateUnixServer, Relay
from test_access_cli import CLIENT_PK, Signer

SECRET = b"Private note that must never hit disk"


class StorePeer(Peer):
    def dispatch(self, msg):
        if msg[0] == "REQ":
            assert msg[2:] == [{"kinds": [30078], "authors": [CLIENT_PK],
                               "#d": ["metadesk:management:v1"], "limit": 1}], msg
        if msg[0] == "EVENT":
            ev = msg[1]
            assert ev["kind"] == 30078 and ev["pubkey"] == CLIENT_PK, ev
            assert ev["tags"] == [["d", "metadesk:management:v1"]], ev
            assert ev["content"].startswith("A") and SECRET.decode() not in ev["content"]
            with self.server.lock:
                self.server.attempts.append(ev)
                if self.server.reject_first and len(self.server.attempts) == 1:
                    self.send(["OK", ev["id"], False, "blocked: scripted first attempt"])
                    return
        super().dispatch(msg)


def main():
    executable, crypto = map(os.path.abspath, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="mdms-") as root:
        root = Path(root).resolve()
        accounts, storage = root / "accounts", root / "store"
        accounts.mkdir(mode=0o700)
        storage.mkdir(mode=0o700)
        servers = []
        threads = []

        def start(server):
            servers.append(server)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            threads.append(thread)

        try:
            sockets = []
            for role in ("client", "host"):
                sock = root / role
                signer = PrivateUnixServer(str(sock), Signer)
                signer.crypto, signer.role = crypto, role
                start(signer)
                sockets.append(sock)
            urls = []
            relays = []
            for reject in (False, True):
                relay = Relay()
                relay.RequestHandlerClass = StorePeer
                relay.reject_first, relay.attempts = reject, []
                start(relay)
                urls.append(f"ws://127.0.0.1:{relay.server_address[1]}")
                relays.append(relay)
            cmd = [executable, accounts, storage, *sockets, *urls]
            crash = subprocess.run([*cmd, "crash"], capture_output=True, text=True, timeout=20)
            assert crash.returncode == 42, (crash.returncode, crash.stdout, crash.stderr)
            assert not any(r.attempts for r in relays)
            run = subprocess.run([*cmd, "reopen"], capture_output=True, text=True, timeout=45)
            assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
            assert len(relays[0].attempts) == 1 and len(relays[1].attempts) == 2
            assert relays[0].attempts[0] == relays[1].attempts[0] == relays[1].attempts[1]
            assert relays[0].eoses >= 2 and all(r.oks == 1 for r in relays)
            owner = storage / CLIENT_PK
            assert owner.stat().st_mode & 0o777 == 0o700
            for path in storage.glob("*/*"):
                assert path.stat().st_mode & 0o777 == 0o600, path
                data = path.read_bytes()
                assert SECRET not in data and b"Test host" not in data and b"schema_version" not in data, path
                assert b"0" * 63 + b"2" not in data and b"127.0.0.1" not in data and b"bunker://" not in data, path
            assert not list(storage.glob("*/store.tmp"))
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()
    print("PASS: managed store with external signer and two private relays")


if __name__ == "__main__":
    main()
