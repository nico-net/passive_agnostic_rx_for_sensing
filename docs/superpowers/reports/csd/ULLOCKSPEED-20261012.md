# ULLOCKSPEED-20261012: UL BWP lock speed (branch csd/ul-lock-speed, from csd/otatest f72ee5c0a2)

Host: DGX GB10 aarch64, CPU/GPU-lib build `cmake_targets/ran_build/build`, 4 RX, all builds/replays via heavy.sh, one at a time. No OTA, no capture, no UHD.
**Verdict: NEGATIVE. The new sequential test (`ISAC_UL_LOCK_SPRT=1`, default off) is statistically sound but did NOT lock faster than the legacy vote rule on any of the three replay captures. Do not enable it on OTA. Legacy path is unchanged when the flag is off.**

## 1. Why the Vodafone runs stall [MEASURED, DGX OTA 2026-10-12, ab_VA1/VB1/VY1/VB2/Trv run.log]
| run | lock | fed / booked probes | strong detections (gold) | votes (ctl votes) | tally at end |
|---|---|---|---|---|---|
| VA1 | none in 150 s | 20013 / 59187 | 126 | 121 (43) | 216@0:9 216@1:6 215@1:7 133@3:5 212@5:5 ... 63 hyps |
| VB1 | none | 8377 / 47279 | 64 | 31 (6) | 216@1:4 215@0:3 212@4:3 ... 192 hyps |
| VY1 | 216@1 at 78.8 s | 9470 / 26362 | 44 | 23 (3) | 216@1:6 217@0:2 215@1:2 |
| VB2 | 216@1 at 18.5 s | 440 / 7548 | 15 | 9 (0) | 216@1:6 217@0:2 |
| Trv (TIM) | 107@5 | 1508 / 3307 | 33 | 9 (3) | 107@5:6 |
Causes, in order of weight:
1. Only ~0.3-0.6 % of probes ever reach 6 dB (PUSCH at the noise floor); the rule discards the other 99.5 % of the evidence.
2. The votes go to near-twins, not to noise: 216@0 / 216@1 / 215@x differ by a 1-PRB (or L-1 PRB) shift of every grant, so a strong detection at the true range is also strong at the shifted range when the neighbouring PRB belongs to another UE (contiguous packing). The "leader >= 2x runner-up" rule then never fires (VA1 9 vs 6 vs 7). About one third of the votes in VA1 are noise (ctl votes 43 of 121).
3. 192 hypotheses (BWP_ALL) dilute the votes further; 27-40 % of the booked probes never reach the worker (drop-oldest queue shared with CRC-first trials, trials 3-24k per run), which lengthens the time further.
4. The lock needs 6 votes after the collision/twin filters; at 0.3 % strong-detection rate that is 2000+ fed probes in the best case.

