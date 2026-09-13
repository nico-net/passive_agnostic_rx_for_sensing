import json
from pathlib import Path
import tempfile
import unittest
from run_raw_replay import bootstrap_hypotheses, parse_log


class RawReplayTests(unittest.TestCase):
    def test_band_aliases_remain_hypotheses(self):
        info = {'sample_rate_hz': 122880000, 'rf_channels': [{'frequency_hz': 3450000000}] * 4}
        tables = [[270] * 15, [273] * 15]
        result = bootstrap_hypotheses(info, [(77, 1, 7700, 1, 7900), (78, 1, 7700, 1, 7900)], tables, 200000)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]['possible_bands'], [77, 78])
        self.assertEqual(result[0]['fft'] * 30000, info['sample_rate_hz'])
        self.assertNotIn('pci', result[0])
        self.assertNotIn('ssb_sc', result[0])

    def test_no_silent_resampling(self):
        info = {'sample_rate_hz': 100000000, 'rf_channels': [{'frequency_hz': 3450000000}]}
        self.assertEqual(bootstrap_hypotheses(info, [(78, 1, 7700, 1, 7900)], [[270]*15, [273]*15], 200000), [])

    def test_fault_cannot_be_hidden_by_eof(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'log'
            path.write_text('RAW_IQ_READY x\nRAW_IQ_VOID bad geometry\nRAW_IQ_EOF x\n')
            result = parse_log(path)
            self.assertTrue(result['eof'])
            self.assertTrue(result['faults'])
            self.assertEqual(result['received_broadcasts']['ssb'], [])

    def test_broadcast_evidence_and_malformed_input(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'log'
            path.write_text('ISAC_ACQ_SSB ' + json.dumps({'pci': 503}) + '\nISAC_ACQ_SIB1 invalid\n')
            result = parse_log(path)
            self.assertEqual(result['received_broadcasts']['ssb'], [{'pci': 503}])
            self.assertTrue(result['faults'])


if __name__ == '__main__':
    unittest.main()
