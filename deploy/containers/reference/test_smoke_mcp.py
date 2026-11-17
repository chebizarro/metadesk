#!/usr/bin/env python3
"""Synthetic smoke-gate regressions, NOT live desktop/runtime evidence."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

SERVER = """
import json, os, signal, sys
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
for line in sys.stdin:
    request = json.loads(line)
    if 'id' not in request:
        continue
    method = request['method']
    result = {'tools': []} if method == 'tools/list' else {}
    if method == 'resources/read':
        result = {'contents': [{'text': os.environ['SMOKE_TEST_TREE']}]}
    print(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}), flush=True)
"""
APP = "DSK[desktop] Desktop\n  APP[n0] gnome-calculator\n"
WINDOW = "    WIN[n1] Calculator\n"
WIDGETS = "\n".join("      " + node for node in (
    "TXT[n2] ''", "TXT[n3] ''", "BTN[n4] 0", "BTN[n5] 7", "BTN[n6] +", "BTN[n7] =")) + "\n"


class SmokeGateTests(unittest.TestCase):
    def smoke(self, tree):
        return subprocess.run(
            [sys.executable, str(Path(__file__).with_name('smoke_mcp.py')),
             '--expect', 'gnome-calculator', '--calculator-widgets', '--',
             sys.executable, '-c', SERVER],
            env=dict(os.environ, SMOKE_TEST_TREE=tree),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)

    def test_real_shaped_calculator_subtree(self):
        result = self.smoke(APP + WINDOW + WIDGETS)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('calculator widgets: PASS', result.stdout)

    def test_unrelated_app_cannot_supply_widgets(self):
        result = self.smoke(APP + WINDOW + '  APP[n8] unrelated\n    WIN[n9] Other\n' + WIDGETS)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('calculator button absent', result.stdout)

    def test_sibling_window_cannot_supply_widgets(self):
        result = self.smoke(APP + WINDOW + '    WIN[n8] Other\n' + WIDGETS)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('calculator button absent', result.stdout)

    def test_name_only_cannot_pass(self):
        result = self.smoke(APP + WINDOW)
        self.assertNotEqual(result.returncode, 0)

    def test_missing_window_does_not_disclose_desktop(self):
        secret = 'PRIVATE-DESKTOP-TEXT-SENTINEL'
        result = self.smoke(APP + '    WIN[n8] Other\n      TXT[n9] ' + secret + '\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('required calculator window subtree absent', result.stdout)
        self.assertNotIn(secret, result.stdout)


if __name__ == '__main__':
    unittest.main()
