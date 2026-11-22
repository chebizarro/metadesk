"""Receipt classification only; not a substitute for real-backend evidence."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('recovery', Path(__file__).with_name('validate-video-recovery.py'))
recovery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recovery)


class Receipts(unittest.TestCase):
    def test_clean_stream(self):
        row = recovery.classify('buffer acquire gen=1\n', 50)
        self.assertTrue(row['clean_stream'])
        self.assertFalse(row['recovered'])

    def test_zero_frame_allocation_recovery(self):
        row = recovery.classify('reject buffer: type=4294967295 flags=0 mapped=0 max=0\n'
                                'restarting capture (attempt 1)\nbuffer acquire gen=2\n', 50)
        self.assertTrue(row['allocation_skeleton'])
        self.assertTrue(row['initial_zero_frames'])
        self.assertTrue(row['recovered'])
        self.assertFalse(row['clean_stream'])

    def test_later_fault_is_not_initial_zero(self):
        row = recovery.classify('buffer acquire gen=1\nrestarting capture (attempt 1)\n', 50)
        self.assertFalse(row['initial_zero_frames'])
        self.assertFalse(row['recovered'])
        self.assertEqual(row['restarts'], 1)

    def test_exhaustion_is_not_recovery(self):
        row = recovery.classify('restarting capture (attempt 1)\nrestarting capture (attempt 2)\n', 0)
        self.assertEqual(row['restarts'], 2)
        self.assertFalse(row['recovered'])
        self.assertFalse(row['clean_stream'])

    def test_failed_preparation_cannot_pass(self):
        row = {'decoded': 50, 'client_bound': True, 'client_exit': 0, 'host_exit': 143,
               'authorization_retained': True, 'preparation_valid': False}
        self.assertIn('preparation cells failed', recovery.row_failures(row))

    def test_injection_label_is_not_proof(self):
        row = {'decoded': 50, 'client_bound': True, 'client_exit': 0, 'host_exit': 143,
               'authorization_retained': True, 'preparation_valid': True}
        self.assertEqual(recovery.row_failures(row), [])
        self.assertTrue(recovery.row_failures(row, injected=True))

    def test_complete_injection_and_contamination(self):
        row = recovery.classify('FAULT_INJECTION no-first-frame destroyed dropped=20 passthrough=0\n'
                                'PipeWire first-frame deadline exceeded: no valid frame within 5000ms\n'
                                'restarting capture (attempt 1)\nbuffer acquire gen=2\n', 50)
        row.update(decoded=50, client_bound=True, client_exit=0, host_exit=143,
                   authorization_retained=True, preparation_valid=True)
        self.assertEqual(recovery.row_failures(row, injected=True), [])
        row['rejected_descriptors'] = 1
        self.assertIn('natural descriptor fault contaminated injection', recovery.row_failures(row, injected=True))

    def test_descriptor_signatures_cannot_cross_log_records(self):
        row = recovery.classify('reject buffer: format=7 type=4294967295 flags=0 mapped=1 max=16 '
                                'INFO: reject buffer: format=7 type=2 flags=0 mapped=0 max=0 ', 0)
        self.assertFalse(row['allocation_skeleton'])

    def test_deadline_signature(self):
        row = recovery.classify('PipeWire first-frame deadline exceeded: no valid frame within 5000ms\n'
                                'restarting capture (attempt 1)\nbuffer acquire gen=2\n', 50)
        self.assertTrue(row['first_frame_deadline'])
        self.assertTrue(row['recovered'])
        self.assertFalse(row['allocation_skeleton'])


if __name__ == '__main__':
    unittest.main()
