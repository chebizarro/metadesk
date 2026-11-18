#!/usr/bin/env python3
"""Run only in the disposable input image: two private, device-free Sway seats."""
import glob
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time

class Receiver:
    def __init__(self, env):
        self.lines = []
        self.condition = threading.Condition()
        self.proc = subprocess.Popen(["stdbuf", "-oL", "wev"], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        def read():
            for line in self.proc.stdout:
                with self.condition:
                    self.lines.append(line)
                    self.condition.notify_all()
        self.thread = threading.Thread(target=read, daemon=True)
        self.thread.start()

    def output(self):
        with self.condition:
            return "".join(self.lines)

    def wait_keys(self, count):
        with self.condition:
            if not self.condition.wait_for(
                lambda: len(re.findall(r"wl_keyboard\] key:[^\n]*\n\s+sym:", "".join(self.lines))) >= count,
                timeout=10):
                raise RuntimeError("missing client keyboard events:\n" + "".join(self.lines))

# Do not inherit any desktop, bus, portal, or device endpoint.
for name in ["DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK",
             "DBUS_SESSION_BUS_ADDRESS", "AT_SPI_BUS_ADDRESS", "PIPEWIRE_REMOTE"]:
    os.environ.pop(name, None)
assert not Path("/dev/uinput").exists()
assert not Path("/dev/dri").exists()
assert not Path("/run/user/1000").exists()
processes = []
receivers = []
logs = []
try:
    with tempfile.TemporaryDirectory(prefix="ah9-9-") as root:
        envs = []
        for name in ["target", "control"]:
            runtime = Path(root) / name
            runtime.mkdir(mode=0o700)
            cfg = runtime / "sway.conf"
            cfg.write_text("xwayland disable\noutput HEADLESS-1 mode 1920x1080@60Hz scale 1\n"
                           "seat seat0 fallback true\ndefault_border none\n")
            env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WLR_BACKENDS="headless",
                       WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman", XDG_SESSION_TYPE="wayland")
            log = open(runtime / "sway.log", "w+")
            logs.append(log)
            sway = subprocess.Popen(["sway", "-d", "-c", str(cfg)], env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
            processes.append(sway)
            deadline = time.monotonic() + 15
            # Bounded startup discovery only; injection/completion are event driven.
            while time.monotonic() < deadline:
                sockets = [p for p in glob.glob(str(runtime / "wayland-*")) if not p.endswith(".lock")]
                ipc = glob.glob(str(runtime / "sway-ipc.*.sock"))
                if sockets and ipc:
                    break
                if sway.poll() is not None:
                    log.seek(0)
                    raise RuntimeError(log.read())
                time.sleep(.05)
            else:
                raise RuntimeError("Sway startup timeout")
            env.update(WAYLAND_DISPLAY=sockets[0], SWAYSOCK=ipc[0])
            receiver = Receiver(env)
            receivers.append(receiver)
            processes.append(receiver.proc)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                tree = subprocess.check_output(["swaymsg", "-r", "-t", "get_tree"], env=env, text=True)
                if '"app_id": "wev"' in tree:
                    break
                time.sleep(.05)
            else:
                raise RuntimeError("wev did not map:\n" + receiver.output())
            envs.append(env)

        for index, name in enumerate(["target", "control"]):
            other = receivers[1 - index]
            before = other.output()
            # Poison ambient variables deliberately: the HAL must use only argv[1].
            env = dict(envs[index], WAYLAND_DISPLAY=envs[1-index]["WAYLAND_DISPLAY"],
                       WAYLAND_SOCKET="99999")
            subprocess.run(["/usr/local/bin/ah9-9-input-probe", envs[index]["WAYLAND_DISPLAY"]],
                           env=env, check=True, timeout=15)
            receivers[index].wait_keys(34)  # 15 text keys + Ctrl/A, down and up
            output = receivers[index].output()
            # Client-side evidence, not just successful protocol sends.
            assert "x, y: 100.000000, 150.000000" in output, output
            assert "x, y: 400.000000, 300.000000" in output, output
            for code in [272, 273, 274]:
                assert re.search(r"button: " + str(code) + r" .*state: 1", output), output
                assert re.search(r"button: " + str(code) + r" .*state: 0", output), output
            assert re.search(r"axis: 0 \(vertical\), value: -30\.000000", output), output
            assert re.search(r"axis: 1 \(horizontal\), value: -20\.000000", output), output
            pressed = re.findall(r"wl_keyboard\] key:[^\n]*state: 1 \(pressed\)\n\s+sym: (\S+)", output)
            text_keys = ["a", "h", "9", "minus", "9", "space", "W", "a", "y", "l",
                         "a", "n", "d", "exclam", "Return"]
            assert pressed == text_keys + ["Control_L", "a"], pressed
            released = re.findall(r"wl_keyboard\] key:[^\n]*state: 0 \(released\)\n\s+sym: (\S+)", output)
            assert released == text_keys + ["a", "Control_L"], released
            assert len(re.findall(r"wl_keyboard\] key:", other.output())) == len(
                re.findall(r"wl_keyboard\] key:", before)), "input leaked to other session"
            assert len(re.findall(r"wl_pointer\] (motion|button|axis):", other.output())) == len(
                re.findall(r"wl_pointer\] (motion|button|axis):", before)), "pointer leaked"
            print(f"{name}: motion, three buttons, axes, shifted text, Ctrl/A PASS; other session unchanged")
            print(output)
        print("ISOLATION: no host runtime/bus/display mounts, no uinput, no DRI; two independent receivers PASS")
finally:
    for proc in reversed(processes):
        if proc.poll() is None:
            proc.terminate()
    for proc in reversed(processes):
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    for log in logs:
        log.close()
