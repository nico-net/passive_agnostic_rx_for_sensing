import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('validate_raw', Path(__file__).with_name('validate_raw.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RawValidation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.info = dict(status='COMPLETE_CONTIGUOUS_RAW_IQ', format='sc16_le', samples_per_channel=12,
                         first_sample_tick=100, channels=4, sample_rate_hz=122880000, blocks=2,
                         rx_metadata_errors=0, timestamp_discontinuities=0)
        (self.root / 'capture_info.json').write_text(json.dumps(self.info))
        (self.root / 'timestamps.tsv').write_text('offset_samples\tcount_samples\tfirst_sample_tick\n0\t8\t100\n8\t4\t108\n')
        for channel in range(4):
            (self.root / f'rx{channel}.sc16').write_bytes(bytes(48))
        for name in ['nic_missed_before.txt', 'nic_missed_after.txt']:
            (self.root / name).write_text('0\n')

    def test_valid_extents_not_a_signal_decode_claim(self):
        self.assertEqual(module.validate(self.root)['samples_per_channel'], 12)

    def test_timestamp_gap_is_void(self):
        (self.root / 'timestamps.tsv').write_text('offset_samples\tcount_samples\tfirst_sample_tick\n0\t8\t100\n8\t4\t109\n')
        with self.assertRaisesRegex(ValueError, 'discontinuity'):
            module.validate(self.root)

    def test_partial_channel_is_void(self):
        (self.root / 'rx3.sc16').write_bytes(bytes(44))
        with self.assertRaisesRegex(ValueError, 'byte count'):
            module.validate(self.root)

    def test_packet_loss_is_void(self):
        (self.root / 'nic_missed_after.txt').write_text('1\n')
        with self.assertRaisesRegex(ValueError, 'NIC missed'):
            module.validate(self.root)


if __name__ == '__main__':
    unittest.main()
