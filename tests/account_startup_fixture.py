#!/usr/bin/env python3
"""Private NIP-01 relay for the real nostrc NIP-46 transport test.

stdlib-only; loopback/random port, bounded in-memory history, REQ replay + EOSE,
EVENT acknowledgement (including duplicate OK), CLOSE and explicit CLOSED.
No user's HOME, keyring, relay, desktop or credentials are used.
"""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading


class Relay(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Peer)
        self.lock = threading.RLock()
        self.peers = set()
        self.events = {}
        self.reqs = self.oks = self.eoses = 0


def matches(event, filt):
    for field in ("kinds", "authors", "ids"):
        if field in filt:
            value = event.get({"kinds": "kind", "authors": "pubkey", "ids": "id"}[field])
            if value not in filt[field]:
                return False
    if event.get("created_at", 0) < filt.get("since", 0):
        return False
    if event.get("created_at", 0) > filt.get("until", 2**63):
        return False
    for key, values in filt.items():
        if key.startswith("#") and not any(
            len(tag) > 1 and tag[0] == key[1:] and tag[1] in values
            for tag in event.get("tags", [])
        ):
            return False
    return True


class Peer(socketserver.StreamRequestHandler):
    def send(self, value, opcode=1):
        payload = json.dumps(value, separators=(",", ":")).encode() if opcode == 1 else value
        length = len(payload)
        head = bytes([0x80 | opcode])
        if length < 126:
            head += bytes([length])
        elif length < 65536:
            head += b"\x7e" + struct.pack("!H", length)
        else:
            head += b"\x7f" + struct.pack("!Q", length)
        with self.send_lock:
            self.wfile.write(head + payload)
            self.wfile.flush()

    def read_exact(self, count):
        data = self.rfile.read(count)
        if len(data) != count:
            raise EOFError
        return data

    def handle(self):
        self.send_lock = threading.Lock()
        self.subs = {}
        # A deadlock safety bound, not a message-delivery/completion timer.
        self.request.settimeout(45)
        try:
            request = self.rfile.readline(8192)
            if not request:
                return
            headers = {}
            while True:
                line = self.rfile.readline(8192)
                if line in (b"\r\n", b"\n", b""):
                    break
                k, v = line.decode().split(":", 1)
                headers[k.lower()] = v.strip()
            if "sec-websocket-key" not in headers:
                body = json.dumps({"supported_nips": [1, 4, 11, 46], "name": "private-md70v3"}).encode()
                self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/nostr+json\r\nContent-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
                return
            gate = getattr(self.server, "handshake_gate", None)
            if gate is not None and not gate.wait(timeout=15):
                raise RuntimeError("fixture websocket handshake gate never released")
            accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
            self.wfile.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
            self.wfile.flush()
            with self.server.lock:
                self.server.peers.add(self)
            while True:
                a, b = self.read_exact(2)
                opcode, length = a & 15, b & 127
                if length == 126:
                    length = struct.unpack("!H", self.read_exact(2))[0]
                elif length == 127:
                    length = struct.unpack("!Q", self.read_exact(8))[0]
                if length > 1024 * 1024 or not a & 128:
                    raise ValueError("oversized/fragmented fixture frame")
                mask = self.read_exact(4) if b & 128 else None
                payload = self.read_exact(length)
                if mask:
                    payload = bytes(v ^ mask[i % 4] for i, v in enumerate(payload))
                if opcode == 8:
                    return
                if opcode == 9:
                    self.send(payload, 10)
                    continue
                if opcode != 1:
                    continue
                self.dispatch(json.loads(payload))
        except (EOFError, OSError, ValueError):
            pass
        finally:
            with self.server.lock:
                self.server.peers.discard(self)

    def dispatch(self, msg):
        with self.server.lock:
            if msg[0] == "REQ":
                sid, filters = msg[1], msg[2:]
                if not filters or any(not f.get("kinds") for f in filters):
                    self.send(["CLOSED", sid, "restricted: fixture requires kinds"])
                    return
                self.subs[sid] = filters
                self.server.reqs += 1
                for filt in filters:
                    history = [e for e in self.server.events.values() if matches(e, filt)]
                    history.sort(key=lambda e: e["created_at"], reverse=True)
                    for event in history[:filt.get("limit", 256)]:
                        self.send(["EVENT", sid, event])
                self.send(["EOSE", sid])
                self.server.eoses += 1
            elif msg[0] == "CLOSE":
                self.subs.pop(msg[1], None)
            elif msg[0] == "EVENT":
                event = msg[1]
                # nostrc bunker/client validate signatures and encrypted payloads.
                # The fixture relay deliberately does not duplicate their crypto.
                duplicate = event["id"] in self.server.events
                if len(self.server.events) >= 256:
                    raise ValueError("fixture history exceeded")
                self.server.events[event["id"]] = event
                self.send(["OK", event["id"], True, "duplicate:" if duplicate else ""])
                self.server.oks += 1
                if not duplicate:
                    for peer in list(self.server.peers):
                        for sid, filters in peer.subs.items():
                            if any(matches(event, f) for f in filters):
                                try:
                                    peer.send(["EVENT", sid, event])
                                except OSError:
                                    pass
            else:
                self.send(["NOTICE", "unsupported fixture command"])


class PrivateUnixServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True


class IdentitySigner(socketserver.StreamRequestHandler):
    def handle(self):
        banner = b'{"name":"private-fixture","version":1}'
        self.wfile.write(struct.pack("!I", len(banner)) + banner)
        self.wfile.flush()
        while True:
            header = self.rfile.read(4)
            if len(header) != 4:
                return
            length = struct.unpack("!I", header)[0]
            if length > 65536:
                return
            rpc = json.loads(self.rfile.read(length))
            if "name" in rpc:
                continue  # client hello, no RPC result
            result = {"id": rpc["id"], "error": None, "result": self.server.pubkey}
            assert rpc["method"] == "get_public_key", rpc["method"]
            payload = json.dumps(result).encode()
            self.wfile.write(struct.pack("!I", len(payload)) + payload)
            self.wfile.flush()


