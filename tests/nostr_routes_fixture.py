#!/usr/bin/env python3
"""Routed bridge against private loopback relays; boundaries are protocol frames."""
import json
import socket
import subprocess
import sys
import threading

from account_startup_fixture import Peer, Relay


class RoutesPeer(Peer):
    def handle(self):
        try:
            super().handle()
        finally:
            self.server.disconnected.set()

    def dispatch(self, msg):
        relay = self.server
        relay.frames.append(msg)
        if msg[0] == "REQ" and relay.role == "silent":
            return  # Upgraded socket, but neither EOSE, CLOSED nor a disconnect.
        if msg[0] == "REQ" and any(1059 in f.get("kinds", []) for f in msg[2:]):
            self.send(["CLOSED", msg[1], "restricted: fixture inbox refusal"])
            return
        if msg[0] == "EVENT" and relay.role == "out":
            # Observe physical retirement while the bridge is still alive
            # and awaiting this EVENT's OK, not after process teardown.
            relay.compacted = all(done.wait(5) for done in relay.retired)
        super().dispatch(msg)


class RoutesRelay(Relay):
    def __init__(self, role):
        super().__init__()
        self.RequestHandlerClass = RoutesPeer
        self.role = role
        self.frames = []
        self.disconnected = threading.Event()
        self.retired = []
        self.compacted = False

    @property
    def url(self):
        return f"ws://127.0.0.1:{self.server_address[1]}"


def run(binary, silent, fallback=False):
    roles = ["bootstrap", "silent", "inbox", "out", "store"]
    relays = [RoutesRelay(role) for role in roles]
    threads = []
    with socket.socket() as closed_port:
        # Keep the address reserved, but do not listen: a failed dial rather
        # than an application-level silent REQ in the second run.
        closed_port.bind(("127.0.0.1", 0))
        try:
            urls = [relay.url for relay in relays]
            if not fallback:
                relays[3].retired = [relays[i].disconnected for i in ([0, 1] if silent else [0])]
            if not silent:
                urls[1] = f"ws://127.0.0.1:{closed_port.getsockname()[1]}"
            # A private URL on the same server is not approved if its scheme
            # or path is different from the operator-configured URL.
            inbox = urls[2].replace("ws://", "wss://") + "/unapproved" if fallback else urls[2]
            result = subprocess.run([binary, "events", inbox, *urls[3:]], check=True,
                                    capture_output=True, text=True, timeout=10)
            for line in result.stdout.splitlines():
                event = json.loads(line)
                relays[0].events[event["id"]] = event
            assert len(relays[0].events) == 3
            for relay in relays:
                thread = threading.Thread(target=relay.serve_forever)
                thread.start()
                threads.append(thread)
            result = subprocess.run([binary, *urls, *(["fallback"] if fallback else [])],
                                    capture_output=True, text=True, timeout=35)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert ("relay-list discovery incomplete" in result.stderr) == silent, result.stderr
            assert relays[3].compacted, result.stderr
            assert ("local DM relay list has no usable targets" in result.stderr) == fallback, result.stderr
            for i, relay in enumerate(relays):
                reqs = [f for frame in relay.frames if frame[0] == "REQ" for f in frame[2:]]
                expected_inbox = i != 1 if fallback else i == 2
                # The C test waits for one terminal response in fallback mode;
                # other relays may not process their REQs before shutdown.
                if not fallback or i == 1:
                    assert any(f.get("kinds") == [1059] for f in reqs) == expected_inbox, relay.frames
                events = [frame[1]["kind"] for frame in relay.frames if frame[0] == "EVENT"]
                assert events == {3: [1059], 4: [1]}.get(i, []), events
            if silent:
                assert any(frame[0] == "REQ" for frame in relays[1].frames), relays[1].frames
            own_authors = {e["pubkey"] for e in relays[0].events.values()}
            assert any(frame[0] == "REQ" and {f["authors"][0] for f in frame[2:]} == own_authors
                       for frame in relays[0].frames)
        finally:
            for relay, thread in zip(relays, threads):
                relay.shutdown()
                thread.join()
            for relay in relays:
                relay.server_close()


if __name__ == "__main__":
    run(sys.argv[1], True)
    run(sys.argv[1], False)
    run(sys.argv[1], False, fallback=True)
