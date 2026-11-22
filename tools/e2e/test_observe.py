#!/usr/bin/env python3
"""Owned-process runner regression tests; no desktop, relay or real input."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).with_name('observe.py')


class ObserveTests(unittest.TestCase):
    def run_case(self, code, deadline=10):
        with tempfile.TemporaryDirectory(prefix='md-l7-observe-') as root:
            path = Path(root)
            result = subprocess.run([sys.executable, str(RUNNER), '--name', 'test',
                '--pid-file', str(path / 'pid'), '--out', root,
                '--deadline', str(deadline), '--', sys.executable, '-c', code],
                capture_output=True, text=True, timeout=15)
            records = [json.loads(line) for line in (path / 'test.jsonl').read_text().splitlines()]
            self.assertFalse((path / 'pid').exists())
            return result, records

    def test_exit_and_output(self):
        result, records = self.run_case("print('owned marker'); raise SystemExit(7)")
        self.assertEqual(result.returncode, 7)
        self.assertTrue(any(r.get('line') == 'owned marker' for r in records))
        self.assertEqual(records[-1]['exit_code'], 7)
        self.assertFalse(records[-1]['deadline'])

    def test_deadline_reaps_child(self):
        result, records = self.run_case('import signal; signal.pause()', deadline=0.2)
        self.assertEqual(result.returncode, 124)
        self.assertTrue(records[-1]['deadline'])
        with self.assertRaises(ProcessLookupError):
            os.kill(records[0]['child_pid'], 0)

    def test_cancellation_reaps_child(self):
        with tempfile.TemporaryDirectory(prefix='md-l7-observe-') as root:
            pidfile = Path(root) / 'pid'
            runner = subprocess.Popen([sys.executable, str(RUNNER), '--name', 'cancel',
                '--pid-file', str(pidfile), '--out', root, '--', sys.executable,
                '-c', "import signal; print('ready', flush=True); signal.pause()"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                spawn = json.loads(runner.stdout.readline())
                ready = json.loads(runner.stdout.readline())
                self.assertEqual(ready['line'], 'ready')
                runner.send_signal(signal.SIGTERM)
                stdout, stderr = runner.communicate(timeout=10)
                self.assertEqual(runner.returncode, 128 + signal.SIGTERM, stderr)
                self.assertFalse(pidfile.exists())
                with self.assertRaises(ProcessLookupError):
                    os.kill(spawn['child_pid'], 0)
            finally:
                if runner.poll() is None:
                    runner.kill()
                    runner.wait()

    def test_existing_pidfile_preserved(self):
        with tempfile.TemporaryDirectory(prefix='md-l7-observe-') as root:
            pidfile = Path(root) / 'pid'
            pidfile.write_text('preexisting owner\n')
            marker = Path(root) / 'must-not-run'
            result = subprocess.run([sys.executable, str(RUNNER), '--name', 'occupied',
                '--pid-file', str(pidfile), '--out', root, '--', sys.executable,
                '-c', 'from pathlib import Path; Path(' + repr(str(marker)) + ').touch()'],
                capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(pidfile.read_text(), 'preexisting owner\n')
            self.assertFalse(marker.exists())

    def test_missing_executable(self):
        with tempfile.TemporaryDirectory(prefix='md-l7-observe-') as root:
            pidfile = Path(root) / 'pid'
            result = subprocess.run([sys.executable, str(RUNNER), '--name', 'missing',
                '--pid-file', str(pidfile), '--out', root, '--', root + '/absent'],
                capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(pidfile.exists())


if __name__ == '__main__':
    unittest.main()
