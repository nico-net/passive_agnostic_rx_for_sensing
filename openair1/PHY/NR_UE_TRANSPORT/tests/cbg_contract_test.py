#!/usr/bin/env python3
"""Source-contract RED tests for passive CBG support; no build or radio required."""
import pathlib
import re
import unittest

HERE = pathlib.Path(__file__).resolve().parent
PHY = HERE.parent
ROOT = PHY.parents[2]
HEADER = (PHY / "nr_pdcch_blind_monitor.h").read_text()
DECODER = (PHY / "nr_pdcch_blind_monitor.c").read_text()
RUNTIME = (PHY / "nr_pdcch_blind_monitor_rt.c").read_text()
QUEUE = (PHY / "nr_pdsch_passive_queue.c").read_text()


def struct_body(source, typedef):
    marker = "/// Result of one blind decode+extract attempt."
    start = source.index(marker)
    begin = source.index("typedef struct {", start)
    end = source.index(f"}} {typedef};", begin)
    return source[begin:end]


def require(test, condition, message):
    test.assertTrue(bool(condition), message)


class PassiveCbgContractTests(unittest.TestCase):
    def test_dci_result_preserves_cbgtI_and_cbgfi(self):
        result = struct_body(HEADER, "nr_pdcch_blind_result_t")
        require(self, re.search(r"\bcbgti\b", result), "decoded result lacks CBGTI")
        require(self, re.search(r"\bcbgfi\b", result), "decoded result lacks CBGFI")

    def test_decoder_splits_and_preserves_cbgtI_and_cbgfi(self):
        require(self, re.search(r"out->cbgti\s*=\s*read_field\(", DECODER), "decoder does not preserve CBGTI")
        require(self, re.search(r"out->cbgfi\s*=\s*read_field\(", DECODER), "decoder does not preserve CBGFI")
        require(self, not re.search(r"\(void\)read_field\(payload,\s*&pos,\s*f\.cbg\)", DECODER), "decoder still discards combined CBG field")

    def test_runtime_grant_and_job_preserve_cbg_mask_and_flush(self):
        require(self, re.search(r"\.cbgti\s*=\s*out\.cbgti", RUNTIME), "runtime grant drops CBGTI")
        require(self, re.search(r"\.cbgfi\s*=\s*out\.cbgfi", RUNTIME), "runtime grant drops CBGFI")
        require(self, re.search(r"cbgti", QUEUE), "queue job lacks CBGTI")
        require(self, re.search(r"cbgfi", QUEUE), "queue job lacks CBGFI")

    def test_selected_group_retransmission_and_cbgfi_flush_are_explicit(self):
        helper = PHY / "nr_pdcch_blind_cbg.h"
        self.assertTrue(helper.exists(), "missing bounded per-HARQ/per-CBG state helper")
        text = helper.read_text()
        self.assertIn("nr_pdcch_blind_cbg_process", text)
        self.assertIn("CBGTI", text)
        self.assertIn("CBGFI", text)
        self.assertIn("flush", text.lower())
        self.assertIn("combine", text.lower())

    def test_harq_pid_0_and_16_have_distinct_bounded_history(self):
        body = re.search(r"static uint32_t blind_harq_tag\(.*?\n\}", RUNTIME, re.S)
        self.assertIsNotNone(body)
        self.assertNotRegex(body.group(0), r"harq_pid\s*%\s*16")
        require(self, re.search(r"NR_PDCCH_BLIND_CBG_MAX_CONTEXTS\s*=\s*64", RUNTIME), "bounded 64-context history missing")
        require(self, re.search(r"NR_PDCCH_BLIND_CBG_MAX_HARQ_PID\s*=\s*32", RUNTIME), "full 5-bit HARQ PID bound missing")

    def test_initial_full_tb_and_reassembled_cbg_tb_are_bit_identical(self):
        test_source = PHY / "tests/nr_pdcch_blind_cbg_test.cc"
        self.assertTrue(test_source.exists(), "missing executable CBG state-machine tests")
        text = test_source.read_text()
        require(self, re.search(r"InitialTbAndCbgReassemblyAreBitIdentical", text), "missing bit-identity test")
        require(self, re.search(r"SelectedCbgRetransmissionReassemblesOriginalTb", text), "missing selected-group test")
        require(self, re.search(r"CbgfiZeroFlushesOnlySelectedGroups", text), "missing CBGFI flush test")
        require(self, re.search(r"HarqPidZeroAndSixteenUseDistinctContexts", text), "missing PID alias test")


if __name__ == "__main__":
    unittest.main(verbosity=2)
