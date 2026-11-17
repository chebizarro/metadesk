#!/usr/bin/env python3
# validated on Docker 29.1.3 / rootless Podman 4.9.3, fixture + live GNOME cells; see validation.md.
"""Functional smoke: HTTP inside container, or stdio child after --."""
import argparse
import json
import os
import re
import select
import subprocess
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--expect", required=True, action="append", help="required tree text; repeat for widget labels")
parser.add_argument("--dump-tree", action="store_true", help="print tree evidence (may contain private desktop data)")
parser.add_argument("--http", action="store_true")
parser.add_argument("--calculator-widgets", action="store_true", help="require calculator window, buttons and display text nodes")
parser.add_argument("command", nargs=argparse.REMAINDER)
args = parser.parse_args()
proc = None
if not args.http:
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
stdio_buffer = bytearray()

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
token = open("/run/secrets/mcp-token").read().strip() if args.http else None


def request(method, params=None, ident=1, credential=True):
    message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
    if ident is not None:
        message["id"] = ident
    if args.http:
        headers = {"Content-Type": "application/json"}
        if credential:
            headers["Authorization"] = "Bearer " + token
        req = urllib.request.Request("http://127.0.0.1:7710/mcp", json.dumps(message).encode(), headers)
        with opener.open(req, timeout=15) as response:
            body = response.read()
            return json.loads(body) if body else None
    proc.stdin.write((json.dumps(message) + "\n").encode())
    proc.stdin.flush()
    if ident is None:
        return None
    deadline = time.monotonic() + 40
    while True:
        while b"\n" in stdio_buffer:
            end = stdio_buffer.index(b"\n")
            line = bytes(stdio_buffer[:end])
            del stdio_buffer[:end + 1]
            response = json.loads(line)
            if response.get("id") == ident:
                return response
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([proc.stdout], [], [], remaining)[0]:
            break
        chunk = os.read(proc.stdout.fileno(), 65536)
        if not chunk:
            raise RuntimeError("stdio exited before response")
        stdio_buffer.extend(chunk)
    raise RuntimeError("stdio response deadline exceeded")


def subtree(lines, pattern, label):
    for index, line in enumerate(lines):
        if re.fullmatch(r"\s*" + pattern, line):
            indent = len(line) - len(line.lstrip())
            end = index + 1
            while end < len(lines) and len(lines[end]) - len(lines[end].lstrip()) > indent:
                end += 1
            return lines[index + 1:end]
    raise AssertionError(f"required {label} subtree absent")


try:
    if args.http:
        try:
            request("initialize", credential=False)
            raise AssertionError("missing credential accepted")
        except urllib.error.HTTPError as error:
            assert error.code == 401, error.code
        print("missing bearer: 401")
    initialized = request("initialize", {"protocolVersion": "2025-03-26", "capabilities": {}, "clientInfo": {"name": "container-smoke", "version": "1"}})
    assert "result" in initialized, initialized
    request("notifications/initialized", ident=None)
    tools = request("tools/list", ident=2)
    names = [tool["name"] for tool in tools["result"]["tools"]]
    forbidden = {"metadesk_click", "metadesk_dbl_click", "metadesk_right_click",
                 "metadesk_type", "metadesk_key_combo", "metadesk_scroll",
                 "metadesk_focus", "metadesk_set_value", "metadesk_screenshot"}
    assert forbidden.isdisjoint(names), names
    tree = request("resources/read", {"uri": "metadesk://ui-tree"}, ident=3)
    assert "result" in tree, tree
    rendered = json.dumps(tree["result"])
    for expected in args.expect:
        assert expected in rendered, f"expected tree text absent: {expected}"
    if args.calculator_widgets:
        text = "\n".join(item.get("text", "") for item in tree["result"]["contents"])
        app = subtree(text.splitlines(), r"APP\[[^\]]+\] gnome-calculator", "calculator application")
        window = "\n".join(subtree(app, r"WIN\[[^\]]+\] Calculator", "calculator window"))
        for label in ("0", "7", "+", "="):
            assert re.search(r"BTN\[[^\]]+\] " + re.escape(label) + r"(?:\n|$)", window), f"calculator button absent: {label}"
        assert len(re.findall(r"TXT\[[^\]]+\]", window)) >= 2, "calculator display text nodes absent"
        print("calculator widgets: PASS; window, buttons 0/7/+/= and two display text nodes")
    if args.dump_tree:
        print("tree evidence:", rendered)
    print("initialize/tools/read: PASS; required tree text present; input/capture tools absent")
finally:
    if proc:
        start = time.monotonic()
        proc.terminate()
        proc.wait(timeout=5)
        assert proc.returncode == 0, proc.returncode
        print(f"stdio SIGTERM: exit={proc.returncode} elapsed={time.monotonic() - start:.3f}s")
