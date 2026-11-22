#!/usr/bin/env python3
"""Stage trusted, installed Linux ELFs. Native keeps distro DSOs; OCI closes them.

Build first with build.sh. Never execute ldd against untrusted input. Outputs must
be new directories outside source/build trees. No source/config/state is copied.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import shlex
import subprocess


def run(*args, **kw):
    return subprocess.check_output(args, text=True, **kw).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elfs(root):
    result = []
    for path in root.rglob('*'):
        if path.is_file() and not path.is_symlink():
            with path.open('rb') as stream:
                if stream.read(4) == b'\x7fELF':
                    result.append(path)
    return result


def revision(root, marker):
    if (root / '.git').exists():
        return run('git', '-C', str(root), 'rev-parse', 'HEAD')
    value = (root / marker).read_text().strip()
    if not re.fullmatch(r'[a-f0-9]{40}', value):
        raise ValueError(f'{marker} must contain a full git revision')
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['native', 'oci'])
    p.add_argument('source', type=Path)
    p.add_argument('build', type=Path)
    p.add_argument('nostrc', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--keep-debug', action='store_true', help='native RPM: let rpmbuild split and strip debug symbols')
    p.add_argument('--base', help='OCI matching-architecture ubuntu@sha256 digest')
    p.add_argument('--ffmpeg-prefix', type=Path)
    p.add_argument('--legal', type=Path, help='curated dependency licenses + source/relink offer tree')
    a = p.parse_args()
    src, build, nostrc, out = [x.resolve() for x in (a.source, a.build, a.nostrc, a.output)]
    if out.exists() or any(out == r or r in out.parents for r in (src, build, nostrc)):
        p.error('output must be new and outside input trees')
    if platform.system() != 'Linux':
        p.error('Linux only')
    source_rev = revision(src, '.metadesk-commit')
    nostrc_rev = revision(nostrc, '.nostrc-commit')
    if a.mode == 'oci' and a.keep_debug:
        p.error('--keep-debug is native-only')
    if a.mode == 'native' and a.ffmpeg_prefix:
        p.error('native packages require system FFmpeg, not --ffmpeg-prefix')
    if a.mode == 'oci':
        os_release = dict(line.strip().split('=', 1) for line in Path('/etc/os-release').read_text().splitlines() if '=' in line)
        if os_release.get('ID', '').strip('"') != 'ubuntu' or os_release.get('VERSION_ID', '').strip('"') != '24.04':
            p.error('OCI producer requires Ubuntu 24.04')
        if not a.base or not re.fullmatch(r'(docker.io/library/)?ubuntu@sha256:[0-9a-f]{64}', a.base):
            p.error('OCI base must be Ubuntu pinned by digest')
        if not a.ffmpeg_prefix or not a.legal or not a.legal.is_dir():
            p.error('OCI requires private LGPL FFmpeg prefix and curated legal/source-offer tree')
    out.mkdir(parents=True)
    if a.mode == 'oci':
        ff = a.ffmpeg_prefix.resolve()
        probe_env = dict(os.environ, PKG_CONFIG_PATH=str(ff / 'lib/pkgconfig'), LD_LIBRARY_PATH=str(ff / 'lib'))
        if Path(run('pkg-config', '--variable=prefix', 'libavcodec', env=probe_env)).resolve() != ff:
            raise ValueError('pkg-config did not select the private FFmpeg prefix')
        flags = shlex.split(run('pkg-config', '--cflags', '--libs', 'libavcodec', 'libavutil', env=probe_env))
        probe = out / 'codec-probe'
        subprocess.run(['cc', str(src / 'packaging/ffmpeg/verify-codecs.c'), *flags, '-o', str(probe)], env=probe_env, check=True)
        encoders = run(str(src / 'packaging/ffmpeg/profile-encoders.sh'), 'container', platform.machine()).split(',')
        receipt = run(str(probe), *encoders, env=probe_env)
        (out / 'codec-inventory.txt').write_text(receipt + '\n')
        probe.unlink()
    stage = out / 'install'
    subprocess.run(['meson', 'install', '-C', str(build), '--no-rebuild', '--destdir', str(stage)], check=True)
    root = out / 'tree'
    root.mkdir()
    prefix = root / ('usr' if a.mode == 'native' else 'opt/metadesk')
    lib = prefix / ('lib/metadesk' if a.mode == 'native' else 'lib')
    lib.mkdir(parents=True)
    (prefix / 'bin').mkdir()
    installed = stage / ('usr' if a.mode == 'native' else 'opt/metadesk')
    names = ['metadesk-host', 'metadesk-client'] if a.mode == 'native' else ['metadesk-host']
    for name in names:
        shutil.copy2(installed / 'bin' / name, prefix / 'bin' / name)
    libs = list(installed.rglob('libmetadesk.so*'))
    if not libs:
        raise ValueError('missing installed libmetadesk')
    for f in libs:
        if f.is_file() and (a.mode == 'oci' or not f.is_symlink()):
            shutil.copy2(f, lib / f.name, follow_symlinks=True)
    if a.mode == 'native':
        real = next(lib.glob('libmetadesk.so.*'))
        soname = re.search(r'\(SONAME\).*\[(.*?)\]', run('readelf', '-d', str(real))).group(1)
        if soname != real.name:
            (lib / soname).symlink_to(real.name)
    origins = {}
    base = re.compile(r'^(ld-linux-.*|lib(c|m|pthread|dl|rt|resolv|util|anl|nss_.*)\.so\..*|libgcc_s\.so\..*|libstdc\+\+\.so\..*)$')
    env = dict(os.environ, LD_LIBRARY_PATH=str(lib))
    if a.ffmpeg_prefix:
        env['LD_LIBRARY_PATH'] += ':' + str(a.ffmpeg_prefix.resolve() / 'lib')
    for f in elfs(prefix):
        output = run('ldd', str(f), env=env)
        if 'not found' in output:
            raise ValueError(output)
        if a.mode == 'native':
            for path in re.findall(r'=> (/\S+)', output):
                origin = Path(path).resolve()
                if origin.name.startswith(('libavcodec.', 'libavutil.')) and not any(root == origin or root in origin.parents for root in (Path('/usr/lib'), Path('/usr/lib64'))):
                    raise ValueError('native FFmpeg must resolve to distro library directories')
        if a.mode == 'native' and re.search(r'lib(nostr|nip|go_fiber|libgo)', output):
            raise ValueError('nostrc must be static for native packages')
        if a.mode == 'oci':
            for path in re.findall(r'=> (/\S+)', output):
                origin = Path(path)
                ffmpeg = re.match(r'lib(avcodec|avutil|avformat|avfilter|avdevice|swscale|swresample|postproc)\.', origin.name)
                if ffmpeg and ffmpeg.group(1) not in ('avcodec', 'avutil'):
                    raise ValueError('FFmpeg component outside minimal profile: ' + origin.name)
                if ffmpeg and a.ffmpeg_prefix.resolve() not in origin.resolve().parents:
                    raise ValueError('refusing system FFmpeg in OCI')
                if base.fullmatch(origin.name):
                    continue
                if re.search(r'(openh264|x264|x265)', origin.name, re.I):
                    raise ValueError('forbidden bundled codec: ' + origin.name)
                dest = lib / origin.name
                if not dest.exists():
                    shutil.copy2(origin, dest)
                    origins[origin.name] = str(origin.resolve())
    for f in elfs(prefix):
        if a.mode == 'oci':
            subprocess.run(['patchelf', '--remove-rpath', str(f)], check=True)
        tags = re.findall(r'\((?:RPATH|RUNPATH)\).*\[(.*?)\]', run('readelf', '-d', str(f)))
        expected = ['$ORIGIN/../lib/metadesk'] if a.mode == 'native' and f.parent == prefix / 'bin' else []
        if tags != expected:
            raise ValueError('unexpected installed RPATH: ' + str(f) + ': ' + repr(tags))
        if not a.keep_debug:
            subprocess.run(['strip', '--strip-unneeded', str(f)], check=True)
        if f.parent == lib:
            f.chmod(0o755 if a.keep_debug else 0o644)
    doc = prefix / ('share/doc/metadesk' if a.mode == 'native' else 'share/licenses')
    doc.mkdir(parents=True)
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(src / name, doc / name)
    shutil.copy2(src / 'imgui_vendor/LICENSE.txt', doc / 'imgui-LICENSE.txt')
    shutil.copy2(nostrc / 'LICENSE', doc / 'nostrc-LICENSE')
    shutil.copy2(nostrc / 'third_party/nsync/LICENSE', doc / 'nsync-LICENSE')
    if a.legal:
        shutil.copytree(a.legal, doc / 'dependencies')
    if a.mode == 'native':
        unit = root / 'usr/lib/systemd/user/metadesk-host.service'
        unit.parent.mkdir(parents=True)
        shutil.copy2(src / 'packaging/linux/systemd/metadesk-host.service', unit)
        manuals = root / 'usr/share/man/man1'
        manuals.mkdir(parents=True)
        for name in names:
            shutil.copy2(src / 'packaging/linux/man' / (name + '.1'), manuals)
    metadata = {
        'schema': 1, 'mode': a.mode, 'arch': platform.machine(), 'source_revision': source_rev,
        'nostrc_revision': nostrc_rev, 'base': a.base, 'private_origins': origins,
        'packaging_sha256': {str(f.relative_to(src)): digest(f) for f in sorted((src / 'packaging/linux').rglob('*')) if f.is_file() and not {'__pycache__', 'artifacts', 'evidence'}.intersection(f.relative_to(src / 'packaging/linux').parts)},
        'ffmpeg': 'system' if a.mode == 'native' else 'private-minimal-LGPL',
        'openh264': 'runtime-only; never bundled',
        'dlopen': ['OpenH264: separately obtained exact vetted ABI', 'NSS: base-owned',
                   'PipeWire/SPA, portal, GPU/VAAPI/QSV: system; not Tier A qualified'],
        'gates': {'static': 'hashes + no producer RPATH; native executables use $ORIGIN/../lib/metadesk', 'runtime': 'must run verify-image.sh; gate 5 requires authorized desktop'},
    }
    manifest = prefix / ('share/metadesk/metadesk-bundle.json' if a.mode == 'native' else 'metadesk-bundle.json')
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(metadata, indent=2) + '\n')
    hashroot = root if a.mode == 'native' else prefix
    checks = hashroot / 'SHA256SUMS'
    checks.write_text(''.join(f'{digest(f)}  {f.relative_to(hashroot)}\n' for f in sorted(hashroot.rglob('*')) if f.is_file()))
    shutil.rmtree(stage)
    print(root)


if __name__ == '__main__':
    main()
