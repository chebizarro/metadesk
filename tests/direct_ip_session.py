#!/usr/bin/env python3
"""Real host/client, private NIP-17 relay/signers and native loopback TCP.

Invoked by the registered NIP-5F session test or directly with HOST CLIENT CRYPTO.
No public relay, keyring, FIPS daemon, capture or input is used; the host may initialize native a11y.
"""
import json
import os
from pathlib import Path
import select
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, PrivateUnixServer
from test_access_cli import Signer, HOST_PK, CLIENT_PK, HOST_NPUB
from test_client_startup import env_for


class Process:
    def __init__(self, args, env):
        self.lines = []
        self.changed = threading.Condition()
        self.p = subprocess.Popen(args, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True)
        def read():
            for line in self.p.stdout:
                with self.changed:
                    self.lines.append(line)
                    self.changed.notify_all()
            with self.changed:
                self.changed.notify_all()
        self.reader = threading.Thread(target=read)
        self.reader.start()

    def wait(self, text, count=1):
        with self.changed:
            assert self.changed.wait_for(
                lambda: sum(text in s for s in self.lines) >= count or self.p.poll() is not None,
                timeout=25), (text, self.lines)
            assert sum(text in s for s in self.lines) >= count, (text, self.lines)

    def stop(self):
        if self.p.poll() is None:
            self.p.terminate()
            try:
                self.p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                # The pre-existing host session-request wait has a 60s failure
                # bound. Do not leave it or any fixture process behind.
                self.p.kill()
                self.p.wait(timeout=5)
        self.reader.join(timeout=5)
        assert not self.reader.is_alive()


def exact(sock, n):
    data = b""
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise EOFError()
        data += part
    return data


class Proxy(socketserver.BaseRequestHandler):
    """Records only the first proof in private memory; can sever one stream."""
    def handle(self):
        try:
            with socket.create_connection(("127.0.0.1", self.server.target), timeout=10) as upstream:
                self.request.settimeout(10)
                header = exact(self.request, 16)
                length = struct.unpack_from("<I", header, 4)[0]
                assert header[1] == 0x20 and 0 < length <= 8192
                proof = header + exact(self.request, length)
                with self.server.lock:
                    self.server.proofs.append(proof)
                    # Own only the client-facing fault handle. Shutting it
                    # wakes this handler, whose context closes the upstream;
                    # retaining an upstream dup would mask natural client EOF.
                    self.server.connections.append((self.request.dup(),))
                upstream.sendall(proof)
                while True:
                    for source in select.select([self.request, upstream], [], [])[0]:
                        data = source.recv(65536)
                        if not data:
                            return
                        (upstream if source is self.request else self.request).sendall(data)
        except (EOFError, OSError):
            pass


class ProxyServer(socketserver.ThreadingTCPServer):
    daemon_threads = True


