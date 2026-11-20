#!/usr/bin/env python3
"""Local rootless-Podman video experiment; no host-session connections.

Run after building Dockerfile.video and compiling capture-probe.c externally.
Evidence contains private backend state: do not commit it; delete after review.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('image')
p.add_argument('probe', type=Path)
p.add_argument('evidence', type=Path, help='new, private task-owned directory')
p.add_argument('--render-node', type=Path, help='optional explicit render node; never a card node')
p.add_argument('--library', type=Path, help='explicit matching native/ASan libmetadesk.so.0.1.0')
p.add_argument('--asan-runtime', type=Path, help='explicit matching libasan.so.8 for an ASan probe/library')
p.add_argument('--prefix', default='atg', help='task-owned container name prefix')
p.add_argument('--skip-idle', action='store_true', help='burst diagnostic: skip static idle checks, never pixel/lifetime checks')
p.add_argument('--reopens', type=int, default=6, help='same-context start/stop cycles')
p.add_argument('--pipewire-debug', help='optional upstream PipeWire log level/topic rules')
p.add_argument('--trace-buffers', action='store_true', help='log private PipeWire buffer metadata')
p.add_argument('--max-size', help='md-awgm capture size cap WxH (0 = unbounded side) for every probe')
a = p.parse_args()
if not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_.-]*', a.prefix) or not 1 <= a.reopens <= 100:
    p.error('invalid prefix or reopen count')
if a.max_size and not re.fullmatch(r'\d{1,5}x\d{1,5}', a.max_size):
    p.error('--max-size requires WxH')
if a.render_node and not str(a.render_node).startswith('/dev/dri/renderD'):
    p.error('only an explicit /dev/dri/renderD* node is accepted')
if a.asan_runtime and not a.library:
    p.error('--asan-runtime requires an explicit matching --library')
a.evidence = a.evidence.resolve()
a.evidence.mkdir(mode=0o700, parents=False)
probe = a.probe.resolve(strict=True)
inputs = {'image_tag': a.image, 'image': subprocess.check_output(['podman', 'image', 'inspect', a.image,
                                          '--format', '{{.Id}}'], text=True).strip(),
          'reopens': a.reopens, 'skip_idle': a.skip_idle, 'trace_buffers': a.trace_buffers, 'pipewire_debug': a.pipewire_debug,
          'max_size': a.max_size}
for key, path in [('probe', probe), ('library', a.library), ('asan_runtime', a.asan_runtime)]:
    if path:
        inputs[key + '_sha256'] = hashlib.sha256(path.resolve(strict=True).read_bytes()).hexdigest()
(a.evidence / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
summary = {}
# Never mount the invoking user's runtime, home, session bus, or accessibility bus.
cells = [('pixman', False, 'allow-isolated-output')]
if a.render_node:
    cells += [('render', True, 'allow-isolated-output'), ('deny', True, 'deny'),
              ('timeout', True, 'deny')]
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
        test "$(cat "$token")" = 'invalid-md-ah9-8-token'
        printf '%s\n' "$?" > malformed_retained.rc
        rm -f "$token"
        /opt/capture-probe > cleared.log 2>&1
        printf '%s\n' "$?" > cleared.rc
        MD_CAPTURE_REOPENS="$MD_VIDEO_REOPENS" /opt/capture-probe > reopens.log 2>&1
        printf '%s\n' "$?" > reopens.rc
        MD_CAPTURE_BREAK_PIPEWIRE=1 /opt/capture-probe > broken.log 2>&1
        printf '%s\n' "$?" > broken.rc
    fi
fi
exit "$rc"
'''
    if cell == 'timeout':
        # Private-session chooser stalls longer than the consumer's 500ms budget.
        command = '''
set -eu
MD_CAPTURE_CONSENT_TIMEOUT_MS=500 /opt/capture-probe > /evidence/fresh.log 2>&1 && rc=0 || rc=$?
printf '%s\\n' "$rc" > /evidence/fresh.rc
exit "$rc"
'''
    name = a.prefix + '-' + cell + '-validation'
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
            '-e', f'MD_VIDEO_CONSENT={consent}',
            '-e', f'MD_VIDEO_REOPENS={a.reopens}',
            '-e', f'MD_CAPTURE_TRACE_BUFFERS={int(a.trace_buffers)}']
    if a.skip_idle:
        args += ['-e', 'MD_CAPTURE_SKIP_IDLE=1']
    if a.max_size:
        args += ['-e', f'MD_CAPTURE_MAX_SIZE={a.max_size}']
    if a.library:
        args += ['-v', f'{a.library.resolve(strict=True)}:/opt/metadesk/lib/libmetadesk.so.0:ro']
    if a.asan_runtime:
        args += ['-v', f'{a.asan_runtime.resolve(strict=True)}:/opt/metadesk/lib/libasan.so.8:ro',
                 '-e', 'LD_LIBRARY_PATH=/opt/metadesk/lib',
                 '-e', 'ASAN_OPTIONS=detect_leaks=1:halt_on_error=1']
    if a.pipewire_debug is not None:
        args += ['-e', f'PIPEWIRE_DEBUG={a.pipewire_debug}']
    if cell == 'timeout':
        args += ['-e', 'MD_VIDEO_CHOOSER_WAIT_TEST=1']
    if render:
        args += ['--device', str(a.render_node), '-e', 'WLR_RENDERER=gles2',
                 '-e', 'WLR_RENDERER_ALLOW_SOFTWARE=1',
                 '-e', f'WLR_RENDER_DRM_DEVICE={a.render_node}',
                 '-e', 'LIBGL_ALWAYS_SOFTWARE=1']
    args += [inputs['image'], 'sh', '-c', command]
    # Names are task-owned; refuse collisions rather than deleting others' work.
    if subprocess.run(['podman', 'container', 'exists', name]).returncode == 0:
        raise SystemExit(f'container already exists: {name}')
    try:
        with (root / 'container.log').open('w') as out:
            try:
                result = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT, timeout=180 + a.reopens * 10)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
        if cidfile.exists():
            cid = cidfile.read_text().strip()
            inspection = subprocess.check_output(['podman', 'inspect', cid], text=True)
            (root / 'inspect.json').write_text(inspection)
        summary[cell] = {'container_exit': code}
        for run in ['fresh', 'reuse', 'stale', 'malformed', 'malformed_retained', 'cleared', 'reopens', 'broken']:
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
expected = {'pixman': {'container_exit': 3, 'fresh': 3}}
negative = [('pixman', 'fresh')]
if a.render_node:
    expected.update(render={'container_exit': 0, 'fresh': 0, 'reuse': 0, 'stale': 0,
                            'malformed': 3, 'malformed_retained': 0, 'cleared': 0, 'reopens': 0, 'broken': 0},
                    deny={'container_exit': 3, 'fresh': 3},
                    timeout={'container_exit': 3, 'fresh': 3})
    negative += [('render', 'malformed'), ('deny', 'fresh'), ('timeout', 'fresh')]
failures = [f'{cell}.{key}: expected {value}, got {summary.get(cell, {}).get(key)}'
            for cell, outcomes in expected.items() for key, value in outcomes.items()
            if summary.get(cell, {}).get(key) != value]
for cell, run in negative:
    log = a.evidence / cell / (run + '.log')
    if not log.exists() or not re.search(r'^frames=0 ', log.read_text(), re.MULTILINE):
        failures.append(f'{cell}.{run}: no zero-frame receipt')
if a.render_node:
    for run in ['fresh', 'reuse', 'stale', 'cleared', 'reopens', 'broken']:
        log = a.evidence / 'render' / (run + '.log')
        content = log.read_text() if log.exists() else ''
        cycles = a.reopens if run == 'reopens' else 1
        receipts = re.findall(r'^frames=\d+ timeouts=\d+ matching_pixels=5,5 ', content, re.MULTILINE)
        if len(receipts) != cycles:
            failures.append(f'render.{run}: expected {cycles} changing-pixel receipts, got {len(receipts)}')
        if run != 'broken':
            held = re.findall(r'^held_buffer_intact=1 after_stop=-1 stop_waiting=1$', content, re.MULTILINE)
            if len(held) != cycles:
                failures.append(f'render.{run}: expected {cycles} outstanding-borrow stop receipts, got {len(held)}')
        if 'reject buffer:' in content or 'ERROR: AddressSanitizer' in content or 'ERROR: LeakSanitizer' in content:
            failures.append(f'render.{run}: rejected descriptor or sanitizer diagnostic')
        # The negotiated size, published before frames, is the delivered size.
        published = re.findall(r'^published_size=(\d+x\d+) expected=(\d+x\d+)$', content, re.MULTILINE)
        if len(published) != cycles or any(got != want for got, want in published):
            failures.append(f'render.{run}: published size receipts {published} for {cycles} cycles')
        latency = re.search(r'^latency_us frames=(\d+) mean=(\d+) p50=(\d+) p95=(\d+) max=(\d+)$', content, re.MULTILINE)
        if latency:
            summary['render'].setdefault('latency_us', {})[run] = dict(zip(
                ['frames', 'mean', 'p50', 'p95', 'max'], map(int, latency.groups())))
    print('latency_us', json.dumps(summary['render'].get('latency_us')), flush=True)
    (a.evidence / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    log = a.evidence / 'timeout' / 'fresh.log'
    timing = re.search(r'start_rc=-1 elapsed=([0-9.]+)', log.read_text()) if log.exists() else None
    if not timing or not 0.4 <= float(timing[1]) < 2:
        failures.append('consent timeout did not fail within 0.4..2 seconds')
if not a.render_node:
    failures.append('no positive render-node capture cell requested')
(a.evidence / 'gate-failures.json').write_text(json.dumps(failures, indent=2) + '\n')
print('gate_failures', json.dumps(failures), flush=True)
if failures:
    raise SystemExit(1)
