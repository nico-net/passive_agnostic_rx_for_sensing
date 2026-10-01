Task A2 evidence: ISAC_METRICS JSON, 1 rfsim 106-PRB arm, 150 s, ISAC_METRICS_PATH set.
[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, base 456be54aa7 + A2 change (commit follows)]
- 7 "ISAC_METRICS {" log lines and 7 JSONL lines (>=6 expected); metrics_tail.jsonl = the file content.
- Last JSON pdschq_crc_ok=15526 == last text "PDSCHQ ... crc_ok=15526".
- gate_score.json: crc 96.75 %, drop_full 1.10 %, CONVERGED 2/2 (GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5 PASS).
- ctest_summary.txt: only known env-only failures; test_nr_passive_metrics 3/3.
- pci=0 is the cell's Nid_cell as set at PBCH lock (UE->frame_parms.Nid_cell).
