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
        shutil.copy2(original / 'ffmpeg/profile-encoders.sh', root / 'ffmpeg/')
        self.source = b'exact pinned upstream archive fixture'
        pin = {'type': 'archive', 'name': 'ffmpeg', 'url': 'https://example.invalid/ffmpeg-1.2.3.tar.xz',
               'sha256': hashlib.sha256(self.source).hexdigest()}
        (root / 'macos/deps.lock.json').write_text(json.dumps([pin]))
        (root / 'flatpak/modules.json').write_text(json.dumps({'modules': [{'name': 'ffmpeg-lgpl', 'sources': [pin]}]}))
        template_patch = patch.object(assemble, 'TEMPLATE', template)
        template_patch.start()
        self.addCleanup(template_patch.stop)
        self.packages = root / 'packages'
        job = self.packages / 'job'
        job.mkdir(parents=True)
        self.output = root / 'assets'
        for platform, arch in assemble.MATRIX:
            name = f'{platform}-{arch}.fixture'
            (job / name).write_bytes(b'binary fixture')
            (job / (name + '.json')).write_text(json.dumps(dict(
                asset=name, platform=platform, arch=arch, revision=self.commit, nostrc=self.nostrc,
                signing='unsigned', inventory={'libraries': {'libmetadesk': {'sha256': 'b'*64}}})))
            if platform in assemble.NATIVE:
                continue
            receipt = job / (name + '.ffmpeg')
            (receipt / 'records').mkdir(parents=True)
            (receipt / 'notices').mkdir()
            (receipt / 'ffmpeg-1.2.3.tar.xz').write_bytes(self.source)
            for item in ('notices/COPYING.LGPLv2.1', 'records/configure-args.txt', 'records/configure.txt',
                         'records/config.h', 'records/config_components.h', 'records/config.log',
                         'records/toolchain.txt', 'THIRD_PARTY_NOTICES.md', 'build-minimal-lgpl.sh'):
                (receipt / item).write_text('fixture')
            profile = {'dmg': 'macos', 'appimage': 'container'}.get(platform, 'flatpak')
            encoders = subprocess.check_output([root / 'ffmpeg/profile-encoders.sh', profile, arch], text=True)
            (receipt / 'records/profile.txt').write_text(profile)
            (receipt / 'records/encoders.txt').write_text(encoders)
            (receipt / 'records/codec-inventory.txt').write_text('PASS: exact minimal codec/parser inventory; LGPL-2.1-or-later\n')

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
        checks = (self.output / 'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(checks), 21)
        for row in checks:
            digest, name = row.split('  ', 1)
            self.assertEqual(assemble.sha(self.output / name), digest)
        with gzip.open(self.output / 'metadesk-0.1.0-alpha.1-source.tar.gz', 'rb') as stream:
            sha = subprocess.check_output(['git', 'get-tar-commit-id'], input=stream.read()).decode().strip()
        self.assertEqual(sha, self.commit)

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


if __name__ == '__main__':
    unittest.main()
