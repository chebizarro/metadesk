#!/usr/bin/env python3
"""metadesk-host mcp-proxy <-> NIP-98 MCP host over loopback (md-ah9.14, md-ah9.3).

Real proxy binary (stdio <-> HTTP, MdSigner NIP-98 signing), real MCP HTTP
server and the production host-allowlist source (saved host role over a
private NIP-5F signer, live kind-30000 REQ on a private relay). Throwaway
keys only; no user HOME/keyring/relay/desktop.

Proves: fail-closed until EOSE; an agent-granted npub succeeds (responses
and SSE notifications); unlisted and video-only npubs get 401; revocation
and re-grant take effect on the running host without a restart. md-5km:
agent without input lists no action tool and gets isError naming input on
call; adding/removing input applies to the next request; a static
--allow-npub principal keeps the full tool surface. md-ht7: initialize
advertises tools.listChanged; widening or narrowing a live principal's tool
set sends exactly one notifications/tools/list_changed on its stream only (an
unrelated live principal's stream gets nothing); a grant change that leaves
the tool set alone sends nothing; revoking agent closes the stream at once.
"""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, Peer, PrivateUnixServer, matches
from test_access_cli import Signer, HOST_PK, CLIENT_PK, HOST_NPUB

PEER_PK = "e493dbf1c10d80f3581e4904930b1404cc6c13900ee0758474fa94abe8c4cd13"  # sk 4
LIST_CHANGED = {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}
ALLOWLIST_FILTER = {"kinds": [30000], "authors": [HOST_PK],
                    "#d": ["metadesk-allowlist"], "limit": 1}


class GatedPeer(Peer):
    """Holds allowlist history (EVENT+EOSE) until the test releases it."""
    def dispatch(self, msg):
        if msg[0] == "REQ":
            with self.server.lock:
                self.server.filters.extend(msg[2:])
            self.server.history_seen.set()
            assert self.server.history_release.wait(30), "history gate not released"
        super().dispatch(msg)


class Proc:
    """Child process with line queues for stdout and stderr."""
    def __init__(self, args, env, name):
        self.name = name
        self.p = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.out, self.err, self.log = queue.Queue(), queue.Queue(), []
        for stream, q in ((self.p.stdout, self.out), (self.p.stderr, self.err)):
            threading.Thread(target=self._pump, args=(stream, q), daemon=True).start()

    def _pump(self, stream, q):
        for line in stream:
            if q is self.err:
                self.log.append(line.rstrip())
            q.put(line.rstrip("\n"))

    def send(self, msg):
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()

    def line(self, timeout=20):
        try:
            return self.out.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError(f"{self.name}: no stdout line; stderr={self.log}")

    def expect_err(self, needle, timeout=20):
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            try:
                line = self.err.get(timeout=max(left, 0.01))
            except queue.Empty:
                raise AssertionError(f"{self.name}: no stderr {needle!r}; stderr={self.log}")
            if needle in line:
                return line

    def close(self, timeout=10):
        start = time.monotonic()
        self.p.stdin.close()
        rc = self.p.wait(timeout=timeout)
        return rc, time.monotonic() - start


def sign(crypto, env, tags, created_at):
    event = {"kind": 30000, "pubkey": HOST_PK, "created_at": created_at,
             "tags": [["d", "metadesk-allowlist"], *tags], "content": ""}
    done = subprocess.run([crypto, "host"], input=json.dumps(
        {"method": "sign_event", "params": {"event": event}}),
        env=env, capture_output=True, text=True, timeout=15, check=True)
    return json.loads(done.stdout)


def publish(relay, event):
    """Store and push to live subscriptions, as a relay does for a new EVENT."""
    with relay.lock:
        relay.events = {event["id"]: event}
        for peer in list(relay.peers):
            for sid, filters in peer.subs.items():
                if any(matches(event, f) for f in filters):
                    peer.send(["EVENT", sid, event])


def grants(client_caps, peer_caps=None):
    tags = [["p", CLIENT_PK], ["metadesk-caps", CLIENT_PK, client_caps],
            ["p", HOST_PK], ["metadesk-caps", HOST_PK, "v1:video"]]
    if peer_caps:
        tags += [["p", PEER_PK], ["metadesk-caps", PEER_PK, peer_caps]]
    return tags


INIT = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                   "clientInfo": {"name": "mcp-proxy-e2e", "version": "1"}}}


def call(proc, rid, tool, args=None):
    proc.send({"jsonrpc": "2.0", "id": rid, "method": "tools/call",
               "params": {"name": tool, "arguments": args or {}}})