## 2. What was built (all behind `ISAC_UL_LOCK_SPRT=1`, needs `ISAC_UL_LAYOUT_LOCK=1`; flag off = old code path)
- `nr_pusch_passive_decode.c` `lp_sprt_eval()`: per-PRB prefix sums of the DM-RS correlation (3 symbol rows x nant x 33 delays) for the true AND the wrong-slot (control) Gold sequence, so EVERY hypothesis range of ANY width (the 16-PRB limit is gone) is scored in O(33x12), for both sequences. Output `zn[h]` = (SNR_est(true) - SNR_est(control)) / sigma0, unbiased (unclamped) estimates. Legacy `snr_db`, set, ctl and comb1 fields still filled.
- `nr_pdcch_ul_layout_lock.c`: per-slot accumulation X_h = sum_probes clip(zn_h) (weight 1), S_h = sum X, W_h = sum X^2, mixture boundary, pairwise guard against the 8 best competitors from a 4096-slot history, SIB1 hypothesis gets half of alpha. Stats line `ULLAYOUT STATS ... sprt[...] overlap[...] wv..[...]`; `ISAC_UL_SPRT_DUMP=1` per-probe diagnostic.
- Booking/plan accept wide grants under the flag.
### False-lock bound (derived)
With no DM-RS at hypothesis h, swapping the true and the control sequence is a symmetry of the noise (the delay grid is symmetric and the statistic is invariant to conjugation), so zn_h flips sign; all X_h of one slot flip together. For conditionally symmetric X: E exp(lX - l^2 X^2/2) = E cosh(lX) exp(-l^2X^2/2) <= 1, so M = sqrt(rho/(W+rho)) exp(S^2/(2(W+rho))) is a nonnegative supermartingale and Ville gives
P(exists t: S_t^2 >= (W_t+rho)(2 ln(1/a) + ln((W_t+rho)/rho))) <= a for any stopping rule, with W the empirical sum of squares (no variance model, no Gaussian assumption). a = alpha/H (Bonferroni, alpha = 1e-3, rho = 30). The pairwise guard (level 1e-2) is a further condition. Caveat: hypotheses that overlap the PRBs of another UE's real DM-RS are not signal-free; the bound does not cover that structural confusion (it is what the guard and the replays check). [DERIVED; Monte Carlo of 300-400 sessions x 1500 slots x 192 hyps with heavy-tailed symmetric noise: crossing rate 0 / 0 / 2.75 % at alpha = 1e-3 / 0.05 / 0.3, bound holds]
## 3. Results
Unit `test_nr_pdcch_blind_monitor --gtest_filter=UlLayoutLock*` 7/7 [UNIT-TESTED, DGX]: new `SprtLocksOnWeakEvidenceAndNeverOnNoise`: weak synthetic evidence (per-PRB mean 0.35 sigma, wide grants included) locks 216@1 in < 3000 probes for 6 seeds; 150 noise-only sessions of 1500 probes with heavy-tailed noise: 0 locks.
Replays [OFFLINE REPLAY, DGX, t26z.env via timul_replay.sh, speed 0.25, CORESET fixture scanfast_coresets.txt for the two PCI 415 captures, one run each, captures not merged]:
| capture | legacy (flag off) | SPRT on |
|---|---|---|
| raw_pci415_12s_b | BWP_LOCK 216@1 at 7.96 s (6 votes vs 1) | no lock; 216@1 ranked first (S 835, W 24251, z 5.36) but 216@0 (z 5.18), 214@0 (6.15) within noise; pairwise z vs 216@0 about 1.7 |
| raw_pci415_12s_20261008 | no lock | no lock (top 180@27 z 4.3) |
| raw_pci923_tim_12s_tile3 | no lock | no lock (top 217@0 z 5.67, 107@2 z 3.3) |
False locks: 0 in all arms. Wrong-hypothesis evidence is as large as the true one: per-probe zn at 216@1 / 216@0 / 213@2 averages 0.25 / 0.20 / 0.14; wins of 216@1 over 213@2 at |dz| >= 3: 298 vs 242. In a busy cell almost any range lands on another UE's DM-RS, and with the same PCI scrambling the sequence cannot tell UEs apart. Argmax winner votes (+/-, exchangeable under the null) at zn >= 10: 216@1 +7-0 against 3 for the runner-up, i.e. not significant against 192 hypotheses either; a rigorous test at alpha 1e-3 needs ~30 net votes, legacy's 6 vs 1 is not significant in that sense.
Dead end measured: simultaneous grants of different RNTIs should not overlap under the true BWP, but 15-16 % of such pairs overlap under every hypothesis including 216@1 (801/5297 vs 851/5308), so disjointness does not discriminate.
## 4. What would help (not done)
- Coherent combining across the PRBs of a range (gain sqrt(L) in deflection, and a UE-ownership test: a range straddling two UEs loses phase coherence). Needs a finer delay grid per range.
- Long-baseline OTA data with `ISAC_UL_SPRT_DUMP=1` (per-probe zn of fixed hypotheses) to fit the weighting; 12 s captures carry 2-5 k probes, about 12x less than a 150 s run, and the soft statistic scales as sqrt(n): pairwise z vs 216@0 about 1.7 x sqrt(12) = 6 would lock around 60-100 s, no better than VY1.
- Drop CRC-first trials while unlocked at a site with CRC ~ 0 so more probes reach the worker (27 % fed in OTA).
## 5. OTA env delta
None. Keep t26z as is. `ISAC_UL_LOCK_SPRT` must stay unset; it adds only diagnostics (`sprt[...]`, `wv..[...]`, `overlap[...]` in the STATS line) and is slower to lock than legacy.
## 6. Files
`openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.{c,h}`, `nr_pusch_passive_decode.c`, `tests/nr_pdcch_blind_monitor_test.cc`. Replay logs (untracked) `replays/replay_{b,c,t}_*`.
