# DECODE-RES (csd/decode-res, 2026-10-10) -- items 1-5

Host DGX GB10. Replays: capture b (raw_pci415_12s_b), env ota/t26j.env minus radio flags (tests/passive_rx/decres_replay.sh), heavy.sh. Capture 8 not used. Capture 8 (raw_pci415_12s_20261008) used from the second session on. Evidence labels: [MEASURED] = DGX replay/OTA log, [INFERRED] otherwise. Replays are bimodal in solver acquisition (see end).

## Item 1 (class B)
Flags (all default off): ISAC_PDSCH_TINT_AB (paired TINTAB diagnostic), ISAC_PDSCH_TINTERP_RETRY (flipped time interpolation arm), ISAC_PDSCH_EVM_RB (DECODEEVMRB per-RB map), ISAC_PDSCH_OWN_PDCCH_RM (+ ISAC_PDSCH_OWN_PDCCH_DB, default 4).

1. TINTERP [MEASURED, paired, same samples, 1 run, 285 MCS0-4 >=10 dB grants]: flipping ISAC_CHEST_TINTERP changes SNR on 1667/5220 decodes but CRC on 0 grants (primary-only 0, flipped-only 0). It is NOT the class-B lever. The within-symbol SNR drift (sym3 26 dB -> sym10 17 dB) is real but the failures are not decided by it.
2. OTA ab_J1 [MEASURED]: class B fail concentrates in S=1/L=13 (nsym 11) grants in slots 1-5/11-15, RB0 start: 54-65 % ok vs 95-98 % for nsym 12 (S=0) and 85-99 % outside those slots. In replay, the DCIRAW TDRA field (payload bits 16-19) is 0000 for 120/123 pass and 1000 for 93/108 final pass: failures are the TDRA=1000 population (true S=0, parsed S=1/L=13; the DECODETDRA arm recovers most).
3. Root-cause hypothesis [MEASURED + INFERRED]: a true S=0 grant loses the PRBs of its own PDCCH on symbol 0 (TS 38.214 5.1.4.1). Per-RB DD-SNR maps (DECODEEVMRB) show symbol 0 dirty on a contiguous PRB run in the remaining failures (e.g. 0x37f3 192.5 RB 48-59, 478.5 RB 48-59, 202.14 RB 24-35; runs are multiples of 6 PRB = CCE size).
4. Fix: ISAC_PDSCH_OWN_PDCCH_RM=1 runs last in the retry chain: decode S=0, detect the dirty run, retry with those PRBs rate-matched on symbol 0 (SSB-plan mechanism), +-1 PRB edge hypotheses, logs OWNPDCCH. Gains over all other arms (identical samples): 12 / 11 / 3 grants in three runs (previously 0 in every baseline run). Pooled whole-run class-B MCS0-4 >=10 dB: base 85.4 / 91.7 %, with flag 88.0 (arm ran first, buggy order) / 91.6 / 71.4 % (low-sample run). Run-to-run sample sets differ (bimodal acquisition), so these totals are NOT evidence; the per-grant gains are.
5. Gate (class B >= 95 %) NOT met: the arm recovers ~10 grants per run of ~10-15 remaining failures at best; the rest (e.g. clean 0x007f S=2/L=12, 0x37f3 860.12 at 21 dB) are unexplained.

## Item 1 update (second session): multi-run PDCCH exclusion, capture b and capture 8
The own-PDCCH detector now keeps every dirty PRB run (an AL>=2 PDCCH with an interleaved CORESET is several 6-PRB bundles, not one run), and tries 7 shapes
(exact, +-1 PRB, longest run only, snapped to 6-PRB multiples). The exclusion is a per-CRB flag array (`nr_ssb_rm_only_crb`, 3 lines in nr_ssb_rate_match.{h,c}; NULL = unchanged).
Three runs per capture, flags `ISAC_PDSCH_DECODE_DIAG=1 ISAC_PDSCH_OWN_PDCCH_RM=1` (arm runs after every other arm, so each win is a grant ALL other arms failed on identical samples):