def tool_names(proc, rid):
    proc.send({"jsonrpc": "2.0", "id": rid, "method": "tools/list"})
    reply = json.loads(proc.line())
    assert reply.get("id") == rid and "result" in reply, reply
    return sorted(t["name"] for t in reply["result"]["tools"])


def call_result(proc, rid, tool, args=None):
    call(proc, rid, tool, args)
    reply = json.loads(proc.line())
    assert reply.get("id") == rid and "result" in reply, reply  # never a protocol error
    return reply["result"]


def expect_refused(proc, rid, tool):
    result = call_result(proc, rid, tool)
    text = result["content"][0]["text"]
    assert result.get("isError") is True and "requires the 'input' grant" in text, result
    return text


def expect_acted(proc, rid):
    result = call_result(proc, rid, "act")
    assert not result.get("isError") and result["content"][0]["text"] == "acted", result


def expect_list_changed(proc):
    msg = json.loads(proc.line())
    assert msg == LIST_CHANGED, msg


def barrier(agent, rid, quiet=()):
    """Event-ordered "nothing else was sent": the host logs a list change only
    after notifying for it, and the fixture's notify then broadcasts one frame
    behind anything already queued on every stream. So agent's next two lines
    are exactly that frame and the call's response, and each quiet stream's
    next line is that frame: any list_changed would come first."""
    call(agent, rid, "notify", {"text": str(1 + len(quiet))})
    got = [json.loads(agent.line()), json.loads(agent.line())]
    note = [m for m in got if m.get("method") == "notifications/message"]
    resp = [m for m in got if m.get("id") == rid]
    assert len(note) == 1 and len(resp) == 1, got
    for proc in quiet:
        msg = json.loads(proc.line())
        assert msg.get("method") == "notifications/message", (proc.name, msg)


def expect_401(proc, rid):
    reply = json.loads(proc.line())
    assert reply.get("id") == rid and "HTTP 401" in reply["error"]["message"], reply
    return reply


