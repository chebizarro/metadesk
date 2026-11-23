#!/usr/bin/env python3
"""Portable parser regressions; real installed/bundle checks run on macOS."""
import unittest
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path
import macho as m

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
        self.pin = {key: self.git('rev-parse', f'HEAD^{{{key}}}') for key in ('commit', 'tree')}

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.repo), *args], text=True).strip()

    def test_export_ignores_working_tree(self):
        (self.repo / 'file').write_text('uncommitted')
        dest = self.root / 'source'
        export_git_source(self.pin, dest, local_repo=self.repo)
        self.assertEqual(list(dest.iterdir()), [dest / 'file'])
        self.assertEqual((dest / 'file').read_text(), 'locked')

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
