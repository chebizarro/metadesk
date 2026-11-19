#!/usr/bin/env python3
"""md-6ns: session correlation over history-retaining private relays.

1. Repeated negotiations on one relay that keeps every gift wrap, so each
   later host/client bridge pair is replayed earlier requests/acceptances.
2. Two relays: one copy of a live request arrives as history, the identical
   copy arrives live from the other relay; it must be answered exactly once.
Throwaway keys only.
"""
import os
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, matches


def replicate(src, dst):
    """Copy src's gift wraps into dst and push them to dst's live subscribers,
    as another relay that received the same publication late would."""
    with src.lock:
        wraps = [e for e in src.events.values() if e.get("kind") == 1059]
    with dst.lock:
        for event in wraps:
            if event["id"] in dst.events:
                continue
            dst.events[event["id"]] = event
            for peer in list(dst.peers):
                for sid, filters in peer.subs.items():
                    if any(matches(event, f) for f in filters):
                        peer.send(["EVENT", sid, event])
    return len(wraps)


def two_relay(binding, env):
    relays = [Relay(), Relay()]
    threads = [threading.Thread(target=r.serve_forever, daemon=True) for r in relays]
    for t in threads:
        t.start()
    try:
        urls = [f"ws://127.0.0.1:{r.server_address[1]}" for r in relays]
        with tempfile.TemporaryFile("w+") as errf:
            p = subprocess.Popen([binding, "--two-relay-fixture", *urls], env=env, text=True,
                                 stdout=subprocess.PIPE, stderr=errf)
            injected = 0
            out = []
            for line in p.stdout:
                out.append(line)
                if line.strip() == "md6ns: inject":
                    injected = replicate(relays[0], relays[1])
            rc = p.wait(timeout=60)
            errf.seek(0)
            err = errf.read()
        sys.stdout.write("".join(out))
        sys.stderr.write(err)
        assert rc == 0, rc
        assert injected == 1, injected
        assert "ignoring session request from relay history" in err
        assert "received session request" in err
        print("PASS: history copy on relay 1, then live copy on relay 2 -> answered once")
    finally:
        for r in relays:
            r.shutdown()
            r.server_close()
        for t in threads:
            t.join()


def main():
    binding = os.path.abspath(sys.argv[1])
    relay = Relay()
    thread = threading.Thread(target=relay.serve_forever, daemon=True)
    thread.start()
    try:
        url = f"ws://127.0.0.1:{relay.server_address[1]}"
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("NOSTR_", "GNOSTR_", "MD_"))}
        p = subprocess.run([binding, "--history-fixture", url], env=env,
                           capture_output=True, text=True, timeout=150)
        sys.stdout.write(p.stdout)
        sys.stderr.write(p.stderr)
        assert p.returncode == 0, p.returncode
        with relay.lock:
            wraps = [e for e in relay.events.values() if e.get("kind") == 1059]
        # 3 requests + 3 acceptances + 2 decoys, all retained as history.
        assert len(wraps) == 8, len(wraps)
        for needle in ("ignoring session request from relay history",):
            assert needle in p.stderr, needle
        print(f"PASS: 3 negotiations on one retaining relay ({len(wraps)} gift wraps retained, "
              f"{relay.reqs} REQ, {relay.eoses} EOSE, {relay.oks} OK)")
    finally:
        relay.shutdown()
        relay.server_close()
        thread.join()
    two_relay(binding, env)


if __name__ == "__main__":
    main()
