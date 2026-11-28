#!/usr/bin/env python3
"""Real Darwin client entry point, with no signer, relay, or desktop."""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile

CLIENT = sys.argv[1]
KEY = 'ab' * 32
SIGNER = 'cd' * 32
CONFIG = dict(v=1, address=KEY, pin=KEY, signer=SIGNER, route='127.0.0.1',
              port=7700, caps=1, relays=['ws://127.0.0.1:1'])
FAILED = b'\x2a'
RESOLVE = b'\x21'


def frame(config):
    body = json.dumps(config).encode()
    return struct.pack('!I', len(body)), body


def managed(env, segments, expected, diagnostic, *, eof=False):
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    parent.settimeout(8)
    argv = [CLIENT, '--managed-session-fd', str(child.fileno())]
    with parent, child, subprocess.Popen(argv, pass_fds=(child.fileno(),), env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
        child.close()
        try:
            for part in segments:
                parent.sendall(part)
            if eof:
                parent.shutdown(socket.SHUT_WR)
            observed = bytearray()
            while packet := parent.recv(64):
                observed.extend(packet)
            stdout, stderr = proc.communicate(timeout=8)
            assert proc.returncode == 1, (proc.returncode, stdout, stderr)
            assert observed == expected, (observed, stdout, stderr)
            assert diagnostic in stderr, (stdout, stderr)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=8)


with tempfile.TemporaryDirectory(prefix='md-launch-darwin-') as home:
    env = dict(os.environ, HOME=home, SDL_VIDEODRIVER='no-display-fixture')
    bad = b'invalid private Manager launch'
    managed(env, frame(CONFIG), RESOLVE + FAILED, b'no usable display')
    managed(env, frame(dict(CONFIG, pin=SIGNER)), FAILED, bad)
    managed(env, frame(dict(CONFIG, address='127.0.0.1')), FAILED, bad)
    managed(env, frame(dict(CONFIG, v=2)), FAILED, bad)
    managed(env, (struct.pack('!I', 0xffffffff),), FAILED, bad)
    managed(env, (struct.pack('!I', 128), b'{'), FAILED, bad, eof=True)
    managed(env, (b'\x00',), FAILED, bad, eof=True)
    # Leave the connection open after an incomplete header: the framing
    # watchdog must reject it, not wait forever for a peer that stays alive.
    managed(env, (b'\x00',), FAILED, bad)
    result = subprocess.run([CLIENT, '--managed-session-fd', '0'], check=False,
                            capture_output=True, timeout=8, env=env)
    assert result.returncode == 1 and bad in result.stderr
