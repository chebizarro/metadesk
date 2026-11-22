"""Wire-level tests of the private relay's opt-in ephemeral policy (no containers)."""
import base64
from contextlib import contextmanager
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class WebSocket:
    def __init__(self, address):
        self.sock = socket.create_connection(address, timeout=5)
        self.file = self.sock.makefile("rb")
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n").encode())
        if b"101" not in self.file.readline():
            raise RuntimeError("fixture websocket upgrade failed")
        while self.file.readline() != b"\r\n":
            pass

    def send(self, value):
        body, mask = json.dumps(value).encode(), os.urandom(4)
        head = (bytes([0x81, 0x80 | len(body)]) if len(body) < 126 else
                bytes([0x81, 0xfe]) + struct.pack("!H", len(body)))
        self.sock.sendall(head + mask + bytes(v ^ mask[i % 4] for i, v in enumerate(body)))

    def receive(self):
        opcode, length = self.file.read(2)
        if opcode != 0x81:
            raise RuntimeError("not a fixture text frame")
        if length == 126:
            length = struct.unpack("!H", self.file.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.file.read(8))[0]
        return json.loads(self.file.read(length))

    def close(self):
        self.sock.shutdown(socket.SHUT_RDWR)
        self.file.close()
        self.sock.close()


@contextmanager
def relay(ephemeral):
    with tempfile.TemporaryDirectory() as work:
        # Reserve a random loopback port; fixture readiness, not a sleep, gates use.
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            address = reservation.getsockname()
        cmd = [sys.executable, "-B", str(HERE / "client-fixture.py"),
               "--tests", str(HERE.parents[2] / "tests"), "--crypto", "/unused",
               "--relay", f"127.0.0.1:{address[1]}", "--sockets", work]
        if ephemeral:
            cmd.append("--no-store-ephemeral")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        sockets = []
        try:
            if not select.select([proc.stdout], [], [], 5)[0] or proc.stdout.readline().strip() != "fixture ready":
                raise RuntimeError("fixture did not become ready")

            def connect():
                ws = WebSocket(address)
                sockets.append(ws)
                return ws
            yield connect
        finally:
            for ws in sockets:
                ws.close()
            proc.terminate()
            out, err = proc.communicate(timeout=10)
            if proc.returncode:
                raise RuntimeError(f"fixture exit={proc.returncode}: {out} {err}")


def event(kind, identity="test", recipient="client"):
    # The fixture deliberately delegates signature verification to nostrc.
    return {"id": identity, "pubkey": "publisher", "created_at": 1,
            "kind": kind, "tags": [["p", recipient]], "content": "test only"}


class EphemeralPolicyTests(unittest.TestCase):
    def test_live_delivery_ack_and_eose_without_replay(self):
        with relay(True) as connect:
            pub, sub = connect(), connect()
            filt = {"kinds": [24133], "authors": ["publisher"], "#p": ["client"]}
            sub.send(["REQ", "live", filt])
            self.assertEqual(sub.receive(), ["EOSE", "live"])
            e = event(24133)
            pub.send(["EVENT", e])
            self.assertEqual(pub.receive(), ["OK", e["id"], True, ""])
            self.assertEqual(sub.receive(), ["EVENT", "live", e])
            late = connect()
            late.send(["REQ", "late", filt])
            self.assertEqual(late.receive(), ["EOSE", "late"])
            sub.send(["CLOSE", "live"])
            after_close = event(24133, "after-close")
            sub.send(["EVENT", after_close])
            self.assertEqual(sub.receive(), ["OK", "after-close", True, ""])
            # Same-connection REQ/EOSE is an ordering barrier: a stale live
            # subscription would deliver EVENT before this EOSE, not later.
            sub.send(["REQ", "barrier", {"kinds": [1059]}])
            self.assertEqual(sub.receive(), ["EOSE", "barrier"])
            sub.send(["REQ", "invalid", {}])
            self.assertEqual(sub.receive(), ["CLOSED", "invalid", "restricted: fixture requires kinds"])

    def test_kind_boundaries_and_tight_live_filter(self):
        with relay(True) as connect:
            ws = connect()
            ws.send(["REQ", "live", {"kinds": [24133], "#p": ["client"]}])
            self.assertEqual(ws.receive(), ["EOSE", "live"])
            for kind in [19999, 20000, 29999, 30000]:
                e = event(kind, str(kind))
                ws.send(["EVENT", e])
                self.assertEqual(ws.receive()[:3], ["OK", e["id"], True])
                ws.send(["REQ", "history", {"kinds": [kind]}])
                if kind in [19999, 30000]:
                    self.assertEqual(ws.receive(), ["EVENT", "history", e])
                self.assertEqual(ws.receive(), ["EOSE", "history"])
                ws.send(["CLOSE", "history"])
            e = event(24133, "wrong-recipient", "other")
            ws.send(["EVENT", e])
            self.assertEqual(ws.receive()[:3], ["OK", e["id"], True])
            ws.send(["REQ", "barrier", {"kinds": [1059]}])
            self.assertEqual(ws.receive(), ["EOSE", "barrier"])

    def test_long_ephemeral_run_preserves_stored_history(self):
        with relay(True) as connect:
            ws = connect()
            for i in range(300):
                e = event(24133, str(i))
                ws.send(["EVENT", e])
                self.assertEqual(ws.receive()[:3], ["OK", str(i), True])
            stored = event(1059, "giftwrap")
            ws.send(["EVENT", stored])
            self.assertEqual(ws.receive()[:3], ["OK", "giftwrap", True])
            ws.send(["REQ", "history", {"kinds": [1059, 24133]}])
            self.assertEqual(ws.receive(), ["EVENT", "history", stored])
            self.assertEqual(ws.receive(), ["EOSE", "history"])

    def test_default_retains_ephemeral_for_existing_client_cells(self):
        with relay(False) as connect:
            ws = connect()
            e = event(24133)
            ws.send(["EVENT", e])
            self.assertEqual(ws.receive()[:3], ["OK", e["id"], True])
            ws.send(["REQ", "history", {"kinds": [24133]}])
            self.assertEqual(ws.receive(), ["EVENT", "history", e])
            self.assertEqual(ws.receive(), ["EOSE", "history"])


if __name__ == "__main__":
    unittest.main()
