#!/usr/bin/env python3
"""Collect and verify corresponding source against measured release payloads."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
from urllib.parse import urlencode

PACKAGING = Path(__file__).resolve().parents[1]
FFMPEG = {'libavcodec', 'libavutil'}
LAUNCHPAD = 'https://api.launchpad.net/1.0/ubuntu/+archive/primary'
# Reviews apply to the selected libraries, not to every file of the source tree.
FLATPAK = {
    'libSDL2-2.0': ('sdl2', 'Zlib'), 'libcjson': ('cjson', 'MIT'),
    'libb2': ('libb2', 'CC0-1.0'), 'libjansson': ('jansson', 'MIT'),
    'libsecp256k1': ('secp256k1', 'MIT'), 'libnsync': ('nsync', 'Apache-2.0'),
    'libnsync_cpp': ('nsync', 'Apache-2.0'), 'libwebsockets': ('libwebsockets', 'MIT'),
    'libyuv': ('libyuv', 'BSD-3-Clause'), 'libnostr-json': ('nostrc', 'MIT'),
    'libvpl': ('onevpl', 'MIT'),
}
DMG = {
    'libSDL2-2.0': ('sdl2', 'Zlib'), 'libcjson': ('cjson', 'MIT'), 'libcrypto': ('openssl', 'Apache-2.0'),
    'libjansson': ('jansson', 'MIT'), 'libnostr-json': ('nostrc', 'MIT'), 'libnsync': ('nsync', 'Apache-2.0'),
    'libsecp256k1': ('secp256k1', 'MIT'), 'libssl': ('openssl', 'Apache-2.0'), 'libwebsockets': ('libwebsockets', 'MIT'),
    'libyuv': ('libyuv', 'BSD-3-Clause'),
}


def digest(path, algorithm='sha256'):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def run(*args, **kw):
    return subprocess.check_output(args, text=True, **kw).strip()


def curl(url, *args):
    return run('curl', '--fail', '--silent', '--show-error', '--location', '--proto', '=https',
               '--proto-redir', '=https', '--connect-timeout', '20', '--max-time', '240', *args, url)


def dsc_files(directory, source, version):
    """Validate the full SHA256 source package set, including Debian patches."""
    candidates = list(directory.glob('*.dsc'))
    if len(candidates) != 1:
        raise ValueError(f'{source}={version}: missing or ambiguous .dsc in {directory}')
    dsc, = candidates
    data = dsc.read_text()
    for field, value in [('Source', source), ('Version', version)]:
        if not re.search(rf'^{field}: {re.escape(value)}$', data, re.M):
            raise ValueError(f'{dsc}: wrong {field}, expected {value}')
    match = re.search(r'^Checksums-Sha256:\n((?: [^\n]+\n)+)', data, re.M)
    if not match:
        raise ValueError(f'{dsc}: missing SHA256 file list')
    files = {dsc.name: digest(dsc)}
    for row in match[1].splitlines():
        checksum, size, name = row.split()
        path = directory / name
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_~:-]*', name) or name in files:
            raise ValueError(f'{dsc}: invalid source member {name}')
        if not path.is_file():
            raise ValueError(f'{dsc}: missing source member {name}')
        if path.stat().st_size != int(size) or digest(path) != checksum:
            raise ValueError(f'{path}: source member does not match .dsc')
        files[name] = checksum
    if set(files) != {p.name for p in directory.iterdir()}:
        raise ValueError(f'{dsc}: unexpected files in source directory')
    return files


def fetch_ubuntu(directory, source, version):
    directory.mkdir(parents=True)
    try:
        run('apt-get', '--download-only', '--only-source', 'source', f'{source}={version}', cwd=directory)
    except subprocess.CalledProcessError:
        # Installed runner packages may predate the current index. Query the
        # same official archive's publishing history, not the latest release.
        for file in directory.iterdir():
            file.unlink()
        query = urlencode({'ws.op': 'getPublishedSources', 'source_name': source,
                           'exact_match': 'true', 'version': version,
                           'distro_series': 'https://api.launchpad.net/1.0/ubuntu/noble'})
        entries = json.loads(curl(f'{LAUNCHPAD}?{query}'))['entries']
        for entry in entries:
            urls = json.loads(curl(entry['self_link'] + '?ws.op=sourceFileUrls'))
            if urls:
                for url in urls:
                    name = url.rsplit('/', 1)[-1]
                    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_~:-]*', name):
                        raise ValueError(f'unsafe source filename: {url}')
                    curl(url, '--output', str(directory / name))
                break
    return dsc_files(directory, source, version)


def ubuntu_owner(path):
    real = path.resolve(strict=True)
    for candidate in dict.fromkeys([str(real), str(path), str(real).removeprefix('/usr')]):
        # dpkg's database can still use /lib paths on usrmerged Noble.
        result = subprocess.run(['dpkg-query', '-S', candidate], text=True, capture_output=True)
        if result.returncode == 0:
            package, sep, found = result.stdout.strip().partition(': ')
            if sep and found == candidate:
                return run('dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}',
                           package).split('\t')
    raise ValueError(f'{path}: no unique installed Ubuntu package owner')


def source_directory(source, version):
    # Actions artifact transport rejects colons (Debian version epochs).
    return f'{source}_{version}'.replace(':', '%3A')


def runtime_lock():
    return json.loads((PACKAGING / 'appimage/runtime-source.lock.json').read_text())


def collect_appimage(appdir, offers, runtime):
    manifest = appdir / 'bundle.json'
    inventory = json.loads(manifest.read_text())
    offers.mkdir()
    legal = appdir / 'usr/share/licenses/ubuntu'
    shutil.copytree('/usr/share/common-licenses', legal / 'common-licenses')
    packages = {}
    for name, info in inventory['libraries'].items():
        path = Path(info['source'])
        if path.parent.is_relative_to('/lib') or path.parent.is_relative_to('/usr/lib'):
            if digest(path) != info['source_sha256']:
                raise ValueError(f'{name}: original DSO changed since assembly')
            package, binary_version, source, version = ubuntu_owner(path)
            key = source_directory(source, version)
            if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+_~%-]*', key):
                raise ValueError(f'{name}: invalid source package identity')
            if key not in packages:
                packages[key] = fetch_ubuntu(offers / 'ubuntu' / key, source, version)
            doc = Path('/usr/share/doc') / package.split(':')[0] / 'copyright'
            if not doc.is_file():
                doc = Path('/usr/share/doc') / package / 'copyright'
            notice = legal / package.replace(':', '_') / 'copyright'
            notice.parent.mkdir(exist_ok=True)
            shutil.copyfile(doc, notice)
            rel = notice.relative_to(legal)
            (offers / 'notices' / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(notice, offers / 'notices' / rel)
            info['ubuntu'] = dict(package=package, binary_version=binary_version, source=source,
                                  version=version, files=packages[key], notice=str(rel),
                                  notice_sha256=digest(notice))
        elif stem(name) not in FFMPEG | {'libmetadesk', 'libnostr-json'}:
            raise ValueError(f'{name}: unclassified private origin {path}')
    shutil.copytree(legal / 'common-licenses', offers / 'notices/common-licenses')
    lock = runtime_lock()
    arch = inventory['arch']
    runtime_name = f'runtime-{arch}'
    runtime_pin, = (p for p in json.loads((PACKAGING / 'appimage/tools.lock.json').read_text())
                    if p['url'].endswith('/' + runtime_name))
    runtime_sha = digest(runtime)
    if runtime_sha != runtime_pin['sha256'] or runtime_pin['url'].split('/')[-2] != lock['release']:
        raise ValueError('AppImage runtime source lock does not match the shipped runtime')
    (offers / 'runtime').mkdir()
    for name, pin in lock['sources'].items():
        destination = offers / 'runtime' / name
        curl(pin['url'], '--output', str(destination))
        if digest(destination) != pin['sha256']:
            raise ValueError(f'{name}: AppImage runtime source pin mismatch')
        with tarfile.open(destination) as archive:
            for member in archive.getmembers():
                rel = '/'.join(member.name.split('/')[1:])
                if rel in pin['notices']:
                    data = archive.extractfile(member).read()
                    (offers / 'runtime' / f'{name}.{rel}').write_bytes(data)
                    (appdir / 'usr/share/licenses' / f'{name}.{rel}').write_bytes(data)
    for name, pin in lock['additional_notices'].items():
        destination = offers / 'runtime' / name
        curl(pin['url'], '--output', str(destination))
        if digest(destination) != pin['sha256']:
            raise ValueError(f'{name}: embedded runtime notice checksum mismatch')
        shutil.copyfile(destination, appdir / 'usr/share/licenses' / name)
    inventory['runtime'] = {'release': lock['release'], 'sha256': runtime_sha,
                            'license': 'MIT with statically linked LGPL-2.1 libfuse',
                            'sources': lock['sources'], 'additional_notices': lock['additional_notices']}
    shutil.copyfile(PACKAGING / 'README.md', offers / 'REBUILD.md')
    manifest.write_text(json.dumps(inventory, indent=2) + '\n')


def stem(name):
    if name == 'libSDL2-2.0.0.dylib':
        return 'libSDL2-2.0'
    return re.sub(r'(\.so(?:\.\d+)*|(?:\.\d+)*\.dylib)$', '', Path(name).name)


def verify(receipt, offers, ffmpeg_name):
    """Fail before GitHub mutation if a measured library has no reviewed route."""
    inventory = receipt['inventory']
    if receipt['platform'] == 'dmg':
        if set(inventory) != {'pins', 'bundles'}:
            raise ValueError('dmg: inventory must contain exactly pins and bundles')
        bundles = inventory['bundles']
        if set(bundles) != {'Metadesk.app', 'Metadesk Manager.app'}:
            raise ValueError('dmg: expected measured inventories for both apps')
        return {name: verify_libraries(receipt, bundle | {'pins': inventory['pins']}, offers, ffmpeg_name)
                for name, bundle in bundles.items()}
    return verify_libraries(receipt, inventory, offers, ffmpeg_name)


def verify_libraries(receipt, inventory, offers, ffmpeg_name):
    platform = receipt['platform']
    if platform in {'deb', 'rpm'}:
        if inventory['private_origins']:
            raise ValueError(f'{platform}: unexpected redistributed private libraries')
        return {}
    libraries = inventory['private_libraries' if platform == 'dmg' else 'libraries']
    if not FFMPEG.issubset({stem(name) for name in libraries}):
        raise ValueError(f'{platform}: missing private FFmpeg libraries from inventory')
    pins = json.loads((PACKAGING / 'release/reviewed-source-pins.json').read_text())
    result, checked_sources = {}, {}
    for name, info in libraries.items():
        library = stem(name)
        if library in FFMPEG:
            result[name] = {'license': 'LGPL-2.1-or-later', 'source_asset': ffmpeg_name}
        elif library == 'libmetadesk' or (platform == 'appimage' and library == 'libnostr-json'):
            result[name] = {'license': 'MIT'}
        elif platform == 'appimage' and 'ubuntu' in info:
            record = info['ubuntu']
            src, version = record['source'], record['version']
            key = src, version
            if key not in checked_sources:
                checked_sources[key] = dsc_files(offers / 'ubuntu' / source_directory(src, version), src, version)
            if checked_sources[key] != record['files']:
                raise ValueError(f'{name}: source receipt mismatch')
            if digest(offers / 'notices' / record['notice']) != record['notice_sha256']:
                raise ValueError(f'{name}: copyright receipt mismatch')
            result[name] = {'copyright': f"notices/{record['notice']}", 'source_package': f'{src}={version}',
                            'source_directory': 'ubuntu/' + source_directory(src, version),
                            'source_asset': offers.name + '-source.tar'}
        elif platform.startswith('flatpak-') and library in FLATPAK:
            module, license_id = FLATPAK[library]
            selected, = (m for m in inventory['modules'] if m['name'] == module)
            reviewed, = (m for m in json.loads((PACKAGING / 'flatpak/modules.json').read_text())['modules']
                         if m['name'] == module)
            pin, = selected['sources']
            if selected != reviewed or pin.get('commit', pin.get('sha256')) != pins['flatpak'][module]:
                raise ValueError(f'{name}: unreviewed private Flatpak source pin')
            result[name] = {'license': license_id, 'module': module, 'sources': selected['sources'],
                            'notices': f'/app/share/licenses/{module}/'}
        elif platform == 'dmg' and library in DMG:
            module, license_id = DMG[library]
            pin, = (p for p in inventory['pins'] if p['name'] == module)
            if pin.get('commit', pin.get('sha256')) != pins['dmg'][module]:
                raise ValueError(f'{name}: unreviewed private DMG source pin')
            result[name] = {'license': license_id, 'module': module, 'source': pin,
                            'notices': 'Contents/Resources/licenses/'}
        else:
            raise ValueError(f'{platform}: no reviewed source/license route for {name}')
    if platform == 'appimage':
        lock = runtime_lock()
        runtime = inventory['runtime']
        pin, = (p for p in json.loads((PACKAGING / 'appimage/tools.lock.json').read_text())
                if p['url'].endswith('/runtime-' + receipt['arch']))
        if (runtime['release'] != lock['release'] or runtime['sources'] != lock['sources'] or
                runtime['additional_notices'] != lock['additional_notices'] or
                runtime['sha256'] != pin['sha256'] or pin['url'].split('/')[-2] != lock['release']):
            raise ValueError('AppImage runtime source lock mismatch')
        for name, pin in lock['sources'].items():
            if digest(offers / 'runtime' / name) != pin['sha256']:
                raise ValueError('AppImage runtime source checksum mismatch')
            for notice in pin['notices']:
                if not (offers / 'runtime' / f'{name}.{notice}').stat().st_size:
                    raise ValueError(f'{name}: empty AppImage runtime notice {notice}')
        for name, pin in lock['additional_notices'].items():
            if digest(offers / 'runtime' / name) != pin['sha256']:
                raise ValueError(f'{name}: embedded runtime notice checksum mismatch')
        if not (offers / 'REBUILD.md').is_file() or not (offers / 'notices/common-licenses').is_dir():
            raise ValueError('missing runtime build instructions or Ubuntu common licenses')
        result['AppImage runtime'] = {'license': runtime['license'], 'source_asset': offers.name + '-source.tar'}
    return result


if __name__ == '__main__':
    collect_appimage(*(Path(arg).resolve() for arg in sys.argv[1:]))
