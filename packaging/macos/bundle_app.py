#!/usr/bin/env python3
"""Turn a staged metadesk install tree into a self-contained macOS .app.

Prototype for md-kb7 (docs/plans/packaging-2026-11-21.md). The rules follow
deploy/containers/reference/bundle-contract.md, translated to Mach-O:

* base-owned: /usr/lib and /System (the macOS equivalent of glibc/libstdc++);
  everything else in the transitive LC_LOAD_DYLIB closure is copied privately;
* no producer rpaths survive: every LC_RPATH is removed and replaced with a
  bundle-relative one;
* the closure walk covers linked images, NOT dlopen; libraries loaded with
  dlopen must be named with --dlopen and are retested by verify-app.sh;
* no keys, tokens, configuration, or build tree enters the bundle.

Only run this on trusted, locally built binaries.
"""
import argparse
from macho import minimums, version_key
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

SYSTEM_PREFIXES = ('/usr/lib/', '/System/')
FRAMEWORKS_RPATH_EXE = '@executable_path/../Frameworks'
FRAMEWORKS_RPATH_LIB = '@loader_path'


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def is_macho(path):
    try:
        with open(path, 'rb') as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xfe\xed\xfa\xcf')


def install_id(path):
    lines = run('otool', '-D', str(path)).splitlines()
    return lines[1].strip() if len(lines) > 1 else None


def load_commands(path):
    """Return (dylib references, rpaths) of one image."""
    out = run('otool', '-l', str(path)).splitlines()
    deps, rpaths = [], []
    cmd = None
    for line in out:
        s = line.strip()
        if s.startswith('cmd '):
            cmd = s.split()[1]
        elif s.startswith('name ') and cmd in ('LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB',
                                               'LC_REEXPORT_DYLIB', 'LC_LOAD_UPWARD_DYLIB'):
            deps.append(re.sub(r' \(offset \d+\)$', '', s[5:]))
        elif s.startswith('path ') and cmd == 'LC_RPATH':
            rpaths.append(re.sub(r' \(offset \d+\)$', '', s[5:]))
    return deps, rpaths


def expand(token_path, image, executable):
    return (token_path.replace('@loader_path', str(Path(image).parent))
            .replace('@executable_path', str(Path(executable).parent)))


PREFIX_MAP = {}  # install prefix -> staged tree (meson writes absolute prefix install names)
SEARCH = []      # fallback directories for @rpath refs whose installed image lost its rpaths


def resolve(ref, image, executable, rpath_chain):
    for prefix, stage in PREFIX_MAP.items():
        if ref.startswith(prefix + '/'):
            cand = Path(stage) / ref[len(prefix) + 1:]
            return cand if cand.exists() else None
    if ref.startswith('@rpath/'):
        tail = ref[len('@rpath/'):]
        for rp in rpath_chain:
            cand = Path(expand(rp, image, executable)) / tail
            if cand.exists():
                return cand
        # meson install strips build rpaths but keeps @rpath install names
        # (e.g. libyuv found via find_library), so the staged tree alone is
        # not resolvable; fall back to the producer's build-tree rpaths.
        for d in SEARCH:
            cand = Path(d) / tail
            if cand.exists():
                return cand
        return None
    cand = Path(expand(ref, image, executable))
    return cand if cand.exists() else None


