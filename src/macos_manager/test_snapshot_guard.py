#!/usr/bin/env python3
"""The snapshot renderer must refuse every root it did not create (md-b9zo B3).

usage: test_snapshot_guard.py RENDERER MESON_BUILD_DIR

Runs the renderer against roots it must refuse and with a forged fixture
invocation, while HOME and XDG_CONFIG_HOME point at a decoy holding a relay
preferences file. Every case must exit 2 without touching the decoy or adding
a run directory. Scratch files live in the build directory, never /private/tmp.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

renderer, build = sys.argv[1], Path(os.path.realpath(sys.argv[2]))
scratch = Path(tempfile.mkdtemp(prefix='snapshot-guard-', dir=build))
try:
    home, config = scratch / 'home', scratch / 'config'
    prefs = config / 'metadesk' / 'manager-relays.ini'
    prefs.parent.mkdir(parents=True)
    home.mkdir()
    prefs.write_text('[relays]\nbootstrap=wss://user.example/;\n')
    before = prefs.read_bytes(), prefs.stat().st_mtime_ns
    env = dict(os.environ, HOME=str(home), CFFIXED_USER_HOME=str(home), XDG_CONFIG_HOME=str(config),
               XDG_STATE_HOME=str(scratch / 'state'), XDG_CACHE_HOME=str(scratch / 'cache'))
    tmp_snapshots = Path('/private/tmp/snapshots')
    tmp_existed = tmp_snapshots.exists()
    link = scratch / 'build-link'
    link.symlink_to(build)
    snapshots = build / 'snapshots'
    runs = set(snapshots.glob('run-*')) if snapshots.is_dir() else set()
    forged = snapshots / 'run-forged-by-guard-test'
    snapshots.mkdir(exist_ok=True)
    forged.mkdir()
    (forged / '.fixture-token').write_text('token')
    for sub in ('home', 'config', 'state', 'cache', 'out'):
        (forged / sub).mkdir()
    cases = [
        [renderer, '/private/tmp'],
        [renderer, str(home)],
        [renderer, str(build / '..' / build.name)],
        [renderer, str(link)],
        [renderer, str(build / 'src')],
        [renderer, '--fixture-run', str(scratch), 'token'],
        [renderer, '--fixture-run', str(forged), 'token'],   # environment is not the run directory's
        [renderer, '--fixture-run', str(forged), 'wrong'],
    ]
    for case in cases:
        result = subprocess.run(case, env=env, capture_output=True, timeout=60)
        assert result.returncode == 2, (case, result.returncode, result.stdout, result.stderr)
    assert (prefs.read_bytes(), prefs.stat().st_mtime_ns) == before, 'decoy relay preferences changed'
    assert not any(home.iterdir()), 'decoy home was written'
    assert not any((forged / 'out').iterdir()), 'forged run directory was rendered into'
    assert set(snapshots.glob('run-*')) - runs == {forged}, 'a refused call created a run directory'
    assert tmp_existed or not tmp_snapshots.exists(), 'renderer wrote under /private/tmp'
    print('PASS: renderer refused every root it did not create')
finally:
    shutil.rmtree(scratch, ignore_errors=True)
    shutil.rmtree(build / 'snapshots' / 'run-forged-by-guard-test', ignore_errors=True)
