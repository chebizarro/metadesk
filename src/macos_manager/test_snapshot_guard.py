#!/usr/bin/env python3
"""The snapshot renderer is confined to a fixture root it creates (md-b9zo B3, R1-R3).

usage: test_snapshot_guard.py RENDERER MESON_BUILD_DIR

1. Refusals: roots it must not use and forged invocations, including a run
   directory with a matching token and a matching environment, an argument
   longer than PATH_MAX, a real root so deep that the run paths would be
   truncated, and a root reached through a symlinked ancestor, exit 2 and
   touch nothing.
2. A successful isolated render into a throwaway meson-style root under
   $TMPDIR (which resolves through /private, so the canonical home-path
   comparison is exercised) must produce exactly the expected artifacts; the
   renderer itself fails on a missing sheet or a scene that does not drain.
   Without a window server the render is skipped and so is this test (77).
Throughout, HOME and XDG_CONFIG_HOME point at a decoy holding a relay
preferences file, and the real runtime directory (/private/tmp/metadesk-UID
and any XDG_RUNTIME_DIR) must be neither created nor modified.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECTED = 95   # 2 appearances x 47 images + menu.txt; see snapshot_pages.m
renderer, build = sys.argv[1], Path(os.path.realpath(sys.argv[2]))


def state(path):
    if not path.exists():
        return None
    st = path.stat()
    return st.st_ino, st.st_mtime_ns, sorted(os.listdir(path)) if path.is_dir() else None


runtime = [Path(f'/private/tmp/metadesk-{os.getuid()}')]
if os.environ.get('XDG_RUNTIME_DIR'):
    runtime.append(Path(os.environ['XDG_RUNTIME_DIR']))
runtime_before = {p: state(p) for p in runtime}
tmp_snapshots = Path('/private/tmp/snapshots')
tmp_existed = tmp_snapshots.exists()

scratch = Path(tempfile.mkdtemp(prefix='snapshot-guard-', dir=build))
fake_root = None
forged = build / 'snapshots' / 'run-forged-by-guard-test'
try:
    home, config = scratch / 'home', scratch / 'config'
    prefs = config / 'metadesk' / 'manager-relays.ini'
    prefs.parent.mkdir(parents=True)
    home.mkdir()
    prefs.write_text('[relays]\nbootstrap=wss://user.example/;\n')
    before = prefs.read_bytes(), prefs.stat().st_mtime_ns
    env = dict(os.environ, HOME=str(home), CFFIXED_USER_HOME=str(home), XDG_CONFIG_HOME=str(config),
               XDG_STATE_HOME=str(scratch / 'state'), XDG_CACHE_HOME=str(scratch / 'cache'))
    link = scratch / 'build-link'
    link.symlink_to(build)
    (build / 'snapshots').mkdir(exist_ok=True)
    runs = set((build / 'snapshots').glob('run-*'))

    # A forged run directory with matching token and environment: there is no
    # entry point that accepts it any more.
    forged.mkdir()
    (forged / '.fixture-token').write_text('token')
    for sub in ('home', 'config', 'state', 'cache', 'runtime', 'out'):
        (forged / sub).mkdir()
    forged_env = dict(env, HOME=str(forged / 'home'), CFFIXED_USER_HOME=str(forged / 'home'),
                      XDG_CONFIG_HOME=str(forged / 'config'), XDG_STATE_HOME=str(forged / 'state'),
                      XDG_CACHE_HOME=str(forged / 'cache'), XDG_RUNTIME_DIR=str(forged / 'runtime'))
    # A root whose run paths would not fit in PATH_MAX. Built with dir_fd so
    # no pathname used here exceeds PATH_MAX either; it holds a Meson marker.
    path_max = os.pathconf('/', 'PC_PATH_MAX')
    deep_fd = os.open(scratch, os.O_RDONLY | os.O_DIRECTORY)
    deep = str(scratch)
    while len(deep) < path_max - 24:
        part = 'd' * min(200, path_max - 24 - len(deep) - 1)
        os.mkdir(part, dir_fd=deep_fd)
        next_fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY, dir_fd=deep_fd)
        os.close(deep_fd); deep_fd = next_fd; deep += '/' + part
    os.mkdir('meson-private', dir_fd=deep_fd)
    marker_dir = os.open('meson-private', os.O_RDONLY | os.O_DIRECTORY, dir_fd=deep_fd)
    os.close(os.open('coredata.dat', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=marker_dir))
    os.close(marker_dir)
    assert len(deep) < path_max, len(deep)
    # A valid-looking root reached through a symlinked ancestor.
    real_parent = scratch / 'real-parent'
    (real_parent / 'root' / 'meson-private').mkdir(parents=True)
    (real_parent / 'root' / 'meson-private' / 'coredata.dat').write_bytes(b'')
    (scratch / 'ancestor-link').symlink_to(real_parent)
    refusals = [
        ([renderer, str(build) + '/' + 'x' * (path_max + 16)], env),
        ([renderer, deep], env),
        ([renderer, str(scratch / 'ancestor-link' / 'root')], env),
        ([renderer], env),
        ([renderer, '/private/tmp'], env),
        ([renderer, str(home)], env),
        ([renderer, str(build / '..' / build.name)], env),
        ([renderer, str(link)], env),
        ([renderer, str(build / 'src')], env),
        ([renderer, str(forged)], forged_env),
        ([renderer, '--fixture-run', str(forged), 'token'], forged_env),
        ([renderer, str(build), 'extra'], env),
    ]
    for case, case_env in refusals:
        result = subprocess.run(case, env=case_env, capture_output=True, timeout=60)
        assert result.returncode == 2, (case, result.returncode, result.stdout, result.stderr)
    assert not any((forged / 'out').iterdir()) and not any((forged / 'home').iterdir()), 'forged run directory was used'
    assert set((build / 'snapshots').glob('run-*')) - runs == {forged}, 'a refused call created a run directory'
    assert sorted(os.listdir(deep_fd)) == ['meson-private'], 'the overlong root was written'
    os.close(deep_fd)
    assert sorted(os.listdir(real_parent / 'root')) == ['meson-private'], 'the root behind a symlinked ancestor was written'

    # A successful isolated render. $TMPDIR resolves through /private, where
    # Foundation reports the home without that prefix.
    fake_root = Path(os.path.realpath(tempfile.mkdtemp(prefix='md-snapshot-root-')))
    (fake_root / 'meson-private').mkdir()
    (fake_root / 'meson-private' / 'coredata.dat').write_bytes(b'')
    result = subprocess.run([renderer, str(fake_root)], env=env, capture_output=True, text=True, timeout=400)
    skipped = result.returncode == 77
    if skipped:
        print('SKIP: no WindowServer session for the isolated render')
    else:
        assert result.returncode == 0, (result.returncode, result.stdout[-2000:], result.stderr[-4000:])
        produced = list((fake_root / 'snapshots').glob('run-*/out'))
        assert len(produced) == 1, produced
        files = sorted(p.name for p in produced[0].iterdir())
        assert len(files) == EXPECTED and 'menu.txt' in files, (len(files), files[:5])
        # R1: discovery is injected, so nothing reached even the fixture runtime directory.
        fixture_runtime = produced[0].parent / 'runtime'
        assert fixture_runtime.is_dir() and not any(fixture_runtime.iterdir()), 'host socket discovery ran during the render'
        for required in ('page-hosts-state-no-signer-account-default-light.png', 'sheet-hosts-key-changed-dark.png',
                         'sheet-access-review-light.png', 'sheet-computer-qr-dark.png', 'page-relays-minimum-light.png'):
            assert required in files, required
        print(f'PASS: isolated render wrote {len(files)} files')

    assert (prefs.read_bytes(), prefs.stat().st_mtime_ns) == before, 'decoy relay preferences changed'
    assert not any(home.iterdir()), 'decoy home was written'
    assert {p: state(p) for p in runtime} == runtime_before, 'the real runtime directory was created or modified'
    assert tmp_existed or not tmp_snapshots.exists(), 'renderer wrote under /private/tmp'
    print('PASS: renderer refused every root it did not create')
    if skipped:
        sys.exit(77)  # the render did not run, so the guard as a whole is skipped
finally:
    shutil.rmtree(scratch, ignore_errors=True)
    shutil.rmtree(forged, ignore_errors=True)
    if fake_root:
        shutil.rmtree(fake_root, ignore_errors=True)
