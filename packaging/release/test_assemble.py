#!/usr/bin/env python3
"""Exercise release assembly/negative gates with fixture installer contents."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import assemble
import source_offers


class AssemblyTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
        self.nostrc = 'a' * 40
        environment = patch.dict(os.environ, TAG='v0.1.0-alpha.1', GITHUB_SHA=self.commit,
                                 NOSTRC_REVISION=self.nostrc, GITHUB_REPOSITORY='owner/repo')
        environment.start()
        self.addCleanup(environment.stop)
        original = assemble.TEMPLATE.parent
        template = root / assemble.TEMPLATE.name
        shutil.copyfile(assemble.TEMPLATE, template)
        (root / 'macos').mkdir()
        (root / 'flatpak').mkdir()
        (root / 'ffmpeg').mkdir()
        (root / 'appimage').mkdir()
        shutil.copy2(original / 'ffmpeg/profile-encoders.sh', root / 'ffmpeg/')
        self.source = b'exact pinned upstream archive fixture'
        pin = {'type': 'archive', 'name': 'ffmpeg', 'url': 'https://example.invalid/ffmpeg-1.2.3.tar.xz',
               'sha256': hashlib.sha256(self.source).hexdigest()}
        (root / 'macos/deps.lock.json').write_text(json.dumps([dict(pin, archive='ffmpeg.tar.xz')]))
        (root / 'flatpak/modules.json').write_text(json.dumps({'modules': [{'name': 'ffmpeg-lgpl', 'sources': [pin]}]}))
        template_patch = patch.object(assemble, 'TEMPLATE', template)
        template_patch.start()
        self.addCleanup(template_patch.stop)
        source_patch = patch.object(source_offers, 'PACKAGING', root)
        source_patch.start()
        self.addCleanup(source_patch.stop)
        (root / 'release').mkdir()
        (root / 'release/reviewed-source-pins.json').write_text('{}')
        runtime_tree = {'release': 'fixture', 'additional_notices': {}, 'sources': {
            'runtime.tar.gz': {'sha256': hashlib.sha256(self.source).hexdigest(), 'notices': ['LICENSE']}}}
        (root / 'appimage/runtime-source.lock.json').write_text(json.dumps(runtime_tree))
        (root / 'appimage/tools.lock.json').write_text(json.dumps([
            {'url': f'https://test.invalid/fixture/runtime-{arch}', 'sha256': 'c' * 64}
            for arch in ('aarch64', 'x86_64')]))
        self.packages = root / 'packages'
        job = self.packages / 'job'
        job.mkdir(parents=True)
        self.output = root / 'assets'
        for platform, arch in assemble.MATRIX:
            name = f'{platform}-{arch}.fixture'
            (job / name).write_bytes(b'binary fixture')
            libraries = {f'{dll}.so.0': {'sha256': 'b' * 64} for dll in source_offers.FFMPEG}
            inventory = {'libraries': libraries}
            if platform == 'dmg':
                inventory = {'pins': [], 'bundles': {
                    app: {'private_libraries': {f'{dll}.0.dylib': {'origin': '/fixture', 'sha256': 'b' * 64}
                                                for dll in source_offers.FFMPEG},
                          'bundle_identifier': f'fixture.{exe}', 'main_executable': exe}
                    for app, exe in [('Metadesk.app', 'metadesk-client'),
                                     ('Metadesk Manager.app', 'metadesk-macos-manager')]
                }}
            elif platform in assemble.NATIVE:
                inventory = {'private_origins': {}}
            elif platform == 'appimage':
                closure = job / (name + '.closure')
                package = closure / 'ubuntu/example_1%3A1-1'
                package.mkdir(parents=True)
                (package / 'example_1.orig.tar.xz').write_bytes(self.source)
                (package / 'example_1-1.dsc').write_text(
                    f'Source: example\nVersion: 1:1-1\nChecksums-Sha256:\n '
                    f'{hashlib.sha256(self.source).hexdigest()} {len(self.source)} example_1.orig.tar.xz\n')
                (closure / 'notices/common-licenses').mkdir(parents=True)
                notice = closure / 'notices/copyright'
                notice.write_text('fixture copyright')
                libraries['libexample.so.1'] = {'ubuntu': {
                    'source': 'example', 'version': '1:1-1', 'notice': 'copyright',
                    'notice_sha256': assemble.sha(notice),
                    'files': {f.name: assemble.sha(f) for f in package.iterdir()}}}
                runtime = closure / 'runtime'
                runtime.mkdir()
                (runtime / 'runtime.tar.gz').write_bytes(self.source)
                (runtime / 'runtime.tar.gz.LICENSE').write_text('fixture license')
                (closure / 'REBUILD.md').write_text('fixture recipe')
                inventory['runtime'] = runtime_tree | {'sha256': 'c' * 64, 'license': 'fixture'}
            (job / (name + '.json')).write_text(json.dumps(dict(
                asset=name, platform=platform, arch=arch, revision=self.commit, nostrc=self.nostrc,
                signing='unsigned', inventory=inventory)))
            if platform in assemble.NATIVE:
                continue
            receipt = job / (name + '.ffmpeg')
            (receipt / 'records').mkdir(parents=True)
            (receipt / 'notices').mkdir()
            filename = 'ffmpeg.tar.xz' if platform == 'dmg' else 'ffmpeg-1.2.3.tar.xz'
            (receipt / filename).write_bytes(self.source)
            for item in ('notices/COPYING.LGPLv2.1', 'records/configure-args.txt', 'records/configure.txt',
                         'records/config.h', 'records/config_components.h', 'records/config.log',
                         'records/toolchain.txt', 'THIRD_PARTY_NOTICES.md', 'build-minimal-lgpl.sh'):
                (receipt / item).write_text('fixture')
            profile = {'dmg': 'macos', 'appimage': 'container'}.get(platform, 'flatpak')
            encoders = subprocess.check_output([root / 'ffmpeg/profile-encoders.sh', profile, arch], text=True)
            (receipt / 'records/profile.txt').write_text(profile)
            (receipt / 'records/encoders.txt').write_text(encoders)
            codec_pass = 'PASS: exact minimal codec/parser inventory; LGPL-2.1-or-later\n'
            (receipt / 'records/codec-inventory.txt').write_text(
                f'Metadesk.app\n{codec_pass}Metadesk Manager.app\n{codec_pass}' if platform == 'dmg' else codec_pass)

    def test_complete_matrix(self):
        with patch('builtins.print'):
            assemble.assemble(self.packages, self.output)
        notes = (self.output / 'release-notes.md').read_text()
        self.assertIn('<!-- metadesk-release-workflow -->', notes)
        self.assertEqual(notes.count(' | PASS | '), 11)
        self.assertEqual(len(list(self.output.glob('*.ffmpeg-source.tar.gz'))), 7)
        bom = json.loads((self.output / 'metadesk.cdx.json').read_text())
        self.assertEqual(bom['bomFormat'], 'CycloneDX')
        self.assertEqual(sum(c['type'] == 'application' for c in bom['components']), 11)
        dmg = next(c for c in bom['components'] if c['name'].startswith('dmg-'))
        manager = next(c for c in dmg['components'] if c['name'] == 'Metadesk Manager.app')
        self.assertEqual(manager['components'][0]['hashes'][0]['content'], 'b' * 64)
        manifest = json.loads((self.output / 'build-manifest.json').read_text())
        dmg_receipt = next(c for c in manifest if c['platform'] == 'dmg')
        self.assertIn('libavcodec.0.dylib', dmg_receipt['source_offers']['Metadesk Manager.app'])
        checks = (self.output / 'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(checks), 23)
        for row in checks:
            digest, name = row.split('  ', 1)
            self.assertEqual(assemble.sha(self.output / name), digest)
        with gzip.open(self.output / 'metadesk-0.1.0-alpha.1-source.tar.gz', 'rb') as stream:
            sha = subprocess.check_output(['git', 'get-tar-commit-id'], input=stream.read()).decode().strip()
        self.assertEqual(sha, self.commit)

    def test_missing_appimage_source_member(self):
        next(self.packages.glob('*/*.closure/ubuntu/*/*.tar.xz')).unlink()
        with self.assertRaisesRegex(ValueError, 'missing source member'):
            assemble.assemble(self.packages, self.output)
        self.assertFalse((self.output / 'SHA256SUMS').exists())

    def test_unclassified_flatpak_dso(self):
        path = next(self.packages.glob('*/flatpak-Client*.json'))
        payload = json.loads(path.read_text())
        payload['inventory']['libraries']['lib/libunknown.so.0'] = {'sha256': 'b' * 64}
        path.write_text(json.dumps(payload))
        with self.assertRaisesRegex(ValueError, 'no reviewed source/license route'):
            assemble.assemble(self.packages, self.output)
        self.assertFalse((self.output / 'SHA256SUMS').exists())

    def test_unclassified_manager_dylib(self):
        path = next(self.packages.glob('*/dmg-*.json'))
        payload = json.loads(path.read_text())
        payload['inventory']['bundles']['Metadesk Manager.app']['private_libraries']['libunknown.dylib'] = {}
        path.write_text(json.dumps(payload))
        with self.assertRaisesRegex(ValueError, 'no reviewed source/license route for libunknown'):
            assemble.assemble(self.packages, self.output)
        self.assertFalse((self.output / 'SHA256SUMS').exists())

    def test_missing_manager_codec_receipt(self):
        path = next(self.packages.glob('*/dmg-*.ffmpeg/records/codec-inventory.txt'))
        path.write_text(path.read_text().split('Metadesk Manager.app')[0])
        with self.assertRaisesRegex(ValueError, 'missing runtime codec verification for both DMG apps'):
            assemble.assemble(self.packages, self.output)

    def test_unexpected_top_level_dmg_inventory_key(self):
        path = next(self.packages.glob('*/dmg-*.json'))
        payload = json.loads(path.read_text())
        payload['inventory']['private_libraries'] = {'libunknown.dylib': {}}
        path.write_text(json.dumps(payload))
        with self.assertRaisesRegex(ValueError, 'inventory must contain exactly pins and bundles'):
            assemble.assemble(self.packages, self.output)
        self.assertFalse((self.output / 'SHA256SUMS').exists())

    def test_missing_manager_inventory(self):
        path = next(self.packages.glob('*/dmg-*.json'))
        payload = json.loads(path.read_text())
        del payload['inventory']['bundles']['Metadesk Manager.app']
        path.write_text(json.dumps(payload))
        with self.assertRaisesRegex(ValueError, 'expected measured inventories for both apps'):
            assemble.assemble(self.packages, self.output)

    def test_missing_cell_fails_closed(self):
        next(self.packages.glob('*/deb-aarch64*.json')).unlink()
        with self.assertRaisesRegex(ValueError, 'missing required package cells'):
            assemble.assemble(self.packages, self.output)
        self.assertFalse((self.output / 'SHA256SUMS').exists())

    def test_corresponding_source_pin_mismatch(self):
        next(self.packages.glob('*/*.ffmpeg/*.tar.xz')).write_bytes(b'mismatched source')
        with self.assertRaisesRegex(ValueError, 'source archive does not match producer pin'):
            assemble.assemble(self.packages, self.output)
        self.assertFalse((self.output / 'SHA256SUMS').exists())


class GitHubReleaseTest(unittest.TestCase):
    def test_draft_and_publication_guards(self):
        # Simulate GitHub and ls-remote; no real tags or releases are created.
        git = shutil.which('git')
        commit = subprocess.check_output([git, 'rev-parse', 'HEAD'], text=True).strip()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = root / 'bin'
            tools.mkdir()
            (tools / 'git').write_text('#!/bin/sh\ncase " $* " in *" ls-remote "*) printf "%s" "$REFS"; exit 0;; esac\n'
                                       f'exec "{git}" "$@"\n')
            (tools / 'gh').write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$CALLS"\n'
                                      'case "$1" in api) printf "%s\\n" "$RELEASES";; '
                                      'release) echo https://example.invalid/release;; *) exit 99;; esac\n')
            if not shutil.which('sha256sum'):
                (tools / 'sha256sum').write_text('#!/bin/sh\nexec shasum -a 256 "$@"\n')
            for tool in tools.iterdir():
                tool.chmod(0o755)
            assets = root / 'assets'
            assets.mkdir()
            (assets / 'installer').write_bytes(b'fixture')
            (assets / 'SHA256SUMS').write_text(f'{assemble.sha(assets / "installer")}  installer\n')
            (assets / 'release-notes.md').write_text('fixture\n')
            calls = root / 'calls'
            tag = 'v0.1.0-alpha.1'
            ref = f'refs/tags/{tag}'
            env = dict(os.environ, PATH=f'{tools}:{os.environ["PATH"]}', RUNNER_TEMP=temporary,
                       TAG=tag, PUBLISH='false', GITHUB_SHA=commit, GITHUB_EVENT_NAME='workflow_dispatch',
                       GITHUB_REPOSITORY='owner/repo', GITHUB_STEP_SUMMARY=str(root / 'summary'),
                       CALLS=str(calls), RELEASES='[]', REFS='')
            scenarios = [
                ({}, f'--draft --target {commit}'),
                ({'PUBLISH': 'true', 'GITHUB_REF': 'refs/heads/task/md-6mqr'}, None),
                ({'PUBLISH': 'true', 'GITHUB_EVENT_NAME': 'push', 'GITHUB_REF': ref}, None),
                ({'PUBLISH': 'true', 'GITHUB_EVENT_NAME': 'push', 'GITHUB_REF': ref,
                  'REFS': f'{commit}\t{ref}\n'}, '--verify-tag --prerelease'),
                ({'RELEASES': json.dumps([{'id': 1, 'draft': True, 'tag_name': tag, 'body': 'owner draft'}])}, None),
            ]
            for changes, expected in scenarios:
                with self.subTest(changes=changes):
                    calls.unlink(missing_ok=True)
                    result = subprocess.run(['bash', Path(__file__).with_name('github.sh'), assets],
                                            env=env | changes, capture_output=True, text=True)
                    argv = calls.read_text() if calls.exists() else ''
                    if expected is None:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertNotIn('release create', argv)
                        self.assertNotIn('DELETE', argv)
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn(expected, argv)


if __name__ == '__main__':
    unittest.main()