class Bundler:
    def __init__(self, frameworks):
        self.frameworks = frameworks
        self.by_real = {}      # realpath -> bundled basename
        self.origin = {}       # bundled basename -> source realpath

    def adopt(self, src, name):
        real = os.path.realpath(src)
        if real in self.by_real:
            return self.by_real[real]
        if name in self.origin and self.origin[name] != real:
            raise SystemExit(f'name collision: {name} from {real} and {self.origin[name]}')
        dst = self.frameworks / name
        shutil.copyfile(real, dst)
        dst.chmod(0o755)
        self.by_real[real] = name
        self.origin[name] = real
        return name

    def walk(self, image, executable, inherited_rpaths, rewrites, queue):
        deps, rpaths = load_commands(image)
        chain = rpaths + inherited_rpaths
        own_id = install_id(image)
        for ref in deps:
            if ref == own_id:
                continue
            if ref.startswith(SYSTEM_PREFIXES):
                continue
            src = resolve(ref, image, executable, chain)
            if src is None:
                raise SystemExit(f'{image}: cannot resolve {ref} (rpaths: {chain})')
            if str(src.resolve()).startswith(SYSTEM_PREFIXES):
                continue
            first = os.path.realpath(src) not in self.by_real
            name = self.adopt(src, Path(ref).name)
            rewrites.setdefault(str(image), {})[ref] = '@rpath/' + name
            if first:
                queue.append((self.frameworks / name, chain))


def rewrite(image, changes, is_exe, new_id=None):
    args = ['install_name_tool']
    if new_id:
        args += ['-id', new_id]
    for old, new in changes.items():
        if old != new:
            args += ['-change', old, new]
    desired_rpath = FRAMEWORKS_RPATH_EXE if is_exe else FRAMEWORKS_RPATH_LIB
    rpaths = list(dict.fromkeys(load_commands(image)[1]))
    for rp in rpaths:
        if rp != desired_rpath:
            args += ['-delete_rpath', rp]
    # install_name_tool rejects deleting and adding the same rpath in one call.
    if desired_rpath not in rpaths:
        args += ['-add_rpath', desired_rpath]
    args += [str(image)]
    subprocess.run(args, check=True, capture_output=True)


def minos(image):
    values = minimums(image)
    if not values:
        raise SystemExit(f'{image}: missing macOS minimum load command')
    return max(values, key=version_key)


