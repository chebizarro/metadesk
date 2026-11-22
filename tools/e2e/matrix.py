#!/usr/bin/env python3
"""md-5zi receipt ledger and native probes, not an unattended desktop-input bot."""
import argparse
import ctypes
import datetime as dt
import itertools
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import tempfile
import time

PLATFORMS = ('windows', 'gnome', 'macos', 'omarchy')
TRANSPORTS = ('ip', 'fips')
CHECKS = ('connect_auth', 'video', 'decode_performance', 'overlay', 'human_input',
          'agent', 'reconnect_restore', 'teardown', 'permissions', 'transport')
STATUSES = ('PASS', 'FAIL', 'NOT_RUN', 'BLOCKED')
ROOT = Path(__file__).resolve().parents[2]
OUTPUT = Path(__file__).resolve().parent / 'output'
DEFAULT_LEDGER = str(OUTPUT / 'pairings.json')


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def revision():
    return subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')


def new_matrix(commit):
    return {'schema': 1, 'utc': utc(), 'commit': commit, 'cells': [
        {'host': h, 'client': c, 'transport': t, 'checks': {
            k: {'status': 'NOT_RUN', 'evidence': '', 'defect': ''} for k in CHECKS}}
        for t, h, c in itertools.product(TRANSPORTS, PLATFORMS, PLATFORMS)]}


def overall(cell):
    statuses = [cell['checks'][k]['status'] for k in CHECKS]
    if 'FAIL' in statuses:
        return 'FAIL'
    if 'BLOCKED' in statuses:
        return 'BLOCKED'
    return 'PASS' if all(s == 'PASS' for s in statuses) else 'NOT_RUN'


def record(matrix, host, client, transport, check, status, evidence, defect=''):
    if status not in STATUSES or check not in CHECKS:
        raise ValueError('invalid check or status')
    if not evidence.strip() or (status == 'FAIL' and not defect.strip()):
        raise ValueError('evidence required; FAIL also requires a defect bead')
    cell = next(c for c in matrix['cells'] if (c['host'], c['client'], c['transport']) ==
                (host, client, transport))
    # Preserve history; a subsequent pass must not erase an earlier failure.
    result = cell['checks'][check]
    result.setdefault('history', []).append({k: v for k, v in result.items() if k != 'history'})
    result.update(status=status, evidence=evidence, defect=defect, utc=utc())


def run(logdir, name, argv, env=None, timeout=120):
    started = time.monotonic()
    try:
        p = subprocess.Popen([str(a) for a in argv], env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, start_new_session=True)
    except OSError as e:
        (logdir / (name + '.log')).write_text(f'HARNESS: launch failed: {e}\n')
        return {'name': name, 'argv': [str(a) for a in argv], 'exit_code': None,
                'seconds': time.monotonic() - started, 'log': name + '.log',
                'launch_error': str(e)}
    try:
        output, _ = p.communicate(timeout=timeout)
        code = p.returncode
    except subprocess.TimeoutExpired:
        # This runner is for Unix builds. Kill the owned group, including test children.
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        output, _ = p.communicate()
        code = None
        output += '\nHARNESS: deadline exceeded; owned process group killed/reaped; not a PASS\n'
    except BaseException:
        # Cancellation must not strand a test child or an unattended capture.
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.communicate()
        raise
    (logdir / (name + '.log')).write_text(output)
    return {'name': name, 'argv': [str(a) for a in argv], 'exit_code': code,
            'seconds': time.monotonic() - started, 'log': name + '.log'}


def default_probe_output():
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    return OUTPUT / ('native-' + stamp)


