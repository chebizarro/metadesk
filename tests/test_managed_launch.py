#!/usr/bin/env python3
"""Exercise the real client entry point without a signer, relay, or desktop."""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

CLIENT = sys.argv[1]
KEY = "ab" * 32
SIGNER = "cd" * 32
BASE = dict(v=1, address=KEY, pin=KEY, signer=SIGNER, route="127.0.0.1",
            port=7700, caps=1, relays=["ws://127.0.0.1:1"])


def managed(payload, expected):
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    parent.settimeout(8)
    env = dict(os.environ, SDL_VIDEODRIVER="no-display-fixture")
    argv = [CLIENT, "--managed-session-fd", str(child.fileno())]
    with parent, child, subprocess.Popen(argv, pass_fds=(child.fileno(),), env=env,
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
        child.close()
        try:
            if payload is None:
                parent.shutdown(socket.SHUT_WR)
            else:
                parent.sendall(payload)
            observed = []
            while packet := parent.recv(64):
                observed.append(packet)
            stdout, stderr = proc.communicate(timeout=8)
            assert proc.returncode == 1, (proc.returncode, stdout, stderr)
            assert observed == expected, (observed, stdout, stderr)
            return stderr
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=8)


resolve, failed = b"\x01\x01", b"\x01\x0a"
assert b"no usable display" in managed(json.dumps(BASE).encode(), [resolve, failed])
assert b"invalid private Manager launch" in managed(json.dumps(dict(BASE, pin=SIGNER)).encode(), [failed])
assert b"invalid private Manager launch" in managed(json.dumps(dict(BASE, address="127.0.0.1")).encode(), [failed])
assert b"invalid private Manager launch" in managed(json.dumps(dict(BASE, v=2)).encode(), [failed])
assert b"invalid private Manager launch" in managed(None, [failed])
result = subprocess.run([CLIENT, "--managed-session-fd", "0"], check=False,
                        capture_output=True, timeout=8)
assert result.returncode == 1 and b"invalid private Manager launch" in result.stderr


def cancel_after_display(crypto):
    from account_startup_fixture import PrivateUnixServer
    from test_access_cli import CLIENT_PK, Signer

    with tempfile.TemporaryDirectory(prefix="md-launch-") as tmp:
        root = Path(tmp)
        config = root / "config/metadesk"
        config.mkdir(mode=0o700, parents=True)
        record = {"backend": "nip5f", "pubkey": CLIENT_PK}
        metadata = config / "management.json"
        metadata.write_text(json.dumps({"version": 1, "accounts": [dict(record, label="Fixture")],
                                        "roles": {"client": record, "host": None}}))
        metadata.chmod(0o600)
        for use_eof in (False, True):
            signer_path = root / ("eof.sock" if use_eof else "cancel.sock")
            seen, release = threading.Event(), threading.Event()

            def observe(rpc, value):
                if rpc["method"] == "get_public_key":
                    seen.set()
                    assert release.wait(10), "signer gate not released"

            with PrivateUnixServer(str(signer_path), Signer) as signer:
                signer.crypto, signer.role, signer.observe_result = crypto, "client", observe
                worker = threading.Thread(target=signer.serve_forever)
                worker.start()
                try:
                    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                    parent.settimeout(8)
                    env = dict(os.environ, SDL_VIDEODRIVER="x11", XDG_CONFIG_HOME=str(config.parent),
                               NOSTR_SIGNER_SOCK=str(signer_path))
                    with parent, child, subprocess.Popen([CLIENT, "--managed-session-fd", str(child.fileno())],
                            pass_fds=(child.fileno(),), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
                        child.close()
                        try:
                            parent.sendall(json.dumps(dict(BASE, signer=CLIENT_PK)).encode())
                            assert parent.recv(64) == resolve
                            assert parent.recv(64) == b"\x01\x02"  # signer phase follows display initialization
                            assert seen.wait(8), "client never reached the real signer adapter"
                            if use_eof:
                                # EOF in the child, but keep reading the outcome on our half.
                                parent.shutdown(socket.SHUT_WR)
                            else:
                                parent.sendall(b"\x01\x0c")  # CANCEL
                            assert parent.recv(64) == b"\x01\x0b"  # CANCELLED, before releasing signer
                            release.set()
                            assert parent.recv(64) == b""
                            stdout, stderr = proc.communicate(timeout=8)
                            assert proc.returncode == 1, (stdout, stderr)
                        finally:
                            release.set()
                            if proc.poll() is None:
                                proc.kill()
                                proc.wait(timeout=8)
                finally:
                    signer.shutdown()
                    worker.join()


if sys.argv[2:3] == ["--switch-seam"]:
    subprocess.run([sys.argv[3], "--managed-switch"], check=True, timeout=8)
elif len(sys.argv) > 2:
    cancel_after_display(sys.argv[2])
