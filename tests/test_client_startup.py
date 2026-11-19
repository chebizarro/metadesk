#!/usr/bin/env python3
"""metadesk-client process contracts without a TTY or display (md-rk4, md-33e).

1. stdout is a pipe: progress lines arrive while the client runs, not at exit.
2. GUI mode with only a headless SDL driver fails closed (exit 1) before it
   connects; it never degrades to an invisible window or decode-only.
3. --no-display stays a working decode-only mode against the same listener.

Every wait reads the client's pipe; deadlines only bound failure.
"""
import os
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import time


def env_for(home):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("NOSTR_", "GNOSTR_", "MD_", "SDL_"))
           and k not in ("DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR")}
    env.update(HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"),
               DBUS_SESSION_BUS_ADDRESS="unix:path=" + os.path.join(home, "no-bus"))
    return env


def read_until(proc, needle, timeout):
    """Lines read from proc.stdout up to and including the first containing needle."""
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    lines, buf, deadline = [], b"", time.monotonic() + timeout
    while True:
        left = deadline - time.monotonic()
        if left <= 0 or not sel.select(left):
            raise AssertionError(f"no {needle!r} line within {timeout}s: {lines} {buf!r}")
        chunk = os.read(proc.stdout.fileno(), 4096)
        if not chunk:
            raise AssertionError(f"stdout closed before {needle!r}: {lines} {buf!r}")
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            lines.append(line.decode(errors="replace"))
            if needle in lines[-1]:
                return lines


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
            r = subprocess.run([client, "127.0.0.1", "--port", port, "--timeout", "5000"],
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

        # Decode-only over a pipe: lines arrive while the process is alive.
        proc = subprocess.Popen([client, "127.0.0.1", "--port", port, "--no-display",
                                 "--timeout", "5000"], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        conn = None
        try:
            lines = read_until(proc, "client: receiving stream", 30)
            assert proc.poll() is None, ("client exited; lines only arrived at exit", lines)
            assert any("display:   disabled" in line for line in lines), lines
            listener.setblocking(True)
            listener.settimeout(10)
            conn = listener.accept()[0]  # it really connected (silent host)
            proc.send_signal(signal.SIGTERM)
            out, err = proc.communicate(timeout=15)
            assert proc.returncode == 0, (proc.returncode, out, err)
            assert b"client: done. decoded 0 frames" in out, out
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.communicate()
            if conn:
                conn.close()
        print("PASS: --no-display progress lines are line-buffered through a pipe; SIGTERM exit 0")


if __name__ == "__main__":
    main()
