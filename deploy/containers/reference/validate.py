#!/usr/bin/env python3
# validated on Docker 29.1.3 / rootless Podman 4.9.3, fixture + live GNOME cells; see validation.md.
# Disposable validation driver, not a supervisor.
"""Run from a task directory with mcp-token; leaves no validation containers.
Default: task-owned fixture-session-address. --live: operator's session bus;
never mutate its directory, stop its processes, or simulate bus loss.
"""
import argparse
import json
import os
from pathlib import Path
import re
import select
import subprocess
import time
import uuid

parser = argparse.ArgumentParser()
parser.add_argument("runtime", choices=["docker", "podman"])
parser.add_argument("--live", action="store_true", help="use the operator-managed GNOME bus, no failure simulation")
parser.add_argument("--expect", action="append", help="required tree text; repeat for widget labels")
parser.add_argument("--dump-tree", action="store_true", help="print potentially private tree evidence")
args = parser.parse_args()
rt = ["sudo", "docker"] if args.runtime == "docker" else ["podman"]
image = "metadesk-agent:ah9-4"
root = Path.cwd()
session = "unix:path=/run/user/1000/bus" if args.live else (root / "fixture-session-address").read_text().strip()
env = dict(os.environ, DBUS_SESSION_BUS_ADDRESS=session)
answer = subprocess.check_output(["gdbus", "call", "--session", "--dest", "org.a11y.Bus", "--object-path", "/org/a11y/bus", "--method", "org.a11y.Bus.GetAddress"], env=env, text=True, timeout=10)
atspi = re.search(r"'([^']+)'", answer)[1]
assert session.startswith("unix:path=") and atspi.startswith("unix:path=")
session_path = session.split("=", 1)[1].split(",")[0]
atspi_dir = str(Path(atspi.split("=", 1)[1].split(",")[0]).parent)
common = ["--read-only", "--cap-drop=ALL", "--security-opt=no-new-privileges",
          "--tmpfs", "/tmp:rw,nosuid,nodev,noexec,mode=1777", "--tmpfs"]
if args.runtime == "podman":
    common += ["/run/user/1000:rw,nosuid,nodev,noexec,mode=0700,U", "--userns=keep-id", "--read-only-tmpfs=false"]
else:
    common += ["/run/user/1000:rw,nosuid,nodev,noexec,mode=0700,uid=1000,gid=1000", "--sysctl", "net.ipv6.conf.all.disable_ipv6=1", "--security-opt", "apparmor=metadesk-agent-ah9-4"]
buses = ["--mount", f"type=bind,source={session_path},target=/run/user/1000/bus,readonly",
         "--mount", f"type=bind,source={atspi_dir},target={atspi_dir},readonly"]
secret = ["--mount", f"type=bind,source={root / 'mcp-token'},target=/run/secrets/mcp-token,readonly"]
host = ["/opt/metadesk/bin/metadesk-host", "--mcp-http", "7710", "--mcp-http-bind", "127.0.0.1", "--no-input", "--mcp-token-file", "/run/secrets/mcp-token"]
names = []
prefix = ("ah9-16" if args.live else "ah9-4") + "-" + uuid.uuid4().hex
expected = args.expect or (["gnome-calculator"] if args.live else ["md-ah9.4 expected accessible button"])
smoke = ["python3", "/opt/metadesk/reference/smoke_mcp.py"]
for label in expected:
    smoke += ["--expect", label]
if args.live:
    smoke += ["--calculator-widgets"]
if args.dump_tree:
    smoke += ["--dump-tree"]


def live_guard():
    if not args.live:
        return
    discovered = subprocess.check_output(["gdbus", "call", "--session", "--dest", "org.a11y.Bus", "--object-path", "/org/a11y/bus", "--method", "org.a11y.Bus.GetAddress"], env=env, text=True, timeout=10)
    address = re.search(r"'([^']+)'", discovered)[1]
    assert address.startswith("unix:path="), address
    path = Path(address.split("=", 1)[1].split(",")[0])
    assert path.is_socket(), path
    call = ["gdbus", "call", "--address", address]
    children = subprocess.check_output(call + ["--dest", "org.a11y.atspi.Registry", "--object-path", "/org/a11y/atspi/accessible/root", "--method", "org.a11y.atspi.Accessible.GetChildren"], text=True, timeout=10)
    apps = []
    for dest, obj in re.findall(r"\('([^']+)', (?:objectpath )?'([^']+)'\)", children):
        apps.append(subprocess.check_output(call + ["--dest", dest, "--object-path", obj, "--method", "org.freedesktop.DBus.Properties.Get", "org.a11y.atspi.Accessible", "Name"], text=True, timeout=10).strip())
    assert any("gnome-calculator" in app for app in apps), (children, apps)
    print(f"live guard: socket={path}; apps={apps}", flush=True)


