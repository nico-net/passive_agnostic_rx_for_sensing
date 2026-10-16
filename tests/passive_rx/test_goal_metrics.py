# tests/passive_rx/test_goal_metrics.py
import os, tempfile, unittest
import crc_margin_curve as cmc
import goal_metrics as gm

def ev(crc=1, mcs=10, tbl=0, nl=1, snr_l="20.0,-99.0,-99.0,-99.0", probe=0, rv=0, tot=400):
    return (f"[PHY]    SENSING: EVMGRANT rnti=0x1 1.2 crc={crc} mcs={mcs} tbl={tbl} Qm=2 Nl={nl} rb=0+5 nsym=12 "
            f"evm=1 snr=20.0 probe={probe} rv={rv} ndi=0 pid=0 tbs=96 snr_l={snr_l} l2=1 zabs=0 "
            f"e2e=100,1,1,1,1,1,{tot},1 stg=fep:1/1/1 cb=1\n")

def write(lines):
    f = tempfile.NamedTemporaryFile('w', suffix='.log', delete=False)
    f.writelines(lines); f.close(); return f.name

class GoalMetricsTest(unittest.TestCase):
    def test_required_snr_monotonic_and_reserved(self):
        self.assertLess(gm.required_snr_db(0, 0), gm.required_snr_db(27, 0))
        self.assertIsNone(gm.required_snr_db(29, 0))
        self.assertIsNone(gm.required_snr_db(28, 1))
        self.assertAlmostEqual(gm.required_snr_db(8, 0), gm.required_snr_db(14, 2), places=6)  # same Qm/R

    def test_qualified_crc_and_latency(self):
        p = write([ev(crc=1, tot=300), ev(crc=0, tot=900), ev(crc=1, snr_l="-10.0,-99,-99,-99", tot=200),
                   "[PHY]    SENSING: PDSCHQ queued=1000 decoded=990 crc_ok=2 (0.2%) dropped[full=3 stale=4] max_lag_slots=1/2 slot_groups=0/0 multi-grant slots dropped_narrow=3\n"])
        d = gm.parse_run(p)['dl']
        self.assertEqual(d['n_real'], 3)
        self.assertEqual(d['n_q'], 2)
        self.assertAlmostEqual(d['crc_q'], 50.0)
        self.assertEqual(d['unqualified_floor'], 1)  # snr_l -10 is below the QPSK floor: now floor, was low_snr
        self.assertAlmostEqual(d['drops_pct'], 1.0)
        self.assertEqual(d['tot_p99'], 900)

    def test_true_snr_floor_and_inversion(self):
        self.assertIsNone(gm.true_snr_db(3.9, 2))
        self.assertIsNone(gm.true_snr_db(4.9, 2))
        self.assertIsNone(gm.true_snr_db(14.4, 8))
        self.assertAlmostEqual(gm.true_snr_db(10.3, 2), 10.0, delta=0.5)
        self.assertAlmostEqual(gm.true_snr_db(35.0, 6), 35.0)
        for qm in (2, 4, 6, 8):
            v = [gm.true_snr_db(e / 10, qm) for e in range(0, 400)]
            v = [x for x in v if x is not None]
            self.assertEqual(v, sorted(v))

    def test_floor_grant_unqualified(self):
        d = gm.parse_run(write([ev(mcs=0, snr_l="3.9,-99,-99,-99"), ev(mcs=0, snr_l="10.3,-99,-99,-99")]))['dl']
        self.assertEqual(d['unqualified_floor'], 1)
        self.assertEqual(d['n_q'], 1)
        # QPSK est 3.9 at MCS0 passed the old rule (req -4.5+2) at any true SNR

    def test_probes_excluded(self):
        d = gm.parse_run(write([ev(probe=1), ev()]))['dl']
        self.assertEqual(d['n_real'], 1)

    def test_retx_never_qualified(self):
        d = gm.parse_run(write([ev(rv=2), ev(mcs=30, tbl=0), ev(mcs=28, tbl=1)]))['dl']
        self.assertEqual(d['n_q'], 0)
        self.assertEqual(d['unqualified_retx'], 3)

    def test_multilayer_uses_min_of_used_layers(self):
        # layer 2 at 0 dB disqualifies an MCS-27 grant; the -99 slots beyond Nl=2 are ignored
        d = gm.parse_run(write([ev(mcs=27, nl=2, snr_l="30.0,0.0,-99.0,-99.0")]))['dl']
        self.assertEqual(d['n_q'], 0)
        d = gm.parse_run(write([ev(mcs=5, nl=2, snr_l="30.0,25.0,-99.0,-99.0")]))['dl']
        self.assertEqual(d['n_q'], 1)

    def test_truncated_and_missing_fields(self):
        d = gm.parse_run(write([ev()[:60] + "\n", ev().replace("e2e=", "x2x="), ev()]))['dl']
        self.assertEqual(d['n_real'], 1)
        self.assertEqual(d['skipped'], 2)

    def test_drop_counters_use_last_line(self):
        q = "[PHY]    SENSING: PDSCHQ queued={} decoded=1 crc_ok=1 (1%) dropped[full={} stale=0] max_lag_slots=1/2 dropped_narrow=0\n"
        d = gm.parse_run(write([q.format(100, 50), q.format(1000, 5)]))['dl']
        self.assertAlmostEqual(d['drops_pct'], 0.5)

    def test_no_qualified_grants(self):
        d = gm.parse_run(write([ev(snr_l="-20.0,-99,-99,-99")]))['dl']
        self.assertIsNone(d['crc_q'])

    def test_ul_lines(self):
        p = write(["[PHY]    SENSING: UL_PUSCH_CRC rnti=0x1 ok=1 mcs=4 prb=1+3 ta=1 t=1 cpu_us=1 status=0 reason=ok snr_db=15.0 delay=0\n",
                   "[PHY]    SENSING: UL_PUSCH_CRC rnti=0x1 ok=0 mcs=4 prb=1+3 ta=1 t=1 cpu_us=1 status=1 reason=x snr_db=-12.0 delay=0\n",
                   "[PHY]    SENSING: UL_E2E rnti=0x1 st=1 prb=3 mcs=4 pick=1 arr2deq=1 wall=1 tot=700 cpu_us=1\n",
                   "[PHY]    SENSING: PUSCHQ2 consumers=2 real[queued=200 full=4] probe[queued=1 decoded=1 full=0]\n"])
        u = gm.parse_run(p)['ul']
        self.assertEqual(u['n_q'], 1)
        self.assertEqual(u['unqualified_floor'], 0)  # UL snr_db is a DM-RS SNR: no EVM floor inversion
        self.assertEqual(u['unqualified_low_snr'], 1)
        self.assertAlmostEqual(u['crc_q'], 100.0)
        self.assertEqual(u['tot_p99'], 700)
        self.assertAlmostEqual(u['drops_pct'], 2.0)

class MarginCurveTest(unittest.TestCase):
    def test_cross80(self):
        # bins: margin -1 dB 0 % ok, +1 dB 50 %, +3 dB 90 % -> first bin >= 80 % is +3
        rows = {-1: [10, 0], 1: [10, 5], 3: [10, 9]}
        self.assertEqual(cmc.cross80(rows), 3)
        self.assertIsNone(cmc.cross80({0: [10, 1]}))

if __name__ == '__main__':
    unittest.main()