class FipsStatus(socketserver.StreamRequestHandler):
    def handle(self):
        rpc = json.loads(self.rfile.readline(65536))
        self.server.commands.append(rpc["command"])
        data = {"npub": self.server.npub} if self.server.npub is not None else {}
        self.wfile.write(json.dumps({"status": "ok", "data": data}).encode() + b"\n")


def startup_contracts(host, client, home, env, nip5f):
    config = home / ("Library/Application Support/metadesk" if sys.platform == "darwin" else ".config/metadesk")
    config.mkdir(parents=True, mode=0o700)
    os.chmod(config, 0o700)
    metadata = config / "management.json"
    pk = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
    account = {"backend": "direct", "pubkey": pk, "label": "fixture only"}
    metadata.write_text(json.dumps({"version": 1, "accounts": [account], "roles": {"host": {"backend": "direct", "pubkey": pk}, "client": {"backend": "direct", "pubkey": pk}}}))
    metadata.chmod(0o600)
    binaries = [(host, [], "host")]
    if client:
        binaries.append((client, ["127.0.0.1", "--no-display"], "client"))
    for binary, args, role in binaries:
        result = subprocess.run([binary, *args], env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode == 1 and f"{role} account restore" in result.stderr, result.stderr
    metadata.write_text("invalid metadata: explicit flags must bypass this")
    for binary, args, _ in binaries:
        result = subprocess.run([binary, *args, "--bunker", "invalid-fixture-uri"], env=env, capture_output=True, text=True, timeout=15)
        assert result.returncode == 1 and "requested signer backend" in result.stderr and "account restore" not in result.stderr, result.stderr
    if nip5f:
        # Real runtime restore from metadata through the private NIP-5F endpoint.
        # Only identity IPC is needed: FIPS must refuse before any session work.
        signer_path, fips_path = home.parent / "s.sock", home.parent / "f.sock"
        signer = PrivateUnixServer(str(signer_path), IdentitySigner)
        fips = PrivateUnixServer(str(fips_path), FipsStatus)
        signer.pubkey = pk
        fips.commands = []
        npub = "npub1sx42mj99aql52aklsg70y2jmr95u7uz2p40k769aw46ppjv302kqkhmu5r"
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in (signer, fips)]
        for thread in threads:
            thread.start()
        env = dict(env, NOSTR_SIGNER_SOCK=str(signer_path))
        try:
            for backend, explicit in (("nip5f", []), ("direct", ["--socket-signer", str(signer_path)])):
                selected = {"backend": backend, "pubkey": pk}
                metadata.write_text(json.dumps({"version": 1, "accounts": [dict(selected, label="private fixture")], "roles": {"host": selected, "client": selected}}))
                for node in (npub, None, "malformed"):
                    fips.npub = node
                    result = subprocess.run([host, "--npub", npub, "--fips-control", str(fips_path), *explicit], env=env, capture_output=True, text=True, timeout=15)
                    assert result.returncode == 1 and "FIPS host identity check refused" in result.stderr, result.stderr
                    assert "host: signer ready" in result.stdout, result.stdout
                    if node == npub:
                        assert "differs from the FIPS node key" in result.stderr, result.stderr
                # Selected client role is checked against the actual socket owner.
                if backend == "nip5f" and client:
                    with socket.socket() as closed_port:
                        closed_port.bind(("127.0.0.1", 0))
                        result = subprocess.run([client, "127.0.0.1", "--port", str(closed_port.getsockname()[1]), "--no-display", "--timeout", "100"], env=env, capture_output=True, text=True, timeout=15)
                        assert result.returncode == 1 and "client: signer ready" in result.stdout and "account restore" not in result.stderr, result.stderr
                    signer.pubkey = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
                    result = subprocess.run([client, "127.0.0.1", "--no-display"], env=env, capture_output=True, text=True, timeout=15)
                    assert result.returncode == 1 and "client account restore" in result.stderr and "pinned account" in result.stderr, result.stderr
                    signer.pubkey = pk
            assert fips.commands.count("show_status") == 12, fips.commands
        finally:
            signer.shutdown()
            fips.shutdown()
            signer.server_close()
            fips.server_close()
            for thread in threads:
                thread.join()


def main():
    nip5f = "--nip5f" in sys.argv
    executable, host, *client = map(os.path.abspath, [a for a in sys.argv[1:] if a != "--nip5f"])
    with tempfile.TemporaryDirectory(prefix="md70v3-private-") as tmp:
        root = Path(tmp).resolve()
        home = root / "home"
        home.mkdir(mode=0o700)
        dirs = [root / "roles", root / "nip46"]
        for path in dirs:
            path.mkdir(mode=0o700)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("NOSTR_", "GNOSTR_", "MD_")) and k not in ("DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR")}
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"), DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-session-bus"))
        relay = Relay()
        thread = threading.Thread(target=relay.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([executable, *map(str, dirs), f"ws://127.0.0.1:{relay.server_address[1]}"], env=env, check=True, timeout=90)
            startup_contracts(host, client[0] if client else None, home, env, nip5f)
            assert relay.reqs > 0 and relay.reqs == relay.eoses
            print(f"Private relay: {relay.reqs} REQ/EOSE, {relay.oks} acknowledged EVENT; fixture removed")
        finally:
            relay.shutdown()
            with relay.lock:
                for peer in list(relay.peers):
                    peer.request.close()
            relay.server_close()
            thread.join()


if __name__ == "__main__":
    main()
