#!/usr/bin/env python3
"""Portable parser regressions; real installed/bundle checks run on macOS."""
import unittest
from unittest.mock import patch
import json
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path
import macho as m
from bundle_app import bundle_product, sign

import measure_dmg

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from git_source import export_git_source, verify_git_source


class GitSourceTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        self.git('init', '--quiet', '--template=', '--object-format=sha1')
        (self.repo / 'file').write_text('locked')
        self.git('add', 'file')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.test',
                 '-c', 'commit.gpgsign=false', 'commit', '--quiet', '-m', 'fixture')
        self.pin = {'commit': self.git('rev-parse', 'HEAD'),
                    'tree': self.git('show', '-s', '--format=%T', 'HEAD')}

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.repo), *args], text=True).strip()

    def test_export_ignores_working_tree(self):
        (self.repo / 'file').write_text('uncommitted')
        dest = self.root / 'source'
        export_git_source(self.pin, dest, local_repo=self.repo)
        self.assertEqual([entry.name for entry in dest.iterdir()], ['file'])
        self.assertEqual((dest / 'file').read_text(), 'locked')

    def test_rejects_non_commit_object(self):
        with self.assertRaisesRegex(ValueError, 'not a commit'):
            verify_git_source(self.repo, self.pin['tree'], self.pin | {'commit': self.pin['tree']})

    def test_commit_mismatch_with_matching_tree(self):
        with self.assertRaisesRegex(ValueError, 'Git commit mismatch'):
            verify_git_source(self.repo, 'HEAD', self.pin | {'commit': '0' * 40})

    def test_tree_mismatch(self):
        dest = self.root / 'source'
        with self.assertRaisesRegex(ValueError, 'Git tree mismatch'):
            export_git_source(self.pin | {'tree': '0' * 40}, dest, local_repo=self.repo)
        self.assertFalse(dest.exists())

    def test_missing_or_short_object_ids(self):
        for field in ('commit', 'tree'):
            with self.subTest(field=field):
                pin = dict(self.pin)
                del pin[field]
                with self.assertRaisesRegex(ValueError, field):
                    verify_git_source(self.repo, 'HEAD', pin)
                with self.assertRaisesRegex(ValueError, field):
                    verify_git_source(self.repo, 'HEAD', self.pin | {field: '0123456'})


class ManagerBundleTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.pins = self.root / 'pins.json'
        self.pins.write_text('[]')
        for name, (main, companion) in measure_dmg.PRODUCTS.items():
            contents = self.root / name / 'Contents'
            for directory in ('MacOS', 'Frameworks', 'Resources'):
                (contents / directory).mkdir(parents=True)
            plist = {'CFBundleName': name.removesuffix('.app'), 'CFBundlePackageType': 'APPL',
                     'CFBundleIdentifier': f'fixture.{main}', 'CFBundleExecutable': main}
            (contents / 'Info.plist').write_bytes(plistlib.dumps(plist))
            images = [f'MacOS/{main}', f'MacOS/{companion}', 'Frameworks/libfixture.dylib']
            for image in images:
                (contents / image).write_bytes(bytes.fromhex('cffaedfe') + name.encode())
            (contents / 'Resources/metadesk-bundle.json').write_text(json.dumps({
                'image_minos': {f'Contents/{image}': '13.0' for image in images},
                'private_libraries': {'libfixture.dylib': '/fixture/build/libfixture.dylib'},
            }))
        self.manager = self.root / 'Metadesk Manager.app'

    def test_manager_plist_and_signing_order(self):
        # Use the real manager metadata template, not a second identifier.
        template = Path(__file__).resolve().parents[2] / 'src/macos_manager/Info.plist.in'
        plist = self.manager / 'Contents/Info.plist'
        plist.write_text(template.read_text().replace('@VERSION@', '0.1.0'))
        executable, metadata = bundle_product(plist)
        self.assertEqual(executable, 'metadesk-macos-manager')
        self.assertEqual(metadata['CFBundleName'] + '.app', self.manager.name)
        self.assertEqual(bundle_product(None), ('metadesk-host', None))
        with patch('bundle_app.subprocess.run') as codesign:
            sign(self.manager, '-', False, None)
        signed = [Path(call.args[0][-1]).name for call in codesign.call_args_list]
        self.assertEqual(signed, ['libfixture.dylib', 'metadesk-client', self.manager.name])

    def test_each_bundles_actual_library_bytes_are_measured(self):
        bundles = measure_dmg.inventory(self.root, self.pins)['bundles']
        self.assertEqual(set(bundles), set(measure_dmg.PRODUCTS))
        self.assertNotEqual(*(b['private_libraries']['libfixture.dylib']['sha256'] for b in bundles.values()))

    def test_missing_manager_client_fails(self):
        (self.manager / 'Contents/MacOS/metadesk-client').unlink()
        with self.assertRaisesRegex(ValueError, 'unexpected executable layout'):
            measure_dmg.inventory(self.root, self.pins)

    def test_undeclared_manager_library_fails(self):
        (self.manager / 'Contents/Frameworks/libunknown.dylib').write_bytes(bytes.fromhex('cffaedfe'))
        with self.assertRaisesRegex(ValueError, 'declared libraries do not match'):
            measure_dmg.inventory(self.root, self.pins)


class MinimumTests(unittest.TestCase):
    def test_agent_layout(self):
        root=Path(__file__).parent
        p=plistlib.loads((root/'dev.metadesk.host.plist').read_bytes())
        self.assertEqual(p['BundleProgram'],'Contents/Resources/metadesk-host-agent')
        self.assertIn('--managed',p['ProgramArguments'])
        self.assertEqual(p['LimitLoadToSessionType'],'Aqua')
        self.assertEqual(p['KeepAlive'],{'SuccessfulExit':False})
        self.assertNotIn('UserName',p)

    def test_all_slices(self):
        self.assertEqual(m.versions('''arm64:
Load command 1
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 13.0
      sdk 27.0
   ntools 1
     tool 3
  version 27037.1
Load command 2
      cmd LC_UUID
x86_64:
Load command 1
      cmd LC_BUILD_VERSION
    minos 14.2
      sdk 27.0
'''), ['13.0', '14.2'])

    def test_legacy(self):
        self.assertEqual(m.versions('''Load command 4
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
  version 10.15
      sdk 13.0
'''), ['10.15'])

    def test_dylib_version_is_not_minos(self):
        self.assertEqual(m.versions('''Load command 1
      cmd LC_ID_DYLIB
  version 50.0
'''), [])

    def test_normalized_comparison(self):
        self.assertEqual(m.version_key('13.0'), m.version_key('13.0.0'))
        self.assertGreater(m.version_key('13.0.1'), m.version_key('13.0'))
        self.assertGreater(m.version_key('13.10'), m.version_key('13.9'))

if __name__ == '__main__':
    unittest.main()
