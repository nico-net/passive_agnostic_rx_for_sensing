import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ota_stats as O  # noqa: E402

E = "\x1b[0m\x1b[32m"


def ev(rnti, fs, crc, mcs, nl, rb, snr, probe=0):
    return (f"{E}[PHY]    SENSING: EVMGRANT rnti=0x{rnti:04x} {fs} crc={crc} mcs={mcs} tbl=1 Qm=2 Nl={nl} "
            f"rb={rb} nsym=9 evm=14.6 snr={snr} br_snr=1,2,3,4 nbrx=4 tok=0 snr_cpe=17.0 probe={probe} kappa=0.00\n")


def seen(rnti, sfn, slot, fmt="1_1"):
    return f"[PHY]    SENSING: blind PDCCH rnti_seen utc_ns=1 rnti=0x{rnti:x} sfn={sfn} slot={slot} fmt={fmt} cce=0 al=4\n"


def metrics(abs_slot):
    return ('[PHY]    SENSING: ISAC_METRICS {"schema":1,"t_mono_ns":%d,"abs_slot":%d,"acq_sync_losses":0,'
            '"scanq_queued":10,"scanq_drop_full":0}\n' % (abs_slot * 500000, abs_slot))


LOG = (
    "[PHY]    Initial sync successful, PCI: 415\n"
    + metrics(4000)
    + seen(0x1111, 200, 0) + seen(0x1111, 200, 1) + seen(0x1111, 200, 1) + seen(0x2222, 210, 5, "1_0")
    + seen(0xffff, 201, 0, "1_0")
    + "[PHY]    SENSING: discovered CORESET 1/2 nid=1 VERIFIED -> bank add index=0 rnti=0x1111 len=49\n"
    + ev(0x1111, "200.0", 1, 0, 2, "10+5", 12.0)
    + ev(0x1111, "200.1", 0, 0, 2, "60+5", 12.0)
    + ev(0x1111, "200.2", 1, 3, 1, "10+5", 3.0)
    + ev(0x1111, "200.3", 1, 3, 1, "10+5", 3.0, probe=1)
    + ev(0xffff, "200.4", 1, 0, 1, "0+5", 12.0)
    + "[PHY]    SENSING: PDSCH_SOLVER rnti=0x1111 SOLVED by 2 independent CRC passes: layout 1\n"
    + "[PHY]    SENSING: SCANTIME_FINE ded-only occasions=4000 p50<=19us p99<=400us over500=33\n"
    + "[PHY]    SENSING: GPU_EXACT_FEP calls=10 fallback=0 (late=0 busy=0) check_ok=10 check_bad=2 direct=1\n"
    + "[HW]     SENSING: RFSTALL USRP_RING reason_code=1\n"
    + metrics(24000)
)


class T(unittest.TestCase):
    def setUp(self):
        d = tempfile.mkdtemp()
        self.p = os.path.join(d, "run.log")
        with open(self.p, "w") as fh:
            fh.write(LOG)
        self.r = O.parse_run(self.p)

    def test_ues(self):
        u = self.r["ues"]
        self.assertEqual(u["dci11"], 1)          # 0x2222 is fmt 1_0, 0xffff is SI
        self.assertEqual(u["dci_any_fmt"], 2)
        self.assertEqual(u["solved"], 1)
        self.assertEqual(u["crc_ok"], 1)
        self.assertFalse(u["ul_dci01_logged"])

    def test_sync_and_times(self):
        self.assertEqual(self.r["t_sync_slot"], 4000)
        # first DCI 1_1 of 0x1111 at 200.0 -> slot 4000 (unwrapped near ref 4000): 0 s after sync
        self.assertAlmostEqual(self.r["conv"]["first_dci"][0x1111], 0.0, places=3)
        self.assertEqual(len(self.r["conv"]["first_bank_add"]), 1)

    def test_probe_and_si_excluded(self):
        self.assertEqual(self.r["evm"]["decoded"], 3)
        self.assertEqual(self.r["evm"]["probes_excluded"], 1)
        self.assertEqual(self.r["evm"]["si_excluded"], 1)

    def test_counters_and_classes(self):
        c = self.r["counters"]
        # snr 12 -> bin 2, mcs0 -> class: 200.0 -> abs 4000%160=0 -> not A; rb 10 -> B ; rb 60 -> C
        self.assertEqual(c["0|2|2|B"], [1, 1])
        self.assertEqual(c["0|2|2|C"], [1, 0])
        self.assertEqual(c["3|1|0|"], [1, 1])

    def test_activity_dedup(self):
        a = self.r["activity"][0x1111]
        self.assertEqual(a["n"], 2)               # duplicate slot collapsed
        self.assertEqual(a["active_s"], 1)

    def test_processing_and_stability(self):
        self.assertEqual(self.r["gpu_fep"]["check_bad"], 2)
        self.assertEqual(self.r["scan"]["fine:ded-only"], (4000, 19, 400))
        self.assertEqual(self.r["stab"]["rfstall"], 1)
        self.assertEqual(self.r["stab"]["n_metrics"], 2)

    def test_frame_wrap(self):
        c = O.Clock()
        c.ref = 20470
        self.assertEqual(c.unwrap(10), 20490)
        c.ref = 20490
        self.assertEqual(c.unwrap(20470 % 20480), 20470)

    def test_wilson_and_newcombe(self):
        lo, hi = O.wilson(50, 100)
        self.assertTrue(40 < lo < 41 and 59 < hi < 60)
        d, lo, hi = O.newcombe(10, 100, 30, 100)
        self.assertAlmostEqual(d, 20.0)
        self.assertTrue(lo > 0)

    def test_aggregate_and_render(self):
        A = O.aggregate([self.r, self.r])
        self.assertEqual(A["ues"]["dci11"]["mean"], 1.0)
        md = O.render_set("x", A, [self.r])
        self.assertIn("CRC-OK", md)
        O.render_compare("a", A, "b", A)


if __name__ == "__main__":
    unittest.main()
