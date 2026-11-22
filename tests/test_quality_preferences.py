#!/usr/bin/env python3
import os, subprocess, sys, threading
sys.dont_write_bytecode=True
from account_startup_fixture import Relay, Peer
class Reject(Peer):
    def dispatch(self,msg):
        if msg[0]=="EVENT" and msg[1]["kind"]==30078:
            self.send(["OK",msg[1]["id"],False,"blocked: deliberate preference rejection"])
            return
        super().dispatch(msg)
for reject in (False,True):
    relay=Relay()
    if reject: relay.RequestHandlerClass=Reject
    t=threading.Thread(target=relay.serve_forever); t.start()
    try:
        p=subprocess.run([os.path.abspath(sys.argv[1]),f"ws://127.0.0.1:{relay.server_address[1]}",*(["--reject"] if reject else [])],capture_output=True,text=True,timeout=30)
        assert p.returncode==0,(p.stdout,p.stderr)
        events=[e for e in relay.events.values() if e["kind"]==30078]
        if reject: assert not events
        else:
            assert events and all(e["tags"]==[["d","metadesk-session-preferences"]] for e in events)
            assert all("quality" not in e["content"] and "aaaaaaaa" not in e["content"] for e in events)
    finally:
        relay.shutdown(); relay.server_close(); t.join()
print("PASS: encrypted per-host NIP-78 roundtrip/preservation and negative OK is not success")