def probe(args):
    if os.name != 'posix':
        raise SystemExit('Use windows-components.ps1 for Windows native probes')
    out = Path(args.out).resolve() if args.out else default_probe_output()
    build = Path(args.build).resolve()
    out.mkdir(parents=True, exist_ok=False)  # never overwrite an older run
    receipt = {'schema': 1, 'scope': 'native components, NOT host/client E2E',
               'utc': utc(), 'commit': revision(), 'platform': platform.platform(), 'runs': []}
    runs = receipt['runs']
    if args.tests:
        runs.append(run(out, 'compile', ['meson', 'compile', '-C', build, '-j', '4'], timeout=600))
        if runs[-1]['exit_code'] == 0:
            env = dict(os.environ)
            for flag in ('MD_INPUT_LIVE_TESTS', 'MD_A11Y_LIVE_TESTS', 'MD_CAPTURE_LIVE_TESTS'):
                env.pop(flag, None)
            runs.append(run(out, 'tests', ['meson', 'test', '-C', build, '--print-errorlogs',
                                          '--num-processes', '4'], env, timeout=600))
    # Resolve /var -> /private/var on macOS: private metadata rejects symlink parents.
    with tempfile.TemporaryDirectory(prefix='md-l7-') as home:
        home = str(Path(home).resolve())
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home, XDG_DATA_HOME=home,
                   XDG_STATE_HOME=home, XDG_RUNTIME_DIR=home)
        for key in ('NOSTR_SIGNER_SOCK', 'DBUS_SESSION_BUS_ADDRESS'):
            env.pop(key, None)
        r = run(out, 'direct-no-authorization', [build / 'metadesk-host', '--no-input',
                    '--no-capture', '--bind', '127.0.0.1', '--relay', 'ws://127.0.0.1:9'], env, 15)
        r['expected_negative_observed'] = (r['exit_code'] == 1 and
            'negotiated stream authorization required; direct TCP disabled' in
            (out / r['log']).read_text())
        runs.append(r)
    receipt['isolated_home_removed'] = not Path(home).exists()
    if platform.system() == 'Darwin':
        cg = ctypes.CDLL('/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics')
        ax = ctypes.CDLL('/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices')
        cg.CGPreflightScreenCaptureAccess.restype = ctypes.c_bool
        ax.AXIsProcessTrusted.restype = ctypes.c_bool
        receipt['permissions'] = {'screen_capture': cg.CGPreflightScreenCaptureAccess(),
                                  'accessibility': ax.AXIsProcessTrusted(), 'prompted': False}
        if args.capture:
            if not receipt['permissions']['screen_capture']:
                receipt['capture'] = 'BLOCKED: grant Screen Recording explicitly; no prompt requested'
            else:
                # No desktop pixels retained. Single-frame native component evidence only.
                runs.append(run(out, 'capture-frame', [build / 'capture_frame', '/dev/null', '', '1280x720']))
    elif args.capture:
        receipt['capture'] = 'NOT_RUN: use platform consent procedure, not unattended live capture'
    write(out / 'receipt.json', receipt)
    print(json.dumps(receipt, indent=2))
    failed = any(r['exit_code'] != 0 for r in runs if r['name'] != 'direct-no-authorization')
    failed |= not next(r for r in runs if r['name'] == 'direct-no-authorization')['expected_negative_observed']
    return int(failed)


def argument_parser():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    init = sub.add_parser('init')
    init.add_argument('file', nargs='?', default=DEFAULT_LEDGER)
    init.add_argument('--commit')
    rec = sub.add_parser('record')
    rec.add_argument('file', nargs='?', default=DEFAULT_LEDGER)
    rec.add_argument('--host', choices=PLATFORMS, required=True)
    rec.add_argument('--client', choices=PLATFORMS, required=True)
    rec.add_argument('--transport', choices=TRANSPORTS, required=True)
    rec.add_argument('--check', choices=CHECKS, required=True)
    rec.add_argument('--status', choices=STATUSES, required=True)
    rec.add_argument('--evidence', required=True)
    rec.add_argument('--defect', default='')
    report = sub.add_parser('report')
    report.add_argument('file', nargs='?', default=DEFAULT_LEDGER)
    pr = sub.add_parser('probe')
    pr.add_argument('--build', default=str(ROOT / 'build'))
    pr.add_argument('--out', help='receipt directory (default: ignored tools/e2e/output/native-UTC)')
    pr.add_argument('--tests', action='store_true')
    pr.add_argument('--capture', action='store_true', help='explicit native macOS screen-capture opt-in')
    return p


def main():
    p = argument_parser()
    args = p.parse_args()
    if args.command == 'probe':
        return probe(args)
    if args.command == 'init':
        if Path(args.file).exists():
            p.error('receipt already exists; choose a new run file')
        write(args.file, new_matrix(args.commit or revision()))
        return 0
    matrix = json.loads(Path(args.file).read_text())
    if args.command == 'record':
        try:
            record(matrix, args.host, args.client, args.transport, args.check,
                   args.status, args.evidence, args.defect)
        except ValueError as e:
            p.error(str(e))
        write(args.file, matrix)
    else:
        print(f"UTC: {matrix['utc']}  Commit: {matrix['commit']}")
        print('| Host | Client | Transport | Overall | Check statuses (documented order) |')
        print('|---|---|---|---|---|')
        for c in matrix['cells']:
            statuses = ', '.join(c['checks'][k]['status'] for k in CHECKS)
            print(f"| {c['host']} | {c['client']} | {c['transport']} | {overall(c)} | {statuses} |")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
