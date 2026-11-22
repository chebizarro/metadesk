#!/usr/bin/env python3
"""Managed host exits and real private-IPC startup, without desktop resources."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode = True
from account_startup_fixture import Relay, PrivateUnixServer
from test_access_cli import Signer, HOST_PK
from direct_ip_session import Process


def main():
    host, crypto, lifecycle = map(os.path.abspath, sys.argv[1:])
    base = '/private/tmp' if sys.platform == 'darwin' else '/tmp'
    with tempfile.TemporaryDirectory(prefix='md-w17-', dir=base) as tmp:
        root = Path(tmp).resolve()
        home = root / 'home'; home.mkdir(mode=0o700)
        env = {k:v for k,v in os.environ.items() if not k.startswith(('MD_', 'XDG_', 'NOSTR_', 'GNOSTR_'))
               and k not in ('DBUS_SESSION_BUS_ADDRESS', 'DISPLAY', 'WAYLAND_DISPLAY')}
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(home/'config'),
                   NOSTR_SIGNER_SOCK=str(root/'signer.sock'), DBUS_SESSION_BUS_ADDRESS='unix:path='+str(root/'no-bus'))
        def run(args, expected=0):
            p = subprocess.run(args, env=env, input='', capture_output=True, text=True, timeout=25)
            assert p.returncode == expected, (args,p.returncode,p.stdout,p.stderr)
            return p
        run([host,'--managed','--no-input','--no-capture'],78)
        run([host,'--managed','--mcp-stdio'],78)
        run([lifecycle,'--config'],78); run([lifecycle,'--failure'],1)
        for sig in (signal.SIGTERM, signal.SIGINT):
            p = Process([lifecycle],env)
            try:
                p.wait('lifecycle-ready'); p.p.send_signal(sig)
                assert p.p.wait(timeout=5) == 0, p.lines
            finally: p.stop()
        relay=Relay(); signer=PrivateUnixServer(env['NOSTR_SIGNER_SOCK'],Signer)
        signer.crypto,signer.role=crypto,'host'
        servers=[relay,signer]; threads=[]
        try:
            for s in servers:
                t=threading.Thread(target=s.serve_forever,daemon=True);t.start();threads.append(t)
            run([host,'account','add','nip5f',HOST_PK,'Managed fixture'])
            run([host,'account','bind-role','host','nip5f',HOST_PK])
            path=root/'host.sock'; generations=[]
            for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGKILL, signal.SIGTERM):
                p=Process([host,'--managed','--no-input','--no-capture','--relay',
                           f'ws://127.0.0.1:{relay.server_address[1]}','--management-socket',str(path)],env)
                try:
                    p.wait('nostr bridge ready, waiting for session requests')
                    view=json.loads(run([host,'requests','--socket',str(path),'list']).stdout)
                    assert view['host'].pop('pid')==p.p.pid,view
                    assert view['host']=={'managed':True,'state':'waiting','mode':'user-session'},view
                    assert not view['requests'] and view['session'] is None,view
                    generations.append(view['generation'])
                    p.p.send_signal(sig)
                    assert p.p.wait(timeout=10)==(-signal.SIGKILL if sig==signal.SIGKILL else 0),p.lines
                    assert path.exists()==(sig==signal.SIGKILL), 'unexpected endpoint lifetime'
                finally:p.stop()
            assert len(set(generations))==4, generations
        finally:
            for s in servers:s.shutdown();s.server_close()
            for t in threads:t.join()
    print('PASS: config=78, failure=1, SIGTERM/SIGINT=0; real managed host IPC status and restart/socket cleanup')

if __name__=='__main__':main()
