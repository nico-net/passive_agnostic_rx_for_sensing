import unittest
from validate_raw_acquisition import queue_counts


class AcquisitionEvidenceTests(unittest.TestCase):
    def test_missing_counter_is_unknown_not_zero(self):
        self.assertIsNone(queue_counts('no scheduling evidence', 'PDSCH'))

    def test_cumulative_summaries_are_not_double_counted(self):
        result = queue_counts('PDSCHQ queued=8 decoded=7 crc_ok=3\nPDSCHQ queued=12 decoded=11 crc_ok=5', 'PDSCH')
        self.assertEqual(result['decoded'], 11)
        self.assertEqual(result['crc_passes'], 5)
        self.assertEqual(result['pending_at_last_summary'], 1)

    def test_impossible_crc_counter_is_rejected(self):
        with self.assertRaises(ValueError):
            queue_counts('PUSCHQ queued=4 decoded=3 crc_ok=5', 'PUSCH')

    def test_zero_crc_is_not_a_success(self):
        result = queue_counts('PUSCHQ queued=5 decoded=5 crc_ok=0', 'PUSCH')
        self.assertEqual(result['crc_pass_rate'], 0)


if __name__ == '__main__':
    unittest.main()
