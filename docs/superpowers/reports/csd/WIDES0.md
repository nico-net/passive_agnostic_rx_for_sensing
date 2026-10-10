# WIDE-S0 (csd/wide-s0, 2026-10-10): wide dedicated PDSCH grants starting at symbol 0

Host DGX GB10 (aarch64). Base `csd/otatest` 95fa2728e1. All builds, ctest and replays through `heavy.sh`; replays use
`tests/passive_rx/decres_replay.sh` (ota/t26j.env minus radio flags, ISAC_MLOCK=0) on capture b
(`raw_pci415_12s_b`, CAP_TICK=208029025) and capture 8 (`raw_pci415_12s_20261008`, CAP_TICK=210036277). No radio, no
hardware tools. Evidence labels: [MEASURED] = DGX replay of the recorded captures, [CODE] = source reading,
[INFERRED] = reasoning on measured data. Results are never pooled across captures.

## Root cause

**The receiver did not rate-match the scheduling PDCCH out of the PDSCH (TS 38.214 5.1.4.1), and the SSB rate-matching
mask the earlier DECODE-RES arm reused skipped symbol 0 in the demodulator fast path.**

1. [CODE, TS 38.214 5.1.4.1] "If a PDSCH scheduled by a PDCCH would overlap with resources in the CORESET containing
   the PDCCH, the resources corresponding to a union of the detected PDCCH that scheduled the PDSCH and associated PDCCH
   DM-RS are not available for the PDSCH." This cell's dedicated CORESETs (nID 62922 / 10163) are 1 symbol, 216 PRB,
   non-interleaved, starting at CRB 0: a DCI 1_1 at CCE c, AL L occupies whole PRBs 6c .. 6(c+L)-1 on symbol 0. A
   grant with S=0 that covers those PRBs loses 12 x 6L REs; every later bit shifts, so **all code blocks fail together
   with ~0 LLR sign errors** (DECODERES). A wide grant from RB 0 always covers its own PDCCH, a narrow one rarely does
   (hence 5-9 % vs 88-100 % on air). S=1 grants never overlap symbol 0 and decode normally; the 13-symbol wide failures
   are the S=0 grants parsed as S=1/L=13 (TDRA alias), retried as S=0 without the exclusion.
2. [MEASURED + CODE] The DECODE-RES own-PDCCH arm guessed the PRBs from symbol-0 EVM dips (the PDCCH, QPSK and often
   power-boosted, is not reliably a dip) and it was disabled in every slot with an SSB *candidate* (slots 0-3 and 10-13
   here), where most wide grants sit. Moving the exclusion to its own layer then exposed a second defect:
   `nr_dlsch_extract_rbs()` took the no-exclusion fast path whenever the SSB layer had no bit on the symbol
   (`nr_dlsch_demodulation.c:455`), so G was reduced but the REs were still extracted. Fixed for the new layer.

**What was ruled out** (all [MEASURED], paired on identical recorded LLRs, offline LDPC on the CPU `libldpc.so`):
CORESET-level exclusion (whole CORESET on symbol 0, and on symbols 0+1), symbol-0 erasure without shift, own-PDCCH
puncturing (erasure), all detected PDCCHs of other RNTIs, PDCCH-DMRS-occupied 6-RB groups (new `DECODEPDCCHOCC`
diagnostic), narrower allocations (top/bottom 1-16 PRB, TBS kept or recomputed), interleaved CCE-to-REG mappings, PT-RS
(QPSK MCS0 wide S=0 grants fail the same way), LBRM (S=1 wide grants with the same TBS/Lbrm decode). The DM-RS position
(dmrs 0x804, pos2 + add_pos 1) is the same for S=0/L=14 and S=1/L=13 and verified on the alignment below.

## Evidence

- [MEASURED] Offline window scan (`tests/passive_rx/wides0_offline.py scan`): on capture-b dumps, deleting one block of
  4-24 symbol-0 RBs (TBS kept) recovers 28/42 failing TDRA-8 S=0 decodes (`base2`) — the failures are a pure RE-count
  shift on symbol 0. QPSK MCS0 grants pass at exactly one deficit (e.g. 0x37f3 29.15: 288 REs = its AL4 PDCCH;
  512.15: 144 REs = AL2).
