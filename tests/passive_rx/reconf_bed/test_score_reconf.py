#!/usr/bin/env python3
"""Unit test of score_reconf.py on synthetic logs: python3 -I test_score_reconf.py"""
import importlib.util
import json
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("score_reconf", os.path.join(HERE, "score_reconf.py"))
sr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sr)

T0 = 1000.0           # rx start
STOP, EXIT, BSTART, READY, END = 1090.0, 1092.0, 1095.0, 1097.0, 1240.0


def grant(t, crc=1, mcs=9, nl=4, probe=0):
    return f"{t:.6f} \x1b[0m[PHY]    SENSING: EVMGRANT rnti=0x1234 10.5 crc={crc} mcs={mcs} tbl=1 Qm=6 Nl={nl} rb=0+273 nsym=12 evm=3.1 snr=20.0 probe={probe} kappa=1\n"


def line(t, s):
    return f"{t:.6f} {s}\n"


def synth(scn, *, epochs=(), extra=(), nl=4, mcs=9, stale=False, norecon=False, pci=0, false_hard=False,
          late=0.0, crc_after=True):
    L = []
    t = T0 + 5
    while t < STOP:                       # baseline: 20 grants/s
        L.append(grant(t)); t += 0.05
    if stale:
        L.append(grant(EXIT + 3))
    if not norecon:
        L.append(line(READY - 1, "[HW]    rfsim passive client reconnected after 4000 ms"))
    t = READY + late
    while crc_after and t < END:
        L.append(grant(t, nl=nl, mcs=mcs)); t += 0.05
    for dt, cls, cause in epochs:
        L.append(line(READY + dt, f"[PHY]    SENSING: CONFIG_EPOCH 1 -> 2 class={cls} cause={cause} scope=cell"))
    if false_hard:
        L.append(line(T0 + 40, "[PHY]    SENSING: CONFIG_EPOCH 0 -> 1 class=HARD_REVERIFY cause=SIB1_CHANGE scope=cell"))
    for dt, s in extra:
        L.append(line(READY + dt, "[PHY]    " + s))
    L.append(line(END - 20, "[PHY]    SENSING: ISAC_METRICS " + json.dumps({"pci": pci, "pdschq_drop_epoch": 3})))
    L.append(line(END, "tail"))
    L.sort(key=lambda s: float(s.split()[0]))
    ev = f"""{T0 - 50} scenario {scn} expect_pci=1 expect_nl={nl} expect_mcs={mcs}
{T0} rx_start
{STOP} gnbA_stop_sent
{EXIT} gnbA_exited
{BSTART} gnbB_start
{READY} gnbB_ready
{END} end
"""
    return sr.parse_log("".join(L)), *sr.parse_events(ev)


def status(res):
    return {n: s for n, s, _ in res}


S1_GOOD = dict(epochs=[(2.0, "HARD_RESET", "CELL_IDENTITY_CHANGE")], pci=1, late=3.0,
               extra=[(2.1, "SENSING: PDSCH_SOLVER reset on new cell identity (identity_gen 1 -> 2)"),
                      (2.5, "SENSING: SIB1 decoded"), (4.0, "SENSING: DCI 1_1 length locked coreset=1 rnti=0x1234 len=48")])


