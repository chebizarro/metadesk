#!/usr/bin/env python3
"""Real host/client, private relay + throwaway NIP-5F keys, local IPC.
No capture, input, FIPS/routing changes, public relay or saved user credentials.
Every step waits on a host/IPC/session transition, never an elapsed-time success.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, PrivateUnixServer
from test_access_cli import Signer, HOST_PK, CLIENT_PK, HOST_NPUB
from direct_ip_session import Process

class Subscription:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(25)
        self.sock.connect(str(path))
        self.sock.sendall(b'{"v":1,"method":"management.subscribe"}\n')
        self.file = self.sock.makefile('rb')
        self.last = None
    def until(self, predicate):
        while True:
            line = self.file.readline(8193)
            assert line and len(line) <= 8192, line
            v = json.loads(line)
            assert v['ok'], v
            if self.last:
                assert v['generation'] == self.last['generation'] and v['revision'] > self.last['revision'], v
            self.last = v
            if predicate(v):
                return v
    def close(self):
        self.file.close()
        self.sock.close()

def main(manager_driver=None):
    host, client, crypto = map(os.path.abspath, sys.argv[1:])
    # Short canonical path fits Darwin's 104-byte sockaddr_un.
    base = '/private/tmp' if sys.platform == 'darwin' else '/tmp'
    with tempfile.TemporaryDirectory(prefix='md-w13-e2e-', dir=base) as tmp:
        root = Path(tmp).resolve()
        relay = Relay()
        servers, threads, children = [relay], [], []
        sub = None
        try:
            envs = {}
            for role, pk in (('host', HOST_PK), ('client', CLIENT_PK)):
                home = root / role
                home.mkdir(mode=0o700)
                env = {k:v for k,v in os.environ.items() if not k.startswith(('NOSTR_', 'GNOSTR_', 'MD_', 'XDG_'))
                       and k not in ('DBUS_SESSION_BUS_ADDRESS', 'DISPLAY', 'WAYLAND_DISPLAY')}
                env.update(HOME=str(home), XDG_CONFIG_HOME=str(home/'config'), NOSTR_SIGNER_SOCK=str(root/(role+'.sock')),
                           DBUS_SESSION_BUS_ADDRESS='unix:path='+str(root/'no-bus'))
                envs[role] = env
                server = PrivateUnixServer(env['NOSTR_SIGNER_SOCK'], Signer)
                server.crypto, server.role = crypto, role
                servers.append(server)
            for server in servers:
                t = threading.Thread(target=server.serve_forever, daemon=True)
                t.start(); threads.append(t)
            def checked(args, role='host', rc=0):
                p = subprocess.run(args, env=envs[role], capture_output=True, text=True, timeout=25)
                assert p.returncode == rc, (args,p.stdout,p.stderr)
                return p
            for role,pk in (('host',HOST_PK),('client',CLIENT_PK)):
                checked([host,'account','add','nip5f',pk,'Throwaway approval'],role)
                checked([host,'account','bind-role',role,'nip5f',pk],role)
            url = f'ws://127.0.0.1:{relay.server_address[1]}'
            with socket.socket() as sock:
                sock.bind(('127.0.0.1',0)); port = str(sock.getsockname()[1])
            path = root/'control.sock'
            def start_host(attended):
                args = [host,'--bind','127.0.0.1','--port',port,'--relay',url,'--no-capture','--no-input',
                        '--management-socket',str(path)] + (['--attended'] if attended else [])
                hp = Process(args,envs['host']); children.append(hp)
                hp.wait('nostr bridge ready, waiting for session requests')
                return hp
            def start_client():
                cp = Process([client,'--host','127.0.0.1','--npub',HOST_NPUB,'--relay',url,'--port',port,
                              '--no-display','--timeout','15000'],envs['client'])
                children.append(cp); return cp
            def command(area,verb,v=None,id=None,extra=(),rc=0):
                args = [host,area,'--socket',str(path),verb]
                if v: args += [id,v['generation'],str(v['revision'])]
                if manager_driver and v and rc == 0:
                    action = 'kick' if verb == 'kick' else 'deny' if verb == 'deny' else 'remember' if '--remember' in extra else 'approve'
                    p = subprocess.run([*manager_driver,str(path),action,v['generation'],str(v['revision']),id],
                                       capture_output=True,text=True,timeout=25)
                    assert p.returncode == 0, (p.stdout,p.stderr)
                    # The UI driver also prints the shared assertion summary.
                    return next(json.loads(line) for line in p.stdout.splitlines() if line.startswith('{'))
                p = checked(args+list(extra),rc=rc)
                return json.loads(p.stdout)
            hp = start_host(True)
            sub = Subscription(path)
            sub.until(lambda v: not v['requests'] and v['session'] is None)
            cp = start_client()
            v = sub.until(lambda v: bool(v['requests']))
            q = v['requests'][0]
            listed = command('requests','list')
            assert listed['requests'][0]['request_id'] == q['request_id']
            approved = command('requests','approve',listed,q['request_id'],['video'])
            assert approved['ok']
            cp.wait('TLS connected (TLSv1.3)'); cp.wait('receiving stream...')
            active = sub.until(lambda v: v['session'] is not None and v['session']['session_id'])
            listed = command('session','list'); sid = listed['session']['session_id']
            assert listed['session']['granted'] == 1
            # A stale decision cannot kick a later revision or substitute a row.
            stale = command('requests','deny',v,q['request_id'],rc=1)
            assert stale['error'] == 'conflict'
            assert checked([host,'access','--relay',url,'list']).stdout.strip() == ''
            assert command('session','kick',listed,sid)['ok']
            cp.wait('connection lost'); cp.stop()
            sub.until(lambda v: v['session'] is None)
            print('PASS: attended pinned TLS 1.3 approve-once (video only), request/session list, stale decision, kick, no remembered grant')
            cp = start_client()
            v = sub.until(lambda v: bool(v['requests']))
            assert command('requests','deny',v,v['requests'][0]['request_id'])['ok']
            cp.p.wait(timeout=20)
            assert cp.p.returncode == 1 and any('host refused session' in line for line in cp.lines), cp.lines
            sub.until(lambda v: not v['requests'])
            print('PASS: attended deny delivered as authenticated correlated refusal')
            cp = start_client()
            v = sub.until(lambda v: bool(v['requests']))
            assert command('requests','approve',v,v['requests'][0]['request_id'],['video','--remember'])['ok']
            cp.wait('TLS connected (TLSv1.3)'); cp.wait('receiving stream...')
            sub.until(lambda v: v['session'] is not None and v['session']['session_id'])
            listed = command('session','list')
            def ipc(command):
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(10); connection.connect(str(path))
                    connection.sendall((json.dumps(command)+'\n').encode())
                    with connection.makefile('rb') as stream:
                        return json.loads(stream.readline(8193))
            policy = ipc({'v':1,'method':'access.list'})
            assert policy['ok'] and policy['policy_available'] and policy['clients'][0]['caps']=='video', policy
            stale_policy = ipc({'v':1,'method':'access.set','generation':listed['generation'],
                                'revision':listed['revision'],'policy_version':policy['policy_version']+1,
                                'peer':CLIENT_PK,'caps':1})
            assert not stale_policy['ok'] and stale_policy['error']=='conflict', stale_policy
            assert checked([host,'access','--relay',url,'list']).stdout.strip() == CLIENT_PK+'\tvideo'
            with relay.lock:
                policies = [e for e in relay.events.values() if e['kind']==30000]
                assert policies and all(e['tags']==[['d','metadesk-allowlist']] and e['content'] for e in policies)
            assert command('session','kick',listed,listed['session']['session_id'])['ok']
            cp.wait('connection lost'); cp.stop()
            sub.until(lambda v: v['session'] is None)
            sub.close(); sub = None
            hp.stop()
            assert not path.exists(), 'host leaked control socket'
            # A fresh allowlist-only host restores private policy and auto-admits.
            hp = start_host(False)
            cp = start_client(); cp.wait('TLS connected (TLSv1.3)'); cp.wait('receiving stream...')
            listed = command('session','list'); assert listed['session']['granted']==1
            assert command('session','kick',listed,listed['session']['session_id'])['ok']
            cp.wait('connection lost'); cp.stop()
            print('PASS: remember acknowledged, private-only persisted grant, fresh-host auto-admission, list then kick')
            if manager_driver:
                cp = start_client(); cp.wait('receiving stream...')
                p = subprocess.run([*manager_driver,str(path),'set'],capture_output=True,text=True,timeout=25)
                assert p.returncode == 0, (p.stdout,p.stderr)
                cp.wait('connection lost'); cp.stop()
                assert checked([host,'access','--relay',url,'list']).stdout.strip() == CLIENT_PK+'\tvideo,agent'
                print('PASS: manager exact grant edit acknowledged and existing session ended')
                cp = start_client(); cp.wait('receiving stream...')
                p = subprocess.run([*manager_driver,str(path),'revoke'],capture_output=True,text=True,timeout=25)
                assert p.returncode == 0, (p.stdout,p.stderr)
                cp.wait('connection lost'); cp.stop()
                assert checked([host,'access','--relay',url,'list']).stdout.strip() == ''
                print('PASS: manager private-policy revoke acknowledged, active session disconnected, remembered grant removed')
        finally:
            if sub: sub.close()
            for p in reversed(children): p.stop()
            for s in servers: s.shutdown(); s.server_close()
            for t in threads: t.join(timeout=10); assert not t.is_alive()
    print('Cleanup: host/client/signers/relay reaped, private accounts and sockets removed; no desktop or routing touched')

if __name__ == '__main__':
    main()
