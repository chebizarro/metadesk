#!/usr/bin/env python3
"""Stage native ARM64 PE closure. Run using CLANGARM64 Python on Windows."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile


def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True, stderr=subprocess.STDOUT)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_ffmpeg(ff, repo):
    require((ff / 'profile.txt').read_text().strip() == 'windows', 'not Windows profile')
    require('License: LGPL version 2.1 or later' in (ff / 'configure.txt').read_text(), 'not LGPL')
    config = (ff / 'build/config.h').read_text()
    require('#define ARCH_AARCH64 1' in config.splitlines(), 'not ARM64 FFmpeg')
    for feature in ('GPL', 'NONFREE', 'VERSION3', 'LIBX264', 'LIBX265', 'LIBOPENH264'):
        require(f'#define CONFIG_{feature} 0' in config, f'forbidden {feature}')
    components = (ff / 'build/config_components.h').read_text()
    expected = {'H264_DECODER', 'H264_PARSER', 'H264_NVENC_ENCODER', 'H264_AMF_ENCODER',
                'H264_MF_ENCODER'}  # QSV belongs to the x86 Windows profile.
    actual = set(re.findall(r'^#define CONFIG_(\w+(?:ENCODER|DECODER|PARSER)) 1$', components, re.M))
    require(actual == expected, f'codec inventory mismatch: {actual}')
    patch = '0001-mf-low-latency-vt-bool.patch'
    require(sha(ff / 'patches' / patch) == sha(repo / 'packaging/ffmpeg/patches' / patch), 'patch receipt mismatch')
    require(sha(ff / 'source/libavcodec/mfenc.c') == '1f96132f7e595b1a44d753df2423059da2bbd39406d6dd4396e89e27a611c9a1', 'MF patch missing')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('repo', 'build', 'nostrc', 'secp', 'ffmpeg', 'inputs', 'prefix', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    require(os.name == 'nt', 'use native CLANGARM64 Python on Windows')
    require(not a.output.exists(), 'output must be new')
    for src in (a.repo, a.build, a.nostrc, a.ffmpeg, a.inputs, a.prefix):
        require(not a.output.resolve().is_relative_to(src.resolve()), 'output must be outside inputs')
    check_ffmpeg(a.ffmpeg, a.repo)
    out = a.output
    out.mkdir(parents=True)
    readobj = a.prefix / 'bin/llvm-readobj.exe'
    roots = [a.ffmpeg / 'install/bin', a.nostrc / 'build/libjson', a.build, a.prefix / 'bin']
    system = Path(os.environ['SystemRoot']) / 'System32'
    queue = [a.build / 'metadesk-host.exe', a.build / 'metadesk-client.exe']
    inventory, systems, packages = {}, set(), {}
    pacman = a.prefix.parent / 'usr/bin/pacman.exe'
    # Each private DLL must have an explicitly reviewed license family; unknowns fail.
    policy = json.loads((a.repo / 'packaging/windows/runtime-licenses.json').read_text())
    legal = out / 'licenses'
    legal.mkdir()
    while queue:
        src = queue.pop(0)
        key = src.name.lower()
        if key in inventory:
            require(inventory[key]['sha256'] == sha(src), f'ambiguous DLL: {key}')
            continue
        require(not re.search(r'x26[45]|openh264|avformat|avfilter|swscale|swresample', key), f'forbidden DLL {key}')
        headers = run(readobj, '--file-headers', '--coff-imports', src)
        require('IMAGE_FILE_MACHINE_ARM64' in headers, f'not ARM64: {src}')
        imports = sorted(set(re.findall(r'^  Name: (.+\.dll)\s*$', headers, re.M | re.I)), key=str.lower)
        entry = {'sha256': sha(src), 'imports': imports, 'origin': str(src)}
        if src.parent == a.prefix / 'bin':
            msys_path = '/clangarm64/bin/' + src.name
            package = run(pacman, '-Qqo', msys_path).strip()
            short = package.removeprefix('mingw-w64-clang-aarch64-')
            require(short in policy, f'unreviewed runtime package: {package}')
            entry['package'] = run(pacman, '-Q', package).strip()
            require(entry['package'].rsplit(' ', 1)[1] == policy[short]['version'], f'unreviewed package version: {entry["package"]}')
            entry['license'] = policy[short]['license']
            if package not in packages:
                dest = legal / short
                source = a.prefix / 'share/licenses' / short
                if source.is_dir() and any(source.iterdir()):
                    shutil.copytree(source, dest)
                else:
                    fallback = policy[short].get('source_license_fallback')
                    require(fallback == short, f'missing license texts: {package}')
                    source = a.inputs / fallback
                    texts = sorted(set(source.glob('COPYING*')) | set(source.glob('LICENSE*')) | set(source.glob('AUTHORS*')))
                    require(texts, f'missing pinned source license texts: {package}')
                    dest.mkdir()
                    for text in texts:
                        shutil.copy2(text, dest / text.name)
                    shutil.copy2(source / 'lib/idn2.h.in', dest / 'idn2.h.in')
                packages[package] = {'version': entry['package'], 'license': policy[short]['license'],
                                     'metadata': run(pacman, '-Qi', package)}
            # sdl2-compat dlopens SDL3: it is not a PE import.
            if short == 'sdl2-compat':
                queue.append(a.prefix / 'bin/SDL3.dll')
        inventory[key] = entry
        shutil.copy2(src, out / src.name)
        for dll in imports:
            matches = [d / dll for d in roots if (d / dll).is_file()]
            if dll.lower().startswith(('avcodec-', 'avutil-')):
                require(matches and matches[0].parent == roots[0], 'FFmpeg must resolve privately')
            if matches:
                # Ordered roots deliberately override the stock FFmpeg DLLs.
                if not dll.lower().startswith(('avcodec-', 'avutil-')):
                    require(all(sha(x) == sha(matches[0]) for x in matches), f'ambiguous import: {dll}')
                queue.append(matches[0])
            elif dll.lower().startswith(('api-ms-win-', 'ext-ms-win-')) or (system / dll).is_file():
                systems.add(dll.lower())
            else:
                raise ValueError(f'unresolved import {dll} from {src}')
    for f in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(a.repo / f, out / f)
    shutil.copy2(a.repo / 'packaging/windows/LGPL-SOURCE-OFFER.md', out / 'LGPL-SOURCE-OFFER.md')
    shutil.copy2(a.ffmpeg / 'install/COPYING.LGPLv2.1', legal / 'COPYING.LGPLv2.1')
    shutil.copy2(a.nostrc / 'LICENSE', legal / 'nostrc-LICENSE')
    shutil.copytree(a.prefix / 'share/licenses/nsync', legal / 'nsync', dirs_exist_ok=True)
    shutil.copy2(a.secp / 'COPYING', legal / 'secp256k1-COPYING')
    shutil.copytree(a.inputs / 'libyuv-notices', legal / 'libyuv-source-notices')
    shutil.copy2(a.repo / 'imgui_vendor/LICENSE.txt', legal / 'imgui-LICENSE.txt')
    headers = a.ffmpeg.parent / 'headers'
    # These headers carry the complete upstream copyright/permission notices.
    shutil.copy2(headers / 'AMF/core/Platform.h', legal / 'AMF-Platform.h')
    shutil.copy2(headers / 'include/ffnvcodec/nvEncodeAPI.h', legal / 'NVIDIA-nvEncodeAPI.h')
    # Exact patched corresponding source is available offline to every installed user.
    with tarfile.open(out / 'ffmpeg-corresponding-source.tar.gz', 'w:gz') as tar:
        tar.add(a.ffmpeg / 'source', arcname='ffmpeg')
        tar.add(a.repo / 'packaging/ffmpeg', arcname='metadesk-recipe')
        tar.add(headers, arcname='build-headers')
        for f in ('configure-args.txt', 'profile.txt', 'configure.txt'):
            tar.add(a.ffmpeg / f, arcname='receipt/' + f)
        for f in ('config.h', 'config_components.h', 'ffbuild/config.log'):
            tar.add(a.ffmpeg / 'build' / f, arcname='receipt/' + f)
    (out / 'sources').mkdir()
    shutil.copy2(a.inputs / 'libidn2.tar.gz', out / 'sources/libidn2.tar.gz')
    (out / 'runtime-manifest.json').write_text(json.dumps({
        'files': inventory, 'os_imports_not_bundled': sorted(systems), 'packages': packages,
        'ffmpeg_source_sha256': sha(out / 'ffmpeg-corresponding-source.tar.gz')}, indent=2) + '\n')
    print(f'Staged {len(inventory)} ARM64 PE files; {len(packages)} MSYS2 runtime packages')


if __name__ == '__main__':
    main()
