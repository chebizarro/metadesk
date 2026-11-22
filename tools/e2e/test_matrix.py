#!/usr/bin/env python3
"""Receipt integrity tests; no desktop, network, signer, or input device."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('matrix', Path(__file__).with_name('matrix.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class MatrixTests(unittest.TestCase):
    def test_default_receipt_paths(self):
        parser = m.argument_parser()
        for command in ('init', 'report'):
            self.assertEqual(str(m.OUTPUT / 'pairings.json'), parser.parse_args([command]).file)
        args = parser.parse_args(['record', '--host', 'macos', '--client', 'windows',
                                  '--transport', 'ip', '--check', 'video',
                                  '--status', 'NOT_RUN', '--evidence', 'not exercised'])
        self.assertEqual(m.DEFAULT_LEDGER, args.file)
        self.assertIsNone(parser.parse_args(['probe']).out)
        self.assertEqual(m.OUTPUT, m.default_probe_output().parent)
        self.assertTrue(m.default_probe_output().name.startswith('native-'))
        self.assertIn('output/', Path(__file__).with_name('.gitignore').read_text().splitlines())
        self.assertIn("Join-Path $Root 'tools\\e2e\\output'",
                      Path(__file__).with_name('windows-components.ps1').read_text())

    def test_full_cartesian_product(self):
        matrix = m.new_matrix('abc')
        self.assertEqual(32, len(matrix['cells']))
        self.assertEqual(32, len({(c['host'], c['client'], c['transport']) for c in matrix['cells']}))
        for cell in matrix['cells']:
            self.assertEqual(set(m.CHECKS), set(cell['checks']))
            self.assertEqual('NOT_RUN', m.overall(cell))

    def test_no_partial_pass(self):
        cell = m.new_matrix('abc')['cells'][0]
        for key in m.CHECKS[:-1]:
            cell['checks'][key]['status'] = 'PASS'
        self.assertEqual('NOT_RUN', m.overall(cell))
        cell['checks'][m.CHECKS[-1]]['status'] = 'PASS'
        self.assertEqual('PASS', m.overall(cell))
        cell['checks']['transport']['status'] = 'BLOCKED'
        self.assertEqual('BLOCKED', m.overall(cell))
        cell['checks']['video']['status'] = 'FAIL'
        self.assertEqual('FAIL', m.overall(cell))

    def test_evidence_defect_and_failure_history(self):
        matrix = m.new_matrix('abc')
        args = (matrix, 'windows', 'macos', 'ip', 'video')
        with self.assertRaises(ValueError):
            m.record(*args, 'PASS', '')
        with self.assertRaises(ValueError):
            m.record(*args, 'FAIL', 'observed')
        m.record(*args, 'FAIL', 'observed', 'md-example')
        m.record(*args, 'PASS', 'new run, old failure retained')
        cell = next(c for c in matrix['cells'] if c['host'] == 'windows' and
                    c['client'] == 'macos' and c['transport'] == 'ip')
        self.assertEqual('md-example', cell['checks']['video']['history'][-1]['defect'])
        self.assertEqual('NOT_RUN', m.overall(cell))

    def test_launch_failure_is_a_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            r = m.run(Path(d), 'missing', [str(Path(d) / 'absent')])
            self.assertIsNone(r['exit_code'])
            self.assertIn('launch_error', r)
            self.assertIn('launch failed', (Path(d) / r['log']).read_text())

    def test_deadline_never_passes(self):
        with tempfile.TemporaryDirectory() as d:
            r = m.run(Path(d), 'deadline', [sys.executable, '-c',
                      'import threading; threading.Event().wait()'], timeout=0.1)
            self.assertIsNone(r['exit_code'])
            self.assertIn('not a PASS', (Path(d) / r['log']).read_text())

    def test_cli_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            path = str(Path(d) / 'matrix.json')
            cmd = [sys.executable, str(Path(__file__).with_name('matrix.py'))]
            subprocess.run([*cmd, 'init', path, '--commit', 'fixture'], check=True)
            p = subprocess.run([*cmd, 'report', path], check=True, capture_output=True, text=True)
            self.assertEqual(32, p.stdout.count('| NOT_RUN |'))
            p = subprocess.run([*cmd, 'init', path], capture_output=True)
            self.assertNotEqual(0, p.returncode)


if __name__ == '__main__':
    unittest.main()
