import os
import unittest

from dci_reinterpret import reinterpret_file

FIX = os.path.join(os.path.dirname(__file__), "fixtures", "cfgref_1_0.jsonl")


class T(unittest.TestCase):
    def test_1_0_fields_from_config(self):
        (rec, f), = list(reinterpret_file(FIX))
        self.assertEqual(f["start_rb_bwp"], rec["start_rb_bwp"])
        self.assertEqual(f["nb_rb"], rec["nb_rb"])
        for k in ("tda_index", "mcs", "k1", "harq_pid", "ndi", "rv"):
            self.assertEqual(f[k], rec[k], k)

    def test_wrong_bwp_size_misreads(self):
        import json
        from dci_reinterpret import reinterpret
        rec = [json.loads(l) for l in open(FIX)][1]
        self.assertNotEqual(reinterpret(rec, 106)["mcs"], rec["mcs"])

    def test_0_0_selected_by_format_code(self):
        from dci_reinterpret import reinterpret
        rec = {"dci_format": 3, "dci_len": 33, "dci_bits": "0000000000"}
        self.assertEqual(reinterpret(rec, 106)["mcs"], 0)
        rec["dci_len"] = 40  # 0_0 is read only at its exact length
        self.assertIsNone(reinterpret(rec, 106))


if __name__ == "__main__":
    unittest.main()
