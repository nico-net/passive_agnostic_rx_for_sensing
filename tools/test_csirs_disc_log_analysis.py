import json
import tempfile
import unittest
from pathlib import Path

from csirs_disc_log_analysis import analyze


class LogAnalysis(unittest.TestCase):
    def test_partial_metrics_and_missing_diagnostics_are_not_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'run.log'
            metrics = dict(t_mono_ns=10, csirs_disc_batches=2, csirs_disc_batch_us=2000000)
            path.write_text('SENSING: ISAC_METRICS ' + json.dumps(metrics) + '\n'
                            'CSIRS_DISC HYPOTHESES n=99 sweep=4\n'
                            'CSIRS_DISC WINNER z=2.42 z2=1.46 mu=1.008 sigma=0.117\n'
                            'CSIRS_DISC FEP slots=334 total_us=10 feed_drops=25\n'
                            'SENSING: ISAC_METRICS {"t_mono_ns":\n')
            result = analyze(path)
            self.assertEqual(result['counts']['confirmed'], 0)
            self.assertEqual(result['counts']['WINNER'], 1)
            self.assertEqual(result['logged_sweeps'], 4)
            self.assertEqual(result['batch_mean_ms'], 1000)
            self.assertEqual(result['malformed_metrics'], 1)
            self.assertEqual(result['last_fep_feed_drops'], 25)
            self.assertIsNone(result['unique_slots_per_hypothesis'])
            self.assertEqual(result['hypothesis_slots'], {})
            self.assertAlmostEqual(result['distributions']['WINNER_gate_excess']['min'], .008)

    def test_diagnostic_slot_sum_is_not_unique_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'run.log'
            line = 'CSIRS_DISC DIAG row=1 fd=2 l0=5 l1=0 rb=0:52 P/O=160/15 slots=4 z=3\n'
            path.write_text(line * 2)
            result = analyze(path)
            self.assertEqual(next(iter(result['hypothesis_slots'].values()))['sum_reported'], 8)
            self.assertIsNone(result['unique_slots_per_hypothesis'])


if __name__ == '__main__':
    unittest.main()
