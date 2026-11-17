#!/usr/bin/env python3
"""Local rootless-Podman video experiment; no host-session connections or src edits.

Run after building Dockerfile.video and compiling capture-probe.c externally.
Evidence contains private backend state: do not commit it; delete after review.
"""
import argparse
import json
import re
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image')
p.add_argument('probe', type=Path)
p.add_argument('evidence', type=Path, help='new, private task-owned directory')
p.add_argument('--render-node', type=Path, help='optional explicit render node; never a card node')
a = p.parse_args()
if a.render_node and not str(a.render_node).startswith('/dev/dri/renderD'):
    p.error('only an explicit /dev/dri/renderD* node is accepted')
a.evidence = a.evidence.resolve()
a.evidence.mkdir(mode=0o700, parents=False)
probe = a.probe.resolve(strict=True)
summary = {}
# Never mount the invoking user's runtime, home, session bus, or accessibility bus.
cells = [('pixman', False, 'allow-isolated-output')]
if a.render_node:
    cells += [('render', True, 'allow-isolated-output'), ('deny', True, 'deny')]
for cell, render, consent in cells:
    root = a.evidence / cell
    root.mkdir(mode=0o700)
    for directory in ['run', 'home']:
        (root / directory).mkdir(mode=0o700)
    command = r'''
set -u
cd /evidence
ldd /opt/metadesk/bin/metadesk-host > closure.txt || exit 7
if grep -q 'not found' closure.txt; then exit 7; fi
dpkg-query -W sway libwlroots12t64 xdg-desktop-portal-wlr xdg-desktop-portal pipewire wireplumber libspa-0.2-modules > versions.txt
grep -E 'CapEff|NoNewPrivs' /proc/self/status > security.txt
ls -l /dev/dri >> security.txt 2>/dev/null || :
/opt/capture-probe > fresh.log 2>&1
rc=$?
printf '%s\n' "$rc" > fresh.rc
if [ "$rc" = 0 ]; then
    token="$XDG_STATE_HOME/metadesk/screencast.token"
    if [ -f "$token" ]; then
        stat -c '%a %s' "$token" > token-metadata.txt
        /opt/capture-probe > reuse.log 2>&1
        printf '%s\n' "$?" > reuse.rc
        printf '00000000-0000-4000-8000-000000000008' > "$token"
        /opt/capture-probe > stale.log 2>&1
        printf '%s\n' "$?" > stale.rc
        printf 'invalid-md-ah9-8-token' > "$token"
        /opt/capture-probe > malformed.log 2>&1
        printf '%s\n' "$?" > malformed.rc
        rm -f "$token"
        /opt/capture-probe > cleared.log 2>&1
        printf '%s\n' "$?" > cleared.rc
    fi
fi
exit "$rc"
'''
    name = 'md-ah9-8-' + cell + '-validation'
    # Outside every container bind mount; only this Podman invocation writes it.
    cidfile = a.evidence / (cell + '.cid')
    args = ['podman', 'run', '--init', '--name', name, '--cidfile', str(cidfile),
            '--userns=keep-id', '--read-only',
            '--network=none', '--cap-drop=all', '--security-opt=no-new-privileges',
            '--pids-limit=256', '--memory=768m',
            '--tmpfs', '/tmp:rw,nosuid,nodev,mode=1777',
            '--tmpfs', '/run:rw,nosuid,nodev,mode=1777',
            '--tmpfs', '/home:rw,nosuid,nodev,mode=1777',
            '-v', f'{root}/run:/run/metadesk:rw',
            '-v', f'{root}/home:/home/metadesk:rw',
            '-v', f'{root}:/evidence:rw', '-v', f'{probe}:/opt/capture-probe:ro',
            '-e', f'MD_VIDEO_CONSENT={consent}']
    if render:
        args += ['--device', str(a.render_node), '-e', 'WLR_RENDERER=gles2',
                 '-e', 'WLR_RENDERER_ALLOW_SOFTWARE=1',
                 '-e', f'WLR_RENDER_DRM_DEVICE={a.render_node}',
                 '-e', 'LIBGL_ALWAYS_SOFTWARE=1']
    args += [a.image, 'sh', '-c', command]
    # Names are task-owned; refuse collisions rather than deleting others' work.
    if subprocess.run(['podman', 'container', 'exists', name]).returncode == 0:
        raise SystemExit(f'container already exists: {name}')
    try:
        with (root / 'container.log').open('w') as out:
            try:
                result = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT, timeout=100)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
        if cidfile.exists():
            cid = cidfile.read_text().strip()
            inspection = subprocess.check_output(['podman', 'inspect', cid], text=True)
            (root / 'inspect.json').write_text(inspection)
        summary[cell] = {'container_exit': code}
        for run in ['fresh', 'reuse', 'stale', 'malformed', 'cleared']:
            rc = root / (run + '.rc')
            if rc.exists():
                summary[cell][run] = int(rc.read_text())
        print(cell, json.dumps(summary[cell]), flush=True)
    finally:
        # A name collision/failed create must never remove someone else's run.
        if cidfile.exists():
            cid = cidfile.read_text().strip()
            subprocess.run(['podman', 'rm', '-f', cid], check=True,
                           stdout=subprocess.DEVNULL)
(a.evidence / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
expected = {'pixman': {'container_exit': 6, 'fresh': 6}}
negative = [('pixman', 'fresh')]
if a.render_node:
    expected.update(render={'container_exit': 0, 'fresh': 0, 'reuse': 0, 'stale': 0,
                            'malformed': 6, 'cleared': 0},
                    deny={'container_exit': 6, 'fresh': 6})
    negative += [('render', 'malformed'), ('deny', 'fresh')]
failures = [f'{cell}.{key}: expected {value}, got {summary.get(cell, {}).get(key)}'
            for cell, outcomes in expected.items() for key, value in outcomes.items()
            if summary.get(cell, {}).get(key) != value]
for cell, run in negative:
    log = a.evidence / cell / (run + '.log')
    if not log.exists() or not re.search(r'^frames=0 ', log.read_text(), re.MULTILINE):
        failures.append(f'{cell}.{run}: no zero-frame receipt')
if not a.render_node:
    failures.append('no positive render-node capture cell requested')
(a.evidence / 'gate-failures.json').write_text(json.dumps(failures, indent=2) + '\n')
print('gate_failures', json.dumps(failures), flush=True)
if failures:
    raise SystemExit(1)
