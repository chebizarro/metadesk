#!/usr/bin/env python3
"""Signed allowlist history on a private relay; real host stream dispatch, no GUI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
sys.dont_write_bytecode = True
from account_startup_fixture import Relay
from test_access_cli import AccessPeer, CLIENT_PK, HOST_PK


def main():
    binding, crypto = map(os.path.abspath, sys.argv[1:])
    relay = Relay()
    relay.RequestHandlerClass = AccessPeer
    relay.history_seen, relay.history_release = threading.Event(), threading.Event()
    relay.publish_seen, relay.publish_release = threading.Event(), threading.Event()
    relay.history_release.set()
    relay.publish_release.set()
    relay.closed = relay.reject = False
    thread = threading.Thread(target=relay.serve_forever, daemon=True)
    thread.start()
    try:
        url = f"ws://127.0.0.1:{relay.server_address[1]}"
        member = ["p", CLIENT_PK]
        grant = ["metadesk-caps", CLIENT_PK, "v1:video"]
        cases = [
            ("view-only input/action/tree denial", [member, grant], 1),
            ("revoked on next negotiation", [member, [*grant[:2], "v1:none"]], 0),
            ("removed on next negotiation", [], 0),
            ("legacy empty", [member], 0),
            ("legacy opaque extra is not policy", [[*member, "video,agent,input"]], 0),
            ("no membership", [grant], 0),
            ("duplicate grant", [member, grant, grant], 0),
            ("duplicate membership", [member, member, grant], 0),
            ("missing payload", [member, grant[:2]], 0),
            ("trailing payload", [member, [*grant, "ignored?"]], 0),
            ("trailing payload beside a valid duplicate", [member, [*grant, "x"], grant], 0),
            # md-jqnz: shapes on which nostrc's nip51 tag policies disagree.
            # The grant is decoded from raw tags, so none may reject the list.
            ("unknown >3-field tag", [member, grant, ["x", "1", "2", "3"]], 1),
            ("unknown 1-field tag", [["x"], member, grant], 1),
            ("NIP-02 p with relay and petname",
             [[*member, "wss://relay.example", "alice"], grant], 1),
            ("other principal's trailing payload", [member, grant, ["p", HOST_PK],
                                                    ["metadesk-caps", HOST_PK, "v1:video", "x"]], 1),
            ("repeated title and a second d tag",
             [["title", "a"], ["title", "b"], ["d", "other"], member, grant], 1),
        ]
        for text in ("video", "v2:video", "v1:", "v1:all", "v1:video,video",
                     "v1:input,video", "v1:video, agent", "v1:video,", "v1:none,input"):
            cases.append(("malformed " + repr(text), [member, [*grant[:2], text]], 0))
        cases.append(("full grant", [member, [*grant[:2], "v1:video,agent,input"]], 7))
        with tempfile.TemporaryDirectory(prefix="md-ikr-relay-") as tmp:
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith(("NOSTR_", "GNOSTR_", "MD_", "XDG_"))
                   and k not in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY")}
            env.update(HOME=tmp, XDG_CONFIG_HOME=str(Path(tmp) / "config"),
                       DBUS_SESSION_BUS_ADDRESS="unix:path=" + tmp + "/no-bus")
            for offset, (label, tags, expected) in enumerate(cases):
                event = {"kind": 30000, "pubkey": HOST_PK, "created_at": int(time.time()) + offset,
                         "tags": [["d", "metadesk-allowlist"]], "content": ""}
                cipher = subprocess.run([crypto, "host"], input=json.dumps(
                    {"method": "nip44_encrypt", "params": {"peer_pub": HOST_PK, "plaintext": json.dumps(tags)}}),
                    env=env, capture_output=True, text=True, timeout=15, check=True)
                event["content"] = json.loads(cipher.stdout)
                signed = subprocess.run([crypto, "host"], input=json.dumps(
                    {"method": "sign_event", "params": {"event": event}}),
                    env=env, capture_output=True, text=True, timeout=15, check=True)
                event = json.loads(signed.stdout)
                with relay.lock:
                    relay.events = {event["id"]: event}
                result = subprocess.run([binding, "--grant-fixture", url, str(expected)],
                                        env=env, capture_output=True, text=True, timeout=25)
                assert result.returncode == 0, (label, result.stdout, result.stderr)
                print("PASS:", label)
        assert relay.reqs == relay.eoses and relay.reqs >= len(cases)
        print(f"Private relay: {relay.reqs} REQ/EOSE; throwaway keys, no desktop/public relay")
    finally:
        relay.shutdown()
        relay.server_close()
        thread.join()


if __name__ == "__main__":
    main()
