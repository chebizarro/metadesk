#!/usr/bin/env python3
"""Assemble the release only from a complete, same-revision package matrix."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone

TEMPLATE = Path(__file__).resolve().parents[1] / 'RELEASE_NOTES_TEMPLATE.md'
MATRIX = {(platform, arch) for platform in ('deb', 'rpm', 'appimage', 'flatpak-Client', 'flatpak-Manager')
          for arch in ('x86_64', 'aarch64')} | {('dmg', 'arm64')}
NATIVE = {'deb', 'rpm'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prop(name, value):
    return {'name': f'org.metadesk:{name}', 'value': str(value)}


def component(kind, name, **kw):
    return {'type': kind, 'name': name, **kw}


def hashes(path):
    return [{'alg': 'SHA-256', 'content': sha(path)}]


def bundle_libraries(inventory):
    for key in ('libraries', 'private_libraries', 'private_origins'):
        for name, info in inventory.get(key, {}).items():
            record = component('library', name)
            if isinstance(info, dict) and 'sha256' in info:
                record['hashes'] = [{'alg': 'SHA-256', 'content': info['sha256']}]
            yield record


def source_receipt(receipt, output, platform, arch):
    for needed in ('notices/COPYING.LGPLv2.1', 'records/configure-args.txt', 'records/configure.txt',
                   'records/config.h', 'records/config_components.h', 'records/config.log',
                   'records/toolchain.txt', 'records/codec-inventory.txt', 'records/encoders.txt',
                   'THIRD_PARTY_NOTICES.md', 'build-minimal-lgpl.sh'):
        if not (receipt / needed).is_file():
            raise ValueError(f'{receipt}: missing {needed}')
    packaging = TEMPLATE.parent
    if platform == 'dmg':
        pin = next(p for p in json.loads((packaging / 'macos/deps.lock.json').read_text()) if p['name'] == 'ffmpeg')
        profile = 'macos'
    else:
        modules = json.loads((packaging / 'flatpak/modules.json').read_text())['modules']
        pin = next(p for m in modules if m['name'] == 'ffmpeg-lgpl' for p in m['sources'] if p['type'] == 'archive')
        profile = 'container' if platform == 'appimage' else 'flatpak'
    upstream_name = pin['url'].rsplit('/', 1)[1]
    archive = receipt / pin.get('archive', upstream_name)
    if not archive.is_file() or sha(archive) != pin['sha256']:
        raise ValueError(f'{receipt}: source archive does not match producer pin')
    encoders = subprocess.check_output([packaging / 'ffmpeg/profile-encoders.sh', profile, arch], text=True).strip()
    if ((receipt / 'records/profile.txt').read_text().strip() != profile or
            (receipt / 'records/encoders.txt').read_text().strip() != encoders or
            'PASS: exact minimal codec/parser inventory; LGPL-2.1-or-later' not in
            (receipt / 'records/codec-inventory.txt').read_text()):
        raise ValueError(f'{receipt}: missing matching profile and runtime verification')
    shutil.make_archive(str(output), 'gztar', receipt.parent, receipt.name)
    return upstream_name.removeprefix('ffmpeg-').split('.tar.', 1)[0], encoders, pin['sha256']


def assemble(packages, out):
    tag, trigger_sha, nostrc, repo = (os.environ[k] for k in ('TAG', 'GITHUB_SHA', 'NOSTRC_REVISION', 'GITHUB_REPOSITORY'))
    commit = subprocess.check_output(['git', 'rev-parse', f'{trigger_sha}^{{commit}}'], text=True).strip()
    version = tag.removeprefix('v')
    if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z]+)*', tag):
        raise ValueError('invalid tag')
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() != commit:
        raise ValueError('checkout does not match workflow SHA')
    now = datetime.now(timezone.utc)
    out.mkdir()
    base = f'https://github.com/{repo}'

    def link(name):
        return f'[{name}]({base}/releases/download/{tag}/{name})'

    seen = set()
    components, manifest, rows, source_rows = [], [], [], []
    for path in sorted(packages.glob('*/*.json')):
        receipt = json.loads(path.read_text())
        name, platform, arch = (receipt[k] for k in ('asset', 'platform', 'arch'))
        cell = platform, arch
        if cell not in MATRIX or cell in seen or path.name != name + '.json' or (out / name).exists():
            raise ValueError(f'unexpected or duplicate package: {path}')
        if receipt['revision'] != commit or receipt['nostrc'] != nostrc:
            raise ValueError(f'{name}: source pin does not match release')
        seen.add(cell)
        asset = out / name
        shutil.copyfile(path.with_name(name), asset)
        libraries = list(bundle_libraries(receipt['inventory']))
        codec = 'System FFmpeg (not bundled)'
        if platform not in NATIVE:
            source_name = name + '.ffmpeg-source'
            ffmpeg_version, encoders, source_hash = source_receipt(
                path.with_name(name + '.ffmpeg'), out / source_name, platform, arch)
            source_name += '.tar.gz'
            codec = f'FFmpeg {ffmpeg_version} LGPL, `h264` decode/parser, `{encoders}`'
            receipt.update(ffmpeg_source=source_name, ffmpeg_source_sha256=source_hash)
            source_rows.append(f'- `{name}`: {link(source_name)} (archive SHA-256 `{sha(out / source_name)}`; '
                               f'upstream FFmpeg archive SHA-256 `{source_hash}`).')
            libraries.append(component('library', 'FFmpeg', version=ffmpeg_version,
                                       licenses=[{'license': {'id': 'LGPL-2.1-or-later'}}],
                                       properties=[prop('encoders', encoders), prop('source-sha256', source_hash)]))
        signing = receipt['signing'] if receipt['signing'] != 'ad-hoc' else 'Unsigned DMG; ad-hoc app'
        rows.append(f'| {link(name)} | {platform} / {arch} | {asset.stat().st_size:,} | {signing} | PASS | '
                    f'{codec} | `{sha(asset)}` |')
        components.append(component('application', name, version=version, hashes=hashes(asset),
                                    properties=[prop('platform', platform), prop('architecture', arch),
                                                prop('signing', signing), prop('codec-policy', codec)],
                                    components=libraries))
        manifest.append(receipt)
    if seen != MATRIX:
        raise ValueError(f'missing required package cells: {sorted(MATRIX - seen)}')

    source = f'metadesk-{version}-source.tar.gz'
    with (out / source).open('wb') as stream:
        subprocess.run(['git', '-c', 'tar.tar.gz.command=gzip -n', 'archive', '--format=tar.gz',
                        f'--prefix=metadesk-{version}/', commit], stdout=stream, check=True)
    components.append(component('file', source, hashes=hashes(out / source)))
    components.append(component('library', 'nostrc', version=nostrc, externalReferences=[
        {'type': 'vcs', 'url': f'https://github.com/chebizarro/nostrc/tree/{nostrc}'}]))
    components.append(component('library', 'Dear ImGui', version='1.90.4',
                                licenses=[{'license': {'id': 'MIT'}}]))
    sbom = {'bomFormat': 'CycloneDX', 'specVersion': '1.6', 'version': 1,
            'metadata': {'timestamp': now.isoformat(), 'component': component(
                'application', 'metadesk', version=version, properties=[prop('git-revision', commit)],
                licenses=[{'license': {'id': 'MIT'}}])}, 'components': components}
    (out / 'metadesk.cdx.json').write_text(json.dumps(sbom, indent=2) + '\n')
    (out / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (out / 'SHA256SUMS').write_text(''.join(f'{sha(path)}  {path.name}\n' for path in sorted(out.iterdir())))

    notes = TEMPLATE.read_text()
    notes = re.sub(r'^> Release editor:.*?\n\n', '', notes, flags=re.S | re.M)
    notes = notes.replace('<COMMIT>', f'`{commit}`', 1).replace('<COMMIT>', f'`{nostrc}`', 1)
    replacements = {
        'VERSION': version, 'DATE': now.date().isoformat(),
        'USER_VISIBLE_CHANGES': 'Preview installers for the Linux host/client, Linux portable client/manager and macOS host/client.',
        'KNOWN_LIMITATIONS': 'No Windows MSI or Intel Mac DMG. Help/codec smokes are not desktop, GPU, or signer qualification.',
        'ARTIFACT_OS_ARCH_URL_AND_SHA256': '\n\n| Download | Target | Bytes | Signing | Smoke | Codec policy | SHA-256 |\n'
            '|---|---|---:|---|---|---|---|\n' + '\n'.join(rows) + '\n\n' +
            f'{link(source)} · {link("SHA256SUMS")}. Draft assets can be downloaded from the attachments below.',
        'SBOM_AND_MANIFEST_URL': f'{link("metadesk.cdx.json")} and {link("build-manifest.json")}',
        'REPRODUCIBILITY_URL': 'Per-bundle source/records archives below',
        'FFMPEG_SOURCE_TABLE': '\n'.join(source_rows),
        'REPLACEMENT_INSTRUCTIONS_URL': f'[Packaging instructions]({base}/blob/{commit}/packaging/README.md#lgpl-library-replacement)',
        'RELINK_OBJECTS_URL_OR_NOT_APPLICABLE': 'Not applicable to shared FFmpeg',
        'COMPONENT_SOURCE_LINKS_AND_LICENSE_TEXT_LOCATIONS': 'See per-package inventories and notices. '
            'Transitive license/source review is an owner gate before public distribution.',
        'DISTRO_COPYRIGHT_AND_SOURCE_INSTRUCTIONS': 'Use Ubuntu or Fedora package metadata and the matching distro source packages.',
    }
    for key, value in replacements.items():
        notes = notes.replace(f'<{key}>', value)
    if re.search(r'<[A-Z][A-Z_0-9/]*>', notes):
        raise ValueError('unexpanded release notes placeholders')
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        notes = notes.replace(f'../{name}', f'{base}/blob/{commit}/{name}')
    (out / 'release-notes.md').write_text(notes + '\n<!-- metadesk-release-workflow -->\n')
    print('\n'.join(rows))


if __name__ == '__main__':
    assemble(*(Path(arg) for arg in sys.argv[1:]))