def run(command, expected=0, timeout=60):
    if command[0] == "run" and "--name" not in command:
        # Register before launch: a timed-out runtime CLI can leave its container alive.
        name = prefix + "-" + args.runtime + "-case-" + str(len(names))
        names.append(name)
        command = ["run", "--name", name, *command[1:]]
    try:
        result = subprocess.run(rt + command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    finally:
        if command[0] == "run":
            live_guard()
    assert result.returncode == expected, (command, result.returncode, result.stdout)
    return result.stdout.strip()


def event(name, text):
    proc = subprocess.Popen(rt + ["logs", "-f", name], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    deadline = time.monotonic() + 40
    data = b""
    try:
        while time.monotonic() < deadline:
            if select.select([proc.stdout], [], [], max(0, deadline - time.monotonic()))[0]:
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    break
                data += chunk
                if text.encode() in data:
                    return
        raise AssertionError(data.decode(errors="replace"))
    finally:
        proc.terminate()
        proc.wait(timeout=5)


try:
    print("runtime:", run(["--version"]))
    print("desktop scope:", "live GNOME registry (calculator, not re-registered shell)" if args.live else "isolated task-owned GTK fixture, not live GNOME registry")
    live_guard()
    for label, options, command, expected, message in [
        ("wrong identity", common + buses + ["--user", "1001:1001"], host, 1, "wrong desktop identity"),
        ("missing bus socket", common, ["/opt/metadesk/bin/metadesk-host", "--mcp", "--no-input"], 1, "missing session bus socket"),
        ("missing secret", common + buses, host, 1, "missing /run/secrets/mcp-token"),
        ("no desktop direct host", [], ["/opt/metadesk/bin/metadesk-host", "--mcp", "--no-input"], 1, "accessibility backend unavailable"),
    ]:
        extra = ["--entrypoint", command[0]] if label == "no desktop direct host" else []
        actual = command[1:] if extra else command
        output = run(["run", "--rm", *options, *extra, image, *actual], expected=expected)
        assert message in output, output
        print(label + ": PASS: " + message)
    closure = run(["run", "--rm", "--entrypoint", "ldd", image, "/opt/metadesk/bin/metadesk-host"])
    assert "not found" not in closure, closure
    print("DT_NEEDED closure: PASS:", len(closure.splitlines()), "resolved entries")
    name = prefix + "-" + args.runtime
    names.append(name)
    run(["run", "-d", "--name", name, *common, *buses, *secret, image, *host])
    event(name, "host[mcp]: serving on HTTP")
    print("readiness:", run(["exec", name, "/opt/metadesk/reference/probe.py", "--http"]) or "PASS")
    print(run(["exec", name, *smoke, "--http"]))
    live_guard()
    check = '''import os,socket
from pathlib import Path
status=Path('/proc/1/status').read_text()
assert 'CapEff:\\t0000000000000000' in status
assert 'NoNewPrivs:\\t1' in status
import errno
try:
 Path('/home/metadesk/readonly-check').touch()
 raise AssertionError('root filesystem writable')
except OSError as error:
 assert error.errno==errno.EROFS,error
for device in ['/dev/uinput','/dev/input','/dev/dri']:
 assert not os.path.exists(device),device
ports=[]
for table in ['/proc/net/tcp','/proc/net/tcp6']:
 for line in Path(table).read_text().splitlines()[1:]:
  fields=line.split()
  if fields[3]=='0A':
   ports.append(int(fields[1].split(':')[1],16))
   # libwebsockets may represent the IPv4 bind as IPv4-mapped IPv6 loopback.
   local=fields[1].split(':')[0]
   assert (table,local) in [('/proc/net/tcp','0100007F'),
                          ('/proc/net/tcp6','0000000000000000FFFF00000100007F')],fields[1]
assert ports==[7710],ports
for address in ['127.0.0.1']:
 with socket.create_connection((address,7710),timeout=2): pass
assert 'pipewire' not in Path('/proc/net/unix').read_text().lower()
print('only TCP listener=7710; 7700 absent; zero caps; no devices/PipeWire socket; IPv4 dial PASS')'''
    print(run(["exec", name, "python3", "-c", check]))
    if args.runtime == "docker":
        assert run(["exec", name, "cat", "/proc/sys/net/ipv6/conf/all/disable_ipv6"]) == "1"
        print("Docker namespace IPv6 disabled: PASS")
    inspection = json.loads(run(["inspect", name]))[0]
    assert inspection["HostConfig"]["ReadonlyRootfs"]
    assert not inspection["HostConfig"].get("Devices")
    assert not inspection["HostConfig"].get("Privileged")
    assert inspection["HostConfig"].get("NetworkMode") != "host"
    assert not inspection["HostConfig"].get("PortBindings")
    print("read-only root/no privileged/no device assignments/private netns/no published ports: PASS")
    start = time.monotonic()
    run(["kill", "--signal", "TERM", name])
    code = run(["wait", name], timeout=5)
    elapsed = time.monotonic() - start
    assert code == "0" and elapsed < 5, (code, elapsed)
    print(f"HTTP PID1 SIGTERM: exit={code} elapsed={elapsed:.3f}s")
    live_guard()
    print(run(["run", "--rm", "-i", *common, *buses, "--entrypoint", "python3", image,
               *smoke[1:], "--", "/opt/metadesk/entrypoint.sh", "/opt/metadesk/bin/metadesk-host", "--mcp", "--no-input"]))
    waiting = name + "-wait"
    names.append(waiting)
    # Session discovery succeeds, but its AT-SPI directory is not mounted.
    # Exercise PID 1 during preparation without mutating a desktop socket.
    run(["run", "-d", "--name", waiting, *common, *buses[:2], image])
    event(waiting, "No such file or directory")
    start = time.monotonic()
    run(["kill", "--signal", "TERM", waiting])
    code = run(["wait", waiting], timeout=5)
    elapsed = time.monotonic() - start
    assert code == "143" and elapsed < 5, (code, elapsed)
    print(f"preparation PID1 SIGTERM: exit={code} elapsed={elapsed:.3f}s")
    print("desktop-loss recovery: separate operator-authorized cell; see validation.md")
finally:
    for name in names:
        subprocess.run(rt + ["rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    live_guard()
