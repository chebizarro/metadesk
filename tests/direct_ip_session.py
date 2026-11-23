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


def reject_replay(port, transcript):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as attacker:
        attacker.sendall(transcript)
        try:
            # EOF/reset is refusal; a watchdog timeout is a failure, not proof.
            while attacker.recv(65536):
                pass
        except ConnectionResetError:
            pass


class Proxy(socketserver.BaseRequestHandler):
    """Transparent byte capture; never terminates TLS or learns its keys."""
    def handle(self):
        try:
            with socket.create_connection(("127.0.0.1", self.server.target), timeout=10) as upstream:
                self.request.settimeout(10)
                with self.server.lock:
                    captured = [bytearray(), bytearray()]
                    index = len(self.server.proofs)
                    self.server.proofs.append(captured)
                    self.server.connections.append((self.request.dup(),))
                while True:
                    readable = select.select([self.request, upstream], [], [])[0]
                    if index in self.server.blackholed:
                        # Silent partition: stop forwarding but hold both
                        # sockets open, so no FIN/RST ever reaches a peer.
                        self.server.release.wait()
                        return
                    for source in readable:
                        data = source.recv(65536)
                        if not data:
                            return
                        with self.server.lock:
                            captured[0 if source is self.request else 1].extend(data)
                        (upstream if source is self.request else self.request).sendall(data)
        except (EOFError, OSError):
            pass


class ProxyServer(socketserver.ThreadingTCPServer):
    daemon_threads = True


def run(host, client, crypto):
    with tempfile.TemporaryDirectory(prefix="md-w12-direct-") as tmp:
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
        acceptances = []
        acceptance_lock = threading.Lock()
        def observe(rpc, value):
            if rpc.get("method") != "nip44_decrypt":
                return
            try:
                obj = json.loads(value) if isinstance(value, str) else value
                if isinstance(obj, dict) and isinstance(obj.get("content"), str):
                    obj = json.loads(obj["content"])
                if isinstance(obj, dict) and obj.get("type") == "session_accept":
                    with acceptance_lock:
                        if obj not in acceptances:
                            acceptances.append(obj)
            except (ValueError, TypeError):
                pass
        signers[1].observe_result = observe
        with socket.socket() as port_socket:
            port_socket.bind(("127.0.0.1", 0))
            port = port_socket.getsockname()[1]
        proxy = ProxyServer(("127.0.0.1", 0), Proxy)
        proxy.target, proxy.lock = port, threading.Lock()
        proxy.proofs, proxy.connections = [], []
        proxy.blackholed, proxy.release = set(), threading.Event()
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
            def assert_unauthorized_no_tcp():
                # This is the fail-closed/no-dial gate, not denial-delivery
                # qualification. The deliberately short watchdog can win while
                # the signer drains retained history under sanitizers. Require
                # the host's correlated refusal AND no TCP attempt/traffic;
                # test_host_approval.py separately requires delivered denial.
                with proxy.lock:
                    before = len(proxy.proofs)
                denied = subprocess.run([*args, "--timeout", "1500", "--socket-signer",
                                         str(root / "host.sock")], env=env,
                                        capture_output=True, text=True, timeout=20)
                assert denied.returncode == 1, denied
                assert any(reason in denied.stderr for reason in (
                    "host refused session", "timed out waiting for session accept")), denied
                request_line = next(line for line in denied.stdout.splitlines()
                                    if "sending session request DM (request_id=" in line)
                prefix = request_line.split("request_id=", 1)[1][:8]
                assert len(prefix) == 8 and all(c in "0123456789abcdef" for c in prefix)
                hp.wait("refused session request " + prefix)
                assert f"client: connecting to 127.0.0.1:{proxy.server_address[1]}" not in denied.stdout, denied
                assert "client: receiving stream" not in denied.stdout, denied
                with proxy.lock:
                    assert len(proxy.proofs) == before, "unauthorized client reached TCP"
                assert hp.p.poll() is None
            # Host identity itself is not in its allowlist: signed != authorized.
            assert_unauthorized_no_tcp()
            print("PASS: correlated host refusal; non-allowlisted signed client never dials TCP")
            cp = Process([*args, "--timeout", "5000", "--socket-signer", str(root / "client.sock")], env)
            children.append(cp)
            cp.wait("client: receiving stream")
            hp.wait("host: streaming to client #1")
            assert cp.p.poll() is None and hp.p.poll() is None
            assert not any("FIPS daemon" in line for line in cp.lines), cp.lines
            print("PASS: allowlisted binary client negotiates over private Nostr, binds IPv4 TCP and receives ACK")
            # After a valid bind the same bytes cannot authorize a second peer.
            with proxy.lock:
                proof = bytes(proxy.proofs[0][0])
                wire = b"".join(bytes(v) for v in proxy.proofs[0])
            with acceptance_lock:
                assert len(acceptances) == 1
                first = acceptances[0]
            assert len(first["tls_cert_sha256"]) == 64
            assert first["stream_token"].encode() not in wire
            assert b"session_bind" not in wire and b"stream_token" not in wire
            assert proof[:2] == b"\x16\x03", "expected TLS handshake, not plaintext framing"
            print("PASS: captured native TCP bytes contain TLS, never the negotiated token or binding JSON")
            reject_replay(port, proof)
            print("PASS: second native TCP peer cannot replay a consumed TLS transcript")
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
            with acceptance_lock:
                assert len(acceptances) == 2
                first, second = acceptances
            assert first["session_id"] != second["session_id"]
            assert first["stream_token"] != second["stream_token"]
            requests = [line for line in cp.lines if "sending session request DM" in line]
            assert len(requests) == 2 and requests[0] != requests[1], requests
            print("PASS: same-process reconnect negotiates fresh request ID, session ID and one-use token")
            # Exact old proof must also fail against the newly negotiated session.
            reject_replay(port, proof)
            assert cp.p.poll() is None and hp.p.poll() is None
            # Silent partition: the path stops carrying bytes but neither
            # socket closes. Both peers must declare the session dead from
            # missed keepalives and renegotiate, not hang on a stale stream.
            with proxy.lock:
                proxy.blackholed.add(1)
            hp.wait("host: client silent")
            cp.wait("client: host silent")
            hp.wait("ready for next authorized session request", count=2)
            cp.wait("client: reconnected", count=2)
            hp.wait("host: streaming to client #3")
            with proxy.lock:
                assert len(proxy.proofs) == 3
            assert cp.p.poll() is None and hp.p.poll() is None
            print("PASS: silent partition is detected by keepalive on both sides; client renegotiates")
            proxy.release.set()
            cp.stop()
            hp.wait("ready for next authorized session request", count=3)
            # A correlated unauthorized request cannot consume the next grant.
            assert_unauthorized_no_tcp()
            # Also exercise positional syntax straight to the native host
            # listener, with no fault proxy anywhere in this connection.
            cp2 = Process([client, "127.0.0.1", "--npub", HOST_NPUB,
                           "--port", str(port), "--no-display", "--relay", url,
                           "--socket-signer", str(root / "client.sock")], env)
            children.append(cp2)
            cp2.wait("client: receiving stream")
            hp.wait("host: streaming to client #4")
            assert cp2.p.poll() is None and hp.p.poll() is None
            print("PASS: idle host survives unauthorized requests; positional direct TCP binds native host listener without proxy")
            assert relay.oks >= 4 and relay.reqs == relay.eoses
            print(f"Private relay: {relay.reqs} REQ/EOSE, {relay.oks} OK; native 127.0.0.1; no public relays")
        finally:
            proxy.release.set()
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