class T(unittest.TestCase):
    def test_s0_pass(self):
        st = status(sr.score(*synth("S0", epochs=[(0.5, "HARD_REVERIFY", "CONTINUITY_LOSS")], late=2.0)))
        self.assertNotIn("FAIL", st.values(), st)

    def test_s0_identity_epoch_fails(self):
        st = status(sr.score(*synth("S0", epochs=[(0.5, "HARD_RESET", "CELL_IDENTITY_CHANGE")])))
        self.assertEqual(st["S0_no_identity_or_sib_epoch"], "FAIL")

    def test_s1_pass(self):
        st = status(sr.score(*synth("S1", **S1_GOOD)))
        self.assertNotIn("FAIL", st.values(), st)

    def test_s1_missing_reset_line_and_wrong_pci(self):
        kw = dict(S1_GOOD, extra=[(4.0, "SENSING: DCI 1_1 length locked coreset=1"), (2.5, "SENSING: SIB1 decoded")], pci=0)
        st = status(sr.score(*synth("S1", **kw)))
        self.assertEqual(st["S1_solver_reset"], "FAIL")
        self.assertEqual(st["S1_metrics_pci"], "FAIL")

    def test_s1_soft_only_fails(self):
        st = status(sr.score(*synth("S1", epochs=[(1.0, "SOFT", "RX_RECOVERY")], pci=1)))
        self.assertEqual(st["S1_hard_reset_identity"], "FAIL")

    def test_s1_no_lock_fails(self):
        kw = dict(S1_GOOD, extra=[(2.1, "SENSING: PDSCH_SOLVER reset on new cell identity"), (2.5, "SENSING: SIB1 decoded")])
        self.assertEqual(status(sr.score(*synth("S1", **kw)))["S1_dci_length_lock"], "FAIL")

    def test_slow_recovery_fails(self):
        st = status(sr.score(*synth("S1", **dict(S1_GOOD, late=60.0))))
        self.assertEqual(st["recovery_crc_ok"], "FAIL")

    def test_no_recovery_fails(self):
        st = status(sr.score(*synth("S0", crc_after=False)))
        self.assertEqual(st["recovery_crc_ok"], "FAIL")
        self.assertEqual(st["tail_crc_fraction"], "FAIL")

    def test_s2_pass_and_identity_forbidden(self):
        ok = dict(epochs=[(1.5, "HARD_REVERIFY", "SIB1_CHANGE")], late=2.0,
                  extra=[(3.0, "SENSING: DCI 1_1 length locked coreset=1")])
        self.assertNotIn("FAIL", status(sr.score(*synth("S2", **ok))).values())
        bad = dict(ok, epochs=ok["epochs"] + [(1.6, "HARD_RESET", "CELL_IDENTITY_CHANGE")])
        self.assertEqual(status(sr.score(*synth("S2", **bad)))["S2_pci_unchanged"], "FAIL")

    def test_s2_lenient_turns_fail_into_warn(self):
        r = sr.score(*synth("S2", late=2.0, extra=[(3.0, "SENSING: DCI 1_1 length locked")]), lenient=True)
        self.assertNotIn("FAIL", status(r).values())
        self.assertEqual(status(r)["S2_hard_sib1_or_mib_epoch"], "WARN")

    def test_s3_pass_and_wrong_rank(self):
        good = dict(nl=2, mcs=16, epochs=[(1.0, "SOFT", "DEDICATED_CHANGE_SUSPECTED")], late=1.0)
        self.assertNotIn("FAIL", status(sr.score(*synth("S3", **good))).values())
        rows, ev, meta = synth("S3", nl=2, mcs=16, late=1.0)
        rows = [(t, s.replace("Nl=2", "Nl=4")) for t, s in rows]
        self.assertEqual(status(sr.score(rows, ev, meta))["S3_new_rank_seen"], "FAIL")

    def test_stale_credit_and_no_reconnect(self):
        st = status(sr.score(*synth("S0", stale=True, norecon=True)))
        self.assertEqual(st["no_stale_credit_in_gap"], "FAIL")
        self.assertEqual(st["client_reconnected"], "FAIL")

    def test_false_hard_before_change(self):
        st = status(sr.score(*synth("S0", false_hard=True)))
        self.assertEqual(st["no_false_hard_before_change"], "FAIL")

    def test_probe_grants_ignored(self):
        rows, ev, meta = synth("S0", crc_after=False)
        rows += [(READY + 1 + i * 0.01, "SENSING: EVMGRANT rnti=0x1 1.1 crc=1 mcs=9 tbl=1 Qm=6 Nl=4 probe=1") for i in range(500)]
        self.assertEqual(status(sr.score(sorted(rows), ev, meta))["recovery_crc_ok"], "FAIL")

    def test_incomplete_bed(self):
        rows, ev, meta = synth("S0")
        del ev["gnbA_exited"]
        self.assertEqual(status(sr.score(rows, ev, meta))["bed_complete"], "FAIL")


if __name__ == "__main__":
    unittest.main()
