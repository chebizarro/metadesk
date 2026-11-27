#!/usr/bin/env python3
"""Isolated fixture for the native manager's real Unix IPC client and account UI model."""
import json
import os
from pathlib import Path
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

KEY = '79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'
GEN = 'ab' * 16
REQ = 'cd' * 16
NEXT = 'ab' * 16


def request(key=REQ):
    return dict(request_id=key, peer=KEY, requested=7, granted=0, expires_at=int(time.time()) + 120, state='pending')


def active(sid):
    return dict(request(NEXT), session_id=sid, granted=1, state='approved', revoked=False)


class Host(socketserver.UnixStreamServer):
    pass


class Handler(socketserver.StreamRequestHandler):
    def frame(self, data):
        self.wfile.write(json.dumps(data, separators=(',', ':')).encode() + b'\n')

    def handle(self):
        host = self.server
        self.connection.settimeout(15)
        try:
            req = json.loads(self.rfile.readline(8193))
            method = req['method']
            if method == 'management.subscribe':
                with host.lock:
                    assert not host.methods
                    host.methods.append(method)
                    wire = json.dumps(host.snapshot).encode() + b'\n'
                    self.wfile.write(wire[:19])
                    self.wfile.write(wire[19:])
                    host.subscription = self
                assert self.rfile.read(1) == b''
                host.closed.set()
                return
            with host.lock:
                host.methods.append(method)
                if method == 'management.snapshot':
                    assert host.snapshot['revision'] == 3
                    self.frame(host.snapshot)
                elif method == 'host.permissions':
                    self.frame(dict(host.snapshot, permissions=dict(screen_recording=True, accessibility=False)))
                elif method == 'access.list':
                    self.frame(dict(host.snapshot, policy_available=True, policy_version=host.policy,
                                    clients=host.clients))
                    if host.snapshot['revision'] == 1:
                        host.snapshot['revision'] = 3  # A coalesced revision gap.
                        host.subscription.frame(host.snapshot)
                    elif host.snapshot['revision'] == 8:
                        host.subscription.frame(dict(host.snapshot, generation='00' * 16))
                else:
                    assert req['v'] == 1 and req['generation'] == GEN
                    assert req['revision'] == host.snapshot['revision']
                    rev = host.snapshot['revision']
                    host.snapshot['revision'] += 1
                    if method == 'requests.decide' and rev == 3:
                        assert req['request_id'] == REQ and req['caps'] == 1 and req['remember'] is True
                        assert 'policy_version' not in req
                        host.policy = 9
                        host.snapshot['requests'] = [request(NEXT)]
                        self.frame(dict(host.snapshot, ok=False, error='cancelled_or_expired_after_publish'))
                    elif method == 'requests.decide' and rev == 4:
                        assert req['request_id'] == NEXT and req['caps'] == 1 and req['remember'] is False
                        host.snapshot.update(requests=[], session=active('session-one'))
                        self.frame(host.snapshot)
                    elif method == 'session.kick' and rev == 5:
                        assert req['session_id'] == 'session-one' and 'caps' not in req
                        host.snapshot['session'] = None
                        self.frame(host.snapshot)
                    elif method == 'access.set' and rev == 6:
                        assert req['peer'] == KEY and req['caps'] == 0 and req['policy_version'] == 9
                        host.clients[0]['caps'] = 'none'
                        host.policy = 10
                        host.snapshot['session'] = active('session-two')
                        self.frame(host.snapshot)
                    elif method == 'access.remove' and rev == 7:
                        assert req['peer'] == KEY and req['policy_version'] == 10 and 'caps' not in req
                        host.clients = []
                        host.snapshot['session'] = None
                        self.frame(host.snapshot)
                    else:
                        raise AssertionError((method, rev))
        except BaseException as error:
            host.errors.append(error)
            raise


class ThreadingHost(socketserver.ThreadingMixIn, Host):
    daemon_threads = True


with tempfile.TemporaryDirectory(prefix='md-local-', dir='/private/tmp') as root:
    directory = Path(root).resolve()
    env = dict(os.environ, HOME=str(directory), CFFIXED_USER_HOME=str(directory))
    if sys.argv[1] == '--accounts':
        subprocess.run([sys.argv[2], '--accounts'], env=env, check=True, timeout=20)
    elif sys.argv[1] == '--pages':
        env.update(XDG_CONFIG_HOME=str(directory), XDG_STATE_HOME=str(directory), XDG_CACHE_HOME=str(directory))
        # An optional third argument selects a part (e.g. "journeys"); the suite runs everything.
        sys.exit(subprocess.run([sys.argv[2], str(directory / 'accounts'), *sys.argv[3:4]], env=env, timeout=60).returncode)
    else:
        path = directory / 'host.sock'
        with ThreadingHost(str(path), Handler) as host:
            path.chmod(0o600)
            host.methods, host.errors = [], []
            host.lock = threading.Lock()
            host.closed = threading.Event()
            host.snapshot = dict(v=1, ok=True, generation=GEN, revision=1, requests=[request()], session=None, supported=7,
                                 host=dict(managed=True, pid=123, mode='user-session', state='waiting'))
            host.policy = 8
            host.clients = [dict(peer=KEY, caps='video,input', review_required=False)]
            thread = threading.Thread(target=host.serve_forever)
            thread.start()
            try:
                subprocess.run([sys.argv[2], str(path)], env=env, check=True, timeout=20)
                assert host.closed.wait(3)
                assert not host.errors, host.errors
                assert host.methods.count('management.snapshot') == 1
                assert [m for m in host.methods if m in ('requests.decide', 'session.kick', 'access.set', 'access.remove')] == [
                    'requests.decide', 'requests.decide', 'session.kick', 'access.set', 'access.remove']
            finally:
                host.shutdown()
                thread.join()
