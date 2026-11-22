#!/usr/bin/env python3
"""Offline safety tests: dry-run purity, destination guards and source manifests."""
import os
import importlib.util
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

LINUX = Path(__file__).resolve().parents[1]


class Safety(unittest.TestCase):
    def test_publish_is_inert_without_execute(self):
        with tempfile.TemporaryDirectory(prefix='md-w9-test-') as directory:
            # PATH contains nothing: accidentally executing podman/cosign fails.
            result = subprocess.run(['/usr/bin/python3', str(LINUX / 'publish.py'),
                                     'localhost/md-w9-test:local', 'ghcr.io/owner/metadesk:test'],
                                    cwd=directory, env={'PATH': directory}, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('DRY RUN', result.stdout)
            self.assertNotIn('--allow-insecure', result.stdout)
            self.assertNotIn('--insecure-ignore', result.stdout)
            self.assertIn('@sha256:RESOLVED_AFTER_PUSH', result.stdout)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_unqualified_registry_is_rejected(self):
        result = subprocess.run(['/usr/bin/python3', str(LINUX / 'publish.py'), 'image', 'metadesk:release'], capture_output=True)
        self.assertEqual(result.returncode, 2)

    def test_demo_rejects_remote_registry(self):
        result = subprocess.run(['/usr/bin/python3', str(LINUX / 'publish.py'), 'image',
                                 'ghcr.io/owner/metadesk:test', '--local-demo'], capture_output=True)
        self.assertEqual(result.returncode, 2)

    def test_execute_requires_explicit_keys(self):
        result = subprocess.run(['/usr/bin/python3', str(LINUX / 'publish.py'), 'image',
                                 'ghcr.io/owner/metadesk:test', '--execute'], capture_output=True)
        self.assertEqual(result.returncode, 2)

    def test_static_overlay_refuses_existing_output(self):
        with tempfile.TemporaryDirectory(prefix='md-w9-test-') as d:
            result = subprocess.run(['/usr/bin/python3', str(LINUX / 'static-json.py'), d, d], capture_output=True)
            self.assertEqual(result.returncode, 2)

    def test_static_overlay_drops_stale_archive_members(self):
        with tempfile.TemporaryDirectory(prefix='md-w9-test-') as d:
            root = Path(d)
            source = root / 'build'
            lib = source / 'libjson'
            lib.mkdir(parents=True)
            manifest = lib / 'CMakeFiles/nostr_json.dir/link.txt'
            manifest.parent.mkdir(parents=True)
            for name in ('current', 'stale'):
                c = root / (name + '.c')
                c.write_text('int ' + name + '(void) { return 0; }')
                subprocess.run(['cc', '-c', str(c), '-o', str(lib / (name + '.o'))], check=True)
            subprocess.run(['ar', 'crs', str(lib / 'libnostr-json.a'), str(lib / 'stale.o')], check=True)
            manifest.write_text('cc -shared -o libnostr-json.so current.o')
            subprocess.run(['/usr/bin/python3', str(LINUX / 'static-json.py'), str(source), str(root / 'overlay')], check=True, capture_output=True)
            contents = subprocess.check_output(['ar', 't', str(root / 'overlay/libjson/libnostr-json.a')], text=True)
            self.assertEqual([name for name in contents.splitlines() if name.endswith('.o')], ['current.o'])

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('readelf'), 'ELF consumer fixture requires Linux readelf')
    def test_consumer_rejects_producer_rpath(self):
        dockerfile = (LINUX.parents[1] / 'deploy/containers/Dockerfile.agent').read_text()
        line = next(line.strip() for line in dockerfile.splitlines() if line.strip().startswith('&& find bin lib '))
        guard = line.removeprefix('&& ').removesuffix('\\').strip()
        with tempfile.TemporaryDirectory(prefix='md-w9-test-') as d:
            root = Path(d)
            (root / 'bin').mkdir()
            (root / 'lib').mkdir()
            source = root / 'fixture.c'
            source.write_text('int fixture(void) { return 0; }')
            library = root / 'lib/fixture.so'
            subprocess.run(['cc', '-shared', str(source), '-Wl,-rpath,/private/producer', '-o', str(library)], check=True)
            self.assertNotEqual(subprocess.run(['sh', '-c', guard], cwd=root, capture_output=True).returncode, 0)
            library.unlink()
            subprocess.run(['cc', '-shared', str(source), '-o', str(library)], check=True)
            self.assertEqual(subprocess.run(['sh', '-c', guard], cwd=root, capture_output=True).returncode, 0)

    def test_no_desktop_gate_requires_application_diagnostic(self):
        with tempfile.TemporaryDirectory(prefix='md-w9-test-') as d:
            root = Path(d)
            podman = root / 'podman'
            podman.write_text("""#!/bin/sh
if [ "$1" = run ]; then
    case " $* " in
        *" md-w9-fixture-direct "*)
            printf '%s\\n' "$MD_FAKE_DIRECT_DIAGNOSTIC" >&2
            exit "$MD_FAKE_DIRECT_STATUS" ;;
    esac
fi
exit 0
""")
            podman.chmod(0o755)
            cases = [(127, 'missing timeout'), (126, 'permission denied'),
                     (134, 'aborted'), (1, 'unrelated error'),
                     (1, 'host[mcp]: accessibility backend unavailable')]
            for status, diagnostic in cases:
                with self.subTest(status=status, diagnostic=diagnostic):
                    env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'],
                               MD_RECEIPT_DIR=str(root / 'receipts'), MD_FAKE_DIRECT_STATUS=str(status),
                               MD_FAKE_DIRECT_DIAGNOSTIC=diagnostic)
                    result = subprocess.run(['sh', str(LINUX / 'verify-image.sh'), 'image', 'md-w9-fixture'],
                                            env=env, capture_output=True)
                    expected = status == 1 and diagnostic.startswith('host[mcp]: accessibility backend unavailable')
                    self.assertEqual(result.returncode == 0, expected)

    def test_staged_ffmpeg_preserves_verified_origin(self):
        spec = importlib.util.spec_from_file_location('linux_produce', LINUX / 'produce.py')
        producer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(producer)
        with tempfile.TemporaryDirectory(prefix='md-w9-test-') as d:
            root = Path(d)
            prefix, staged, system = root / 'verified', root / 'staged', root / 'system'
            for directory in (prefix / 'lib', staged, system):
                directory.mkdir(parents=True)
            reference = prefix / 'lib/libavcodec.so.62'
            reference.write_bytes(b'verified-minimal-codec')
            producer.require_minimal_ffmpeg_origin(reference, staged, prefix)
            copied = staged / reference.name
            shutil.copy2(reference, copied)
            producer.require_minimal_ffmpeg_origin(copied, staged, prefix)
            copied.write_bytes(b'unverified-codec')
            with self.assertRaises(ValueError):
                producer.require_minimal_ffmpeg_origin(copied, staged, prefix)
            outside = system / reference.name
            shutil.copy2(reference, outside)
            with self.assertRaises(ValueError):
                producer.require_minimal_ffmpeg_origin(outside, staged, prefix)
            with self.assertRaises(ValueError):
                producer.require_minimal_ffmpeg_origin(prefix / 'lib/libavformat.so.62', staged, prefix)

    def test_release_checksums_are_not_skipped(self):
        text = (LINUX / 'aur/PKGBUILD').read_text()
        self.assertNotIn("=('SKIP')", text)
        self.assertIn('OWNER_SHA256_AARCH64', text)


if __name__ == '__main__':
    unittest.main()
