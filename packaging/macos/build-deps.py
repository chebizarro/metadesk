#!/usr/bin/env python3
"""Build the pinned native arm64 macOS 13 dependency closure, without Homebrew libraries.

Usage: build-deps.py NEW_ABSOLUTE_OUTPUT [--downloads CACHE]
Build tools (CMake, Meson, Ninja, pkg-config, make, Perl) must already be installed.
Archives, exact commands, logs, license texts and build trees are retained in OUTPUT.
No system-wide installs. This is a source-pinned recipe, not a bit-identical-build claim.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('--downloads', type=Path)
    ap.add_argument('--nostrc-repo', type=Path, required=True, help='read-only git repository containing the pinned nostrc commit')
    a = ap.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        ap.error('this profile is native arm64 macOS only; x86_64/universal2 is follow-up work')
    out = a.output.resolve()
    if not a.output.is_absolute() or out.exists() or out.is_relative_to(REPO):
        ap.error('OUTPUT must be new, absolute and outside the source tree')
    out.mkdir(parents=True)
    prefix = out / 'prefix'
    downloads = a.downloads.resolve() if a.downloads else out / 'downloads'
    downloads.mkdir(parents=True, exist_ok=True)
    for d in ('sources', 'build', 'logs', 'licenses'):
        (out / d).mkdir()
    prefix.mkdir()
    env = os.environ.copy()
    # Tools may come from Homebrew; target libraries and headers may not.
    for key in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                'DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH', 'CMAKE_PREFIX_PATH',
                'CMAKE_LIBRARY_PATH', 'CMAKE_INCLUDE_PATH', 'CMAKE_TOOLCHAIN_FILE',
                'PKG_CONFIG_SYSROOT_DIR', 'PKG_CONFIG'):
        env.pop(key, None)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    env.update(MACOSX_DEPLOYMENT_TARGET='13.0', CC='/usr/bin/clang', CXX='/usr/bin/clang++',
               SDKROOT=sdk, PKG_CONFIG_PATH='', PKG_CONFIG_LIBDIR=str(prefix / 'lib/pkgconfig'),
               CFLAGS=f'-O2 -arch arm64 -mmacosx-version-min=13.0 -isysroot {sdk}',
               CXXFLAGS=f'-O2 -arch arm64 -mmacosx-version-min=13.0 -isysroot {sdk}',
               CPPFLAGS=f'-I{prefix}/include', LDFLAGS=f'-L{prefix}/lib')
    jobs = os.environ.get('MD_DEPS_JOBS', '4')
    pins = json.loads((HERE / 'deps.lock.json').read_text())
    (out / 'deps.lock.json').write_text(json.dumps(pins, indent=2) + '\n')
    (out / 'toolchain.txt').write_text(subprocess.check_output(['clang', '--version'], text=True) +
        subprocess.check_output(['xcodebuild', '-version'], text=True) +
        f'SDKROOT={sdk}\nMACOSX_DEPLOYMENT_TARGET=13.0\n' +
        '\n'.join(f'{k}={v}' for k, v in sorted(env.items()) if k in
            ('CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'PKG_CONFIG_LIBDIR')) + '\n')

    def run(name, args, cwd=None):
        print(name + ': ' + shlex.join(map(str, args)), flush=True)
        with (out / 'logs' / (name + '.log')).open('a') as log:
            log.write('$ ' + shlex.join(map(str, args)) + '\n'); log.flush()
            subprocess.run(list(map(str, args)), cwd=cwd, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)

    sources = {}
    for pin in pins:
        name = pin['name']
        archive = downloads / pin['archive']
        if name == 'nostrc' and not archive.exists():
            with archive.open('wb') as f:
                subprocess.run(['git', '-C', str(a.nostrc_repo), 'archive', '--format=tar',
                                '--prefix=nostrc/', pin['version']], stdout=f, check=True)
        if not archive.exists():
            partial = archive.with_suffix(archive.suffix + '.partial')
            run('download-' + name, ['/usr/bin/curl', '--fail', '--location', '--proto', '=https',
                                    '--tlsv1.2', '--output', partial, pin['url']])
            partial.rename(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sha256']:
            raise SystemExit(f'SHA256 mismatch: {archive}')
        dest = out / 'sources' / name
        dest.mkdir()
        # Strip the archive's top directory except googlesource's flat libyuv archive.
        with tarfile.open(archive) as tar:
            members = tar.getmembers()
            for m in members:
                if name != 'libyuv':
                    m.name = m.name.partition('/')[2]
                if not m.name:
                    continue
                tar.extract(m, dest, filter='data')
        sources[name] = dest
        legal = out / 'licenses' / name
        legal.mkdir()
        for f in dest.iterdir():
            if f.is_file() and f.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE', 'PATENTS', 'AUTHORS')):
                shutil.copy2(f, legal / f.name)
        if not list(legal.iterdir()):
            raise SystemExit('missing upstream license files: ' + name)

    def cmake(name, *options, targets=None, install=True):
        build = out / 'build' / name
        run(name, ['cmake', '-S', sources[name], '-B', build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
            '-DPKG_CONFIG_EXECUTABLE=' + shutil.which('pkg-config'),
            '-DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=FALSE',
            '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
            '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local',
            '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0', '-DCMAKE_OSX_ARCHITECTURES=arm64',
            '-DCMAKE_OSX_SYSROOT=' + sdk, '-DCMAKE_INSTALL_RPATH=' + str(prefix / 'lib'),
            '-DBUILD_SHARED_LIBS=ON', *options])
        run(name, ['cmake', '--build', build, '--parallel', jobs] + (['--target', *targets] if targets else []))
        if install:
            run(name, ['cmake', '--install', build])

    def configure(name, *options):
        build = out / 'build' / name
        build.mkdir()
        run(name, [sources[name] / 'configure', '--prefix=' + str(prefix),
                   '--disable-static', '--enable-shared', *options], build)
        run(name, ['make', '-j', jobs], build)
        run(name, ['make', 'install'], build)

    # Use Apple's libcurl and headers, not a Homebrew HTTP/TLS closure.
    curl_header = Path(sdk) / 'usr/include/curl/curlver.h'
    curl_version = re.search(r'#define LIBCURL_VERSION\s+"([^"]+)"', curl_header.read_text())[1]
    (prefix / 'lib/pkgconfig').mkdir(parents=True, exist_ok=True)
    (prefix / 'lib/pkgconfig/libcurl.pc').write_text(f'''Name: libcurl
Description: Apple SDK libcurl (OS-owned, not redistributed)
Version: {curl_version}
Libs: -lcurl
Cflags: -I{sdk}/usr/include
''')

    build = out / 'build' / 'openssl'
    build.mkdir()
    run('openssl', [sources['openssl'] / 'Configure', 'darwin64-arm64-cc', 'shared',
                   'no-tests', 'no-module', '--prefix=' + str(prefix), '--libdir=lib'], build)
    run('openssl', ['make', '-j', jobs], build)
    run('openssl', ['make', 'install_sw'], build)
    configure('libsodium')
    configure('libb2', '--disable-native', '--disable-openmp')
    cmake('cjson', '-DENABLE_CJSON_TEST=OFF', '-DENABLE_CJSON_UTILS=OFF')
    cmake('jansson', '-DJANSSON_BUILD_DOCS=OFF', '-DJANSSON_BUILD_SHARED_LIBS=ON', '-DJANSSON_WITHOUT_TESTS=ON')
    cmake('secp256k1', '-DSECP256K1_BUILD_BENCHMARK=OFF', '-DSECP256K1_BUILD_TESTS=OFF',
          '-DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF', '-DSECP256K1_BUILD_CTIME_TESTS=OFF',
          '-DSECP256K1_ENABLE_MODULE_RECOVERY=ON')
    cmake('nsync', '-DNSYNC_ENABLE_TESTS=OFF')
    cmake('libwebsockets', '-DLWS_WITHOUT_TESTAPPS=ON', '-DLWS_WITH_STATIC=OFF',
          '-DLWS_WITH_SHARED=ON', '-DLWS_WITH_HTTP3=OFF', '-DLWS_ROLE_QUIC=OFF',
          '-DLWS_WITH_GNUTLS=OFF', '-DLWS_WITH_LIBUV=OFF', '-DLWS_WITH_LIBEVENT=OFF',
          '-DLWS_WITH_GLIB=OFF', '-DLWS_WITH_ZLIB=OFF', '-DLWS_WITH_EXTERNAL_POLL=ON',
          '-DOPENSSL_ROOT_DIR=' + str(prefix))
    cmake('sdl2', '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST=OFF', '-DSDL_TESTS=OFF')
    cmake('libyuv', '-DLIBYUV_DISABLE_JPEG=ON', targets=['yuv', 'yuv_shared'], install=False)
    shutil.copytree(sources['libyuv'] / 'include', prefix / 'include', dirs_exist_ok=True)
    shutil.copy2(out / 'build/libyuv/libyuv.dylib', prefix / 'lib/libyuv.dylib')
    # Upstream libyuv has no pkg-config file; keep headers and the @rpath dylib
    # in this prefix so consumers do not fall back to an unversioned local copy.
    (prefix / 'lib/pkgconfig/libyuv.pc').write_text(f'''prefix={prefix}
libdir=${{prefix}}/lib
includedir=${{prefix}}/include
Name: libyuv
Description: pinned libyuv without JPEG
Version: 0.0.0
Libs: -L${{libdir}} -lyuv
Cflags: -I${{includedir}}
''')
    ffout = out / 'build/ffmpeg'
    env.update(MD_FFMPEG_JOBS=jobs, MD_FFMPEG_ARCH='arm64',
               MD_FFMPEG_CFLAGS=env['CFLAGS'], MD_FFMPEG_LDFLAGS=env['LDFLAGS'])
    run('ffmpeg', [REPO / 'packaging/ffmpeg/build-minimal-lgpl.sh', 'macos', sources['ffmpeg'], ffout])
    for sub in ('include', 'lib'):
        shutil.copytree(ffout / 'install' / sub, prefix / sub, dirs_exist_ok=True, symlinks=True)
    for pc in (prefix / 'lib/pkgconfig').glob('libav*.pc'):
        pc.write_text(pc.read_text().replace(str(ffout / 'install'), str(prefix)))
    codec_flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'libavcodec', 'libavutil'], env=env, text=True))
    run('codec-inventory', [env['CC'], *shlex.split(env['CFLAGS']),
        REPO / 'packaging/ffmpeg/verify-codecs.c', '-o', out / 'build/verify-codecs', *codec_flags])
    run('codec-inventory', [out / 'build/verify-codecs', 'h264_videotoolbox'])
    (sources['nostrc'] / 'SOURCE_REVISION').write_text(next(p['version'] for p in pins if p['name'] == 'nostrc') + '\n')
    # Keep a targeted, GLib-free build: the full install also builds NIP-55L,
    # whose Keychain implementation at this pin requires GLib headers.
    cmake('nostrc', '-DBUILD_SHARED_LIBS=OFF', '-DNOSTR_WITH_GLIB=OFF',
          '-DNOSTR_USE_SYSTEM_NSYNC=ON', '-DBUILD_TESTING=OFF', '-DBUILD_TESTING_FRAMEWORK=OFF',
          '-DWITH_NOSTRDB=OFF', '-DLIBNOSTR_WITH_NOSTRDB=OFF', '-DBUILD_APPS=OFF',
          '-DBUILD_FIPS=OFF', '-DBUILD_RELAYD=OFF', '-DBUILD_LIBHANAMI=OFF',
          '-DBUILD_LIBMARMOT=OFF', '-DBUILD_LIBBLOSSOM=OFF', '-DBUILD_LIBPORTHOME=OFF',
          '-DBUILD_LIBVAULT=OFF', '-DBUILD_LIBVAULT_MARMOT=OFF', '-DBUILD_MARMOT_GOBJECT=OFF',
          '-DBUILD_NOSTR_GOBJECT=OFF', '-DBUILD_NOSTR_GTK=OFF', '-DSIGNET_ENABLE=OFF',
          '-DENABLE_NIP04=ON', '-DENABLE_NIP06=ON', '-DENABLE_NIP11=ON', '-DENABLE_NIP17=ON',
          '-DENABLE_NIP19=ON', '-DENABLE_NIP44=ON', '-DENABLE_NIP46=ON', '-DENABLE_NIP49=OFF',
          '-DENABLE_NIP51=ON', '-DENABLE_NIP55L=OFF', '-DENABLE_NIP5F=ON', '-DENABLE_NIP98=ON',
          '-DOPENSSL_ROOT_DIR=' + str(prefix),
          targets=['nostr', 'nostr_json', 'nostrgo', 'go_fiber', 'nip04', 'nip06', 'nip19',
                   'nip11', 'nostr_nip44_core', 'nostr_nip46_core', 'nostr_nip5f_core',
                   'nostr_nip51_core', 'nostr_nip98', 'nostr_nip17_core'], install=False)
    run('minimum-os', ['python3', HERE / 'macho.py', '13.0', prefix, out / 'build/nostrc/libjson'])
    print(f'Built {prefix}; nostrc source={sources["nostrc"]}, build={out / "build/nostrc"}')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as e:
        sys.exit(f'command failed ({e.returncode}); see OUTPUT/logs: {e.cmd}')
