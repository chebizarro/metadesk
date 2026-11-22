#!/usr/bin/env python3
"""Portable parser regressions; real installed/bundle checks run on macOS."""
import unittest
import macho as m

class MinimumTests(unittest.TestCase):
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
