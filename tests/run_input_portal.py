#!/usr/bin/env python3
"""Never use a real desktop bus or its persistent input token in the fixture."""
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="md-w4-portal-") as root:
    env = os.environ.copy()
    for name in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(name, None)
    env.update(XDG_RUNTIME_DIR=root, XDG_STATE_HOME=root, MD_PORTAL_PRIVATE_TEST="1")
    raise SystemExit(subprocess.call(["dbus-run-session", "--", sys.argv[1]], env=env))
