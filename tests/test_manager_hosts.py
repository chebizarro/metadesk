#!/usr/bin/env python3
"""GTK cache and key review with throwaway NIP-5F and loopback relay."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

sys.dont_write_bytecode = True
from account_startup_fixture import Peer, PrivateUnixServer, Relay
from test_access_cli import CLIENT_PK, HOST_PK, Signer

class CatchUpPeer(Peer):
    def dispatch(self, msg):
        if msg[0] == 'REQ' and any(f.get('#d') for f in msg[2:]):
            self.server.documents_requested.set()
            # Leave this subscription before EOSE for the catch-up test.
            return
        super().dispatch(msg)


class RoleSigner(PrivateUnixServer):
    @property
    def role(self):
        return self.role_file.read_text()


def account(directory, key):
    directory.mkdir(parents=True, mode=0o700)
    selected = {'backend': 'nip5f', 'pubkey': key}
    metadata = directory / 'management.json'
    metadata.write_text(json.dumps({'version': 1, 'accounts': [dict(selected, label='Fixture Client')],
                                    'roles': {'client': selected, 'host': None}}))
    metadata.chmod(0o600)


binary, crypto = map(os.path.abspath, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="md-hosts-") as tmp:
    root = Path(tmp)
    config, state = root / "config/metadesk", root / "state"
    second = root / 'second'
    account(config, CLIENT_PK)
    account(second, HOST_PK)
    state.mkdir(mode=0o700)
    path = root / 'signer.sock'
    role = root / 'signer-role'
    role.write_text('client')
    with RoleSigner(str(path), Signer) as signer, Relay() as relay, Relay() as catching_up, socket.socket() as unreachable:
        unreachable.bind(("127.0.0.1", 0))
        signer.crypto, signer.role_file = crypto, role
        catching_up.RequestHandlerClass = CatchUpPeer
        catching_up.documents_requested = threading.Event()
        servers = (signer, relay, catching_up)
        threads = [threading.Thread(target=server.serve_forever) for server in servers]
        for thread in threads:
            thread.start()
        try:
            env = dict(os.environ, XDG_CONFIG_HOME=str(config.parent), XDG_STATE_HOME=str(state),
                       NOSTR_SIGNER_SOCK=str(path), MD_FIXTURE_CLIENT=CLIENT_PK,
                       MD_FIXTURE_SECOND_DIRECTORY=str(second), MD_FIXTURE_SIGNER_ROLE=str(role),
                       MD_FIXTURE_RELAY=f"ws://127.0.0.1:{relay.server_address[1]}",
                       MD_FIXTURE_CATCHING_UP=f"ws://127.0.0.1:{catching_up.server_address[1]}",
                       MD_FIXTURE_UNREACHABLE=f"ws://127.0.0.1:{unreachable.getsockname()[1]}")
            subprocess.run([binary], env=env, check=True, timeout=30)
        finally:
            for server in servers:
                server.shutdown()
            for thread in threads:
                thread.join()
        assert catching_up.documents_requested.is_set(), 'saved relay did not receive a scoped document REQ'
        assert relay.eoses >= 4