def main():
    host, fixture, crypto = map(os.path.abspath, sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="md-ah9-14-") as tmp:
        root = Path(tmp).resolve()
        home = root / "home"
        home.mkdir(mode=0o700)
        env = {k: v for k, v in os.environ.items() if not k.startswith(("NOSTR_", "GNOSTR_", "MD_", "XDG_"))
               and k not in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY")}
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"),
                   NOSTR_SIGNER_SOCK=str(root / "host.sock"),
                   DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-bus"))
        servers = []
        host_signer = PrivateUnixServer(env["NOSTR_SIGNER_SOCK"], Signer)
        host_signer.crypto, host_signer.role = crypto, "host"
        client_signer = PrivateUnixServer(str(root / "client.sock"), Signer)
        client_signer.crypto, client_signer.role = crypto, "client"
        relay = Relay()
        relay.RequestHandlerClass = GatedPeer
        relay.filters = []
        relay.history_seen, relay.history_release = threading.Event(), threading.Event()
        servers = [host_signer, client_signer, relay]
        threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers]
        for t in threads:
            t.start()
        procs = []
        try:
            for args in (["add", "nip5f", HOST_PK, "private host"],
                         ["bind-role", "host", "nip5f", HOST_PK]):
                done = subprocess.run([host, "account", *args], env=env,
                                      capture_output=True, text=True, timeout=20)
                assert done.returncode == 0, (args, done.stdout, done.stderr)
            keys = {}
            for name, sk in (("client", 2), ("video", 1), ("stranger", 3), ("peer", 4)):
                path = root / f"{name}.key"
                path.write_text(f"{sk:064x}\n")
                path.chmod(0o600)
                keys[name] = str(path)
            loose = root / "loose.key"
            loose.write_text(f"{2:064x}\n")
            loose.chmod(0o644)
            now = int(time.time())
            relay.events = {}
            first = sign(crypto, env, grants("v1:agent"), now)
            relay.events = {first["id"]: first}

            url_relay = f"ws://127.0.0.1:{relay.server_address[1]}"
            hostp = Proc([fixture, url_relay], env, "host-fixture")
            procs.append(hostp)
            port = int(hostp.line().split()[1])
            url = f"http://[::1]:{port}/mcp"  # the server's default loopback bind
            assert relay.history_seen.wait(20), "no allowlist REQ"

            # Bad invocations fail before any I/O.
            for args in ([], [url, url], ["--key-file", str(loose), url], ["http://[::1]/x"],
                         ["--key-file", keys["client"], "--dbus-signer", url]):
                done = subprocess.run([host, "mcp-proxy", *args], env=env, input="",
                                      capture_output=True, text=True, timeout=20)
                assert done.returncode != 0 and done.stdout == "", (args, done.stdout, done.stderr)

            # 1. Fail closed while history is pending (REQ held before EOSE).
            agent = Proc([host, "mcp-proxy", "--key-file", keys["client"], url], env, "agent")
            procs.append(agent)
            agent.expect_err("signing as c6047f9441ed")
            agent.send(INIT)
            expect_401(agent, 1)
            relay.history_release.set()
            hostp.expect_err("host allowlist loaded (v1): 2 member(s), 1 with agent")
            print("PASS: NIP-98 fails closed until allowlist EOSE")

            # 2. Agent grant: responses and SSE notifications flow.
            agent.send(INIT)
            reply = json.loads(agent.line())
            assert reply["id"] == 1 and reply["result"]["serverInfo"]["name"] == "mcp-proxy-fixture", reply
            assert reply["result"]["capabilities"]["tools"] == {"listChanged": True}, reply
            agent.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            agent.expect_err("notification stream open")
            call(agent, 2, "echo", {"text": "hi é"})
            reply = json.loads(agent.line())
            assert reply["id"] == 2 and reply["result"]["content"][0]["text"] == "hi é", reply
            call(agent, 3, "notify")
            got = [json.loads(agent.line()), json.loads(agent.line())]
            note = [m for m in got if m.get("method") == "notifications/message"]
            resp = [m for m in got if m.get("id") == 3]
            assert note and note[0]["params"]["data"] == "fixture-notify", got
            assert resp and resp[0]["result"]["content"][0]["text"] == "notified", got
            agent.send({"jsonrpc": "2.0", "id": 4, "method": "ping"})
            reply = json.loads(agent.line())
            assert reply["id"] == 4 and "result" in reply, reply
            agent.p.stdin.write("not json\n")
            agent.p.stdin.flush()
            assert json.loads(agent.line())["error"]["code"] == -32700
            print("PASS: agent-granted npub: initialize, tools, ping, SSE notification")

            # 2b. agent without input: no action tool listed; calling it anyway
            # is a tool error naming the missing grant (md-5km).
            assert tool_names(agent, 20) == ["echo", "notify"]
            expect_refused(agent, 21, "act")
            print("PASS: agent-only grant hides action tools and refuses calls naming 'input'")

            # 3. Unlisted and video-only npubs get 401 (exit cleanly on EOF).
            for name in ("stranger", "video"):
                other = Proc([host, "mcp-proxy", "--key-file", keys[name], url], env, name)
                procs.append(other)
                other.send(INIT)
                expect_401(other, 1)
                other.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
                other.expect_err("notification not delivered")
                rc, _ = other.close()
                assert rc == 0, other.log
            print("PASS: unlisted and video-only npubs refused with HTTP 401")

            # 4. Live revocation and re-grant on the running host, no restart.
            # Revoking agent closes the open SSE stream on the change itself,
            # with no broadcast needed (md-ht7); the proxy's reconnect then
            # gets 401 until the re-grant.
            publish(relay, sign(crypto, env, grants("v1:video"), now + 1))
            hostp.expect_err("host allowlist updated (v2): 2 member(s), 0 with agent")
            agent.expect_err("notification stream ended (HTTP 200)")
            call(agent, 5, "echo", {"text": "after revoke"})
            expect_401(agent, 5)
            publish(relay, sign(crypto, env, grants("v1:video,agent"), now + 2))
            hostp.expect_err("host allowlist updated (v3): 2 member(s), 1 with agent")
            call(agent, 6, "echo", {"text": "regranted"})
            reply = json.loads(agent.line())
            assert reply["id"] == 6 and reply["result"]["content"][0]["text"] == "regranted", reply
            expect_refused(agent, 7, "act")
            agent.expect_err("notification stream open")
            print("PASS: revocation closes the SSE stream at once; re-grant applies live")

            # 4b. An unrelated live principal ("peer") with its own stream.
            # video,agent -> agent leaves the agent's tool set alone: nothing.
            publish(relay, sign(crypto, env, grants("v1:agent", "v1:agent"), now + 3))
            hostp.expect_err("host allowlist updated (v4): 3 member(s), 2 with agent (0 also input)")
            barrier(agent, 30)
            peer = Proc([host, "mcp-proxy", "--key-file", keys["peer"], url], env, "peer")
            procs.append(peer)
            peer.expect_err("signing as e493dbf1c10d")
            peer.send(INIT)
            assert json.loads(peer.line())["result"]["capabilities"]["tools"] == {"listChanged": True}
            peer.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            peer.expect_err("notification stream open")
            barrier(agent, 31, [peer])
            print("PASS: a grant change that keeps the tool set sends no list_changed")

            # 4c. Widening agent -> agent,input: exactly one list_changed, to
            # the agent's stream only; actions apply on the next request.
            publish(relay, sign(crypto, env, grants("v1:agent,input", "v1:agent"), now + 4))
            hostp.expect_err("host allowlist updated (v5): 3 member(s), 2 with agent (1 also input)")
            expect_list_changed(agent)
            barrier(agent, 32, [peer])
            assert tool_names(agent, 8) == ["act", "echo", "notify"]
            expect_acted(agent, 9)
            # Video is not an MCP grant: still the same tool set, nothing sent.
            publish(relay, sign(crypto, env, grants("v1:video,agent,input", "v1:agent"), now + 5))
            hostp.expect_err("host allowlist updated (v6): 3 member(s), 2 with agent (1 also input)")
            barrier(agent, 33, [peer])
            print("PASS: widening sends exactly one list_changed, only to that principal")

            # 4d. Narrowing agent,input -> agent: exactly one list_changed;
            # the next request is refused (md-5km).
            publish(relay, sign(crypto, env, grants("v1:agent", "v1:agent"), now + 6))
            hostp.expect_err("host allowlist updated (v7): 3 member(s), 2 with agent (0 also input)")
            expect_list_changed(agent)
            barrier(agent, 34, [peer])
            expect_refused(agent, 10, "act")
            assert tool_names(agent, 11) == ["echo", "notify"]
            print("PASS: narrowing sends exactly one list_changed, only to that principal")

            # 4e. Revoking the peer closes only the peer's stream.
            publish(relay, sign(crypto, env, grants("v1:agent", "v1:video"), now + 7))
            hostp.expect_err("host allowlist updated (v8): 3 member(s), 1 with agent (0 also input)")
            peer.expect_err("notification stream ended (HTTP 200)")
            barrier(agent, 35)
            rc, _ = peer.close()
            assert rc == 0, peer.log
            print("PASS: revoking agent closes just that principal's stream")

            rc, elapsed = agent.close()
            assert rc == 0 and elapsed < 5, (rc, elapsed, agent.log)
            print(f"PASS: proxy exits on stdin EOF with an open SSE stream ({elapsed:.2f}s)")

            # 5. MdSigner IPC path: NIP-5F socket signer (client role key).
            sock = Proc([host, "mcp-proxy", "--socket-signer", str(root / "client.sock"),
                         "--no-sse", url], env, "socket")
            procs.append(sock)
            sock.expect_err("(NIP-5F")
            sock.send(INIT)
            assert json.loads(sock.line())["result"]["serverInfo"]["name"] == "mcp-proxy-fixture"
            rc, _ = sock.close()
            assert rc == 0, sock.log
            print("PASS: NIP-5F socket signer path")

            rc, _ = hostp.close()
            assert rc == 0, hostp.log

            # 6. Static --allow-npub principal: full tool surface (documented
            # operator credential), although its live grant is video-only.
            statp = Proc([fixture, url_relay, "--allow-npub", HOST_NPUB], env, "static-fixture")
            procs.append(statp)
            sport = int(statp.line().split()[1])
            statp.expect_err("host allowlist loaded")
            static = Proc([host, "mcp-proxy", "--key-file", keys["video"], "--no-sse",
                           f"http://[::1]:{sport}/mcp"], env, "static")
            procs.append(static)
            static.send(INIT)
            assert json.loads(static.line())["result"]["serverInfo"]["name"] == "mcp-proxy-fixture"
            static.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            assert tool_names(static, 2) == ["act", "echo", "notify"]
            expect_acted(static, 3)
            rc, _ = static.close()
            assert rc == 0, static.log
            rc, _ = statp.close()
            assert rc == 0, statp.log
            print("PASS: static --allow-npub principal keeps action tools")

            # Minimal bridge: exactly the allowlist filter, no DM/gift-wrap REQ;
            # one REQ/EOSE per host fixture run.
            with relay.lock:
                assert relay.filters and all(f == ALLOWLIST_FILTER for f in relay.filters), relay.filters
                assert relay.reqs == relay.eoses == 2, (relay.reqs, relay.eoses)
            print("Private relay: 1 REQ/EOSE per host (allowlist only), live EVENT pushes; throwaway keys")
        finally:
            relay.history_release.set()
            for proc in procs:
                if proc.p.poll() is None:
                    proc.p.kill()
                    proc.p.wait()
            for s in servers:
                s.shutdown()
                s.server_close()
            with relay.lock:
                for peer in list(relay.peers):
                    peer.request.close()
            for t in threads:
                t.join()


if __name__ == "__main__":
    main()
