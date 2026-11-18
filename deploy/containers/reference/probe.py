#!/usr/bin/env python3
# validated on Docker 29.1.3 / rootless Podman 4.9.3, fixture + live GNOME + accessibility-loss restart recovery cells; see validation.md.
"""Readiness only: existing D-Bus tooling and /healthz, never GET /mcp."""
import os
import re
import stat
import subprocess
import sys
import urllib.request


def call(address, dest, path, method):
    return subprocess.check_output(
        ["gdbus", "call", "--address", address, "--dest", dest,
         "--object-path", path, "--method", method],
        stderr=subprocess.DEVNULL, timeout=3, text=True)


def socket_address(address, uid):
    match = re.fullmatch(r"unix:path=([^,;%]+)(?:,guid=[0-9a-f]+)?", address)
    if not match:
        raise ValueError("only filesystem Unix bus addresses are supported")
    st = os.stat(match[1])
    if not stat.S_ISSOCK(st.st_mode) or st.st_uid != uid:
        raise ValueError("bus socket type/owner does not match desktop identity")


def desktop():
    uid = os.getuid()
    if uid == 0 or uid != int(os.environ["MD_UID"]):
        raise ValueError("identity must match MD_UID and the desktop owner")
    runtime = os.environ["XDG_RUNTIME_DIR"]
    st = os.stat(runtime)
    if st.st_uid != uid or st.st_mode & 0o777 != 0o700:
        raise ValueError("XDG_RUNTIME_DIR must be owned by the desktop UID, mode 0700")
    session = os.environ["DBUS_SESSION_BUS_ADDRESS"]
    socket_address(session, uid)
    discovered = call(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus.GetAddress")
    match = re.fullmatch(r"\('([^']+)',\)\s*", discovered)
    if not match:
        raise ValueError("invalid org.a11y.Bus.GetAddress reply")
    atspi = match[1]
    socket_address(atspi, uid)
    children = call(atspi, "org.a11y.atspi.Registry", "/org/a11y/atspi/accessible/root",
                    "org.a11y.atspi.Accessible.GetChildren")
    if "/org/a11y/atspi/accessible/" not in children or "@a(so) []" in children:
        raise ValueError("desktop has no registered accessible applications")
    return atspi


if __name__ == "__main__":
    try:
        address = desktop()
        if "--print-address" in sys.argv:
            print(address)
        if "--http" in sys.argv:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open("http://127.0.0.1:7710/healthz", timeout=2) as response:
                if response.status != 200:
                    raise ValueError("HTTP health check failed")
    except (ValueError, OSError, KeyError, subprocess.SubprocessError) as error:
        print("metadesk readiness: " + str(error), file=sys.stderr)
        sys.exit(1)
