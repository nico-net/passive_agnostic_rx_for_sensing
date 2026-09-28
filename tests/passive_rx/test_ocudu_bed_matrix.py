import unittest

import ocudu_bed_matrix as plan


class BedMatrixPlanTest(unittest.TestCase):
    def test_full_schedule_is_three_alternated_pairs_per_feature(self):
        runs = plan.make_schedule()
        self.assertTrue(plan.validate_schedule(runs))
        self.assertEqual(len(plan.FEATURES), 18)
        self.assertEqual(len(runs), 108)
        for i in range(0, len(runs), 2):
            self.assertEqual(runs[i][1].name, "baseline")
            self.assertEqual(runs[i + 1][1].name, runs[i + 1][0].rsplit("_f", 1)[0])

    def test_each_feature_has_exactly_three_baseline_and_candidate_runs(self):
        runs = plan.make_schedule()
        for arm in plan.FEATURES:
            candidate = [name for name, got in runs if got.name == arm.name]
            baseline = [name for name, got in runs
                        if got.name == "baseline" and name.startswith(arm.name + "_b")]
            self.assertEqual(candidate, [f"{arm.name}_f1", f"{arm.name}_f2", f"{arm.name}_f3"])
            self.assertEqual(baseline, [f"{arm.name}_b1", f"{arm.name}_b2", f"{arm.name}_b3"])

    def test_known_unroutable_features_are_explicitly_blocked(self):
        self.assertEqual(plan.blocked_routes(), ("transform_precoding", "multi_ue"))
        for arm in plan.FEATURES:
            if arm.name == "qam256":
                self.assertIn(("SRSUE_ADVERTISE_256QAM", "1"), arm.srsue_env)
            if arm.name == "al16":
                self.assertIn("--ss2_n_candidates 0 0 0 0 1", arm.gnb_extra)

    def test_invalid_schedule_is_rejected(self):
        runs = plan.make_schedule()
        with self.assertRaises(ValueError):
            plan.validate_schedule(runs[:-1])


if __name__ == "__main__":
    unittest.main()
