#!/usr/bin/env python3
"""Live launchd/systemd qualification with private signer/relay and no desktop I/O.
Run from the assigned tree: python3 tests/qualify_managed_supervisor.py HOST CRYPTO [APP]
APP is required on macOS; it must be a private bundled runtime outside Documents.
Creates only md-w17 resources and removes them, including on failure.
"""
import json
import os
from pathlib import Path
import plistlib
import select
import signal
import time
import subprocess
import sys
import tempfile
import threading
sys.dont_write_bytecode=True
from account_startup_fixture import Relay,PrivateUnixServer
from test_access_cli import Signer,HOST_PK
from direct_ip_session import Process


def main():
    host,crypto=map(os.path.abspath,sys.argv[1:3])
    mac=sys.platform=='darwin'; base='/private/tmp' if mac else '/tmp'
    with tempfile.TemporaryDirectory(prefix='md-w17-supervisor-',dir=base) as tmp:
        root=Path(tmp).resolve();home=root/'home';home.mkdir(mode=0o700)
        env={k:v for k,v in os.environ.items() if not k.startswith(('MD_','XDG_','NOSTR_','GNOSTR_'))
             and k not in ('DISPLAY','WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS')}
        env.update(HOME=str(home),XDG_CONFIG_HOME=str(home/'config'),NOSTR_SIGNER_SOCK=str(root/'signer.sock'),
                   DBUS_SESSION_BUS_ADDRESS='unix:path='+str(root/'no-bus'))
        def checked(args, **kw):
            p=subprocess.run(args,capture_output=True,text=True,timeout=40,**kw)
            assert p.returncode==0,(args,p.returncode,p.stdout,p.stderr)
            return p.stdout
        relay=Relay();signer=PrivateUnixServer(env['NOSTR_SIGNER_SOCK'],Signer)
        signer.crypto,signer.role=crypto,'host';servers=[relay,signer];threads=[]
        installed=False;observer=None;queue=None;output_file=None
        unit='md-w17-host.service';domain=f'gui/{os.getuid()}'
        label='dev.metadesk.md-w17-host';unit_path=Path.home()/'.config/systemd/user'/unit
        try:
            for s in servers:
                t=threading.Thread(target=s.serve_forever,daemon=True);t.start();threads.append(t)
            for args in (['account','add','nip5f',HOST_PK,'W17 supervisor'],['account','bind-role','host','nip5f',HOST_PK]):
                checked([host]+args,env=env)
            args=[host,'--managed','--no-input','--no-capture','--management-socket',str(root/'host.sock'),
                  '--relay',f'ws://127.0.0.1:{relay.server_address[1]}']
            if mac:
                # Test the shipped wrapper and plist, not a substitute daemon.
                app=Path(sys.argv[3]).resolve();bins=app/'Contents/MacOS'
                source=app/'Contents/Library/LaunchAgents/dev.metadesk.host.plist'
                p=plistlib.loads(source.read_bytes());p['Label']=label
                p['Program']=str(app/p.pop('BundleProgram'))
                p['ProgramArguments']=[p['Program']]+args[1:]
                p['EnvironmentVariables']=env
                output_path=root/'stdout';output_path.touch()
                p['StandardOutPath']=str(output_path)
                p['StandardErrorPath']=str(root/'stderr')
                plist=root/'agent.plist';plist.write_bytes(plistlib.dumps(p))
                # Watch actual log writes before starting launchd. No guessed
                # delay or repeated endpoint probes serves as readiness.
                output_file=open(output_path)
                queue=select.kqueue()
                queue.control([select.kevent(output_file.fileno(),filter=select.KQ_FILTER_VNODE,
                    flags=select.KQ_EV_ADD|select.KQ_EV_CLEAR,fflags=select.KQ_NOTE_WRITE)],0,0)
                lines=[]
                def wait_ready(count):
                    deadline=time.monotonic()+30
                    while sum('nostr bridge ready' in l for l in lines)<count:
                        lines.extend(output_file.readlines())
                        if sum('nostr bridge ready' in l for l in lines)>=count:break
                        left=deadline-time.monotonic()
                        assert left>0 and queue.control(None,1,left),(lines,(root/'stderr').read_text())
                checked(['launchctl','bootstrap',domain,str(plist)]);installed=True
                wait_ready(1)
            else:
                assert not unit_path.exists(),unit_path
                unit_path.parent.mkdir(parents=True,exist_ok=True)
                shipped=(Path(__file__).resolve().parents[1]/'packaging/linux/systemd/metadesk-host.service').read_text()
                # Preserve all production dependencies/restart/stop settings.
                shipped=shipped.replace('ExecStart=/usr/bin/metadesk-host --managed',
                     'ExecStart='+ ' '.join('"'+a.replace('\\','\\\\').replace('"','\\"')+'"' for a in args))
                for k,v in env.items():
                    if k in ('HOME','XDG_CONFIG_HOME','NOSTR_SIGNER_SOCK','DBUS_SESSION_BUS_ADDRESS'):
                        shipped+='\n' if not shipped.endswith('\n') else ''
                        # Environment belongs in Service, not Install.
                        shipped=shipped.replace('[Service]',f'[Service]\nEnvironment="{k}={v}"',1)
                unit_path.write_text(shipped);installed=True
                checked(['systemctl','--user','daemon-reload'])
                observer=Process(['journalctl','--user','-f','-u',unit,'-o','cat','--since','now'],os.environ.copy())
                checked(['systemctl','--user','start',unit]);observer.wait('nostr bridge ready')
            def snapshot():
                v=json.loads(checked([host,'requests','--socket',str(root/'host.sock'),'list'],env=env))
                assert v['host']['managed'] and v['host']['state']=='waiting',v
                return v['generation']
            first=snapshot()
            if mac:
                pid=json.loads(checked([host,'requests','--socket',str(root/'host.sock'),'list'],env=env))['host']['pid']
                process_queue=select.kqueue()
                process_queue.control([select.kevent(pid,filter=select.KQ_FILTER_PROC,
                    flags=select.KQ_EV_ADD|select.KQ_EV_ONESHOT,fflags=select.KQ_NOTE_EXIT)],0,0)
                checked(['launchctl','kill','SIGTERM',domain+'/'+label])
                assert process_queue.control(None,1,30),'host exit watchdog'
                process_queue.close()
                # Wait on the actual host before removing the wrapper job.
                checked(['launchctl','bootout',domain+'/'+label]);installed=False
                assert not (root/'host.sock').exists()

                checked(['launchctl','bootstrap',domain,str(plist)]);installed=True;wait_ready(2)
                second=snapshot();assert second!=first
                crash_pid=json.loads(checked([host,'requests','--socket',str(root/'host.sock'),'list'],env=env))['host']['pid']
                os.kill(crash_pid,signal.SIGKILL)
                wait_ready(3)
                assert snapshot() not in (first,second)
                pid=json.loads(checked([host,'requests','--socket',str(root/'host.sock'),'list'],env=env))['host']['pid']
                process_queue=select.kqueue()
                process_queue.control([select.kevent(pid,filter=select.KQ_FILTER_PROC,
                    flags=select.KQ_EV_ADD|select.KQ_EV_ONESHOT,fflags=select.KQ_NOTE_EXIT)],0,0)
                checked(['launchctl','bootout',domain+'/'+label]);installed=False
                assert process_queue.control(None,1,30),'host exit watchdog'
                process_queue.close()
                print('PASS: LaunchAgent bootstrap, running managed host/IPC, SIGTERM/bootout, bootstrap restart, crash auto-restart/stale-socket recovery, bootout/unregister')
            else:
                checked(['systemctl','--user','restart',unit]);observer.wait('nostr bridge ready',count=2)
                second=snapshot();assert second!=first
                crash_pid=json.loads(checked([host,'requests','--socket',str(root/'host.sock'),'list'],env=env))['host']['pid']
                os.kill(crash_pid,signal.SIGKILL)
                observer.wait('nostr bridge ready',count=3)
                assert snapshot() not in (first,second)
                checked(['systemctl','--user','stop',unit])
                state=checked(['systemctl','--user','show',unit,'-p','ExecMainStatus','-p','ActiveState','-p','Result'])
                assert 'ExecMainStatus=0' in state and 'ActiveState=inactive' in state and 'Result=success' in state,state
                print('PASS: systemd user start/restart/crash-auto-restart/stop; IPC generation changed; '+state.replace('\n',' '))
            assert not (root/'host.sock').exists(),'host endpoint leaked'
        finally:
            if mac and installed:subprocess.run(['launchctl','bootout',domain+'/'+label],capture_output=True,timeout=40)
            if not mac and installed:
                subprocess.run(['systemctl','--user','stop',unit],capture_output=True,timeout=40)
                unit_path.unlink(missing_ok=True)
                checked(['systemctl','--user','daemon-reload'])
                subprocess.run(['systemctl','--user','reset-failed',unit],capture_output=True)
            if observer:observer.stop()
            if queue:queue.close()
            if output_file:output_file.close()
            for s in servers:s.shutdown();s.server_close()
            for t in threads:t.join()
        if mac:
            p=subprocess.run(['launchctl','print',domain+'/'+label],capture_output=True)
            assert p.returncode!=0,'LaunchAgent leaked'
        else:assert not unit_path.exists()
    print('CLEANUP: no W17 unit/agent, socket, fixture credentials, relay or temporary directory remains')

if __name__=='__main__':main()
