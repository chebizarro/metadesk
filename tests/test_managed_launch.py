#!/usr/bin/env python3
"""Exercise the real client entry point without a signer, relay, or desktop."""
import json
import os
import socket
import subprocess
import sys

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
