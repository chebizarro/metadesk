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
import threading
import time
sys.dont_write_bytecode = True
from test_access_cli import HOST_NPUB
from account_startup_fixture import Relay, Peer


def env_for(home):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("NOSTR_", "GNOSTR_", "MD_", "SDL_"))
           and k not in ("DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR")}
    env.update(HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"),
               DBUS_SESSION_BUS_ADDRESS="unix:path=" + os.path.join(home, "no-bus"))
    return env


class PendingSessionPeer(Peer):
    def dispatch(self, msg):
        super().dispatch(msg)
        if msg[0] == "EVENT" and msg[1]["kind"] == 1059:
            self.server.request_published.set()


def pending_shutdown(client, env):
    relay = Relay()
    relay.RequestHandlerClass = PendingSessionPeer
    relay.request_published = threading.Event()
    thread = threading.Thread(target=relay.serve_forever, daemon=True)
    thread.start()
    process = subprocess.Popen([client, "--host", "127.0.0.1", "--npub", HOST_NPUB,
                                "--relay", f"ws://127.0.0.1:{relay.server_address[1]}",
                                "--timeout", "180000", "--no-display"],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert relay.request_published.wait(20), "pending request never published"
        start = time.monotonic()
        process.terminate()
        out, err = process.communicate(timeout=5)
        assert process.returncode == 0, (process.returncode, out, err)
        assert "client: done. decoded 0 frames" in out, (out, err)
        assert time.monotonic() - start < 5
        print("PASS: SIGTERM during pending session exits 0 with decode summary in <5s")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
        relay.shutdown()
        relay.server_close()
        thread.join()


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
        for args, diagnostic in (
            (["--managed", "alice@127.0.0.1"], "no relays configured"),
            (["--pin", "f" * 64, HOST_NPUB], "host key does not match the pin"),
            (["alice@127.0.0.1"], "NIP-05 refused an unsafe destination"),
        ):
            result = subprocess.run([client, "--no-display", *args], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 1 and diagnostic in result.stderr, result
            assert "signer ready" not in result.stdout, result
        print("PASS: managed no-relay, pin mismatch and unsafe NIP-05 fail before signaling")
        for secret, options in (
            ("NSeC1fixture-secret", ["--npub", "alice@127.0.0.1"]),
            ("nCRYPTsec1fixture-secret", ["--npub", "alice@127.0.0.1", "--host"]),
            ("nostr:NSEC1fixture-secret", ["--npub"]),
        ):
            result = subprocess.run([client, "--no-display", *options, secret], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 1 and "secret keys are not valid host" in result.stderr, result
            assert not result.stdout and secret not in result.stderr and "NIP-05" not in result.stderr, result
        print("PASS: secret-like host arguments refused without echo or lookup")
        pending_shutdown(client, env)


if __name__ == "__main__":
    main()
