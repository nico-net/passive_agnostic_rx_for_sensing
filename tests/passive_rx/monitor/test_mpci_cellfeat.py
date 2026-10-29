#!/usr/bin/env python3
"""CELLFEAT: the dashboard shows one Sensing export view per cell. The receiver writes cell 0 on the configured export names and cell k > 0 on
<name>.cell<k><ext> (nr_sense_cell_file); the monitor feeds each family into the state of its cell.
    python3 -m unittest test_mpci_cellfeat      (run from this directory)
"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import monitor  # noqa: E402
import test_multicell  # noqa: E402


def rec(source, i, typ="rs"):
    return json.dumps({"schema": 2, "type": typ, "source": source, "t_mono_ns": 1000 + i}) + "\n"


class CellFeat(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        with open(os.path.join(self.d, "run.log"), "w") as f:
            f.writelines(test_multicell.mixed_lines())

    def write(self, name, lines):
        with open(os.path.join(self.d, name), "w") as f:
            f.writelines(lines)

    def test_discover_keeps_cell_zero_on_its_own_name(self):
        self.write("records.jsonl", [rec("csirs", 0)])
        self.write("records.cell1.jsonl", [rec("prach", 0)] * 5)   # the larger file must not become cell 0's records
        self.write("cfr.bin", [""])
        self.write("cfr.bin.cell1", [""])
        f = monitor.discover(self.d)
        self.assertEqual(os.path.basename(f["records"]), "records.jsonl")
        self.assertEqual({k: os.path.basename(v) for k, v in f["records_cells"].items()}, {1: "records.cell1.jsonl"})
        self.assertEqual(os.path.basename(f["cfr"]), "cfr.bin")
        self.assertEqual({k: os.path.basename(v) for k, v in f["cfr_cells"].items()}, {1: "cfr.bin.cell1"})

    def test_cell_zero_named_like_the_cell_family_is_not_mistaken(self):
        self.write("sense_records.jsonl", [rec("csirs", 0)])
        self.write("sense_records.cell1.jsonl", [rec("prach", i) for i in range(9)])
        f = monitor.discover(self.d)
        self.assertEqual(os.path.basename(f["records"]), "sense_records.jsonl")
        self.assertEqual(list(f["records_cells"]), [1])

    def test_sensing_view_per_cell(self):
        self.write("records.jsonl", [rec("csirs", i) for i in range(3)] + [rec("ssb", 9, "session")])
        self.write("records.cell1.jsonl", [rec("prach", i) for i in range(7)] + [rec("srs", 9)])
        mgr = monitor.Manager(test_multicell._args(self.d))
        run_ = mgr.resolve("primary")
        run_.advance()
        p0 = monitor.state_payload(mgr, run_, {"cell": "421"})
        p1 = monitor.state_payload(mgr, run_, {"cell": "508"})
        s0, s1 = p0["snap"]["sense"], p1["snap"]["sense"]
        self.assertEqual(s0["records"], 4)
        self.assertEqual(s0["by_source"], {"csirs": 3})
        self.assertEqual(s1["records"], 8)
        self.assertEqual(s1["by_source"], {"prach": 7, "srs": 1})

    def test_single_cell_run_unchanged(self):
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "run.log"), "w") as f:
            f.writelines(test_multicell.read("cell_a.log"))
        with open(os.path.join(d, "records.jsonl"), "w") as f:
            f.writelines([rec("csirs", i) for i in range(2)])
        mgr = monitor.Manager(test_multicell._args(d))
        run_ = mgr.resolve("primary")
        run_.advance()
        p = monitor.state_payload(mgr, run_, {})
        self.assertEqual(p["snap"]["sense"]["records"], 2)
        self.assertEqual(run_.state.children, {})


if __name__ == "__main__":
    unittest.main()
