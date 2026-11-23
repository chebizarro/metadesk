#!/usr/bin/env python3
"""Small policy/wiring guards; real ELF/container evidence is in VALIDATION.md."""
import importlib.util
import json
from pathlib import Path
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('assemble', ROOT / 'packaging/appimage/assemble.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
class Packaging(unittest.TestCase):
    def test_glibc_is_not_bundled(self):
        for name in ('libc.so.6','libm.so.6','libpthread.so.0','ld-linux-aarch64.so.1','libresolv.so.2'):
            self.assertIsNotNone(mod.BASE.fullmatch(name))
        for name in ('libmetadesk.so.0','libcjson.so.1','libSDL2-2.0.so.0','libavcodec.so.62'):
            self.assertIsNone(mod.BASE.fullmatch(name))
    def test_forbidden_codecs(self):
        for name in ('libopenh264.so.8','libx264.so.164','libx265.so.209'):
            self.assertIsNotNone(mod.FORBIDDEN.search(name))
    def test_missing_closure_is_fatal(self):
        from unittest.mock import patch
        result = subprocess.CompletedProcess([],0,'libmissing.so => not found','')
        with patch.object(subprocess,'run',return_value=result):
            with self.assertRaises(ValueError): list(mod.dependencies(Path('/trusted/test'),{}))
    def test_no_host_or_broad_permissions(self):
        for product in ('Client','Manager'):
            manifest = json.loads((ROOT/f'packaging/flatpak/org.metadesk.{product}.json').read_text())
            self.assertEqual(manifest['app-id'],f'org.metadesk.{product}')
            self.assertEqual(manifest['runtime'],'org.gnome.Platform')
            self.assertIn('sh packaging/flatpak/wrap-runtime.sh '+product.lower(), manifest['modules'][-1]['build-commands'])
            self.assertEqual(set(manifest['finish-args']),{'--share=network','--socket=wayland','--socket=fallback-x11','--share=ipc','--device=dri','--talk-name=org.freedesktop.secrets'})
    def test_intel_only_vpl(self):
        mods = json.loads((ROOT/'packaging/flatpak/modules.json').read_text())['modules']
        vpl = next(m for m in mods if m['name']=='onevpl')
        self.assertEqual(vpl['only-arches'],['x86_64'])
        for module in mods:
            for source in module['sources']:
                if source['type']=='archive':
                    self.assertNotRegex(source['url'], r'codeload\.github\.com|/\+archive/|/archive/')
                    self.assertRegex(source['sha256'],r'^[0-9a-f]{64}$')
                    self.assertIn(source['archive-type'], ('tar-gzip','tar-bzip2','tar-xz'))
                if source['type']=='git': self.assertRegex(source['commit'],r'^[0-9a-f]{40}$')
    def test_frontend_staging(self):
        script = (ROOT/'packaging/flatpak/build-product.sh').read_text()
        self.assertNotIn('meson install', '\n'.join(line for line in script.splitlines() if not line.startswith('#')))
        self.assertNotIn('metadesk-host',script)
        self.assertIn('-j2',script)
if __name__ == '__main__': unittest.main()
