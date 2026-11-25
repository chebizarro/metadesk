#!/usr/bin/env python3
"""Drive scoped REQs and both ACK outcomes against real websocket transport."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

from account_startup_fixture import Peer, Relay, matches


class StorePeer(Peer):
    def dispatch(self, frame):
        relay = self.server
        with relay.lock:
            relay.frames.append(frame)
            if frame[0] == "REQ" and any(f.get("#d") == ["metadesk-favorites"] for f in frame[2:]):
                self.send(["CLOSED", frame[1], "restricted: fixture subscription refusal"])
                return
            if frame[0] == "EVENT" and not relay.rejected:
                relay.rejected = frame[1]["id"]
                # The pending event is intentionally echoed before its negative OK.
                for sid, filters in self.subs.items():
                    if any(matches(frame[1], f) for f in filters):
                        self.send(["EVENT", sid, frame[1]])
                self.send(["OK", frame[1]["id"], False, "blocked: fixture review"])
                return
        super().dispatch(frame)


with tempfile.TemporaryDirectory(prefix="md-store-controller-") as directory, Relay() as relay:
    relay.RequestHandlerClass = StorePeer
    relay.frames = []
    relay.rejected = None
    future = json.loads(subprocess.check_output([sys.argv[1], "future-event"], timeout=10))
    relay.events[future["id"]] = future
    thread = threading.Thread(target=relay.serve_forever)
    thread.start()
    try:
        # On Windows the account open must create the directory with an
        # owner-only ACL, not inherit the temporary parent's default ACL.
        state = Path(directory) / "state" if os.name == "nt" else directory
        result = subprocess.run([sys.argv[1], state, f"ws://127.0.0.1:{relay.server_address[1]}"],
                                capture_output=True, text=True, timeout=25)
        assert result.returncode == 0, result.stderr
        assert "future timestamp" in result.stderr, result.stderr
    finally:
        relay.shutdown()
        thread.join()
    assert [f[1]["id"] for f in relay.frames if f[0] == "EVENT"] == [relay.rejected] * 2
    filters = [f for frame in relay.frames if frame[0] == "REQ" for f in frame[2:]]
    for coordinate in ("metadesk-hosts", "metadesk-favorites", "metadesk:management:v1"):
        assert any(f.get("#d") == [coordinate] and len(f.get("authors", [])) == 1 and
                   f.get("limit") == 1 for f in filters), filters
    assert not any(1059 in f.get("kinds", []) for f in filters)
