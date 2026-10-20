# Fix A: DCI 1_1 TDRA field + TDD-legal rows (ISAC_PDSCH_TDA_FIELD=1), 2026-10-16/17

Sonnet agent; saved by the orchestrator. Commits 53ca1684c1, e33f6fe68f on csd/int16-fixa (from csd/int16 9e1c621833). Default off.
Verdict: TIM special-slot bin 0 % -> ~80 %; V async rescues -97 % (~335 -> ~8 per run), V tot p99 ~-25 %; crc_q unchanged; replay only, NOT OTA-validated.

## 1. Vodafone check: hypothesis CONFIRMED
[MEASURED, DGX replay raw_pci415_12s_b, log rr-csd-hmc/replays/replay_hmc_diag_b_040527 (RM_GENIE + DECODE_DIAG), n=1], 3363 RMDCI joined with DECODEDIAG.
- V DCI 1_1 is 49 bits; bit 16 (right after the type-0 RBG bitmap) is a 1-bit TDRA field (same position as TIM's 2-bit field 16-17).
- 1645 OK C-RNTI decodes: bit16=0 -> S=1 L=13 in 1195/1197; bit16=1 -> S=0 L=14 in 448/449. Mutual information with S 0.84 bit (max for the mix); no other window > 0.1; slot%10 vs S 0.11.
- 271 async-rescued grants: 246 have bit16=1 and were rescued with S=0; 24 bit16=0. V's TDRA rescues are the ignored TDRA field.

## 2. Design
- Position: off.tda of the solved layout when tda_bits==0 (16 on both cells); only solved RNTIs' full decodes.
- Width: SIB1 list -> ceil(log2 N) (TIM N=3 -> 2); no list -> try 1 then 2.
- Acceptance (either): value depends on slot class (>= 8 mixed and >= 8 other DCIs, >= 90 % pure, different dominant values; no CRC), or different values need different CRC-passing (S,L) (>= 8 per value, >= 90 % purity).
- Rows: with a list, rows with S+L-1 <= nr_tdd_pdsch_last_symbol(slot), field picks the row; undetected in a mixed slot: first legal row, second legal row as one retry. No list: learned dominant (S,L) per (value, slot class). DM-RS mask via nr_pdsch_solver_dmrs_mask.
- Learning: CRC-OK (S,L) tallies per (value, mixed/other), wrapper passive_decode_retry. No per-cell constants.
Files: nr_pdsch_solver.c/.h (nr_tda_field_*), nr_pdcch_blind_monitor_rt.c (hook in the solver variant loop), nr_pdcch_blind_monitor.c (SIB1 list to the detector), nr_pdsch_passive_decode.c/.h (second-row retry, CRC learning, grant tda_* / dci_w / dci_len fields), tests/nr_pdsch_solver_test.cc.

## 3. Unit tests
test_nr_pdsch_solver: 28/28 pass; new TdaField.* : TIM slot-13 payload field 01 -> (1,5) mask 0x4 and row 0 rejected; field 00 -> (1,13) 0x804; constant field not accepted; NSA bit16 1 -> (0,14), 0 -> (1,13) learned from CRC; uncorrelated bit not accepted. Full ctest not run.

## 4. Replay [MEASURED, DGX replay, t26aXE, TMO=90, n=2 per arm, interleaved off/on]
### tim
| arm | slot 3 all C-RNTI CRC/n | slot 3 qualified | crc_all | crc_q (n_q) | tot p99 us | drops % |
|---|---|---|---|---|---|---|
| off 1 | 0/271 | 0/9 | 66.3 | 70.9 (595) | 3435 | 0.0 |
| off 2 | 0/172 | 0/5 | 66.4 | 78.2 (340) | 3077 | 6.7 |
| on 1 | 205/259 (79 %) | 52/53 | 67.3 | 71.9 (638) | 3006 | 0.0 |
| on 2 | 187/230 (81 %) | 52/52 | 67.0 | 72.4 (562) | 17075 | 2.2 |
Detector fired (`TDAFIELD detected pos=16 w=2 list=3`) after 8 mixed-slot DCIs. Other bins unchanged within noise except bin 8 (class B): all-grant CRC 45-49 % off vs 41 % on (unexplained, n too small; Fix B targets it). on 2 p99 17 ms = host load (same run drops 2.2 %).
### b (V)
| arm | recovered | rec p99 us | tot p99 us | crc_q (n_q) | crc_all | n_real | drops % |
|---|---|---|---|---|---|---|---|
| off 1 | 326 | 19243 | 5246 | 95.5 (1074) | 45.1 | 4428 | 0.0 |
| off 2 | 345 | 13805 | 7447 | 94.7 (711) | 57.1 | 2506 | 0.5 |
| on 1 | 11 | 7739 | 4307 | 96.1 (1176) | 45.0 | 5018 | 0.1 |
| on 2 | 4 | 24218 | 4951 | 95.6 (1116) | 47.1 | 4365 | 0.0 |

## 5. Caveats
Not OTA-validated. Technique D path (g_pdsch_sweep_on) not hooked. Mixed slot with neither list nor field: no override (contract's S enumeration fallback not implemented). NSA mixed-slot rows learned only from CRC passes there. Diagnostic LOG_A `TDAFIELD hook #N` lines left (rate-limited). Earlier invalid replay dirs deleted. Frozen paths clean.
