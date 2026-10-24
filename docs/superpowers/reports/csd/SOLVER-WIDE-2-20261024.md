# SOLVER-WIDE-2: the CRC gap, the solver-based gate proposal, the 415 replay

Branch csd/solver-wide (base e9c874c308 + 08d2906482). Host DGX aarch64 (GB10), build_t26, 4 RX. rfsim CRC is an upper bound; no result is merged across beds or hosts.

## 1. CRC 84.7 % vs the 93.4 floor: cause and fix
[MEASURED, DGX rfsim 106 PRB, 4 RX, 420 s] In the fixed build 86.6 % of the solved RNTI's full decodes pass and 13.4 % fail. All failures are in slots 7 and 17 of every 20 (EVMGRANT `skip=1`, nsym=0, DM-RS SNR -20 dB): the TDD special slots. Every other slot decodes at 100 % (24399 of 24399). So it is not a gating, rate-match, scrambling or TB-size error on the common path.
Cause [READ + MEASURED]: the layout the solver settles on has no TDA field (tda_bits = 0), so the solved (S, L) learnt on full slots was applied to special-slot grants, whose PDSCH is shorter and starts elsewhere. The TDA-field learner (Fix A) never fired either (`rows=0 det=0`) because it learns only from CRC passes, and none could occur. Before 0fb22ecbd0 Technique D learnt a context per (RNTI, TDA) with its own sweep.
Fix (commit a6b95559de, one rule, no flag): a solved RNTI whose full decodes at one slot phase (absolute slot mod 20) fail 8 times without a pass, while it has >= 20 passes elsewhere, searches (S, end symbol) at that phase with first-code-block probes (4 per grant, rotating, solved add_pos and table). A probe pass stores (S, L) for the phase; later grants of that phase decode in full with it. Probe passes and phase decodes never enter the S-map evidence. Commit 0e57216695 (L learnt per TDA index) is the same idea keyed by TDA; it is inert on this bed (TDA index is always 0) and kept for layouts that do carry the field.
Unit tests [MEASURED, DGX unit test]: test_nr_pdsch_solver 31 pass; PdschSolverPhase: special slots 372/400 full decodes ok after learning, full slots 3598/3598 unchanged.

| arm (one 420 s 106-PRB run each unless noted) | solved at | post-solve CRC | crc_floor (overall) | notes |
|---|---|---|---|---|
| main e9c874c308 | never | n/a | n/a (0 decodes) | wide=27999 |
| + wide fix 08d2906482 | 1.35 s | 86.59 | 84.69 | 13 % in slots 7/17 fail |
| + wide fix, repeat with L per TDA | 1.09 s | 86.59 | 85.0 | inert (TDA always 0) |
| + phase fix a6b95559de, run 1 | 1.20 s | 99.93 | 97.84 | learnt phases 7 and 17: S=1 L=5 |
| + phase fix, run 2 | 1.08 s | 99.93 | 98.06 | same |

Arm B (commit before 0fb22ecbd0, Technique D) was NOT run: it needs a configured build dir. Worktree ready at /home/nicola/NICOLA/wt/rr-solverw-pre (0e9044042b, detached); please configure a build dir there (e.g. cmake_targets/ran_build/build_t26) if you still want the comparison. The cause above does not depend on it.
One invalid run: a phase-fix run killed at 265 s when the OTA started (exit 143) showed 16.5 % with a different representative layout (192, 33 consistent layouts, cdm=2 read) instead of 211; that is a different effect (representative choice among CRC-equivalent layouts) seen once, not reproduced in the two valid runs, and not investigated. Treat as an open risk.

## 2. Proposed solver-based gate (commit 9421050e91, NOT the gate; score_rx.py and rfsim_regress.sh untouched)
tests/passive_rx/dgx/score_rx_solver.py replaces n_contexts / ttc_tda* / reopens / postconv window (Technique D lines, absent since 0fb22ecbd0) with: n_solved >= 1, solve_s <= 10 (PROVISIONAL; measured 1.1-1.4 s), post_solve_crc >= 99.8 over post_solve_tr >= 5000 full decodes (from the solver's per-rnti counters), and keeps crc_floor >= 93.4 and drop_full <= 2.5 unchanged.
Scoring: main e9c874c308 FAIL (n_solved=0); wide fix only FAIL (post_solve_crc 86.59, crc_floor 84.69); phase fix run 1 PASS (solved 1.204 s, 99.93, 27732 fulls, floor 97.84, drop 0.0028 %); run 2 PASS (1.08 s, 99.93, 27767, 98.06, 0.043 %). Provisional thresholds, n = 2 runs: recalibrate with >= 5.

## 3. raw_pci415_12s_b replay, main vs fix (4 arms interleaved: base r1, fix r1, base r2, fix r2)
[MEASURED, DGX replay, 4 RX, FROZEN_20261018.env, sd_t26ul.cfg, SPEED 0.25, ISAC_PDCCH_NID_GPU_CORE=11 under td_measure.lock; "fix" = a6b95559de, "base" = e9c874c308 binary]. Per-rnti counters from the last `PDSCH_SOLVER per-rnti` line.
| run | RNTIs seen | solved | full ok/tr | PDSCHQ crc_ok/decoded |
|---|---|---|---|---|
| base r1 | 40 | 34 | 2568/5235 | 2483/9280 |
| fix r1 | 38 | 22 | 1472/2377 | 1573/10017 |
| base r2 | 40 | 33 | 2523/5143 | 2581/9134 |
| fix r2 | 40 | 33 | 2534/5152 | 2479/10034 |
Rep 2 is at parity (33 vs 33 solved, 2534/5152 vs 2523/5143 full; 27 RNTIs solved in both with similar solve times, differences within the base-vs-base spread). Rep 1 fix is an outlier (22 solved, half the full decodes): the RNTI sets and solve times vary a lot between runs of the same binary (compare base r1 vs base r2: 14 RNTIs solved in one and not the other across the pairs), so 2 reps cannot separate a regression from replay noise. I did NOT establish no-regression: fix r1 needs a rerun (suggest 3+ interleaved reps) before the merge. No wide-only RNTI is expected in this capture (narrow grants dominate), so the wide rule should be inert there; the phase rule can act on any RNTI that fails at one slot phase, and neither new rule fired in these replays (0 `slot phase ... learnt` lines and 0 `wide-only for` lines in fix r1 and r2 [MEASURED]), so the fix r1 outlier is replay noise of the unchanged narrow path, not a regression from this change, but a 3+ rep rerun would confirm it.

## 4. 273-PRB bed, probe cost: NOT done.
