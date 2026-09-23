import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from validate_streamed_raw import validate


class StreamValidationTests(unittest.TestCase):
    def fixture(self, root):
        data = bytes(40)
        info = dict(recorder='bounded_stream_v1', status='COMPLETE_CONTIGUOUS_RAW_IQ', format='sc16_le',
                    channels=1, samples_per_channel=10, requested_samples_per_channel=10,
                    sample_rate_hz=1000, first_sample_tick=100, timestamp_tick_rate_hz=1000,
                    blocks=2, rx_metadata_errors=0)
        (root / 'capture_info.json').write_text(json.dumps(info))
        (root / 'rx0.sc16').write_bytes(data)
        (root / 'timestamps.tsv').write_text('offset_samples\tfirst_tick\tsamples\n0\t100\t6\n6\t106\t4\n')
        (root / 'nic_before.txt').write_text('0\n')
        (root / 'nic_after.txt').write_text('0\n')
        (root / 'IQ_SHA256SUMS').write_text(hashlib.sha256(data).hexdigest() + '  rx0.sc16\n')

    def test_complete_recording(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); self.fixture(root)
            self.assertEqual(validate(root)['samples_per_channel'], 10)

    def test_timestamp_gap_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); self.fixture(root)
            (root / 'timestamps.tsv').write_text('offset_samples\tfirst_tick\tsamples\n0\t100\t6\n6\t107\t4\n')
            with self.assertRaises(ValueError): validate(root)

    def test_partial_file_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); self.fixture(root)
            (root / 'rx0.sc16').write_bytes(bytes(36))
            with self.assertRaises(ValueError): validate(root)

    def test_nic_loss_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); self.fixture(root)
            (root / 'nic_after.txt').write_text('1\n')
            with self.assertRaises(ValueError): validate(root)

    def test_incomplete_status_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); self.fixture(root)
            info = json.loads((root / 'capture_info.json').read_text())
            info['status'] = 'VOID'
            (root / 'capture_info.json').write_text(json.dumps(info))
            with self.assertRaises(ValueError): validate(root)


if __name__ == '__main__':
    unittest.main()
