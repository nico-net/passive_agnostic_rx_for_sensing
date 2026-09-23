import unittest
import numpy as np
from check_ssb_reference import CP, NFFT, detect, pss, sss_catalog, symbol


class SsbReference(unittest.TestCase):
    def test_pss_standard_initial_bits(self):
        self.assertEqual(pss(0)[:7].tolist(), [1, -1, -1, 1, -1, -1, -1])

    def test_pci_and_cfo_with_unknown_time(self):
        for pci in (0, 2, 503, 1007):
            y = np.zeros(4096, dtype=np.complex64)
            start = 713
            for pos, values in ((start, pss(pci % 3)),
                                (start + 2 * (NFFT + CP), sss_catalog(pci % 3)[pci // 3])):
                samples = symbol(values)
                y[pos - CP:pos] = samples[-CP:]
                y[pos:pos + NFFT] = samples
            y *= np.exp(-2j * np.pi * 12345 * np.arange(len(y)) / 7680000)
            result = detect(y, 7680000)
            found = [x for x in result['observations'] if x['sequence_evidence']]
            self.assertEqual(len(found), 1)
            self.assertEqual(found[0]['pci'], pci)
            self.assertAlmostEqual(found[0]['cfo_hz'], -12345, delta=1)

    def test_noise_and_missing_sss_do_not_establish_identity(self):
        rng = np.random.default_rng(940)
        noise = (rng.normal(size=40000) + 1j * rng.normal(size=40000)).astype(np.complex64)
        self.assertFalse(any(x['sequence_evidence'] for x in detect(noise, 7680000)['observations']))
        y = np.zeros(4096, dtype=np.complex64)
        wave = symbol(pss(2))
        y[500 - CP:500] = wave[-CP:]
        y[500:500 + NFFT] = wave
        self.assertFalse(any(x['sequence_evidence'] for x in detect(y, 7680000)['observations']))


if __name__ == '__main__':
    unittest.main()
