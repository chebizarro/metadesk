#!/usr/bin/env python3
"""Exercise the native Hosts model against the shared WebSocket fixture."""
from collections import deque
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


# The bridge's synchronous discovery may use its 10 s history deadline on
# each start. Application history is a separate EOSE barrier. Don't charge
# both starts, their backfill, and the edits against the first pipe watchdog.
STEPS = (
    'offline-review-ready', 'review-bridge-started', 'review-before-eose', 'review-history-ready',
    'edit-bridge-started', 'edit-history-ready',
    'reject-hosts', 'reject-meta', 'event ', 'reject-meta', 'event ',
    'reload-bridge-started', 'event ',
)


class HostsPeer(Peer):
    def send(self, value, opcode=1):
        if (opcode == 1 and value[0] == 'EOSE' and self.server.document_gate
                and any(f.get('#d') for f in self.subs.get(value[1], []))):
            self.server.document_eoses.append((self, value))
            return
        super().send(value, opcode)
        if opcode == 1 and value[0] == 'EOSE':
            self.server.trace.append(('EOSE sent', self.client_address[1], value[1]))

    def dispatch(self, message):
        relay = self.server
        with relay.lock:
            if message[0] == 'REQ':
                relay.filters.extend(message[2:])
                kinds = {kind for f in message[2:] for kind in f.get('kinds', [])}
                relay.trace.append(('REQ received', self.client_address[1], message[1], sorted(kinds)))
                if kinds == {10002, 10050} and relay.silent_phase:
                    # Receive discovery REQs without replying. Document REQs
                    # on the very same connections still receive EOSE.
                    relay.withheld.add(relay.silent_phase)
                    relay.trace.append(('EOSE withheld', relay.silent_phase, self.client_address[1], message[1]))
                    return
            if message[0] == 'EVENT':
                event = message[1]
                relay.writes.append(event)
                if ['d', relay.reject] in event['tags']:
                    relay.reject = None
                    relay.rejected.append(event['id'])
                    self.send(['OK', event['id'], False, 'blocked: fixture'])
                    return
            super().dispatch(message)


def run(binary, *, silent_discovery=False):
    with tempfile.TemporaryDirectory(prefix='md-macos-hosts-') as root, Relay() as relay, socket.socket() as down:
        directory = Path(root).resolve()
        down.bind(('127.0.0.1', 0))  # Held, but not listening: connection refusal.
        relay.RequestHandlerClass = HostsPeer
        relay.reject, relay.rejected, relay.writes, relay.filters = None, [], [], []
        relay.trace = deque(maxlen=32)
        relay.silent_phase = 'review' if silent_discovery else None
        relay.withheld = set()
        relay.document_gate, relay.document_eoses = True, []
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
                    for expected in STEPS:
                        assert select.select([child.stdout], [], [], 15)[0], f'waiting for {expected}'
                        message = child.stdout.readline().strip()
                        matches_step = message.startswith(expected) if expected == 'event ' else message == expected
                        assert matches_step, (expected, message)
                        with relay.lock:
                            if expected == 'event ':
                                event = json.loads(message[len(expected):])
                                relay.events[event['id']] = event
                                for peer in relay.peers:
                                    for sid, filters in peer.subs.items():
                                        if any(matches(event, f) for f in filters):
                                            peer.send(['EVENT', sid, event])
                            elif expected in ('reject-hosts', 'reject-meta'):
                                relay.reject = 'metadesk-hosts' if expected == 'reject-hosts' else 'metadesk:management:v1'
                            elif expected == 'review-before-eose':
                                relay.document_gate = False
                                for peer, eose in relay.document_eoses:
                                    peer.send(eose)
                                relay.document_eoses.clear()
                            elif silent_discovery:
                                # Arm before ACK: the child can't begin the
                                # next discovery until it receives that ACK.
                                if expected == 'review-history-ready':
                                    relay.silent_phase = 'edit'
                                elif expected in ('review-bridge-started', 'edit-bridge-started'):
                                    relay.silent_phase = None
                        child.stdin.write('\n')
                        child.stdin.flush()
                    assert child.wait(timeout=15) == 0, child.stdout.read()
                    print(child.stdout.read().strip())
                except BaseException:
                    with relay.lock:
                        print('Recent relay frames:', *relay.trace, sep='\n', file=sys.stderr)
                    stderr.seek(0)
                    print(stderr.read(), file=sys.stderr)
                    raise
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait(timeout=5)
            assert relay.withheld == ({'review', 'edit'} if silent_discovery else set()), relay.withheld
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
    elif sys.argv[1] == '--qr':
        with tempfile.TemporaryDirectory(prefix='md-macos-qr-') as root:
            home = str(Path(root).resolve())
            env = dict(os.environ, HOME=home, CFFIXED_USER_HOME=home, XDG_CONFIG_HOME=home)
            subprocess.run([sys.argv[2], home], env=env, check=True, timeout=25)
    elif sys.argv[1] == '--bootstrap':
        with tempfile.TemporaryDirectory(prefix='md-macos-bootstrap-') as root:
            home = Path(root).resolve()
            config = home / 'config'
            config.mkdir(mode=0o700)
            env = dict(os.environ, HOME=str(home), CFFIXED_USER_HOME=str(home), XDG_CONFIG_HOME=str(config))
            subprocess.run([sys.argv[2], '--bootstrap', str(config / 'metadesk' / 'manager-relays.ini')],
                           env=env, check=True, timeout=10)
    elif sys.argv[1] == '--silent-discovery':
        run(sys.argv[2], silent_discovery=True)
    else:
        run(sys.argv[1])
