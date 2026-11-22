#!/usr/bin/env python3
"""Real host/watchdog + real decoder in private Sway sessions (md-atg).

Uses validate-client.py's retaining Nostr relay, signers, account/grant commands
and FIPS-control fixture. This is local TCP video recovery, NOT real FIPS proof.
Prepare --work as for validate-client.py; --video-image must contain matching
host, decoder client, and NIP-46/NIP-5F-enabled library. Never mount a host bus.
Every sequence is counted once; a recovered fault remains a fault, not a clean
backend pass. Raw work/evidence contains keys and tokens: never commit it.
"""
import argparse
import hashlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('video_client', Path(__file__).with_name('validate-client.py'))
vc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vc)


class RecoveryRun(vc.Run):
    def host(self, cell):
        root = self.w / f'run-host-{cell}'
        root.mkdir(mode=0o700)
        # Same private backend: fresh, reuse, unknown UUID, malformed UUID,
        # clear token, THEN launch the unmodified host for the reopened stream.
        script = r'''
set -eu
cd /evidence
token="$XDG_STATE_HOME/metadesk/screencast.token"
rm -f "$token"
for cell in fresh reuse stale malformed; do
    case "$cell" in
        stale) printf '00000000-0000-4000-8000-000000000008' > "$token" ;;
        malformed) printf 'invalid-md-ah9-8-token' > "$token" ;;
    esac
    /opt/capture-probe > "prep-$cell.log" 2>&1 && rc=0 || rc=$?
    printf '%s\n' "$rc" > "prep-$cell.rc"
    if [ "$cell" = malformed ]; then
        [ "$(cat "$token")" = invalid-md-ah9-8-token ] && retained=1 || retained=0
        printf '%s\n' "$retained" > prep-malformed-retained.rc
    fi
done
rm -f "$token"
printf 'cleared_token_host_start=1\n'
( while :; do for c in 204060 a08020; do
    swaymsg -q output HEADLESS-1 bg "#$c" solid_color || exit 0
    sleep 0.1
done; done ) &
exec /opt/metadesk/bin/metadesk-host --bind HOST_ADDRESS --port 7400 --relay RELAY_URL --no-input --no-nvenc --fps 15 --capture-unattended "$@"
'''.replace('HOST_ADDRESS', shlex.quote(self.ids['host']['address'])).replace('RELAY_URL', shlex.quote(self.relay))
        if self.a.fault_injection:
            script = script.replace('exec /opt/metadesk/bin/metadesk-host ',
                                    'exec env LD_PRELOAD=/opt/no-first-frame.so /opt/metadesk/bin/metadesk-host ')
        r = self.a.render_node
        args = ['--network', self.net, '--ip6', self.ids['host']['address'], *vc.SANDBOX,
                '--memory', '768m', '--tmpfs', '/tmp:rw,nosuid,nodev,mode=1777',
                '-v', f'{root}:/run/metadesk:rw', '-v', f'{root}:/evidence:rw',
                '-v', f'{self.w}/home-host:/home/metadesk:rw',
                '-v', f'{self.a.probe}:/opt/capture-probe:ro',
                '-e', 'MD_VIDEO_CONSENT=allow-isolated-output',
                '-e', 'MD_CAPTURE_TRACE_BUFFERS=1',
                '--device', r, '-e', 'WLR_RENDERER=gles2',
                '-e', 'WLR_RENDERER_ALLOW_SOFTWARE=1', '-e', f'WLR_RENDER_DRM_DEVICE={r}',
                '-e', 'LIBGL_ALWAYS_SOFTWARE=1']
        if self.a.fault_injection:
            args += ['-v', f'{self.a.fault_injection}:/opt/no-first-frame.so:ro']
        name, logs = self.start(f'host-{cell}', args, self.a.video_image,
                                ['sh', '-c', script, 'host', *self.bunker('host')])
        if not logs.wait(r'nostr bridge ready, waiting for session requests', 90):
            raise vc.Incomplete('host or sequence preparation did not become ready')
        return name, logs


def classify(host_text, decoded):
    first_restart = host_text.find('restarting capture (attempt ')
    initial_zero = first_restart >= 0 and 'buffer acquire ' not in host_text[:first_restart]
    restarts = len(re.findall(r'restarting capture \(attempt ', host_text))
    rejected = host_text.count('reject buffer:')
    injection = re.search(r'FAULT_INJECTION no-first-frame destroyed dropped=(\d+) passthrough=(\d+)', host_text)
    return {'injection_dropped': int(injection[1]) if injection else 0,
            'injection_passthrough': int(injection[2]) if injection else 0,
            'injection_destroyed': injection is not None,
            'restarts': restarts, 'rejected_descriptors': rejected,
            'initial_zero_frames': initial_zero,
            'recovered': initial_zero and decoded >= 30,
            'clean_stream': decoded >= 30 and not restarts and not rejected,
            'allocation_skeleton': bool(re.search(r'reject buffer: (?:format=\d+ )?type=4294967295 flags=\d+ mapped=0 max=0(?=\s|$)', host_text)),
            'first_frame_deadline': 'PipeWire first-frame deadline exceeded:' in host_text}


