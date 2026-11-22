"""Offline regression checks for mesh-video cleanup and restart-log boundaries."""
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("mesh_video", Path(__file__).with_name("validate-mesh-video.py"))
mesh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mesh)


class CleanupTests(unittest.TestCase):
    def setUp(self):
        # Expected failure receipts below are assertions, not test failures.
        self.output = io.StringIO()
        self.capture = redirect_stdout(self.output)
        self.capture.__enter__()
        self.addCleanup(self.capture.__exit__, None, None, None)

    def tearDown(self):
        mesh.results.clear()

    def test_success_requires_confirmed_absence(self):
        with patch.object(mesh, "podman", side_effect=[SimpleNamespace(returncode=0), SimpleNamespace(returncode=1)]) as pm:
            self.assertTrue(mesh.remove_resource("container", "md-l3-test"))
        self.assertEqual(pm.call_args_list[1].args, ("container", "exists", "md-l3-test"))

    def test_already_absent_is_safe(self):
        with patch.object(mesh, "podman", side_effect=[SimpleNamespace(returncode=1), SimpleNamespace(returncode=1)]):
            self.assertTrue(mesh.remove_resource("network", "md-l3-test"))

    def test_failed_removal_retains_ownership_for_final_cleanup(self):
        run = mesh.MeshRun.__new__(mesh.MeshRun)
        run.created = ["md-l3-test"]
        with patch.object(mesh, "podman", side_effect=[SimpleNamespace(returncode=125), SimpleNamespace(returncode=0)]):
            with self.assertRaises(mesh.Incomplete):
                run.remove("md-l3-test")
        self.assertEqual(run.created, ["md-l3-test"])
        self.assertEqual(mesh.results[-1]["result"], "FAIL")

    def test_exists_error_is_not_absence(self):
        with patch.object(mesh, "podman", side_effect=[SimpleNamespace(returncode=0), SimpleNamespace(returncode=125)]):
            self.assertFalse(mesh.remove_resource("volume", "md-l3-test"))

    def test_timeout_records_failure(self):
        with patch.object(mesh, "podman", side_effect=subprocess.TimeoutExpired("podman", 120)):
            self.assertFalse(mesh.remove_resource("container", "md-l3-test"))


class RestartLogTests(unittest.TestCase):
    def logs(self, lines):
        logs = mesh.Logs.__new__(mesh.Logs)
        logs.lines, logs.done, logs.cond = lines, True, threading.Condition()
        return logs

    def test_old_address_receipt_does_not_satisfy_next_occurrence(self):
        logs = self.logs(["accepted connection from fd00::1"])
        self.assertIsNone(logs.wait_nth(r"accepted connection from (\S+)", 2, 0))
        logs.lines.append("accepted connection from fd00::2")
        self.assertEqual(logs.wait_nth(r"accepted connection from (\S+)", 2, 0)[1], "fd00::2")

    def test_restart_cursor_sees_an_immediate_reconnect_but_not_old_progress(self):
        logs = self.logs(["decoded=100", "client: reconnecting in 1s"])
        self.assertIsNone(logs.wait_from(1, r"decoded=\d+", 0))
        self.assertIsNotNone(logs.wait_from(1, r"client: reconnecting in", 0))


if __name__ == "__main__":
    unittest.main()
