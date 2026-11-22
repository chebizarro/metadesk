#!/usr/bin/env python3
import os, subprocess, sys, threading
sys.dont_write_bytecode=True
from account_startup_fixture import Relay
relay=Relay(); t=threading.Thread(target=relay.serve_forever); t.start()
try:
    p=subprocess.run([os.path.abspath(sys.argv[1]),f"ws://127.0.0.1:{relay.server_address[1]}"],capture_output=True,text=True,timeout=30)
    assert p.returncode==0,(p.stdout,p.stderr)
    events=[e for e in relay.events.values() if e["kind"]==30078]
    assert events and all(e["tags"]==[["d","metadesk-session-preferences"]] for e in events)
    assert all("quality" not in e["content"] and "aaaaaaaa" not in e["content"] for e in events)
    print("PASS: self-encrypted NIP-78 quality roundtrip, second host preserves first, EOSE/OK")
finally:
    relay.shutdown(); relay.server_close(); t.join()
