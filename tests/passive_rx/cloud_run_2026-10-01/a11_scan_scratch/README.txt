A11 ISAC_SCAN_SCRATCH_MB (code + default-unchanged regression). Timing measurement is DGX-only; not run in cloud session 2026-10-01.
Label: [OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, base b6e5fb27ac + A11 working tree] gtest; [SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, base b6e5fb27ac + A11] rfsim arms

gtest: test_nr_initial_sync_budget 5/5 PASS (default 512 == old formula; clamp 64/16384; len_thr cap; env parse). Seen failing first against a stub.
ctest: 3 failures, all in the host baseline set: test_thread-pool, time_management_tests, nr_cuup_functional_test.

== Arm 1: env unset, GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5 (PASS) ==
{"arm": "base_r1", "bank_len": 46, "conv_s": 35.83, "cpu_pct": 205, "crc_pct": 95.4, "drop_full_pct": 0.713, "first_crnti_s": 32.035, "ldpc_ok": 16284, "ldpc_seg_fail": 727, "max_rss_kb": 889324, "n_converged": 2, "pdsch_crc_ok": 16284, "pdsch_decoded": 17070, "scanq_drop_full": 945, "scanq_queued": 132546, "sync_s": 6.874, "ttc_s": 3.795}
PASS base_r1
6.576 [0m[NR_PHY] Scan scratch budget 512 MB (ISAC_SCAN_SCRATCH_MB), 4 MB per GSCN, 8 worker threads: scanning 1 GSCN in batches of 1

== Arm 2: ISAC_SCAN_SCRATCH_MB=2048 (PASS) ==
{"arm": "base_r1", "bank_len": 46, "conv_s": 34.564, "cpu_pct": 205, "crc_pct": 96.33, "drop_full_pct": 1.1037, "first_crnti_s": 29.971, "ldpc_ok": 20386, "ldpc_seg_fail": 707, "max_rss_kb": 887224, "n_converged": 2, "pdsch_crc_ok": 20386, "pdsch_decoded": 21162, "scanq_drop_full": 1772, "scanq_queued": 160556, "sync_s": 6.417, "ttc_s": 4.593}
PASS base_r1
6.191 [0m[NR_PHY] Scan scratch budget 2048 MB (ISAC_SCAN_SCRATCH_MB), 4 MB per GSCN, 8 worker threads: scanning 1 GSCN in batches of 1

Note: rfsim bed (106 PRB, 1 GSCN) gives batch 1 in both arms; the knob parses/logs, behaviour identical. Raw logs: /tmp/claude-0/ev/a11{,_2048}.
