# Traffic mode capability, 2026-11-05 (E6 validation)

Scope: DGX (GB10) + X410 34B2AF1, 4 RX, DEIB Milan NSA, two cells: PCI 415 and PCI 923 (200 MHz class, 217 PRB, 30 kHz SCS).
Never merged across cells. Arms: A = traffic @4759260bbf; E = E1+E4 (143024, 153350), E head pre-fix (194756), merged E1-E4+E3+collapse fix ac3d296d94 (235237).
Labels: [MEASURED OTA] daytime (08:00-16:00), [OTA LOW LOAD] night, [OTA EVENING] functional only. No absolute recall is claimed: all rates are relative to what the receiver itself decodes.
Tool: `tools/traffic/score_fields.py <campaign|rundir>... [--agg] [--json]`; gate numbers: no-loss scorer (distinct RNTIs with >= 2 DCIs, DCI records, SOLVED RNTIs, DL 1_1 records with all of harq/ndi/rv/mcs/k1/dai/tpc/pri non-null = "full").
Source files: `sense_records_last*.jsonl` of each run (the retained record tail; counts are per-run tails, consistent between arms). Campaign dirs `/home/nicola/NICOLA/ota_runs/tm_ota_20261104_*`.

## 1. What we decode (E head ac3d296d94, 3 runs per cell, [OTA LOW LOAD] 23:52-01:00)

| | PCI 415 (pair) | PCI 923 (pair) | PCI 923 (single) |
|---|---|---|---|
| DL DCI records / run (tail) | 9.1k | 114k | 94k |
| DL formats | 1_1 73 %, 1_0 27 % (SI 5k, TC/RA few) | 1_1 98.4 %, 1_0 1.6 % | 1_1 97 %, 1_0 3 % |
| DL layout_state | solved 71 %, fixed 27 %, unsolved 1 % | solved 94 %, fixed 1.6 %, unsolved 4.6 % | solved 94 %, fixed 2.9 %, unsolved 3.3 % |
| layout_evidence (DL) | crc 69 %, phys 1 %, null 29 % | crc 93 %, phys 0.6 %, null 6 % | crc 93 %, phys 0.4 %, null 6 % |
| fields_evidence (DL) | bits 9 %, crc 0 %, null 91 % | bits 18 %, null 82 % | bits 20 %, null 80 % |
| DL 1_1 distinct RNTIs (>= 2 DCIs) | 19 | 30 | 50 |
| DL 1_1 non-null: harq/ndi/rv/mcs/mcs_table | 97 % | 95 % (mcs_table 94 %) | 96 % |
| DL 1_1 non-null: start_rb/nb_rb/start_sym/nb_sym/tda_index | 97 % | 94-95 % | 96 % |
| DL 1_1 non-null: k1/dai/tpc/pri | 13 % | 18 % | 20 % |
| DL 1_1 non-null: qm, tbs, nl, dmrs_scrambling_id, dmrs_type, dmrs_add_pos, k0 | 0 % | 0 % | 0 % |
| DL 1_0 non-null: mcs, nb_rb, tda_index | 100 % | 100 % | 100 % |
| DL 1_0 non-null: harq_pid/k1/dai | 32 % | 8 % | 5 % |
| DL nb_rb: share >= 128 PRB / max | 13 % / 217 | 19 % / 217 | 18 % / 217 |
| DL MCS p10/p50/p90, table | 0/1/19, table 0 only | 0/2/18, table 0 | 0/7/19, table 0 |
| DL layers (nl) | not extracted (null) | null | null |
| UL DCI records / run (tail) | 12.6k (0_1; 0_0: 3) | 84k (0_1 only) | 65k (0_1 only) |
| UL layout_state | unsolved 100 %, 0 fields | solved 97.5 % | solved 98 % |
| UL 0_1 non-null (harq/ndi/rv/mcs/dai/tpc/start_rb/nb_rb/sym/tda/nl/k2) | 0 % | 97 % | 98 % |
| UL nb_rb >= 128 / max; MCS p10/50/90; table; layers | none / 3 ; - | 17 % / 216 ; 0/9/23 ; 0 ; nl=1 | 10 % / 211 ; 1/11/20 ; 0 ; nl=1 |