def run(host, client, crypto):
    with tempfile.TemporaryDirectory(prefix="md-l8-direct-") as tmp:
        root = Path(tmp).resolve()
        config = "Library/Application Support" if sys.platform == "darwin" else ".config"
        (root / config).mkdir(mode=0o700, parents=True)
        env = env_for(str(root))
        (root / ".config").mkdir(mode=0o700, exist_ok=True)
        # env_for isolates accounts and rejects inherited Nostr configuration.
        relay = Relay()
        url = f"ws://127.0.0.1:{relay.server_address[1]}"
        items = [["p", CLIENT_PK], ["metadesk-caps", CLIENT_PK, "v1:video"]]
        encrypted = subprocess.run([crypto, "host"], input=json.dumps(
            {"method": "nip44_encrypt", "params": {"peer_pub": HOST_PK, "plaintext": json.dumps(items)}}),
            env=env, capture_output=True, text=True, timeout=15, check=True)
        event = {"kind": 30000, "pubkey": HOST_PK, "created_at": int(time.time()),
                 "tags": [["d", "metadesk-allowlist"]], "content": json.loads(encrypted.stdout)}
        signed = subprocess.run([crypto, "host"], input=json.dumps(
            {"method": "sign_event", "params": {"event": event}}),
            env=env, capture_output=True, text=True, check=True, timeout=15)
        event = json.loads(signed.stdout)
        relay.events[event["id"]] = event
        signers = [PrivateUnixServer(str(root / (role + ".sock")), Signer)
                   for role in ("host", "client")]
        for server, role in zip(signers, ("host", "client")):
            server.crypto, server.role = crypto, role
        with socket.socket() as port_socket:
            port_socket.bind(("127.0.0.1", 0))
            port = port_socket.getsockname()[1]
        proxy = ProxyServer(("127.0.0.1", 0), Proxy)
        proxy.target, proxy.lock = port, threading.Lock()
        proxy.proofs, proxy.connections = [], []
        servers = [relay, *signers, proxy]
        threads = [threading.Thread(target=s.serve_forever) for s in servers]
        children = []
        for thread in threads:
            thread.start()
        try:
            refused = subprocess.run([host, "--no-capture", "--no-input", "--bind", "127.0.0.1",
                                      "--port", str(port), "--relay", url], env=env,
                                     capture_output=True, text=True, timeout=15)
            assert refused.returncode == 1 and "negotiated stream authorization required" in refused.stderr, refused
            print("PASS: real identity-free host refuses unnegotiated direct TCP")
            hp = Process([host, "--bind", "127.0.0.1", "--port", str(port),
                          "--no-capture", "--no-input", "--relay", url,
                          "--socket-signer", str(root / "host.sock")], env)
            children.append(hp)
            hp.wait("nostr bridge ready, waiting for session requests")
            try:
                socket.create_connection(("127.0.0.1", port), timeout=5).close()
                raise AssertionError("host listened before an authorized negotiation")
            except ConnectionRefusedError:
                pass
            print("PASS: signer-equipped host has no direct listener before negotiation")
            args = [client, "--host", "127.0.0.1", "--npub", HOST_NPUB,
                    "--port", str(proxy.server_address[1]), "--no-display", "--relay", url]
            # Host identity itself is not in its allowlist: signed != authorized.
            denied = subprocess.run([*args, "--timeout", "1500", "--socket-signer",
                                     str(root / "host.sock")], env=env,
                                    capture_output=True, text=True, timeout=20)
            assert denied.returncode == 1 and "host refused session" in denied.stderr, denied
            hp.wait("refused session request")
            assert not proxy.proofs, "unauthorized client reached TCP"
            print("PASS: non-allowlisted signed client is refused before TCP")
            cp = Process([*args, "--timeout", "5000", "--socket-signer", str(root / "client.sock")], env)
            children.append(cp)
            cp.wait("client: receiving stream")
            hp.wait("host: streaming to client #1")
            assert cp.p.poll() is None and hp.p.poll() is None
            assert not any("FIPS daemon" in line for line in cp.lines), cp.lines
            print("PASS: allowlisted binary client negotiates over private Nostr, binds IPv4 TCP and receives ACK")
            # After a valid bind the same bytes cannot authorize a second peer.
            with proxy.lock:
                proof = proxy.proofs[0]
            with socket.create_connection(("127.0.0.1", port), timeout=5) as attacker:
                attacker.sendall(proof)
                try:
                    assert not attacker.recv(16), "replayed proof received data/ACK"
                except ConnectionResetError:
                    pass
            print("PASS: second native TCP peer cannot replay a consumed stream proof")
            # Force a real transport loss. The SAME client and host renegotiate;
            # success is a new correlated Nostr exchange, never a replayed token.
            with proxy.lock:
                for sock in proxy.connections[0]:
                    sock.shutdown(socket.SHUT_RDWR)
            hp.wait("ready for next authorized session request")
            cp.wait("client: reconnected")
            hp.wait("host: streaming to client #2")  # active-stream extras do not increment this counter
            with proxy.lock:
                assert len(proxy.proofs) == 2
                first, second = [json.loads(p[16:]) for p in proxy.proofs]
            assert first["session_id"] != second["session_id"]
            assert first["stream_token"] != second["stream_token"]
            requests = [line for line in cp.lines if "sending session request DM" in line]
            assert len(requests) == 2 and requests[0] != requests[1], requests
            print("PASS: same-process reconnect negotiates fresh request ID, session ID and one-use token")
            # Exact old proof must also fail against the newly negotiated session.
            with socket.create_connection(("127.0.0.1", port), timeout=5) as attacker:
                attacker.sendall(proof)
                try:
                    assert not attacker.recv(16)
                except ConnectionResetError:
                    pass
            assert cp.p.poll() is None and hp.p.poll() is None
            cp.stop()
            hp.wait("ready for next authorized session request", count=2)
            # Keep admission idle across multiple shutdown-check wakeups;
            # unauthenticated activity cannot expire or consume the next grant.
            denied = subprocess.run([*args, "--timeout", "1500", "--socket-signer",
                                     str(root / "host.sock")], env=env,
                                    capture_output=True, text=True, timeout=20)
            assert denied.returncode == 1 and "host refused session" in denied.stderr, denied
            assert hp.p.poll() is None
            # Also exercise positional syntax straight to the native host
            # listener, with no fault proxy anywhere in this connection.
            cp2 = Process([client, "127.0.0.1", "--npub", HOST_NPUB,
                           "--port", str(port), "--no-display", "--relay", url,
                           "--socket-signer", str(root / "client.sock")], env)
            children.append(cp2)
            cp2.wait("client: receiving stream")
            hp.wait("host: streaming to client #3")
            assert cp2.p.poll() is None and hp.p.poll() is None
            print("PASS: idle host survives unauthorized requests; positional direct TCP binds native host listener without proxy")
            assert relay.oks >= 4 and relay.reqs == relay.eoses
            print(f"Private relay: {relay.reqs} REQ/EOSE, {relay.oks} OK; native 127.0.0.1; no public relays")
        finally:
            for child in reversed(children):
                child.stop()
            with proxy.lock:
                for pair in proxy.connections:
                    for sock in pair:
                        try:
                            sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        finally:
                            sock.close()
            for server in servers:
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join(timeout=5)
                assert not thread.is_alive()
            # Production output contains no bearer tokens; never print proofs.
            for name, child in zip(("host", "client", "positional-client"), children):
                print(f"--- {name} ---\n{''.join(child.lines)}")
            print("Cleanup: all child processes reaped, private servers closed and temporary HOME removed")


if __name__ == "__main__":
    run(*map(os.path.abspath, sys.argv[1:4]))
