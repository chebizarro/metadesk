#!/usr/bin/env python3
"""Check real Windows video progress reaches a pipe before signer completion.

Use a valid but UNUSED signer transport pubkey on a controlled private relay.
No signer serves that key: its first stderr diagnostic is the completion barrier
for startup printf calls. PeekNamedPipe then observes buffering without sleeps.
The owned no-capture/no-input process is terminated after the observation; this
is a progress regression, not an authentication or graceful-teardown receipt.
"""
import argparse
import ctypes
from ctypes import wintypes
import ipaddress
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import threading
import urllib.parse

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--role', choices=['host', 'client'], required=True)
p.add_argument('--relay', required=True)
p.add_argument('--host-npub', required=True)
p.add_argument('--unused-signer-pubkey', required=True)
p.add_argument('--expect-buffered', action='store_true')
a = p.parse_args()
if os.name != 'nt':
    raise SystemExit(77)
try:
    relay = urllib.parse.urlparse(a.relay)
    address = ipaddress.ip_address(relay.hostname or '')
except ValueError:
    p.error('use a controlled relay with a private/loopback IP literal')
if relay.scheme != 'ws' or not (address.is_private or address.is_loopback):
    p.error('only a controlled private/loopback ws relay is allowed')
if not re.fullmatch('[0-9a-f]{64}', a.unused_signer_pubkey):
    p.error('unused signer transport must be a valid 64-hex public key')
if not a.binary.is_file():
    p.error('binary does not exist')

import msvcrt
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
peek = kernel.PeekNamedPipe
peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                 wintypes.LPVOID, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
peek.restype = wintypes.BOOL
uri = 'bunker://' + a.unused_signer_pubkey + '?relay=' + urllib.parse.quote(a.relay, safe='')
uri += '&secret=md-l7-unused-progress-probe'
command = [str(a.binary.resolve()), '--relay', a.relay, '--bunker', uri]
if a.role == 'host':
    command += ['--no-capture', '--no-input', '--bind', '127.0.0.1', '--port', '17709']
else:
    command += ['--no-display', '--host', '127.0.0.1', '--npub', a.host_npub, '--timeout', '1000']

child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
ready = queue.Queue()


def observe_stderr():
    for line in child.stderr:
        if line.startswith(b'[nip46]'):
            ready.put(True)
            return
    ready.put(False)


reader = threading.Thread(target=observe_stderr, daemon=True)
reader.start()
try:
    if not ready.get(timeout=30):  # Failure guard, never sleep-as-readiness.
        raise RuntimeError('child exited without entering the signer request')
    if child.poll() is not None:
        raise RuntimeError('child must still be waiting for unused signer')
    available = wintypes.DWORD()
    handle = msvcrt.get_osfhandle(child.stdout.fileno())
    if not peek(handle, None, 0, None, ctypes.byref(available), None):
        raise ctypes.WinError(ctypes.get_last_error())
    buffered = available.value == 0
    if buffered != a.expect_buffered:
        raise RuntimeError('unexpected progress buffering: bytes=%d' % available.value)
    print(json.dumps(dict(role=a.role, child_alive=True, stdout_bytes=available.value,
                          observed_buffered=buffered, expected_buffered=a.expect_buffered)))
finally:
    if child.poll() is None:
        child.kill()
    child.wait(timeout=10)
    reader.join(timeout=10)
    child.stdout.close()
    child.stderr.close()
    if reader.is_alive():
        raise RuntimeError('stderr observer did not finish')