| run | capture | EVMGRANT | arm wins (all grants) | wins in MCS0-4 >=10 dB Nl1 QPSK | class B MCS0-4 >=10 dB final |
|---|---|---:|---:|---:|---|
| bo1 | b | 5402 | 11 | 4 | 92/102 (90.2 %) |
| bo2 | b | 2732 | 6 | 2 | 53/58 (91.4 %) |
| bo3 | b | 3872 | 7 | 5 | 57/62 (91.9 %) |
| c8o1 | 8 | 969 | 6 | 0 | 43/50 (86.0 %) |
| c8o2 | 8 | 487 | 6 | 1 | 48/55 (87.3 %) |
| c8o3 | 8 | 677 | 6 | 0 | 29/32 (90.6 %) |
Flag-off reference (b): 85.4 % and 91.7 % (base2/base3); capture 8 has no flag-off run of the new code (earlier session runs c8opr1-3, old single-run detector: 5/4/2 wins, class B 85.5/86.7/85.4 %).
**Class-B gate (>= 95 %) NOT met on either capture**; the arm adds a few percent at most and the whole-run totals stay inside run-to-run noise.

## Why wide grants fail [MEASURED, OTA ab_J1+J2, Qm>=4, Nl<=2, SNR margin >= 4 dB over a Shannon+2 dB requirement, DECODETDRA recoveries counted]
| grant | pass |
|---|---|
| wide (>=96 PRB) from RB 0, 14 symbols (S=0) | Nl1 2/23 (9 %), Nl2 4/74 (5 %) |
| wide from RB 0, 13 symbols | Nl1 37/109 (34 %), Nl2 101/300 (34 %) |
| narrow (<=32 PRB), 14 symbols | Nl1 88 %, Nl2 94-100 % |
A wide S=0 grant always overlaps its own PDCCH on symbol 0; a narrow one rarely does. In S=1 grants symbol 1 is clean (4 % noise level, `decode`-RB map on b), so the 13-symbol wide failures are
mostly S=0 grants parsed as S=1/L=13 (the TDRA alias, TDRA field 1000 in 93 of 108 final passes vs 120/123 for 0000). All CBs fail together (DECODEDIAG `cb=[..:0,..:0]`), sign errors ~0: a rate-matching / RE-count error, not SNR.
This is the strongest OTA lever left: `ISAC_PDSCH_OWN_PDCCH_RM=1` was never run on air; the OTA window should show wide S=0 grants (5-9 % today) rising.

## Item 2: non-TRS full-band CSI-RS (abs%160 24, 63, 105, 135)
Pattern from CSIFULL.md section 1 (raw-IQ map, not re-run here): 24: sym 10-13 sc {0-3, 8-11}; 63: sym 10-13 sc 4-11; 105: sym 10,12,13 sc 0-3; 135: sym 7 sc 4-7; all RBs.
Implemented as `ISAC_PDSCH_STATIC_CSI_RETRY=1` (default off, network-specific): on a failed grant of those four phases, retry with the pattern as row-4 zero-power resources (grant S and toggled S), log `STATICCSI`.
[MEASURED] **No measurable benefit and no measured need**: pooling all my replays (unique-grant outcomes not deduplicated across runs), Nl1 MCS0-9 >=10 dB pass rate is phase 24: 59/60, 105: 24/28, 135: 57/69, 63: no grants, versus 2277/2707 (84 %) elsewhere. Those slots are not worse than the rest.
The four replays that ran the arm (bsc1/2, c8sc1/2) acquired badly (50-140 EVMGRANT) and tried 3 grants, 0 recoveries. Verdict: keep off; re-test only if OTA shows excess failures at those phases.

## Item 3: RV != 0 [MEASURED, 3 b replays, all dedicated attempt-0 full decodes]
RV is payload bits 26-27, NDI bit 25, MCS bits 20-24, HARQ PID bits 28-30 (3 bits, mod 8; the parsed `pid=` in DECODEDIAG is always 0 - the metadata bug of DECODEFIX).
| RV | >=10 dB (QPSK+QAM) | <10 dB | passes |
|---|---:|---:|---|
| 0 | 2457 grants | 10 000 | 5700 |
| 1 | 5 | 283 | 0 |
| 2 | 5 | 885 | 1 |
| 3 | 0 | 645 | 0 |
RV != 0 is 0.4 % of >=10 dB grants (0 of the MCS0-4 >=10 dB failures in these captures) and ~15 % of <10 dB grants, where nothing self-decodes (1 pass of ~1800). The rate-matching start uses TS 38.212 Table 5.4.2.1-2 k0 (index_k0 {0,17,33,56} BG1 / {0,13,25,43} BG2, scaled by Ncb/N) in `nr_rate_matching_ldpc_rx` - checked, correct, and used for rv 1-3. Remaining share: all of the RV != 0 grants (needs HARQ history).

