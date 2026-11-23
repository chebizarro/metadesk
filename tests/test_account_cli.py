#!/usr/bin/env python3
"""Headless CLI contract; only throwaway direct keys over stdin."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

host = sys.argv[1]
sk = "0" * 63 + "1"
pk = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"
other = "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
with tempfile.TemporaryDirectory(prefix="md-account-cli-") as root:
    directory = str(Path(root).resolve())

    def run(*args, data=None, success=True):
        result = subprocess.run([host, "account", "--config-dir", directory, *args],
                                input=data, text=True, capture_output=True, timeout=15)
        assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
        assert sk not in result.stdout + result.stderr
        return result

    # Help must describe the real startup contract (docs/accounts.md): bound
    # roles are restored at the next host/client start unless a signer flag
    # overrides them. It once claimed adoption was unimplemented.
    usage = run("--help", success=False).stderr
    assert "not implemented" not in usage, usage
    assert "restored at the next" in usage, usage
    run("list")
    run("add", "direct", pk, "Test")
    run("add", "direct", pk, success=False)
    run("bind-role", "host", "direct", pk, success=False)
    run("bind-role", "host", "direct", pk, "--secret-stdin", "--fips-node", other,
        data=sk, success=False)
    run("bind-role", "host", "direct", pk, "--secret-stdin", "--fips-node", pk, data=sk + "\n")
    run("bind-role", "client", "direct", pk, "--secret-stdin", data=sk)
    run("check-fips-host", pk)
    assert "FIPS hosting refused" in run("check-fips-host", other, success=False).stderr
    assert "client\thost" in run("list").stdout
    run("label", "direct", pk, "Renamed")
    path = Path(directory) / "management.json"
    metadata = path.read_text()
    assert sk not in metadata and "client_secret" not in metadata
    doc = json.loads(metadata)
    assert doc["roles"]["client"] == doc["roles"]["host"]
    assert doc["accounts"][0]["label"] == "Renamed"
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    run("unbind-role", "client")
    run("remove", "direct", pk)
    assert run("list").stdout == ""
    run("check-fips-host", pk, success=False)
    run("add", "bogus", pk, success=False)
    run("add", "nip55l", "bad", success=False)
    run("add", "nip46", pk, "Remote")
    # Refuse credentials for the wrong account before reaching the OS store.
    payload = json.dumps(dict(version=1, user_pubkey=other, remote_pubkey=other,
                              client_secret=sk, relays=["wss://relay.example.test"]))
    run("credentials-stdin", pk, data=payload, success=False)
print("account CLI contract passed")
