#!/usr/bin/env python3
"""Portable authoring tests; native MSI gates live in test-install.ps1."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).parent
spec = importlib.util.spec_from_file_location('harvest', ROOT / 'harvest.py')
harvest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harvest)
N = {'w': harvest.NS}


class PackagingTests(unittest.TestCase):
    def test_stable_component_identity_and_user_keypaths(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp) / 'stage'
            (stage / 'licenses/a').mkdir(parents=True)
            (stage / 'metadesk-host.exe').write_bytes(b'fixture')
            (stage / 'licenses/a/LICENSE').write_text('fixture')
            one, two = Path(tmp) / 'one.wxs', Path(tmp) / 'two.wxs'
            harvest.harvest(stage, one)
            harvest.harvest(stage, two)
            self.assertEqual(one.read_bytes(), two.read_bytes())
            root = ET.parse(one)
            components = root.findall('.//w:Component', N)
            self.assertEqual(len(components), 4)
            for comp in components:
                key = comp.find('w:RegistryValue', N)
                self.assertEqual(key.get('Root'), 'HKCU')
                self.assertEqual(key.get('KeyPath'), 'yes')
            self.assertEqual(len(root.findall('.//w:RemoveFolder', N)), 2)
            old = {c.get('Id'): c.get('Guid') for c in components}
            (stage / 'new.txt').write_text('fixture')
            harvest.harvest(stage, two)
            new = {c.get('Id'): c.get('Guid') for c in ET.parse(two).findall('.//w:Component', N)}
            self.assertTrue(all(new[k] == v for k, v in old.items()))

    def test_scope_upgrade_and_path(self):
        root = ET.parse(ROOT / 'Metadesk.wxs')
        package = root.find('w:Package', N)
        self.assertEqual(package.get('Scope'), 'perUser')
        self.assertEqual(package.get('UpgradeCode'), '0D682EE1-44D8-49AA-865E-EB91C50890FB')
        self.assertIsNotNone(package.find('w:MajorUpgrade', N))
        search = package.find("w:Property[@Id='INSTALLDIR']/w:RegistrySearch", N)
        self.assertEqual(search.get('Name'), 'InstallDir')
        saved = package.find(".//w:RegistryValue[@Name='InstallDir']", N)
        self.assertEqual(saved.get('Value'), '[INSTALLDIR]')
        path = package.find('.//w:Environment', N)
        self.assertEqual(path.get('System'), 'no')
        self.assertEqual(path.get('Permanent'), 'no')
        self.assertEqual(len(package.findall('.//w:Shortcut', N)), 3)
        feature = package.find("w:Feature[@Id='HostLogon']", N)
        self.assertEqual(feature.get('Level'), '2')
        remove = package.find("w:CustomAction[@Id='RemoveHostTask']", N)
        self.assertIsNotNone(remove)
        self.assertIn('-File "[INSTALLDIR]host-task.ps1"', remove.get('ExeCommand'))


if __name__ == '__main__':
    unittest.main()
