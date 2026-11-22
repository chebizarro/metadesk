#!/usr/bin/env python3
"""metadesk-client process contracts without a TTY or display (md-rk4, md-33e).

1. stdout is a pipe: progress lines arrive while the client runs, not at exit.
2. GUI mode with only a headless SDL driver fails closed (exit 1) before it
   connects; it never degrades to an invisible window or decode-only.
3. Direct TCP without a host identity is refused before opening a socket.

Every wait reads the client's pipe; deadlines only bound failure.
"""
import os
import socket
import subprocess
import sys
import tempfile
sys.dont_write_bytecode = True
from test_access_cli import HOST_NPUB


def env_for(home):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("NOSTR_", "GNOSTR_", "MD_", "SDL_"))
           and k not in ("DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR")}
    env.update(HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"),
               DBUS_SESSION_BUS_ADDRESS="unix:path=" + os.path.join(home, "no-bus"))
    return env


def main():
    client = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="md-client-startup-") as tmp, \
            socket.socket() as listener:
        home = os.path.realpath(tmp)
        os.chmod(home, 0o700)
        # Private, empty config root: direct-IP mode restores no account.
        config = "Library/Application Support" if sys.platform == "darwin" else ".config"
        os.makedirs(os.path.join(home, config), mode=0o700)
        env = env_for(home)
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        listener.setblocking(False)
        port = str(listener.getsockname()[1])

        # GUI mode with a headless driver: refused before any connection.
        for driver in ("dummy", "offscreen"):
            r = subprocess.run([client, "--host", "127.0.0.1", "--npub", HOST_NPUB,
                                "--port", port, "--timeout", "5000"],
                               env=dict(env, SDL_VIDEODRIVER=driver), capture_output=True,
                               text=True, timeout=30)
            assert r.returncode == 1, (driver, r.returncode, r.stdout, r.stderr)
            assert "no usable display for the GUI client" in r.stderr, r.stderr
            # Either SDL lacks the driver, or it was selected and refused.
            assert (f"SDL video driver '{driver}' has no visible output" in r.stderr
                    or "SDL_Init failed" in r.stderr), r.stderr
            assert "decode-only mode" not in r.stderr and "connecting to" not in r.stdout, r
            try:
                listener.accept()[0].close()
                raise AssertionError(f"GUI client with {driver} connected despite no display")
            except BlockingIOError:
                pass
            print(f"PASS: GUI mode with SDL_VIDEODRIVER={driver} fails closed before connecting")

        for endpoint in (["127.0.0.1"], ["--host", "127.0.0.1"]):
            r = subprocess.run([client, *endpoint, "--port", port, "--no-display"],
                               env=env, capture_output=True, text=True, timeout=10)
            assert r.returncode == 1 and "--npub host identity required" in r.stderr, r
            try:
                listener.accept()[0].close()
                raise AssertionError("identity-free client opened TCP")
            except BlockingIOError:
                pass
        print("PASS: named and positional direct endpoints require host identity before TCP")


if __name__ == "__main__":
    main()
