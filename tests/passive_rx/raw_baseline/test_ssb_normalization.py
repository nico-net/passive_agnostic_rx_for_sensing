"""Regression tests for empty-window normalization, independent of RF access."""
import unittest
import numpy as np
from check_ssb_reference import CP, NFFT, detect, pss, sss_catalog, symbol


class NormalizationTests(unittest.TestCase):
    def test_empty_input_has_no_evidence(self):
        result = detect(np.zeros(4096, dtype=np.complex64), 7680000)
        self.assertEqual(result['max_pss_score'], 0.0)
        self.assertEqual(result['observations'], [])

    def test_sparse_signal_correlation_is_bounded(self):
        for pci in (0, 2, 503, 1007):
            with self.subTest(pci=pci):
                y = np.zeros(4096, dtype=np.complex64)
                start = 713
                for pos, values in (
                    (start, pss(pci % 3)),
                    (start + 2 * (NFFT + CP), sss_catalog(pci % 3)[pci // 3]),
                ):
                    useful = symbol(values)
                    y[pos - CP:pos] = useful[-CP:]
                    y[pos:pos + NFFT] = useful
                y *= np.exp(-2j * np.pi * 12345 * np.arange(len(y)) / 7680000)
                result = detect(y, 7680000)
                self.assertTrue(np.isfinite(result['max_pss_score']))
                self.assertLessEqual(result['max_pss_score'], 1.0 + 1e-10)
                found = [x for x in result['observations'] if x['sequence_evidence']]
                self.assertEqual(len(found), 1)
                self.assertEqual(found[0]['pci'], pci)
                self.assertAlmostEqual(found[0]['cfo_hz'], -12345, delta=1)


if __name__ == '__main__':
    unittest.main()