def sign(app, identity, hardened, entitlements):
    base = ['codesign', '--force', '--sign', identity]
    if hardened:
        base += ['--options', 'runtime', '--timestamp']
    # Inside-out: nested code first, the bundle (main executable) last.
    for lib in sorted((app / 'Contents/Frameworks').iterdir()):
        subprocess.run(base + [str(lib)], check=True, capture_output=True)
    exe_args = base + (['--entitlements', str(entitlements)] if entitlements else [])
    for exe in sorted((app / 'Contents/MacOS').iterdir()):
        if exe.name != 'metadesk-client':
            subprocess.run(exe_args + [str(exe)], check=True, capture_output=True)
    subprocess.run(exe_args + [str(app)], check=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--stage', required=True, type=Path,
                    help='installed prefix tree containing bin/ (e.g. DESTDIR/opt/metadesk)')
    ap.add_argument('--prefix', default='/opt/metadesk',
                    help='install prefix the stage was configured with')
    ap.add_argument('--search', action='append', default=[], metavar='DIR',
                    help='fallback directory for unresolved @rpath references')
    ap.add_argument('--out', required=True, type=Path, help='output Metadesk.app (must not exist)')
    ap.add_argument('--dlopen', action='append', default=[], metavar='NAME=PATH',
                    help='library loaded with dlopen, bundled as Frameworks/NAME')
    ap.add_argument('--bundle-id', default='dev.metadesk.Metadesk')
    ap.add_argument('--version', required=True)
    ap.add_argument('--source-rev', required=True)
    ap.add_argument('--nostrc-rev', required=True)
    ap.add_argument('--sign-identity', default='-', help="codesign identity; '-' = ad-hoc")
    ap.add_argument('--hardened-runtime', action='store_true')
    ap.add_argument('--entitlements', type=Path)
    a = ap.parse_args()
    a.out = a.out.resolve()
    PREFIX_MAP[a.prefix.rstrip('/')] = str(a.stage.resolve())
    SEARCH.extend(a.search)

    if a.out.exists():
        raise SystemExit(f'{a.out} must not exist')
    bins = [a.stage / 'bin' / n for n in ('metadesk-client', 'metadesk-host')]
    for b in bins:
        if not b.exists():
            raise SystemExit(f'missing {b}; build with -Dclient=true')

    contents = a.out / 'Contents'
    macos, frameworks, resources = contents / 'MacOS', contents / 'Frameworks', contents / 'Resources'
    for d in (macos, frameworks, resources):
        d.mkdir(parents=True)

    agents = contents / 'Library/LaunchAgents'
    agents.mkdir(parents=True)
    shutil.copyfile(Path(__file__).with_name('dev.metadesk.host.plist'), agents / 'dev.metadesk.host.plist')
    shutil.copyfile(Path(__file__).with_name('metadesk-host-agent'), resources / 'metadesk-host-agent')
    (resources / 'metadesk-host-agent').chmod(0o755)

    bundler = Bundler(frameworks)
    rewrites, queue, staged = {}, [], []
    for b in bins:
        dst = macos / b.name
        shutil.copyfile(b, dst)
        dst.chmod(0o755)
        staged.append(dst)
    for spec in a.dlopen:
        name, _, path = spec.partition('=')
        if not name or not path or not Path(path).exists():
            raise SystemExit(f'bad --dlopen {spec!r}')
        bundler.adopt(Path(path), name)
        queue.append((frameworks / name, []))

    # Walk with the ORIGINAL rpaths of the staged executables; resolution
    # happens against the producer, rewriting happens afterwards.
    for exe in staged:
        orig = a.stage / 'bin' / exe.name
        deps_q = []
        bundler.walk(orig, orig, [], rewrites, deps_q)
        rewrites[str(exe)] = rewrites.pop(str(orig), {})
        queue.extend(deps_q)
        while queue:
            img, chain = queue.pop()
            src = bundler.origin[img.name]
            sub = []
            bundler.walk(Path(src), orig, chain, rewrites, sub)
            rewrites[str(img)] = rewrites.pop(src, {})
            queue.extend(sub)

    for exe in staged:
        rewrite(exe, rewrites.get(str(exe), {}), True)
    for lib in sorted(frameworks.iterdir()):
        rewrite(lib, rewrites.get(str(lib), {}), False, '@rpath/' + lib.name)

    images = staged + sorted(frameworks.iterdir())
    min_versions = {str(p.relative_to(a.out)): minos(p) for p in images}
    effective = max((v for v in min_versions.values() if v),
                    key=lambda v: tuple(int(x) for x in v.split('.')))
    arch = run('lipo', '-archs', str(staged[0])).strip()

    plist = {
        'CFBundleIdentifier': a.bundle_id,
        'CFBundleName': 'Metadesk',
        'CFBundleDisplayName': 'Metadesk',
        'CFBundleExecutable': 'metadesk-client',
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': a.version,
        'CFBundleVersion': a.version,
        'CFBundleInfoDictionaryVersion': '6.0',
        'LSMinimumSystemVersion': effective,
        'NSHighResolutionCapable': True,
    }
    with open(contents / 'Info.plist', 'wb') as f:
        plistlib.dump(plist, f)
    manifest = {
        'contract': 'deploy/containers/reference/bundle-contract.md (Mach-O translation)',
        'source_rev': a.source_rev,
        'nostrc_rev': a.nostrc_rev,
        'arch': arch,
        'effective_minimum_macos': effective,
        'image_minos': min_versions,
        'base_owned': list(SYSTEM_PREFIXES),
        'dlopen_bundled': [s.partition('=')[0] for s in a.dlopen],
        'private_libraries': {n: bundler.origin[n] for n in sorted(bundler.origin)},
    }
    (resources / 'metadesk-bundle.json').write_text(json.dumps(manifest, indent=2) + '\n')

    sign(a.out, a.sign_identity, a.hardened_runtime, a.entitlements)
    print(f'{a.out}: {len(bundler.origin)} private libraries, minimum macOS {effective}, {arch}')


if __name__ == '__main__':
    sys.exit(main())
