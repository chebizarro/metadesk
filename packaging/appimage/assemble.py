#!/usr/bin/env python3
"""Assemble an AppDir from trusted Linux build output, with a verified FFmpeg prefix.

Does not download, sign, publish, copy the host, or replace the system glibc.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

BASE = re.compile(r'^(ld-linux[^/]*|lib(c|m|dl|pthread|rt|resolv|util|nss_[^.]+)\.so(?:\..*)?)$')
FORBIDDEN = re.compile(r'(openh264|x264|x265)', re.I)
FFMPEG = re.compile(r'^lib(avcodec|avutil)\.so(?:\..*)?$')
HERE = Path(__file__).resolve().parent

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def dependencies(path, env):
    result = subprocess.run(['ldd', str(path)], env=env, text=True, capture_output=True, check=True)
    if 'not found' in result.stdout:
        raise ValueError(f'unresolved closure for {path}: {result.stdout}')
    for line in result.stdout.splitlines():
        match = re.search(r'=> (/\S+)', line)
        if match:
            yield Path(match[1])

def assemble(prefix, ffmpeg, output, arch):
    if output.exists():
        raise ValueError('output must be new')
    for name in ('libavcodec', 'libavutil'):
        if not list((ffmpeg / 'lib').glob(name + '.so.*')):
            raise ValueError('requires a private minimal LGPL FFmpeg install')
    env = dict(os.environ, LD_LIBRARY_PATH=f'{ffmpeg}/lib:{prefix}/lib')
    encoders = subprocess.check_output([str(HERE.parent / 'ffmpeg/profile-encoders.sh'), 'container', arch], text=True).strip().split(',')
    subprocess.run([str(prefix / 'libexec/metadesk/verify-codecs'), *encoders], env=env, check=True)
    lib = output / 'usr/lib'
    lib.mkdir(parents=True)
    records = {}
    queue = []
    for rel in ('bin/metadesk-client', 'libexec/metadesk/verify-codecs', 'libexec/metadesk/decode-smoke'):
        source = prefix / rel
        dest = output / 'usr' / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
        queue.append(source)
    visited = set()
    while queue:
        source = queue.pop()
        if source.resolve() in visited:
            continue
        visited.add(source.resolve())
        for dep in dependencies(source, env):
            if BASE.fullmatch(dep.name):
                continue
            if FORBIDDEN.search(dep.name):
                raise ValueError('forbidden bundled codec: ' + str(dep))
            if dep.name.startswith('libav') and not FFMPEG.fullmatch(dep.name):
                raise ValueError('unexpected FFmpeg component: ' + str(dep))
            if FFMPEG.fullmatch(dep.name) and dep.resolve().parent != (ffmpeg / 'lib').resolve():
                raise ValueError('FFmpeg escaped the verified prefix: ' + str(dep))
            dest = lib / dep.name
            sha = digest(dep)
            if dest.exists() and digest(dest) != sha:
                raise ValueError('dependency collision: ' + dep.name)
            if not dest.exists():
                shutil.copy2(dep, dest, follow_symlinks=True)
                records[dep.name] = {'source': str(dep), 'source_sha256': sha}
                queue.append(dep)
    # Strip build RPATHs, relying solely on the AppDir-local launcher scope.
    for path in [*lib.iterdir(), *(output / 'usr/bin').iterdir(), *(output / 'usr/libexec/metadesk').iterdir()]:
        subprocess.run(['patchelf', '--remove-rpath', str(path)], check=True)
    for name, record in records.items():
        record['sha256'] = digest(lib / name)
    launcher = output / 'AppRun'
    launcher.write_text('''#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
export LD_LIBRARY_PATH="$root/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$root/usr/bin/metadesk-client" "$@"
''')
    launcher.chmod(0o755)
    assets = HERE.parent / 'flatpak/assets'
    for suffix in ('desktop', 'svg'):
        shutil.copy2(assets / ('org.metadesk.Client.' + suffix), output)
    (output / '.DirIcon').symlink_to('org.metadesk.Client.svg')
    shutil.copytree(prefix / 'share/licenses', output / 'usr/share/licenses')
    shutil.copy2(ffmpeg / 'COPYING.LGPLv2.1', output / 'usr/share/licenses/FFmpeg-LGPL-2.1.txt')
    (output / 'usr/share/metadesk').mkdir()
    shutil.copy2(HERE / 'tests/frame.h264', output / 'usr/share/metadesk/frame.h264')
    receipt = {'arch': arch, 'glibc': 'system, not bundled', 'encoders': encoders, 'libraries': records}
    (output / 'bundle.json').write_text(json.dumps(receipt, indent=2) + '\n')
    final_env = dict(os.environ, LD_LIBRARY_PATH=str(lib))
    for path in (output / 'usr/bin/metadesk-client', output / 'usr/libexec/metadesk/decode-smoke'):
        list(dependencies(path, final_env))
    subprocess.run([str(output / 'AppRun'), '--help'], env=final_env, check=True)
    subprocess.run([str(output / 'usr/libexec/metadesk/decode-smoke'), str(output / 'usr/share/metadesk/frame.h264')], env=final_env, check=True)
    return receipt

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('prefix', type=Path)
    p.add_argument('ffmpeg', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--arch', choices=['aarch64', 'x86_64'], required=True)
    a = p.parse_args()
    assemble(a.prefix.resolve(), a.ffmpeg.resolve(), a.output.resolve(), a.arch)