## Item 4: 2-layer 64/256QAM margin list [MEASURED OTA ab_J1+J2, tools/decres_margin.py; requirement = Shannon(Qm*R)+2 dB; snr = grant DD-EVM SNR (mean over layers; per-layer needs QAMAB)]
Nl2 Qm4: margin >=4 dB 1376/1867 (74 %); Qm6 >=4 dB 186/532 (35 %); Qm8 0/51. Nl1 Qm4 >=4: 66 %, Qm6 49 %, Qm8 1/18.
1557 grants fail with >= 2 dB margin (list: `decres_margin.py --list`, top 40 have 14-24 dB margin and are all wide RB0 1-layer 16QAM - 0x0f0a 254.13 mcs5 217 PRB 28 dB, 0x3fc3 657.12 96 PRB 27.8 dB ...).
The dominant remaining error is the same wide-grant failure above, not PT-RS, LBRM/Ncb (Ncb=N, lbrm 999576 > TBS, fine) or the Qm=8 path (the Qm=8 failures are wide 217-PRB grants too). A genuinely Qm=8-specific error could not be separated from it; fix expected only via the own-PDCCH arm and its OTA test.

## Item 5: DCI misses (HARQ-PID continuity), current code `tools/decres_dcimiss.py` [MEASURED, replay; ESTIMATE]
PID = 3 bits at payload bit 28 (mod 8), mm=0 DCIs only; step 0 = repeat, +1 normal, 2/3 = 1/2 missed. Per RNTI with a clean +1 pattern (>= 0.5):
| capture | RNTI | DCIs seen | est. missed | miss % |
|---|---|---:|---:|---:|
| b (base2) | 0x37f3 | 381 | 31 | 7.5 |
| b | 0x5d93 / 0xe513 / 0x59ab | 202/184/170 | 23/9/14 | 10.2/4.7/7.6 |
| b | 0xb3d3 / 0x5aea / 0x3383 / 0x65d3 | 144/129/69/48 | 4/3/1/3 | 2.7/2.3/1.4/5.9 |
| b | pooled reliable RNTIs | 1559 | 112 | **7.2** |
| 8 (c8opr1) | 0x8f5a | 394 | 17 | **4.1** (c8opr2: 4.6 %) |
| 8 | 0xb876 | 298 | 21 | 6.6 (c8opr2: 9.1) |
| 8 | 0xe01e | 61 | 24 | PID pattern not +1 (frac 0.21): UNRELIABLE, cannot be estimated this way |
Previous: ~6 % for 0x8f5a, ~30 % for 0xe01e (before ACQ-FIX). 0x8f5a is now ~4 %; 0xe01e cannot be re-measured with this method (and had only 61 DCIs).

## Bimodality
Solver acquisition: runs with ~50-140 EVMGRANT (of 24 total on b/c8) show `stats ... probe_ok=0 candidates=0 solved=0` (layouts never get two CRC passes; probe CRC is ~4-5 %). On capture 8, 4 of 13 runs; on b, 2 of 13. Not chased.

## OTA env
t26j.env + ISAC_PDSCH_OWN_PDCCH_RM=1 (ISAC_PDSCH_STATIC_CSI_RETRY stays off; optional diagnostics: ISAC_PDSCH_DECODE_DIAG=1 ISAC_PDSCH_EVM_RB=1). Do not set ISAC_PDSCH_TINTERP_RETRY / ISAC_CHEST_TINTERP.
Tools: tools/decres_final.py, decres_rbmap.py, decres_tintab.py, decres_dcimiss.py, decres_margin.py.