def row_failures(row, injected=False):
    failures = []
    if row.get('setup_failure'): failures.append('setup failed')
    if not row.get('preparation_valid'): failures.append('preparation cells failed')
    if not row.get('client_bound') or row['decoded'] < 30: failures.append('no bound 30-frame decode receipt')
    if not row.get('authorization_retained'): failures.append('authorization missing')
    if row.get('client_exit') != 0 or row.get('host_exit') not in (0, 143):
        failures.append('unexpected host/client exit')
    if injected:
        if not (row.get('injection_destroyed') and row.get('injection_dropped', 0) > 0 and
                row.get('first_frame_deadline') and row.get('initial_zero_frames') and
                row.get('recovered') and row.get('restarts') == 1):
            failures.append('incomplete injected deadline/watchdog recovery evidence')
        if row.get('rejected_descriptors'):
            failures.append('natural descriptor fault contaminated injection')
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True, help='new private fixture work directory')
    p.add_argument('--prefix', required=True)
    p.add_argument('--video-image', required=True)
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--render-node', default='/dev/dri/renderD128')
    p.add_argument('--sequences', type=int, default=20)
    p.add_argument('--fault-injection', type=Path, help='explicit test-only no-first-frame.so; NEVER count as natural recovery')
    a = p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', a.prefix) or not 1 <= a.sequences <= 100:
        p.error('invalid prefix or sequence count')
    if not re.fullmatch(r'/dev/dri/renderD[0-9]+', a.render_node):
        p.error('only an explicit /dev/dri/renderD* node is accepted')
    a.probe = a.probe.resolve(strict=True)
    if a.fault_injection:
        a.fault_injection = a.fault_injection.resolve(strict=True)
    w = a.work.resolve(strict=True)
    os.umask(0o077)
    if (w / 'summary.json').exists() or (w / 'keys').exists():
        p.error('work directory has already been used')
    image_tag = a.video_image
    a.video_image = vc.podman('image', 'inspect', image_tag, '--format', '{{.Id}}').stdout.strip()
    a.decode_image = a.video_image
    inputs = {'image_tag': image_tag, 'image': a.video_image, 'sequences': a.sequences,
              'probe_sha256': hashlib.sha256(a.probe.read_bytes()).hexdigest(),
              'fault_injection_sha256': hashlib.sha256(a.fault_injection.read_bytes()).hexdigest() if a.fault_injection else None}
    (w / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    for d in ['keys', 'sock/host', 'sock/client', 'sock/stranger', 'sock/fips', 'home-host/.config']:
        (w / d).mkdir(parents=True)
    ids = {r: vc.identity(w, r) for r in ('host', 'client', 'stranger')}
    run = RecoveryRun(a, w, ids)
    # NIP-01 ephemeral NIP-46 traffic is live-only. Retain the authorization
    # and session history, not bunker RPCs that exhaust the fixture's 256 slots.
    run.fixture_extra = ['--no-store-ephemeral']
    rows, created_net = [], False
    try:
        vc.podman('network', 'create', '--ipv6', '--subnet', vc.SUBNET4,
                  '--subnet', str(ipaddress.IPv6Network(ids['host']['address'] + '/64', strict=False)), run.net)
        created_net = True
        run.start_fixture()
        for i in range(1, a.sequences + 1):
            row = {'sequence': i, 'decoded': 0, 'restarts': 0, 'initial_zero_frames': False, 'recovered': False}
            row['fault_injection'] = 'no-first-frame' if a.fault_injection else None
            rows.append(row)
            before = set(run.created)
            try:
                host, hlogs = run.host(str(i))
                client, clogs = run.client(str(i), 'client', a.video_image, [], [*run.npub_args(), '--no-display'])
                bound = clogs.wait(r'client: receiving stream', 60)
                stats = clogs.wait(r'decoded=(\d+)', 60, lambda m: int(m[1]) >= 30) if bound else None
                row['decoded'] = int(stats[1]) if stats else 0
                row['client_bound'] = bool(bound)
            except vc.Incomplete as error:
                # A new sequence is not a retry of this failed one. Preserve it
                # and continue counting; setup failure is not a backend fault.
                row['setup_failure'] = str(error)
            finally:
                for name in reversed([n for n in run.created if n not in before]):
                    code, _ = run.stop(name)
                    role = 'client' if '-client-' in name else 'host'
                    row[role + '_exit'] = code
                    logs = run.logs[name]
                    text = '\n'.join(logs.lines) + '\n'
                    (w / f'{i}-{role}.log').write_text(text)
                    if role == 'host':
                        row.update(classify(text, row['decoded']))
                    vc.podman('rm', '-f', name)
                    run.created.remove(name)
                    logs.close()
                row['authorization_retained'] = (w / 'home-host/.local/state/metadesk/screencast.token').is_file()
                prep_root = w / f'run-host-{i}'
                row['preparation'] = {cell: int(path.read_text()) if path.exists() else None
                                     for cell in ('fresh', 'reuse', 'stale', 'malformed', 'malformed-retained')
                                     for path in [prep_root / f'prep-{cell}.rc']}
                row['preparation_valid'] = row['preparation'] == {
                    'fresh': 0, 'reuse': 0, 'stale': 0, 'malformed': 3, 'malformed-retained': 1}
                row['gate_failures'] = row_failures(row, bool(a.fault_injection))
                print(json.dumps(row), flush=True)
                (w / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
    finally:
        (w / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
        try:
            if run.fixture in run.created:
                run.retire_fixture()
        finally:
            for name in reversed(run.created):
                vc.podman('rm', '-f', name, check_rc=False)
            for logs in run.logs.values():
                if logs.proc.poll() is None:
                    logs.close()
            if created_net:
                vc.podman('network', 'rm', run.net, check_rc=False)
    if len(rows) != a.sequences or any(row_failures(row, bool(a.fault_injection)) for row in rows):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
