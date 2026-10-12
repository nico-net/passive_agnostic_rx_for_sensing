# BLINDAUDIT-20261012: audit of cell-specific constants in the passive receive chain

Branch `csd/blind-audit` (from `csd/otatest` 2117fb03ae). Host: DGX Spark aarch64, CPU build, 4 RX.
Evidence labels follow PROJECT_MEMORY section 0.1. Everything below is `[CODE-READ]` unless stated; the two fixes
carry `[UNIT-TESTED]` / `[OFFLINE REPLAY]` labels in section 5.

Goal (operator): the receiver must be fully blind and cell-agnostic. No parameter may be tuned on Vodafone PCI 415,
TIM PCI 923 or the older PCI 421/249 cells.

## 1. Scope and method

Read: `openair1/PHY/NR_UE_TRANSPORT/*` (passive, blind, solver, dmrs, csirs, pdcch, sss/pss, initial sync, UL layout
lock, GPU decision logic), `executables/nr-ue.c` watchdog, `openair2/LAYER2/NR_MAC_UE` harvesting, the OTA config
`sd_t26ul.cfg`, `tests/passive_rx/dgx/*.cfg`, and the OTA env `ota/t26w.env`. Method: grep of every `#define` with a
numeric value in the passive modules, every `getenv("ISAC_*")` (about 500 distinct names), every `if (... > N)` with a
literal, every comment containing "this cell / this deployment / measured on / PCI 4xx", plus `git log -S` for the
origin of each value. **Coverage statement**: all `#define` thresholds of the pdcch/csirs/solver/dmrs/ul modules and all
config fields were classified. The roughly 500 env names were scanned by name and the decision-bearing ones (about 60)
read in code; pure perf/diagnostic switches were not read one by one. PROJECT_MEMORY.md (3566 lines) was read by
section (1.3, 10.2, 10.5, headings), not line by line.

Classes: A spec-derived; B statistical (false-alarm or binomial) and scale-invariant; C absolute level in ADC/energy
units; D reflects one cell's configuration; E resource/perf knob.

## 2. What is already good (do not touch)

- Energy-like decisions are mostly relative: PSS `peak < 5*avg` (pss_nr.c:272), occupancy gates (floor + dB), CORESET
  map correlation 4x the pure-noise floor (nr_pdcch_coreset_map.c:54, derived), DCI-length sweep significance
  `Z_SIGMA 6` with Bonferroni over 34 lengths (nr_pdcch_dci_length_sweep.c:81-89), PDSCH solver `SIGNIF_ALPHA 1e-3`
  (nr_pdsch_solver.c:379), joint solve `JOINT_PFA 1e-4` (nr_pdcch_joint_solve.c:7), UL disc `ALPHA 1e-6`
  (nr_pusch_ul_disc.c:15), CSI-RS discovery gate K=12 sigma / M=4 sigma over a per-sweep noise model
  (nr_csirs_disc_gate_internal.h:28-31), GPU-vs-CPU parity checks (corr >= 0.9999).
- The absolute `energy_min` gate defaults off and says why (nr_pdcch_blind_monitor.c:3243).
- DL BWP discovery (`nr_passive_bwp.c`) enumerates every RIV size; it does not assume a size.
- Numerology is almost never hard-coded: only two slot-count windows (section 5) and the test-only PRACH branch.
- 273 PRB constants are array bounds (spec maximum), not cell fits: class A.

## 3. Ranked findings (highest risk first)

Risk = what cell would break. "Fix" = proposed general replacement. IDs F1..F25.

