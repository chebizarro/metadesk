#!/usr/bin/env python3
"""md-ah9.11 private session fixture; see client-validation.md.

Runs in its own container: the tests/ private NIP-01 relay bound to a container
address; per role, a NIP-5F signer socket (nip5f-crypto) and/or a NIP-46 bunker
(nip46-bunker) over that relay, each holding a private throwaway key file; and
a FIPS control endpoint that reports the host peer as connected. That control
socket is a fixture, not a FIPS daemon: the TCP stream then travels a container
bridge without TUN. Bunker URIs go to ROLE/bunker.uri (0600). Prints `fixture
ready` when everything listens; on SIGTERM prints its counters and exits.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socketserver
import struct
import subprocess
import sys
import threading

sys.dont_write_bytecode = True


class Signer(socketserver.StreamRequestHandler):
    def handle(self):
        def send(value):
            data = json.dumps(value).encode()
            self.wfile.write(struct.pack("!I", len(data)) + data)
            self.wfile.flush()
        send({"name": "private-md-ah9-11", "version": 1})
        while True:
            header = self.rfile.read(4)
            if len(header) != 4:
                return
            length = struct.unpack("!I", header)[0]
            if length > 131072:
                return
            rpc = json.loads(self.rfile.read(length))
            if "name" in rpc:
                continue
            result = subprocess.run([self.server.crypto, self.server.key], input=json.dumps(rpc),
                                    capture_output=True, text=True, timeout=10)
            with self.server.lock:
                counts = self.server.counts
                counts[rpc.get("method")] = counts.get(rpc.get("method"), 0) + 1
            if result.returncode:
                send({"id": rpc.get("id"), "error": "signer refused", "result": None})
            else:
                send({"id": rpc["id"], "error": None, "result": json.loads(result.stdout)})


class Control(socketserver.StreamRequestHandler):
    def handle(self):
        rpc = json.loads(self.rfile.readline(65536))
        command = rpc.get("command")
        if command == "show_status":
            data = {"npub": self.server.local_npub, "state": "running"}
        elif command == "show_peers":
            data = {"peers": [{"npub": self.server.peer_npub, "connectivity": "connected", "link_id": 1}]}
        elif command == "ping":
            data = {}
        else:
            self.wfile.write(json.dumps({"status": "error", "message": "unsupported"}).encode() + b"\n")
            return
        with self.server.lock:
            self.server.counts[command] = self.server.counts.get(command, 0) + 1
        self.wfile.write(json.dumps({"status": "ok", "data": data}).encode() + b"\n")


class UnixServer(socketserver.ThreadingUnixStreamServer):
    daemon_threads = True

    def __init__(self, path, handler):
        super().__init__(str(path), handler)
        self.lock = threading.Lock()
        self.counts = {}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tests", type=Path, required=True, help="directory holding account_startup_fixture.py")
    p.add_argument("--crypto", required=True)
    p.add_argument("--relay", required=True, help="IP:PORT to bind")
    p.add_argument("--signer", action="append", default=[], metavar="ROLE=KEYFILE")
    p.add_argument("--bunker", action="append", default=[], metavar="ROLE=KEYFILE")
    p.add_argument("--bunker-bin")
    p.add_argument("--sockets", type=Path, required=True, help="ROLE/signer.sock is created under this")
    p.add_argument("--control", metavar="ROLE", help="role directory that also gets control.sock")
    p.add_argument("--local-npub")
    p.add_argument("--peer-npub")
    a = p.parse_args()
    sys.path.insert(0, str(a.tests))
    from account_startup_fixture import Peer, Relay

    class BoundRelay(Relay):
        def __init__(self, address):
            socketserver.ThreadingTCPServer.__init__(self, address, Peer)
            self.lock = threading.RLock()
            self.peers, self.events = set(), {}
            self.reqs = self.oks = self.eoses = 0

    host, port = a.relay.rsplit(":", 1)
    servers = {"relay": BoundRelay((host, int(port)))}
    for spec in a.signer:
        role, key = spec.split("=", 1)
        server = UnixServer(a.sockets / role / "signer.sock", Signer)
        server.crypto, server.key = a.crypto, key
        servers["signer:" + role] = server
    if a.control:
        server = UnixServer(a.sockets / a.control / "control.sock", Control)
        server.local_npub, server.peer_npub = a.local_npub, a.peer_npub
        servers["control:" + a.control] = server
    threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers.values()]
    for t in threads:
        t.start()
    bunkers, bunker_log = {}, {}
    for spec in a.bunker:
        role, key = spec.split("=", 1)
        proc = subprocess.Popen([a.bunker_bin, key, f"ws://{host}:{port}"], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        uri = proc.stdout.readline().strip()
        if not uri.startswith("bunker://"):
            raise SystemExit(f"bunker {role} failed: {proc.stderr.read()}")
        fd = os.open(a.sockets / role / "bunker.uri", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as out:
            out.write(uri + "\n")
        bunkers[role], bunker_log[role] = proc, []
        threading.Thread(target=lambda p=proc, log=bunker_log[role]: log.extend(p.stderr), daemon=True).start()
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    print("fixture ready", flush=True)
    stop.wait()
    relay = servers.pop("relay")
    report = {"relay": {"REQ": relay.reqs, "EOSE": relay.eoses, "OK": relay.oks, "events": len(relay.events)}}
    report.update({name: s.counts for name, s in servers.items()})
    for role, proc in bunkers.items():
        proc.terminate()
        proc.wait(timeout=10)
        report["bunker:" + role] = {"exit": proc.returncode,
                                    "connect": [l.split()[-1] for l in bunker_log[role] if "connect" in l]}
    for s in [relay, *servers.values()]:
        s.shutdown()
        s.server_close()
    print("fixture counters " + json.dumps(report, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
