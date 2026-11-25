#!/usr/bin/env python3
"""GTK cache and key review with throwaway NIP-5F and loopback relay."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from account_startup_fixture import PrivateUnixServer, Relay
from test_access_cli import CLIENT_PK, Signer

binary, crypto = map(os.path.abspath, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="md-hosts-") as tmp:
    root = Path(tmp)
    config, state = root / "config/metadesk", root / "state"
    config.mkdir(parents=True, mode=0o700)
    state.mkdir(mode=0o700)
    account = {"backend": "nip5f", "pubkey": CLIENT_PK, "label": "Fixture Client"}
    metadata = config / "management.json"
    metadata.write_text(json.dumps({"version": 1, "accounts": [account],
                                   "roles": {"client": {"backend": "nip5f", "pubkey": CLIENT_PK}, "host": None}}))
    metadata.chmod(0o600)
    path = root / "signer.sock"
    with PrivateUnixServer(str(path), Signer) as signer, Relay() as relay:
        signer.crypto, signer.role = crypto, "client"
        servers = (signer, relay)
        threads = [threading.Thread(target=server.serve_forever) for server in servers]
        for thread in threads:
            thread.start()
        try:
            env = dict(os.environ, XDG_CONFIG_HOME=str(config.parent), XDG_STATE_HOME=str(state),
                       NOSTR_SIGNER_SOCK=str(path), MD_FIXTURE_CLIENT=CLIENT_PK,
                       MD_FIXTURE_RELAY=f"ws://127.0.0.1:{relay.server_address[1]}")
            subprocess.run([binary], env=env, check=True, timeout=30)
        finally:
            for server in servers:
                server.shutdown()
            for thread in threads:
                thread.join()
        assert relay.eoses >= 4