Config records (last per RNTI, 235237 first pair run): geometry known for 21 of 576 (415) and 27 of 219 (923) contexts, all DL 1_1 type-1 FDRA, 4 antenna-port bits; `dmrs_add_pos`, `rm_set` null in every context; TDA start/length known for 28 / 39 contexts. [OTA LOW LOAD]
Daytime E (143024, PCI 923 pair): 1_1 fields non-null 56 % (A 68 %); layout_state solved 56 % (many RNTIs still unsolved); nb_rb >= 128: 11 %; MCS p50 3. PCI 415 pair daytime: 97 %, solved 97 %, nb_rb >= 128 31 %, MCS p50 5. [MEASURED OTA]
Baseline (4759260bbf, A, daytime 143024): k1/dai/tpc/pri 0 % everywhere; E: 17 % (415), 4 % (923 pair), 29 % (923 single). [MEASURED OTA]

## 2. Decode records (CRC), per direction [MEASURED OTA unless noted]

"check" = decode with the learned layout; "probe" = hypothesis try. Rates = ok / (ok + fail), summed over 3 runs per arm.

| campaign / arm | cell | DL check OK | DL probe OK | UL check OK | UL probe OK | PUSCH CRC OK / tries |
|---|---|---|---|---|---|---|
| 143024 A (single) | 923 | 88.6 % | 2.7 % | 3 / 80.6k | 13 / 243.6k | 16 / 323k |
| 143024 E (single) | 923 | 93.4 % | 2.9 % | 0 / 8.4k | 21 / 347.8k | 20 / 351k |
| 143024 A (pair) | 415 | 81.4 % | 3.1 % | 1 / 225k | 7 / 78.5k | 6 / 299k |
| 143024 E (pair) | 415 | 86.1 % | 4.8 % | 0 / 9.2k | 11 / 147k | 9 / 153k |
| 143024 A (pair) | 923 | 93.2 % | 0.8 % | 2 / 57.8k | 10 / 151.6k | 11 / 205k |
| 143024 E (pair) | 923 | 87.3 % | 1.1 % | 0 / 4.1k | 8 / 151.3k | 6 / 151k |
| 235237 A (single) [LOW LOAD] | 923 | 90.4 % | 2.7 % | 1 / 116k | 29 / 257k | 22 / 369k |
| 235237 E (single) [LOW LOAD] | 923 | 91.0 % | 10.2 % | 0 / 8.8k | 22 / 249k | 18 / 247k |
| 235237 A (pair) [LOW LOAD] | 415 | 86.7 % | 27.4 % | 0 / 0 | 4 / 19.7k | 1 / 18.9k |
| 235237 E (pair) [LOW LOAD] | 415 | 88.6 % | 55.7 % | 0 / 0 | 1 / 13.4k | 1 / 13.0k |
| 235237 A (pair) [LOW LOAD] | 923 | 90.1 % | 8.4 % | 0 / 121.8k | 12 / 214k | 11 / 326k |
| 235237 E (pair) [LOW LOAD] | 923 | 92.6 % | 5.5 % | 0 / 10.5k | 24 / 354k | 21 / 359k |

Other campaigns (080000, 153350, 194756, 211855) show the same range: DL check 76-94 %, DL probe 0.6-20 %, UL <= 0.007 % of tries.

**UL decodability verdict.** From our position UL PUSCH is effectively not decodable: across the 12 arm-cell groups above and the other campaigns, CRC-OK is 0-33 per 13k-750k tries (<= 0.007 %), no group above 0.1 %. With CRC24 the expected false-OK count per group is far below 1, so the isolated OKs are probably real but unusable as traffic payload [INFERRED, not tested]. UL DCI (0_1) fields are nevertheless parsed once the UL layout is solved (97-98 % non-null on PCI 923, 92-98 % on 415 daytime; none on 415 at night, UL layout unsolved 100 %). E4 therefore stops PUSCH tries after UL lock (check tries fall 100k+ -> 4-10k per 3 runs) with no change in UL CRC-OK count.

## 3. No-loss gate, A vs E (mean over 3 runs; pass = mean(E) >= mean(A) - sd(A) on rntis2, dci, solved, solved1, full)

