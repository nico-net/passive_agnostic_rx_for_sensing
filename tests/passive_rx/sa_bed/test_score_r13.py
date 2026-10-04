#!/usr/bin/env python3
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from score_r13 import score_run
from check_traffic import recent_rate

FIXTURE = Path(__file__).parent / "fixtures/bwp_good"


class ScoreR13Test(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "run"
        shutil.copytree(FIXTURE, self.root)
        (self.root / "gnb/before.txt").rename(self.root / "gnb/before.log")
        (self.root / "rx/rx.txt").rename(self.root / "rx/rx.log")

    def events(self, **changes):
        path = self.root / "events.jsonl"
        rows = [dict(json.loads(s), **changes) for s in path.read_text().splitlines()]
        path.write_text("".join(json.dumps(x) + "\n" for x in rows))

    def test_good_switch_uses_postchange_truth_and_all_milestones(self):
        r = score_run(self.root)
        self.assertEqual(r["status"], "PASS")
        self.assertEqual(r["recovery_s"], 4)
        self.assertEqual(r["recovery_origin"], "apply")
        self.assertEqual(r["recovery_from_apply_s"], 4)
        self.assertEqual(r["gnb_dci_11_bits_after"], 48)
        self.assertEqual(r["dropped_epoch"]["pdschq_drop_epoch"], 3)

    def test_wrong_length_is_stale_even_if_recovered(self):
        with (self.root / "rx/rx.log").open("a") as f:
            f.write("1025.000000 SENSING: DCI 1_1 length locked coreset=1 rnti=0x1234 len=50\n")
        r = score_run(self.root)
        self.assertEqual(r["stale_dci_winners"], 1)
        self.assertEqual(r["status"], "INCOMPLETE")

    def test_pre_relock_convergence_does_not_count_as_recovery(self):
        p = self.root / "rx/rx.log"
        p.write_text(p.read_text().replace("1024.000000 SENSING: Technique D CONVERGED",
                                           "1021.500000 SENSING: Technique D CONVERGED"))
        r = score_run(self.root)
        self.assertIsNone(r["recovery_s"])
        self.assertEqual(r["status"], "INCOMPLETE")

    def test_bwp_soft_epoch_with_wrong_cause_is_rejected(self):
        p = self.root / "rx/rx.log"
        p.write_text(p.read_text().replace("cause=BWP_CHANGE", "cause=NOISE"))
        self.assertEqual(score_run(self.root)["status"], "INCOMPLETE")

    def test_false_bump_outside_window_and_missing_audit(self):
        self.events(scenario="stable")
        (self.root / "td_truth_audit.json").unlink()
        with (self.root / "rx/rx.log").open("a") as f:
            f.write("1035.000000 SENSING: CONFIG_EPOCH 2 -> 3 class=HARD_RESET cause=PCI_CHANGE scope=CELL\n")
        r = score_run(self.root)
        self.assertEqual(r["false_hard"], 1)
        self.assertIsNone(r["stale_winners"])
        self.assertEqual(r["status"], "INCOMPLETE")

    def test_missing_ground_truth_and_milestone_cannot_pass(self):
        (self.root / "gnb/before.log").unlink()
        (self.root / "rx/rx.log").write_text("1020.300000 SENSING: CONFIG_EPOCH 1 -> 2 class=SOFT cause=BWP_CHANGE scope=RNTI\n")
        r = score_run(self.root)
        self.assertIsNone(r["recovery_s"])
        self.assertTrue(r["errors"])

    def test_same_cell_restart_uses_first_dci_origin(self):
        self.events(scenario="same_cell_restart_size_change")
        (self.root / "gnb/after.log").write_text("1021.000000 DCI11_WIDTHS total=49 fdra=9\n")
        (self.root / "rx/rx.log").write_text(
            "1013.000000 SIB1 decoded\n"
            "1020.500000 CONFIG_EPOCH 1 -> 2 class=HARD_REVERIFY cause=CONTINUITY_LOSS\n"
            "1022.000000 SENSING: DCI length RELOCK rnti=0x1234 old=50 new=49\n"
            "1024.000000 SENSING: Technique D CONVERGED rnti=0x1234 tda=0 S=2 L=12 mask=0x4 table=0\n")
        r = score_run(self.root)
        self.assertEqual(r["status"], "PASS")
        self.assertEqual(r["recovery_origin"], "validation_start")
        self.assertEqual(r["recovery_s"], 3)
        self.assertEqual(r["recovery_from_apply_s"], 4)
        self.assertEqual(r["epoch_count"], 1)

    def test_cell_restart_needs_hard_reset_and_new_pci(self):
        self.events(scenario="cell_restart")
        (self.root / "gnb/after.log").write_text("1021.000000 DCI11_WIDTHS total=50 fdra=10\n")
        (self.root / "rx/rx.log").write_text(
            "1013.000000 SIB1 decoded\n"
            "1020.500000 SENSING: CONFIG_EPOCH 1 -> 2 class=HARD_RESET cause=CELL_IDENTITY_CHANGE scope=cell_identity\n"
            "1022.000000 SENSING: ACQ_STATE SEARCHING -> CORESET_VERIFIED\n"
            "1023.000000 SENSING: DCI 1_1 length locked coreset=1 rnti=0x1234 len=50\n"
            "1025.000000 SENSING: Technique D CONVERGED rnti=0x1234 tda=0 S=2 L=12 mask=0x4 table=0\n")
        p = self.root / "metrics.jsonl"
        rows = [json.loads(s) for s in p.read_text().splitlines()]
        rows[0]["pci"], rows[1]["pci"] = 0, 1
        p.write_text("".join(json.dumps(x) + "\n" for x in rows))
        r = score_run(self.root)
        self.assertEqual(r["status"], "PASS")
        self.assertEqual(r["receiver_pci_after"], 1)
        self.assertEqual(r["recovery_origin"], "validation_start")
        self.assertEqual(r["recovery_s"], 4)
        self.assertEqual(r["recovery_from_apply_s"], 5)

    def test_restart_downtime_is_secondary_and_missing_truth_has_no_origin_time(self):
        self.test_same_cell_restart_uses_first_dci_origin()
        ep = self.root / "events.jsonl"
        rows = [json.loads(s) for s in ep.read_text().splitlines()]
        # Move the restart request earlier without moving first DCI or recovery.
        next(x for x in rows if x["event"] == "apply")["t"] = 1014
        ep.write_text("".join(json.dumps(x) + "\n" for x in rows))
        r = score_run(self.root)
        self.assertEqual(r["status"], "PASS")
        self.assertEqual(r["recovery_s"], 3)
        self.assertEqual(r["recovery_from_apply_s"], 10)
        (self.root / "gnb/after.log").unlink()
        r = score_run(self.root)
        self.assertIsNone(r["recovery_origin_t"])
        self.assertIsNone(r["recovery_s"])
        self.assertEqual(r["status"], "INCOMPLETE")

    def test_same_cell_restart_requires_continuity_loss(self):
        self.test_same_cell_restart_uses_first_dci_origin()
        p = self.root / "rx/rx.log"
        p.write_text("\n".join(s for s in p.read_text().splitlines() if "CONFIG_EPOCH" not in s))
        self.assertEqual(score_run(self.root)["status"], "INCOMPLETE")

    def test_legacy_dedicated_name_is_scored_as_restart(self):
        self.test_same_cell_restart_uses_first_dci_origin()
        self.events(scenario="dedicated_change")
        r = score_run(self.root)
        self.assertEqual(r["scenario"], "same_cell_restart_size_change")
        self.assertEqual(r["recovery_origin"], "validation_start")

    @patch("check_traffic.time.monotonic_ns", return_value=21000000000)
    def test_smoke_rate_rejects_sparse_missing_and_stale_traffic(self, _clock):
        rows = [{"t_mono_ns": 1000000000, "pdcch_accepts_c": 0},
                {"t_mono_ns": 11000000000, "pdcch_accepts_c": 2000},
                {"t_mono_ns": 21000000000, "pdcch_accepts_c": 3000}]
        self.assertEqual(recent_rate(rows), 100)
        rows[-1]["pdcch_accepts_c"] = 2999
        self.assertLess(recent_rate(rows), 100)
        self.assertIsNone(recent_rate(rows[:-1]))
        self.assertIsNone(recent_rate([]))

    def test_sib1less_rejects_sib1_context_evidence(self):
        self.events(sib="sib1less")
        p = self.root / "uectx.jsonl"
        p.write_text(json.dumps(dict(schema="uectx/1", type="ue_snapshot", cfg={"SIB1_HASH": None})) + "\n")
        r = score_run(self.root)
        self.assertEqual(r["status"], "PASS")
        p.write_text(json.dumps(dict(schema="uectx/1", type="ue_snapshot", cfg={"SIB1_HASH": {"value": 1}})) + "\n")
        self.assertEqual(score_run(self.root)["status"], "INCOMPLETE")

    def test_flag_off_control_has_no_epoch_and_is_never_pass(self):
        self.events(reconf="off")
        p = self.root / "rx/rx.log"
        p.write_text("\n".join(s for s in p.read_text().splitlines() if "CONFIG_EPOCH" not in s) + "\n")
        self.assertEqual(score_run(self.root)["status"], "CONTROL")

    def test_stable_cell_needs_fifteen_minutes(self):
        self.events(scenario="stable")
        (self.root / "gnb/before.log").write_text(
            "1010.000000 Filling Format 1_1 DCI of size 50\n"
            "1021.000000 Filling Format 1_1 DCI of size 50\n")
        p = self.root / "rx/rx.log"
        p.write_text("\n".join(s for s in p.read_text().splitlines()
                               if "CONFIG_EPOCH" not in s and "BWP RESOLVED" not in s
                               and "DCI length RELOCK" not in s) + "\n")
        self.assertEqual(score_run(self.root)["status"], "INCOMPLETE")
        ep = self.root / "events.jsonl"
        rows = [json.loads(s) for s in ep.read_text().splitlines()]
        next(x for x in rows if x["event"] == "end")["t"] = 1910.0
        ep.write_text("".join(json.dumps(x) + "\n" for x in rows))
        mp = self.root / "metrics.jsonl"
        metrics = [json.loads(s) for s in mp.read_text().splitlines()]
        metrics[-1]["t_mono_ns"] = 901000000000
        metrics[-1]["pdcch_accepts_c"] = 112500
        mp.write_text("".join(json.dumps(x) + "\n" for x in metrics))
        self.assertEqual(score_run(self.root)["status"], "PASS")
        with p.open("a") as f:
            f.write("1500.000000 SENSING: CONFIG_EPOCH 2 -> 3 class=SOFT cause=NOISE scope=RNTI\n")
        self.assertEqual(score_run(self.root)["status"], "INCOMPLETE")
        with p.open("a") as f:
            f.write("1800.000000 SENSING: CONFIG_EPOCH 3 -> 4 class=SOFT cause=NOISE scope=RNTI\n")
        self.assertEqual(score_run(self.root)["status"], "INCOMPLETE")

    def test_startup_failure_is_incomplete_instead_of_crashing_campaign(self):
        p = self.root / "events.jsonl"
        p.write_text(p.read_text().splitlines()[0] + "\n")
        r = score_run(self.root)
        self.assertEqual(r["status"], "INCOMPLETE")
        self.assertIsNone(r["recovery_s"])

    def test_campaign_failure_cannot_pass_from_complete_looking_logs(self):
        (self.root / "run.json").write_text(json.dumps(dict(rc=1, status="done", timed_out=False,
                                                      escalation_stage=0)))
        self.assertEqual(score_run(self.root)["status"], "INCOMPLETE")

    def test_campaign_needs_all_sixteen_arms(self):
        campaign = Path(self.tmp.name) / "campaign"
        runs = campaign / "runs"
        runs.mkdir(parents=True)
        for n in range(5):
            shutil.copytree(self.root, runs / f"{n:03d}_bwp_sa_on")
        subprocess.run([sys.executable, str(Path(__file__).parent / "score_r13.py"), str(campaign)],
                       check=True, capture_output=True, text=True)
        summary = json.loads((campaign / "r13_summary.json").read_text())
        self.assertFalse(summary["r13_pass"])
        self.assertEqual(summary["groups"]["sens6/bwp_switch/sa/on"]["pass_count"], 5)
        self.assertEqual(len(summary["missing_arms"]), 15)


if __name__ == "__main__":
    unittest.main()
