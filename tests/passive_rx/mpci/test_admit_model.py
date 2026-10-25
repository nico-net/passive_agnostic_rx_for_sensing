#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
"""admit_model.py (offline planner) against the C function nr_cell_admit_plan on the same fixtures (admit_fixtures.txt), through the C fixture runner
nr_cell_admit_cli. Usage: test_admit_model.py --cli <path to nr_cell_admit_cli>   (CTest passes the built binary; NR_CELL_ADMIT_CLI also works)."""
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import admit_model as am  # noqa: E402

FIX = os.path.join(HERE, "admit_fixtures.txt")


def load():
    with open(FIX) as f:
        return am.parse_fixture(f.read())

CLI = os.environ.get("NR_CELL_ADMIT_CLI", "")


class AdmitModel(unittest.TestCase):
    def test_matches_the_c_function_on_all_fixtures(self):
        if not CLI or not os.path.exists(CLI):
            self.skipTest("no nr_cell_admit_cli (pass --cli or set NR_CELL_ADMIT_CLI)")
        c_out = subprocess.run([CLI, FIX], check=True, capture_output=True, text=True).stdout.splitlines()
        py_out = []
        cases = load()
        for name, k, cells in cases:
            py_out += am.format_case(name, k, cells)
        self.assertGreater(len(cases), 15)
        self.assertEqual(len(c_out), len(py_out))
        for a, b in zip(c_out, py_out):
            self.assertEqual(a, b)

    def test_fixture_expectations(self):
        by = {n: am.plan(k, c) for n, k, c in load()}
        self.assertEqual(by["sens6_2x106"]["served"], 2)
        self.assertEqual(by["sens6_2x273"]["served"], 1)
        self.assertEqual(by["sens6_2x273"]["cell"][1]["reason"], am.GPU_MEM)
        self.assertEqual(by["dgx_6x217_cores"]["served"], 4)
        self.assertEqual(by["cloud4_3x106"]["cell"][1]["reason"], am.CORES)
        self.assertEqual(by["seventeen_cells"]["served"], 16)
        self.assertEqual(by["seventeen_cells"]["cell"][16]["reason"], am.MAX)
        self.assertEqual(by["zero_cells"]["served"], 0)
        self.assertEqual(by["sens6_refuse"]["refuse"], 1)

    def test_one_cell_budget_rule(self):
        # N=1 on an unlimited-GPU host, host side: B = 0.8 x avail - 1.5 GiB, everything full when it fits
        k = am.default_caps()
        k.update(mem_avail=am.SIZE_MAX, csirs=1, recovery=1, gw=1)
        p = am.plan(k, [dict(rb=273, scs=30, rx=4)])
        self.assertEqual((p["cell"][0]["snap"], p["cell"][0]["csirs"], p["cell"][0]["td"], p["cell"][0]["gw"]), (8, 640, 1024, 4096))
        self.assertEqual(am.host_calc(9216 * am.MIB), 9216 * am.MIB // 5 * 4 - 1536 * am.MIB)

    def test_table_runs(self):
        am.table()


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--cli" in args:
        i = args.index("--cli")
        CLI = args[i + 1]
        del args[i:i + 2]
    unittest.main(argv=[sys.argv[0]] + args)