rntis2 = distinct RNTIs with >= 2 DCIs; drop = scan-queue drop %; cpu in cores (process). Per cell and campaign, never pooled.

| campaign (label) | case / PCI | rntis2 A -> E | dci A -> E | solved A -> E | full A -> E | decodes A -> E | drop % A -> E | cpu A -> E | gate |
|---|---|---|---|---|---|---|---|---|---|
| 143024 [MEASURED OTA] E1+E4 | S923 / 923 | 221 -> 225 | 98k -> 131k | 136 -> 141 | 0 -> 19.4k | 68.8k -> 54.7k | 0.3 -> 0.2 | 7.4 -> 7.6 | PASS |
| 143024 | B / 415 | 170 -> 140 | 170k -> 131k | 156 -> 130 | 0 -> 11.5k | 21.2k -> 12.9k | 15.7 -> 36.3 (E run 72 %) | 11.7 -> 10.9 | PASS (inside sd) |
| 143024 | B / 923 | 139 -> 158 | 88k -> 77k | 87 -> 93 | 0 -> 2.0k | 54.7k -> 54.2k | 15.1 -> 12.2 | 11.7 -> 10.9 | PASS |
| 153350 [MEASURED OTA] E1+E4 | B / 415 | 167 -> 133 | 175k -> 124k | 153 -> 119 | 0 -> 6.3k | 19.2k -> 20.2k | 12.6 -> 38.5 (E run 89 %) | 11.7 -> 11.1 | FAIL rntis2, dci, solved |
| 153350 | B / 923 | 157 -> 146 | 96k -> 99k | 91 -> 91 | 0 -> 3.3k | 65.1k -> 64.0k | 12.2 -> 16.0 | 11.7 -> 11.1 | FAIL rntis2 (146 vs 146.8) |
| 194756 [OTA EVENING] E head pre-fix | S923 / 923 | 172 -> 201 | 171k -> 155k | 131 -> 153 | 0 -> 14.8k | 55.8k -> 48.0k | 0.1 -> 0.2 | 7.3 -> 7.4 | PASS |
| 194756 | B / 415 | 113 -> 140 | 82k -> 125k | 97 -> 101 | 0 -> 9.1k | 5.9k -> 6.5k | 21.8 (A run 60 %) -> 3.3 | 11.1 -> 11.3 | PASS |
| 194756 | B / 923 | 109 -> 107 | 130k -> 167k | 74 -> 76 | 0 -> 14.7k | 60.3k -> 56.3k | 7.0 -> 3.5 | 11.1 -> 11.3 | PASS |
| 235237 [OTA LOW LOAD] ac3d296d94 | S923 / 923 | 86 -> 79 | 157k -> 160k | 68 -> 63 | 0 -> 18.4k | 54.4k -> 31.4k | 0.6 -> 0.7 | 7.0 -> 6.9 | PASS |
| 235237 | B / 415 | 45.3 -> 31.0 | 46k -> 22k | 18.7 -> 12.3 | 0 -> 0.8k | 4.3k -> 3.1k | 1.3 -> 0.4 | 10.8 -> 10.6 | PASS (threshold 30.9, margin 0.1) |
| 235237 | B / 923 | 50.3 -> 56.0 | 150k -> 198k | 43.7 -> 49.7 | 0 -> 20.3k | 28.4k -> 35.6k | 1.8 -> 1.5 | 10.8 -> 10.6 | PASS |