- [MEASURED] Re-encode oracle (`tests/passive_rx/wides0_oracle.c` + `wides0_align.py`): decode a passing hypothesis,
  re-encode the TB, align the codeword with the received symbols. Symbols 1..13 align at a constant offset (97-99 %
  bit agreement); the gap is on symbol 0 only, e.g. 0x37f3 202.14: exactly PRB 24-35 = DCI CCE 4 AL2; 478.5 PRB 48-59 =
  CCE 8 AL2; 869.4 / 1016.15 / 424.11: the PDCCH's PRBs clipped to the grant.
- [MEASURED] Paired runtime arm (ISAC_PDSCH_PDCCH_RM=2, after every other arm, same samples): every grant studied with
  the oracle (0x37f3 9.2, 993.2, 115.13, 76.13, 1009.11, 513.2, 8.5) is recovered by the exact own-PDCCH exclusion.
  (Some oracle maps also showed other 4/8-RB "unaligned" blocks; the exact exclusion recovers those grants, so those
  maps were confounded by deep fades in the per-RB agreement test, not a second mechanism.)

## Fix (default off)

- `nr_pdsch_passive_grant_t` gains `pdcch_sym0/pdcch_nsym/pdcch_crb[35]`; the blind monitor's DCI 1_1 (solver) path
  fills them from the DCI's CCE/AL through `nr_pdcch_candidate_rbs()` with the CORESET's own mapping (the demapper's),
  CRB = BWPStart + CORESET start + rb_offset + REG-bundle RB.
- `nr_ssb_rate_match.{h,c}`: the mask gets an independent second layer (`symbols2 x prb2`), filled from the
  thread-local `nr_ssb_rm_extra` independently of the SSB event; G (`nr_ssb_rm_plan`), the first-data-symbol rule and
  the demodulator honour it; SSB-only behaviour unchanged.
- `nr_dlsch_demodulation.c:455`: the fast path also checks `symbols2`.
- `nr_pdsch_passive_decode.c`: `ISAC_PDSCH_PDCCH_RM=1` wraps every decode attempt (primary and all retry arms, e.g. the
  TDRA S=0 alias) with the exclusion when the PDSCH symbols overlap the PDCCH's CORESET symbols on one of its PRBs; `=2`
  is the paired diagnostic/recovery arm (logs `PDCCHRM`). Diagnostic `ISAC_PDSCH_PDCCH_OCC_DIAG=1`
  (+`ISAC_PDSCH_PDCCH_OCC_IDS`) logs per-RB PDCCH-DMRS coherence and symbol-0/1 power (`DECODEPDCCHOCC`).
  GrantWork fingerprints include the exclusion. Flag unset: identical decode path (the grant fields are data only).
- Unit test `SsbRateMatch.PdcchLayerIsIndependentOfTheSsbEvent` (and the header-only test now defines the two
  thread-local switches it links against).

## Before / after [MEASURED, replay, TDRA field 8 (true S=0), margin >= 4 dB over Shannon(Qm R)+2 dB; `tools/wides0_cohort.py`]

Capture b, RNTI 0x37f3 (the layout-verified RNTI; TDRA position 16-19 confirmed by DECODEFIX):

| cohort | mode 0 (3 runs, flag off) | mode 2 paired, base -> final (3 runs) | mode 1 (3 runs) |
|---|---|---|---|
| wide >= 96 PRB from RB 0 | 0/12 (0 %) | 0/21 -> **18/21 (85.7 %)** | 4/5 (80 %) |
| mid 33-95 PRB | 11/17 (64.7 %) | 23/37 -> 35/37 (94.6 %) | 9/10 (90 %) |
| narrow <= 32 PRB | 156/176 (88.6 %) | 184/210 -> 206/210 (98.1 %) | 128/129 (99.2 %) |

Capture b, all RNTIs (TDRA read at bits 16-19 for every RNTI — not verified for other layouts; QPSK low-SNR grants dominate):

| cohort | mode 0 | mode 2 base -> final | mode 1 |
|---|---|---|---|
| wide, Qm >= 4 | 0/23 | 0/27 -> 18/27 (66.7 %) | 4/8 (50 %) |
| wide, all Qm | 0/68 | 0/83 -> 51/83 (61.4 %) | 22/36 (61.1 %) |
| narrow, all Qm | 833/1469 (56.7 %) | 917/1652 -> 1029/1652 (62.3 %) | 704/1116 (63.1 %) |

Capture 8, all RNTIs (no 0x37f3 traffic): wide 0/10 (mode 0) -> 1/10 paired (mode 2), 3/15 (mode 1); narrow 617/826
(74.7 %) -> 838/987 (84.9 %) paired, 769/905 (85.0 %) mode 1. The remaining wide capture-8 failures are 256QAM (Qm 8,
MCS 20-22, margin 4-6 dB) where the exclusion is applied (`PDCCHRM tried=1 ok=0`): a Qm=8-specific limit, not geometry.

**Gate** (wide RB-0 S=0, margin >= 4 dB, >= 85 % on replay, no narrow regression): **met for the layout-verified RNTI on
capture b** (18/21 paired, narrow 88.6 -> 98-99 %); **not met** for the all-RNTI population (61-67 %, unverified
per-RNTI layouts and low-SNR QPSK) nor on capture 8 (256QAM). No narrow regression anywhere. Runs are bimodal in
acquisition (509-5585 EVMGRANT per run); totals above are per-grant pooled within a capture and mode only.

Remaining wide failures (capture b): QPSK MCS 0-3 grants at 4-8 dB DD-SNR where the 2 dB gap overstates the margin, a
few grants whose FDRA parse differs between runs (e.g. 513.2 as 0+112 passes, as 0+128 fails), and 0x37f3 178.1
(own PDCCH 0-11, MCS 8, 16.8 dB) — unexplained.

## ctest [MEASURED, DGX]
`ninja tests` + `ctest -j4 -E ^nr_td_sim_test$`: new/affected tests pass (`test_nr_ssb_rate_match`,
`test_nr_ssb_rate_match_prod`). Failures: known ARM (dft_test, test_nr_modulation, test_nr_pusch_ra0_qam64/256),
time_management_tests, nr_polar_sc_cuda_test (not run), `test_nr_pdcch_blind_monitor` (UlUeIds / UlFirstPusch — the two
UL tests the parallel Sonnet task fixes), `test_nr_td_grantwork` GrantWork.AllocatorPoolsAndIsUsable (CUDA unified
memory check under heavy.sh; no code path touched here).

## Exact OTA env
`ota/t26j.env` (it already has ISAC_PDSCH_TDRA_RETRY=1, needed for the S=1/L=13 alias) **+ `ISAC_PDSCH_PDCCH_RM=1`**.
Leave `ISAC_PDSCH_OWN_PDCCH_RM` unset (superseded: it guesses the PRBs). Optional diagnostics: `ISAC_PDSCH_DECODE_DIAG=1
ISAC_PDSCH_EVM_RB=1 ISAC_DCI_RAW_LOG=1`. For a paired on-air A/B use `ISAC_PDSCH_PDCCH_RM=2` instead (PDCCHRM lines,
base vs recovered on the same samples). Expected on air: wide S=0 Qm >= 4 grants from 5-9 % towards 80 %+.

## Reproduction
- Replay: `ISAC_PDSCH_PDCCH_RM=1 tests/passive_rx/decres_replay.sh <name> ISAC_PDSCH_DECODE_DIAG=1 ISAC_PDSCH_EVM_RB=1 ISAC_DCI_RAW_LOG=1`
  (pass the flag as an argument; capture 8: prefix `CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008 CAP_TICK=210036277`).
  Cohort: `RNTIS=0x37f3 python3 tools/wides0_cohort.py replays/replay_<name>_*/run.log`.
- Offline: add `ISAC_PDSCH_LLR_DUMP=<f> ISAC_PDSCH_LLR_DUMP_FULL=1`; `wides0_offline.py scan|oracle`, build the oracle with
  `heavy.sh python3 tests/passive_rx/wides0_build_oracle.py`, then `wides0_align.py OUT BITS LOG`.
- Run dirs (ignored): `replays/replay_m{0,1,2}_b{1,2,3}_*`, `replay_m{0,1,2}_c8_{1,2}_*`, `replay_w3_b*`.

## DEFERRED / not done
- OTA A/B of ISAC_PDSCH_PDCCH_RM=1 (no radio by order).
- The non-solver DCI paths (format 1_0, config-sweep path) do not fill `pdcch_*` yet (no exclusion there).
- Data-aided reconstruction of a TB decoded with the exclusion uses the PDU without the symbol-0 hole (as the DECODE-RES arm).
