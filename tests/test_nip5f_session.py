#!/usr/bin/env python3
"""md-ebm: real host admission + NIP-17 through a private NIP-5F signer.

The host's decrypted seal is a JSON string with quotes, so the NIP-5F wire
result must be JSON-unescaped before nostrc parses the seal. No live services.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, PrivateUnixServer
from test_access_cli import Signer


def main():
    binding, crypto = map(os.path.abspath, sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="md-nip5f-session-") as home:
        env = {**os.environ, "HOME": home, "XDG_CONFIG_HOME": home,
               "XDG_DATA_HOME": home, "XDG_CACHE_HOME": home}
        relay = Relay()
        path = str(Path(home) / "signer.sock")
        signer = PrivateUnixServer(path, Signer)
        signer.crypto, signer.role = crypto, "client"  # key 2, the host probe
        decrypted = []
        lock = threading.Lock()
        def observe(rpc, value):
            if rpc["method"] == "nip44_decrypt":
                assert isinstance(value, str) and '"' in value
                with lock:
                    decrypted.append(json.loads(value))
        signer.observe_result = observe
        threads = [threading.Thread(target=s.serve_forever, daemon=True)
                   for s in (relay, signer)]
        for t in threads:
            t.start()
        try:
            url = f"ws://127.0.0.1:{relay.server_address[1]}"
            p = subprocess.run([binding, "--nip5f-fixture", url, path], env=env,
                               capture_output=True, text=True, timeout=60)
            sys.stdout.write(p.stdout)
            sys.stderr.write(p.stderr)
            assert p.returncode == 0, p.returncode
            with lock:
                assert {13, 14} <= {e["kind"] for e in decrypted}, decrypted
                seal = next(e for e in decrypted if e["kind"] == 13)
                assert len(seal["sig"]) == 128 and seal["content"]
            with relay.lock:
                assert len([e for e in relay.events.values() if e["kind"] == 1059]) == 2
            print("PASS: NIP-5F host decrypted a quoted seal and negotiated a correlated session/token")
        finally:
            for s in (relay, signer):
                s.shutdown()
                s.server_close()
            for t in threads:
                t.join()


if __name__ == "__main__":
    main()