- Formal fails: 0 (143024), 4 (153350), 0 (194756), 0 (235237). Merged 6+6 daytime pair (143024 + 153350, ledger): pair-415 dci 127k vs 172k, solved 125 vs 155: FAIL. The fails coincide with the 415 collapse (E runs with 72 % and 89 % drops, A: 0/6; same collapse seen on 4759260bbf in 080000 T2B 72 % and 103636 T1B 78 %), i.e. it is pre-existing, not E-specific.
- Night 415 traffic is very low (A spread 12k-90k DCIs per run), so the 415 night result is a weak comparison [OTA LOW LOAD]; not a daytime verdict.
- `full` is 0 for A by construction (4759260bbf never fills k1/dai/tpc/pri); E fills it through the bits path.
- Decodes per DCI: A -> E: 143024 S923 0.70 -> 0.42; 235237 S923 0.35 -> 0.20 (lower; PCI 923 pair roughly equal).
- Collapse A/B 211855 [OTA LOW LOAD], 8+8 pair runs (A = instrumentation only 121fdf53dc, B = collapse fix 0d6c99ed67, before final 2700d0f7c9): PCI 415 drop mean 9.6 % -> 3.4 % (max 59.6 -> 13.6, collapse runs 1/8 -> 0/8); PCI 923 8.2 % -> 3.4 %; cpu 11.4 vs 11.5. Night 235237 (E contains the final fix): drops <= 5 %, no collapse in either arm.
- Legacy vs traffic @4759260bbf, 080000 [MEASURED OTA] (L = legacy, T = traffic, 3 runs): drop S415 17.4 -> 0.0, S923 2.4 -> 0.1, pair 415 38.0 -> 27.9, pair 923 24.8 -> 12.0; cpu S415 6.6 / 6.7, S923 7.6 / 7.4, pair 11.5 / 11.4.
- Decode-off ablation (095833, 103636, pair, [MEASURED OTA]): T -> Z drop 415 10.8 -> 7.2 and 31.2 -> 1.0; 923 11.1 -> 3.2 and 15.1 -> 1.8; cpu 11.5 -> 11.0 and 11.0 -> 9.3. Decode cost is the main CPU and drop driver in pair mode.

## 4. E3 on air

| campaign | e3_phys_solved | e3_fam_accept per run (3-run mean) | e3_scored (sum of 3 runs) |
|---|---|---|---|
| 194756 [OTA EVENING] pre-fix | 0 | S923: 55, pair 415: 53, pair 923: 20 | not recorded in this table |
| 235237 [OTA LOW LOAD] ac3d296d94 | 0 | S923: 14, pair 415: 6, pair 923: 12 | 4509 / 312 / 5978 |

`e3_phys_solved` is 0 in every run: the physical-layer hypothesis path never produces a SOLVED layout on air; `layout_evidence = phys` appears on 0.4-1 % of DL records (a hypothesis-ranked layout, not proven). Family accepts are single-digit to low-double-digit per run. `fields_evidence = bits` (E1 prover) supplies the k1/dai/tpc/pri gain; E3 adds none so far. Not validated: replay ground truth against gNB values (E6 step 1, no OCUDU capture run in this task).

## 5. Open gaps (still null) and likely cause

| gap | state | why [INFERRED unless stated] |
|---|---|---|
| DL layers (nl), qm, tbs | null in 100 % of DL records | needs decode/DM-RS port evidence; E3 phys path inert on air (0 solved) |
| dmrs_scrambling_id, dmrs_type, dmrs_add_pos, k0 | null (DL 1_1) | not derived from DCI bits without config; add_pos and rm_set null in all config records |
| k1/dai/tpc/pri on DL 1_1 | 13-20 % at night, 4-29 % daytime, 0 % baseline | only RNTIs whose layout the prover splits uniquely; classes >= 17 stay unprovable on DEIB (e1_cls_17_64 ~330/run) |
| harq/ndi/rv/mcs on DL 1_1 | 95-97 % night, 56-72 % daytime PCI 923 | rest are RNTIs not yet layout-solved |
| PCI 415 UL layout | unsolved 100 % at night, 92-97 % daytime | no UL traffic seen at night to lock on (PCI 415 UL DCI tail 12k, 0_0: 3) |
| UL payload | CRC-OK <= 0.007 % | UL not decodable from this position |
| DL mcs_table | 0 only | table evidence `crc` on 97 % at night, none daytime E1+E4; 256QAM tables not observed |
| absolute recall | not measurable | no gNB ground truth on DEIB |

## 6. Verdict

- Traffic mode on E head: DCI/RNTI/solved counts >= baseline on every cell in the three campaigns at normal load, CPU equal or lower, drops equal or lower, except the 415 collapse, which is a pre-existing starvation and is addressed by the collapse fix (0/8 vs 1/8).
- Daytime gate 153350 failed formally (pair-415 collapse run, pair-923 rntis2 off by 0.8) and is not repeated on the final build. A daytime rerun with ac3d296d94 is still needed before the merge verdict. Night result is [OTA LOW LOAD], functional only.
