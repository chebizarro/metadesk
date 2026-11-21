#!/usr/bin/env python3
"""Private, controlled NIP-01/NIP-42 relay pair for test_nostr.

Only the test process advances history/acknowledgements; no sleep determines
completion. Socket/subprocess/condition deadlines are deadlock safety bounds.
Uses the existing stdlib websocket fixture, private loopback ports and test keys.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

from account_startup_fixture import Relay, Peer, matches


class AclRelay(Relay):
    def __init__(self):
        super().__init__()
        self.RequestHandlerClass = AclPeer
        self.cv = threading.Condition(self.lock)
        self.reset()

    def reset(self, auth=False):
        self.active = None
        self.reqs = self.publishes = self.auths = self.protected = 0
        self.mode = "hold"
        self.held = []
        self.published = []
        self.auth_required = auth
        self.challenge = "challenge-one"
        self.authenticated = False
        self.last_auth = None
        self.challenged = False
        self.handshake_gate = threading.Event()
        self.handshake_gate.set()

    def wait(self, predicate):
        if not self.cv.wait_for(predicate, timeout=15):
            raise RuntimeError("fixture protocol boundary never arrived")

    def emit(self, name, *args):
        assert self.active is not None
        for sid in list(self.active.subs):
            self.active.send([name, sid, *args])


class AclPeer(Peer):
    def dispatch(self, msg):
        s = self.server
        with s.cv:
            if msg[0] == "REQ":
                s.active = self
                s.reqs += 1
                assert all(f.get("kinds") for f in msg[2:])
                acl = [f for f in msg[2:] if 30000 in f["kinds"]]
                assert len(acl) == 1 and acl[0].get("authors") and acl[0].get("#d") == ["metadesk-allowlist"] and acl[0].get("limit") == 1
                if s.auth_required and not s.authenticated:
                    if not s.challenged:
                        self.send(["AUTH", s.challenge])
                        s.challenged = True
                    self.send(["CLOSED", msg[1], "auth-required: private fixture"])
                else:
                    self.subs[msg[1]] = msg[2:]
                    if s.auth_required:
                        s.protected += 1
                        self.send(["EOSE", msg[1]])
            elif msg[0] == "EVENT":
                assert msg[1]["kind"] != 22242, "AUTH sent over EVENT"
                s.publishes += 1
                s.published.append(msg[1])
                if s.mode == "reject":
                    self.send(["OK", msg[1]["id"], False, "blocked: fixture rejection"])
                elif s.mode == "accept":
                    self.send(["OK", msg[1]["id"], True, ""])
                else:
                    s.held.append((self, msg[1]))
            elif msg[0] == "AUTH":
                event = msg[1]
                assert event["kind"] == 22242
                assert ["challenge", s.challenge] in event["tags"]
                assert ["relay", f"ws://127.0.0.1:{s.server_address[1]}"] in event["tags"]
                s.auths += 1
                s.last_auth = (self, event)
            elif msg[0] == "CLOSE":
                self.subs.pop(msg[1], None)
            else:
                raise AssertionError(f"unexpected client frame {msg[0]}")
            s.cv.notify_all()


def control(servers, command):
    s = servers[command.get("relay", 0)]
    with s.cv:
        op = command["op"]
        if op == "reset":
            s.reset(command.get("auth", False))
        elif op == "hold_connection":
            s.handshake_gate.clear()
        elif op == "release_connection":
            s.handshake_gate.set()
        elif op == "req":
            s.wait(lambda: s.reqs >= command.get("count", 1))
        elif op == "eose":
            s.emit("EOSE")
        elif op == "closed":
            # Recovery is reason-specific: AUTH OK, backoff or reconnect.
            s.emit("CLOSED", command.get("reason", "restricted: private fixture"))
            s.active.subs.clear()
        elif op == "req_after_drop":
            s.wait(lambda: s.reqs >= s.after_drop_reqs)
        elif op == "drop":
            s.after_drop_reqs = s.reqs + 1
            # Transient loss: disconnect, hold the reconnect at the handshake
            # until release_connection.
            s.handshake_gate.clear()
            s.active.request.shutdown(socket.SHUT_RDWR)
        elif op == "event":
            s.emit("EVENT", command["event"])
        elif op == "event_eose":
            s.emit("EVENT", command["event"])
            s.emit("EOSE")
        elif op == "auth_count":
            assert s.auths == command["count"]
        elif op == "mode":
            s.mode = command["value"]
        elif op == "held":
            s.wait(lambda: bool(s.held))
        elif op == "echo":
            assert s.held
            s.emit("EVENT", s.held[-1][1])
        elif op == "ack":
            for peer, event in s.held:
                peer.send(["OK", event["id"], command["ok"], "" if command["ok"] else "blocked: fixture"])
            s.held.clear()
        elif op == "challenge":
            s.challenge = command["value"]
            s.authenticated = False
            s.active.send(["AUTH", s.challenge])
        elif op == "auth":
            s.wait(lambda: s.auths >= command["count"])
            assert s.last_auth
            return s.last_auth[1]
        elif op == "auth_result":
            assert s.last_auth
            s.authenticated = command["ok"]
            peer, event = s.last_auth
            peer.send(["OK", event["id"], command["ok"], "" if command["ok"] else "restricted: fixture"])
        elif op == "published":
            assert s.published
            return s.published[-1]
        elif op == "protected":
            assert s.protected == command["count"]
        else:
            raise AssertionError(op)
    return "OK"


def main():
    servers = [AclRelay(), AclRelay()]
    threads = [threading.Thread(target=s.serve_forever, daemon=True) for s in servers]
    for t in threads:
        t.start()
    child = None
    try:
        with tempfile.TemporaryDirectory(prefix="md-nostr-acl-") as home:
            env = {**os.environ, "HOME": home, "XDG_CONFIG_HOME": home,
                   "XDG_DATA_HOME": home, "XDG_CACHE_HOME": home,
                   "PYTHONDONTWRITEBYTECODE": "1"}
            urls = [f"ws://127.0.0.1:{s.server_address[1]}" for s in servers]
            child = subprocess.Popen([sys.argv[1], "--acl-fixture", *urls],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True, env=env)
            for line in child.stdout:
                if line.startswith("CONTROL "):
                    response = control(servers, json.loads(line[8:]))
                    child.stdin.write(json.dumps(response, separators=(",", ":")) + "\n")
                    child.stdin.flush()
                else:
                    print(line, end="", flush=True)
            assert child.wait(timeout=15) == 0
    finally:
        if child is not None and child.poll() is None:
            child.kill()
            child.wait()
        for s in servers:
            s.shutdown()
            s.server_close()
        for t in threads:
            t.join()


if __name__ == "__main__":
    main()