| # | file:line | value | class | risk (cell that breaks) | proposed general replacement |
|---|---|---|---|---|---|
| F1 | ota/t26w.env `ISAC_CSIRS_FULL_BLOCK_INFER=1`; nr_csirs_blind_rt.c:1778-1786, CSIFULL.md:240 | "every confirmed row-1 TRS of a 52-RB block occurs with the first one seen" | D (PCI 415 network property, documented as such; origin f1099a2573 2026-10-10) | any network whose TRS resources of a block are not co-scheduled (TIM, other vendors): wrong ZP/rate-matching REs -> PDSCH LLR damage, silent | keep off by default in env; replace by an observed-co-occurrence test (infer only for resource pairs already seen together in >= 2 bursts). Backlog. |
| F2 | cfg `pdcch_blind_monitor_pdsch = "1:1:0:1:..."` 4th field `rv0_only=1`; nr_pdcch_blind_monitor.c:3285, nr_pdcch_blind_monitor_rt.c:9539/9665/9844 | drops every grant with rv != 0 | D (assumes a retransmission is never the first thing seen) | SIB1 (SI-RNTI) repeats on an RV cycle 0,2,3,1: a weak cell where the receiver sees rv != 0 first never decodes SIB1 (observed WEAKSYNC 2026-10-12 PCI 508, per wsy agent's code) | exempt SI-RNTI / RA-RNTI (self-decodable); wsy agent owns this (`ISAC_SI_RV_ANY`), report only. |
| F3 | cfg `noise_gates = "0:2:500:0:3.0"` (all dgx cfgs); nr_pdcch_blind_monitor_rt.c:7062-7075, nr_pdcch_blind_phase2.c:98-146 | adaptive energy gate: reject candidates below 3.0 x running median of candidate energy | B in form (dimensionless), D in effect: the file itself records a cell where the median converges to signal level and the gate rejected 100 % of candidates (nr_pdcch_blind_monitor.c:2688-2699, "held[energy=17296703] accepts=0") | any dense or small dedicated CORESET (heavily loaded cell, few empty CCEs) -> receiver deaf | drop the 5th field from the OTA cfgs (gate off) or make the floor a low quantile of unoccupied CCEs with a z-threshold from a chi-square false-alarm bound. Backlog; config-only workaround: `0:2:500:0`. |
| F4 | sss_nr.h:44 `SSS_METRIC_FLOOR_NR 30000`; sss_nr.c:221-292 | absolute metric floor in ADC counts squared | C | every cell received below roughly Vodafone/TIM level (PCI 4/403/675 at 11-13k, 2026-10-11) -> never found | already mitigated by `ISAC_SSS_REL_RATIO` (2.5 in t26w.env), but **default is off** (sss_nr.c:225) so any env without it regresses; PBCH CRC24 verifies each accept. Proposal: make 2.5 the default after the survey replays confirm no false locks (backlog, needs the initial-sync owner `rr-csd-wsy`). |
| F5 | nr_pdcch_ul_layout_lock.c:94-95 `best_pairs>=64, best>=1.2, best-sec>=0.3`; :14-21 `VOTE_DB 6, LOCK_MIN 6, LOCK_RATIO 2, LOCK_RNTIS 2, FIT_MIN 256` | MCS-offset score thresholds quoted "true offset scores ~1.8 on PCI 415, a one-bit shift ~1.3" | D (fitted on PCI 415 UL traffic; origin afa9caa56b 2026-10-10) | a cell with a different UL traffic mix (few HARQ steps, other RV pattern, other beta) scores below 1.2 -> UL layout never locks; or a different mix scores a wrong offset above it | calibrate against a permutation null (score of shifted offsets) and require a z-threshold; scores should be computed relative to the bank's own null. Backlog. |
| F6 | nr_pdcch_ul_layout_lock.c:365-368, :415 `std_n[]`, sizes only car-0..5, dlbwp-0..5, SIB1 size; starts `s <= 5` | UL BWP size and start hypotheses | D (the DL side enumerates all sizes; UL does not) | UL BWP of any other size or a start offset > 5 RB (e.g. 100 PRB at RB 20 of a 106 carrier) -> UL DCI 0_1 never layout-locked. The 64-hypothesis cap is already near full, so widening needs a smarter prior | enumerate N per fitted RIV width as nr_passive_bwp.c does, rank by CRC evidence. Backlog. |
| F7 | cfg `pdsch` 2nd field `mcs_table=1` (qam256); nr_pdcch_blind_monitor_rt.c:8754, 9140; rt.h:142 | table used for DCI 1_1 TBS and for TBS_LBRM | D | cell configured with qam64 (table 0): TBS wrong for MCS >= 20, N_ref wrong -> CRC never passes. `ISAC_QM_ORACLE` (default on) and `rnti_nl_latch` partly rescue | pass 0 (= unknown, Technique D sweeps tables); verify on replays. Config-only change, not made here (config files are shared). |
| F8 | nr_hyp_sweep.h:35-37 and nr_pdsch_config_sweep.c:43-51 `MIN_TRIALS 300, WIN_RATIO 3.0, MIN_RATE 0.02` | winner needs >= 2 % CRC rate and 3x the runner-up | B/D (comment: "set for ~76 % working rate") | marginal link with true CRC rate < 2 % on every hypothesis -> engine never declares, stays undecided (fail-safe but blind) | replace the 0.02 floor by a binomial test against the false-pass probability (2^-24 per CRC-24 TB): >= 3 passes in the window is conclusive whatever the rate. Backlog (touches the engine owned by gsq/tim-solve2). |
| F9 | nr_pdsch_dmrs_scan.c:255-290 (contrast), `NR_DMRS_SCAN_MIN_VOTES 2` | id vote handling after TIM measurement | B (contrast) | low; written for TIM, is population-relative | none. |
| F10 | nr_pdcch_blind_monitor.h:290-303, 834-849 and nr_pdcch_blind_monitor.c:3689 | default DCI 1_1 / 0_1 field widths (harq pid 4, dai 2, pdsch_to_harq 3 "this cell's list has 8", antenna ports 4, srs 2, tci 0, ...) | D (spec defaults of one RRC configuration) | cell with other RRC choices: payload fits no layout -> must come from the layout sweeps (rr-csd-drv) | owned by drv (DCI layout agent); report only. |
| F11 | nr_pdcch_dci_length_sweep.c:32,85,137-163 lengths 30..63 cold, wide probes beyond | DCI length range | D-mitigated | CBG/2-codeword/dynamic bundling cells above 63 bits are found only by low-duty probes -> slow | none now; `ISAC_DCI_LEN_MAX` exists. |
| F12 | nr_pdcch_blind_rnti_bootstrap.c:47 `STALE_SLOTS 20000`; nr_pusch_solver.c:12 `REFILL_SLOTS 40` | slot-count windows assuming 30 kHz (2000 slots/s) | D (numerology) | 15 kHz cells: windows 2x too long; 60/120 kHz: 2x/4x too short -> RNTI evidence expires early, UL probe rate too high | **fixed, section 5** (`ISAC_SLOT_SCALE=1`). Also AUTODISCOVER_OBS_CALLS (nr_pdcch_blind_monitor.c:677, "~4-5 s on this cell's occasion rate") and the census window in nr-ue.c:2063 (2000 slots = 1 s at mu=1) are of the same kind, left. |
| F13 | cfg `ul_uci = "1024:11:0:32"` -> nr_pdcch_blind_monitor.c:2954-2961 | beta index 11, alpha 0 ("what this deployment logs") | D but inert: `ul_uci_beta/alpha` have no consumer outside the parser (grep) | none today | delete the fields or make the UCI footprint learned (nr_passive_uci_learn.h already learns it). |
| F14 | nr_pdsch_passive_decode.c:5181-5213 `static_csi_resources` | PCI-415 non-TRS CSI-RS map (abs_slot%160 in {24,63,105,135}, symbols, REs, all RBs) | D (hard-coded cell map) | default off (`ISAC_PDSCH_STATIC_CSI_RETRY`, commit message "off, no measured need"); would corrupt any other cell if enabled | delete when the CSI-RS discovery covers non-TRS (CSIRSPORTS report) or keep with a comment that it is an oracle arm. |
| F15 | nr_pdcch_blind_monitor_rt.c:290-327 `ISAC_PDCCH_C0_OCC_DB` 6 dB (env `ISAC_PDCCH_C0_OCC_GATE=1`) | CORESET#0 occupancy: max 6-RB group energy > 4x the EMA of the min group | B in form, mis-calibrated: the floor is the minimum over groups x symbols, biased low by roughly the number of groups, so the gate is mostly permissive | safe direction (extra decodes), but cost/benefit differs per CORESET#0 width | use the median group energy and a chi-square threshold. Backlog. |
| F16 | nr_csirs_blind_search.c:260-261 `CSIRS_DETECT_MARGIN 3.0, MIN_HITS 3`; nr_csirs_blind_search.h:348 `ZP_MIN_SCORE 0.5` | hit must beat the null median by 3x, at least 3 hits | B/D | SNR-limited on weak cells; ratio-to-median has no stated false-alarm rate | convert to a z-score against the null with a Bonferroni bound over candidates (as the DISC gate already does). Backlog. |
| F17 | nr_pdsch_passive_decode.c:2477-2495 `n_L` for TBS_LBRM | comment says "4 is this deployment's value" | D-mitigated | handled by `rnti_nl_get` latch decided by the TB CRC | none. |
| F18 | cfg/env `ISAC_PDSCH_SNR_GATE=1`, `_DB` 3 dB; nr_td_gate.c:5-14, nr_pdcch_blind_monitor_rt.c:1311 | skip full decode if SNR+3 dB < Shannon requirement (se/0.75) with 1-in-8 exploration | A/B (AWGN 1-layer theory) | rank-4 cells: per-layer SINR is lower than the per-RNTI SNR used, so decodable grants are skipped 7 of 8 times | use per-layer post-MMSE SINR; backlog. |
| F19 | executables/nr-ue.c:2056-2284 RF-stall watchdog `bad_needed 8`, window 2000 slots | PBCH dead for 8 x "2000 slots" | D (comments: "50 SSB/s at this cell's 20 ms period") | cell with 160 ms SSB: still >= 6 SSB per window, ok; mu=0: window 2 s, ok. Low | scale window with numerology (same family as F12). |
| F20 | nr_pdsch_passive_decode.c:1299 branch-FO clamp +-1500 Hz | CFO range (path disabled; `ISAC_RX_BRANCH_FO` must stay unset) | D | n/a (off) | none. |
| F21 | initial sync: `ISAC_AUTO_ACQUIRE` requires explicit `ISAC_ACQ_CFO_MAX_HZ` (nr_initial_sync.c:589); `--initial-fo` per cell in the replay recipe (FO 6900, -4800) | CFO search domain / seed | A (allowed receiver knowledge: search domain) but the manual `--initial-fo` per cell is not blind | cell needing a seed outside the narrow window | initial-sync owner (wsy/afe); report only. |
| F22 | `--ssb 1171/233`, `-C`, `-r 217`, `--numerology 1`, `--band 78` in every replay and OTA | carrier width 217 PRB, SCS 30 kHz, band n78, GSCN | A (allowed: search domain), but `-r`/`--numerology` are not discovered | 100 MHz/273 PRB, 15 kHz or other-band cells need different launch args; auto_acquire.py exists but is NOT VALIDATED | afe agent (auto front end); report only. |
| F23 | nr_pdcch_coreset_map.c:60-61, nr_pdcch_blind_monitor.c:2149-2150 `AUTODISCOVER_MIN_HITS 3, HITS_PER_WINDOW 30`, `DOMINANCE K 4 N 3` | footprint discovery | B (derived from pure-noise moments, comments cite the derivation) | low | none. |
| F24 | nr_pusch_passive_decode.c:158 `UL_DELAY_TOL 6`, `TA_SWEEP_MAX 33`; nr_pusch_ul_disc.c:91 `score < 0.35, +-2048` | UL timing search ranges (samples) | D (sample units at 122.88 MS/s, i.e. one carrier width) | other sample rate/numerology change the meaning | express in CP fractions. Backlog. |
| F25 | E-class loads: ISAC_PDCCH_DED_BUDGET_US 300, SOLVER_BUDGET 24, PROBE_RATE/FILL/BURST, UL_SOLVER CPU cap, GPU budget 100 MB/s (nr_dl_gpu_budget.h:13), core maps | resource knobs | E | load-dependent: on a busier cell budgets starve discovery (documented) | none; note only. |

Top-10 by risk: F1, F2, F3, F4, F5, F6, F7, F8, F18, F12 (F12 is fixed).

## 4. Config and env notes

- `sd_t26ul.cfg`: `pdcch_blind_monitor_ul_uci = "1024:11:0:32"` search budget 1024, beta 11, alpha 0, explore 1/32: beta and
  alpha inert (F13), the rest E. `rnti_range 1:65519` is TS 38.321 Table 7.1-1 C-RNTI range (A). `scan_thread 2:32:6`
  and `pdsch ...:16:5:64:7` (consumers, depth, first core) are E. `noise_gates` F3, `pdsch` fields 2 and 4 F7 and F2.
- `t26w.env`: search-domain and cores (`ISAC_SCAN_ANT`, cores, `ISAC_USRP_*`) are E. Cell-flavoured switches:
  `ISAC_CSIRS_FULL_BLOCK_INFER` (F1), `ISAC_PDSCH_SNR_GATE` (F18), `ISAC_PDSCH_DMRS_ID_SCAN=16`, `ISAC_PDSCH_DMRS_ID_REFRESH=64`,
  `ISAC_UL_LOCK_CRC_K=3` (statistical: three CRC passes), `ISAC_SSS_REL_RATIO=2.5` (the C fix of 2026-10-11, keep).
  `ISAC_RX_BRANCH_FO` is unset, as required.

## 5. Fixes made in this branch

Only the two items that are simple, local and testable were changed. Both are behind `ISAC_SLOT_SCALE=1`, default off, so with
the flag unset behaviour is byte-identical.

1. F12 `nr_pdcch_blind_rnti_bootstrap.c` (+ `nr_pdcch_blind_monitor.h`, call in `nr_pdcch_blind_monitor_process`): the
   staleness window becomes `10000 << mu` slots (about 10 s at any SCS, mu=1 gives the existing 20000).
   Test: `RntiBootstrap.SlotScaleKeepsTenSecondsAtAnyNumerology` (flag unset ignores mu; flag set at mu=3 keeps an entry alive at
   20001 slots and expires it after 80000; mu=1 reproduces the constant).
2. F12 `nr_pusch_solver.c` (+ `.h`, call in `nr_pusch_passive_queue.c`): UL probe refill period `20 << mu` slots (20 ms
   at any SCS; mu=1 gives 40). No dedicated unit test (the token bucket is exercised by the existing solver tests with the flag off);
   `[CODE-READ]` only for the flag-on path.

Not fixed on purpose: F1-F8 need a replay corpus per cell or touch areas owned by other agents (drv, gsq, wsy, afe).

## 6. Verification

- Build (DGX aarch64, via heavy.sh, `nr-uesoftmodem tests`): OK `[UNIT-TESTED]`.
- ctest `bootstrap|solver|pdcch_blind` (4 tests incl. new `SlotScaleKeepsTenSecondsAtAnyNumerology`): 4/4 passed `[UNIT-TESTED]`.
- Offline replay raw_pci415_12s_b, ENVFILE t26w.env + `ISAC_SLOT_SCALE=1`: PCI 415 locked, PDSCHQ 174 decoded, crc_ok 147 (84.5 %),
  no drops `[OFFLINE REPLAY]` (cell PCI 415 only; single run). At mu=1 the flag is the identity (20000 / 40 slots), so this shows no
  regression but not a gain; with the flag unset the code path is byte-identical. The PCI 923 replay was NOT run (no behaviour
  difference expected at mu=1; time budget).

## 7. Backlog (prioritized)

1. F3 gate-off in OTA cfgs, or quantile-based floor.
2. F7 mcs_table 0 (unknown) in OTA cfgs, replay A/B on PCI 415, 923, 421.
3. F5/F6 UL layout lock: permutation-null calibration and RIV-width-driven size enumeration.
4. F1 co-occurrence-based block inference.
5. F8 binomial-bound replacement of the 2 % CRC-rate floor.
6. F4 default `ISAC_SSS_REL_RATIO` after a survey false-lock check (ota_runs/survey_1s_1011).
7. F18 per-layer SINR in the PDSCH gate.
8. F16, F15, F24 conversions to statistical thresholds.
9. Delete or document-as-oracle F13, F14.

## 8. Backlog status (BLINDFIX-20261012, branch csd/blind-fix)

F1, F3, F5, F6, F7, F8: implemented behind default-off flags with unit tests and offline replay (see BLINDFIX-20261012.md); not yet OTA-validated.
Remaining unchanged: F2 (wsy), F4, F10 (drv), F15, F16, F18, F22 (afe), F24, F13/F14.
