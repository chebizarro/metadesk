#!/usr/bin/env python3
"""Exercise the native Hosts model against the shared WebSocket fixture."""
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tests'))
from account_startup_fixture import Peer, Relay, matches


class HostsPeer(Peer):
    def dispatch(self, message):
        relay = self.server
        with relay.lock:
            if message[0] == 'REQ':
                relay.filters.extend(message[2:])
            if message[0] == 'EVENT':
                event = message[1]
                relay.writes.append(event)
                if ['d', relay.reject] in event['tags']:
                    relay.reject = None
                    relay.rejected.append(event['id'])
                    self.send(['OK', event['id'], False, 'blocked: fixture'])
                    return
            super().dispatch(message)


def run(binary):
    with tempfile.TemporaryDirectory(prefix='md-macos-hosts-') as root, Relay() as relay, socket.socket() as down:
        directory = Path(root).resolve()
        down.bind(('127.0.0.1', 0))  # Held, but not listening: connection refusal.
        relay.RequestHandlerClass = HostsPeer
        relay.reject, relay.rejected, relay.writes, relay.filters = None, [], [], []
        thread = threading.Thread(target=relay.serve_forever)
        thread.start()
        try:
            env = dict(os.environ, HOME=str(directory), CFFIXED_USER_HOME=str(directory))
            with (directory / 'stderr').open('w+') as stderr, subprocess.Popen(
                [binary, str(directory), f'ws://127.0.0.1:{relay.server_address[1]}',
                 f'ws://127.0.0.1:{down.getsockname()[1]}'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=stderr, text=True, env=env
            ) as child:
                try:
                    for expected in ('reject-hosts', 'reject-meta', 'event ', 'reject-meta', 'event ', 'event '):
                        assert select.select([child.stdout], [], [], 15)[0], f'waiting for {expected}'
                        message = child.stdout.readline().strip()
                        assert message.startswith(expected), (expected, message)
                        with relay.lock:
                            if expected == 'event ':
                                event = json.loads(message[len(expected):])
                                relay.events[event['id']] = event
                                for peer in relay.peers:
                                    for sid, filters in peer.subs.items():
                                        if any(matches(event, f) for f in filters):
                                            peer.send(['EVENT', sid, event])
                            else:
                                relay.reject = 'metadesk-hosts' if expected == 'reject-hosts' else 'metadesk:management:v1'
                        child.stdin.write('\n')
                        child.stdin.flush()
                    assert child.wait(timeout=15) == 0, child.stdout.read()
                    print(child.stdout.read().strip())
                except BaseException:
                    stderr.seek(0)
                    print(stderr.read(), file=sys.stderr)
                    raise
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=5)
            assert len(relay.rejected) == 3
            assert sum(event['id'] == relay.rejected[0] for event in relay.writes) == 2
            assert all(f.get('limit') == 1 and len(f.get('authors', [])) == 1
                       for f in relay.filters if any(d.startswith('metadesk') for d in f.get('#d', [])))
        finally:
            relay.shutdown()
            thread.join()


if __name__ == '__main__':
    if sys.argv[1] == '--launch':
        with tempfile.TemporaryDirectory(prefix='md-macos-hosts-launch-') as root:
            subprocess.run([sys.argv[2], str(Path(root).resolve())], check=True, timeout=15)
    elif sys.argv[1] == '--bootstrap':
        with tempfile.TemporaryDirectory(prefix='md-macos-bootstrap-') as root:
            home = Path(root).resolve()
            config = home / 'config'
            config.mkdir(mode=0o700)
            env = dict(os.environ, HOME=str(home), CFFIXED_USER_HOME=str(home), XDG_CONFIG_HOME=str(config))
            subprocess.run([sys.argv[2], '--bootstrap', str(config / 'metadesk' / 'manager-relays.ini')],
                           env=env, check=True, timeout=10)
    else:
        run(sys.argv[1])
