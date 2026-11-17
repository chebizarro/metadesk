#!/usr/bin/env python3
"""Never connect to the caller's session bus, runtime directory, or keyring."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def inside(executable, root):
    control = str(Path(root) / "run" / "keyring")
    daemon = subprocess.Popen(["gnome-keyring-daemon", "--foreground", "--unlock",
                               "--components=secrets", "--control-directory", control],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        daemon.stdin.write("metadesk-private-test-password\n")
        daemon.stdin.close()
        # The daemon emits its environment after initialization, not a timer.
        line = daemon.stdout.readline().strip()
        if not line.startswith("GNOME_KEYRING_CONTROL="):
            raise RuntimeError("private keyring did not initialize: " + line)
        subprocess.run([executable, "--secret-service"], check=True, timeout=25)
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
        daemon.stdout.close()


if len(sys.argv) == 4 and sys.argv[1] == "--inside":
    inside(sys.argv[2], sys.argv[3])
else:
    with tempfile.TemporaryDirectory(prefix="md-account-keyring-") as directory:
        root = Path(directory).resolve()
        env = os.environ.copy()
        for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE",
                    "GNOME_KEYRING_CONTROL", "GNOME_KEYRING_PID", "SESSION_MANAGER"):
            env.pop(key, None)
        for key, suffix in (("HOME", "home"), ("XDG_RUNTIME_DIR", "run"),
                            ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                            ("XDG_CACHE_HOME", "cache")):
            path = root / suffix
            path.mkdir(mode=0o700)
            env[key] = str(path)
        env["MD_PRIVATE_KEYRING_FIXTURE"] = "1"
        subprocess.run(["dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()),
                        "--inside", sys.argv[1], str(root)], env=env, check=True, timeout=40)
