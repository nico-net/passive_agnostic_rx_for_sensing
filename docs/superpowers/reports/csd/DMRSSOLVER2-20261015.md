# DMRSSOLVER2-20261015: blind DM-RS solver as primary PDCCH N_ID source (branch csd/dmrs2 = csd/gpupass 6d8d66b316 + csd/dmrssolver)

Everything is default off behind `ISAC_DMRS_NID_SOLVE=3` (flags off = unchanged). Evidence labels per PROJECT_MEMORY §0.1. Replay only, `[NOT MEASURED ON AIR]`.
Method `[MEASURED, DGX replay <cap>]`: `tests/passive_rx/p99_replay.sh`, ENVFILE `rr-orchestration/ota/t26f.env` (frozen), 4 RX, SPEED=1, heavy.lock, private MPS dir, arms interleaved (off, p3, offG, p3G per round).
Captures never pooled: b = Vodafone PCI 415 (`raw_pci415_12s_b`), tim = TIM PCI 923 (`raw_pci923_tim_12s_tile3`).
Scope note: the id handled is the PDCCH DM-RS N_ID found by the GPU sweep `nr_pdcch_nid_gpu.c` (that is what the existing solver and the sweep feed); the PDSCH data DM-RS id scan (`ISAC_PDSCH_DMRS_ID_*`) is a separate path and is untouched.

## 1. What was built
- `ISAC_DMRS_NID_SOLVE=3` (reuses the solver flag; 1 = observe, 2 = leader hint, 3 = primary):
  - (a) top-2 hint: the (at most) two ids with >= 8 votes (`nr_dmrs_nid_top2`), merged in front of the sweep's records (sweep record wins for the same id). A DCI-less id (Vodafone 9020) takes one slot, never both.
  - (b) GPU nID sweep on demand only (`nr_dmrs_nid_sweep_demand`, worker in nr_pdcch_nid_gpu.c): held `GRACE_MS` (4000) with no result (fallback), then a burst of `WINDOW` (10) batches when (i) no solver result, (ii) hinted/listed ids not CRC-confirmed after `MAINTAIN_MS` (2000), (iii) unexplained PDCCH occupancy wake, (iv) slow re-verify every `RECHECK_MS` (60000). Batches in between are dropped, nothing uploaded.
  - Confirmation = any one listed id confirmed by a CRC-valid DCI in the bank (a DCI-less id must not keep the sweep running); in primary mode only.
  - (c) the full sweep stays the fallback and is unchanged when it runs.
- Primary mode forces the duty path (feedback/wake) on and polls the discovered-CORESET source every 64 occasions instead of 1024 (the id set grows within a second; 1024 polls = ~10 s on replay).
- Env knobs: `ISAC_DMRS_NID_SOLVE_EVERY` (offer 1 symbol in N, default 32; arms below used 8), `_GRACE_MS`, `_MAINTAIN_MS`, `_RECHECK_MS`, `_CORE`, `_OPT`.
- Logs: `DMRS_NID_SOLVE hint served`, `sweep on demand`, `PDCCH_NID_GPU t0 / cumulative / stop summary`.
- Unit tests `[OFFLINE VERIFIED, DGX aarch64]` in test_nr_dmrs_nid_solve: `NidPrimary.Top2KeepsTheRealIdBesideADciLessLeader`, `NidPrimary.SweepRunsOnlyOnDemand` (grace, fallback, unverified cross-check, verified idle until re-verify, wake).

## 2. Results (final build, RECHECK 60 s). Cell values: median [min-max] over n runs
Columns: first correct N_ID = first `hint served`/sweep record naming a CRC-valid id (b: 62922 or 10163; tim: 1931), seconds after `PDCCH_NID_GPU t0` (GPU sweep start; replay sync dominates, ~5 s). Sweep passes = completed GPU sweep passes (= launches of the stage-1/2 pass; skipped batches are not launches). Sweep kernel ms = cumulative CUDA-event kernel time at the last cumulative log line (printed at passes 1, 9, 17, 25: a lower bound, within 7 passes of the true value). DL = whole-call decode (EVMGRANT dec_us, all grants). UL cpu = `UL_PUSCH_CRC cpu_us` (UL CRC-OK was ~0 in every arm on both captures, so UL means decode cost only). Solver CPU = `CLOCK_THREAD_CPUTIME_ID` of the dmrs_nid_solve thread over the 12 s capture. off = flags of t26f; p3 = `ISAC_DMRS_NID_SOLVE=3 _EVERY=8`; offG / p3G = same plus `ISAC_GPU_PASS=1`.

Vodafone (b), `[MEASURED, DGX replay b, n=4 per arm]`
| arm | first correct N_ID (s) | sweep passes | sweep kernel ms | DL CRC-OK % | DL p50 / p99 us | UL cpu p50 / p99 us | solver CPU s | ids outside off set |
|---|---|---|---|---|---|---|---|---|
| off | 10.59 [9.89-11.57] | 18.5 [17-20] | 4815 [4231-5111] | 47.4 [44.9-53.6] | 1111 / 4942 | 1577 / 26662 | - | 0 |
| p3 | 9.27 [7.09-11.63] | 5.5 [0-20] | 1338 [0-3791] | 53.4 [51.9-57.7] | 1104 / 5380 | 1542 / 25893 | 0.41 [0.24-0.53] | 1 (one anonymous DCI hit, id 48960) |
| offG | 9.43 [8.87-10.64] | 16 [16-22] | 2896 [2527-3955] | 47.4 [41.0-49.6] | 1215 / 5264 | 1844 / 27021 | - | 0 |
| p3G | 7.72 [7.12-10.14] | 5 [0-13] | 1217 [0-2722] | 58.0 [45.5-58.5] | 1163 / 5442 | 1264 / 22220 | 0.40 [0.27-0.52] | 0 |

TIM (tim), `[MEASURED, DGX replay tim, n=3 per arm]` (n=3: time budget, flagged)
| arm | first correct N_ID (s) | sweep passes | sweep kernel ms | DL CRC-OK % | DL p50 / p99 us | UL cpu p50 / p99 us | solver CPU s | ids outside off set |
|---|---|---|---|---|---|---|---|---|
| off | 5.86 [5.66-6.47] | 22 [21-24] | 4286 [3987-5694] | 64.7 [64.3-64.9] | 584 / 3204 | 2711 / 24595 | - | 0 |
| p3 | 4.04 [4.03-4.12] | 10 [0-10] | 1840 [0-1985] | 64.0 [63.5-66.6] | 578 / 2701 | 2650 / 25807 | 0.14 [0.14-0.15] | 0 |
| offG | 5.78 [5.76-5.88] | 24 [19-29] | 4916 [4502-5149] | 65.9 [63.3-100*] | 672 / 3014 | 2725 / 25306 | - | 0 |
| p3G | 4.29 [4.12-4.29] | 7 [0-10] | 296 [0-1862] | 66.5 [63.9-67.0] | 682 / 2780 | 2628 / 25080 | 0.14 [0.14-0.15] | 0 |
(* one offG run decoded only 33 grants; median is the robust figure.)

Reading `[MEASURED, DGX replay]`:
- First correct N_ID: tim 5.9 -> 4.0 s (-1.8 s, all 3 runs consistent), b 10.6 -> 9.3 s median but with a wide spread (7.1-11.6): the b gain is not resolved at n=4. It is bounded by receiver sync (~5-7 s after sweep start on replay); after the first solver symbol the id is named within ~0.2-1 s.
- GPU sweep: passes -70 % (b, median 18.5 -> 5.5) and -55 % (tim 22 -> 10); kernel time -72 % (b) and -57 % (tim). The saving is bimodal, not uniform: runs where the hinted ids were CRC-confirmed inside 2 s ran 0 passes; runs where the bank had not confirmed in time paid one 10-pass cross-check burst (tim: 10 passes), and 1 of 4 b runs ran as long as off (20 passes). In 12 s replays the 60 s re-verify never fires, so these numbers do not include the periodic cost (about one 10-pass burst per minute by construction, vs continuous when off).
- DL CRC-OK: b 47.4 -> 53.4 % (p3), tim 64.7 -> 64.0 % (p3): within run-to-run spread (b off range 44.9-53.6); no drop > 1 % outside the spread on tim (-0.7). CRC counts depend on how many grants a replay decodes (b: 1.2k-5k per run), so treat as no resolvable change.
- DL decode p50 unchanged (b 1111 -> 1104, tim 584 -> 578); p99 b 4942 -> 5380 (+9 %, inside the 4176-5526 / 4913-5877 spreads), tim 3204 -> 2701. UL cpu p50/p99 unchanged within spread (outliers 19.7 ms max-of-run in one b run). n=3-4 cannot exclude a few-percent p99 cost on b.
- Solver thread CPU: 0.14 s (tim) and 0.24-0.53 s (b) per 12 s capture at EVERY=8 = 1.2 % / 2-4 % of one core; at the default EVERY=32 about a quarter of that.
- Wrong ids: 0 on tim. b: 1 anonymous-DCI hit (`JOINT_ANON_HIT`, RNTI 0x9539, once) under id 48960 in one of 4 p3 runs; 48960 is also listed by the GPU sweep in the off arm, so it is not a solver-only id, but whether it is a real CORESET or a false alarm of the anonymous decode is not established. No PDSCH wrong-id decode can be told apart here: every counted CRC-OK is CRC-valid.
- Composes with ISAC_GPU_PASS=1 (Stage 1): p3G vs offG shows the same sweep/first-id effect; DL p50/p99 of p3G equal offG within spread (b 1163/5442 vs 1215/5264; tim 682/2780 vs 672/3014); no GPUPASS rejection log in any arm.

## 3. Earlier builds (same method, n=4 per arm, kept as intermediate evidence)
- RECHECK 10 s, hint polled every 1024 occasions (`[MEASURED, DGX replay b|tim]`): b sweep passes 17 -> 8.5, tim 23 -> 12; first correct N_ID b 10.1 -> 8.8, tim 6.2 -> 5.0. The 1024-poll cadence delayed the second hinted id (62922 behind the DCI-less 9020) by ~10 s in 3 of 4 b runs; fixed by the 64-poll cadence (3 follow-up runs: 62922 named first within 1 s of the hint). A 10 s re-verify burst was ~30 % GPU duty, so the default became 60 s.
- Intermediate RECHECK 60 s with the 1024 cadence: b first correct N_ID worse than off in 3 of 4 runs (hint set stuck on 9020 alone): the case the top-2 hint exists for, only fixed together with the poll cadence.

## 4. Verdict
- Works as intended on both captures: solver names the id before the sweep, the sweep is cut by about half to three quarters of passes and kernel time, decode metrics unchanged within the replay spread, 0 wrong ids on tim, 1 unexplained anonymous hit on b.
- Not shown: any gain in time to first decode on b beyond noise; cold-start timing on air (replay cannot show it); the 60 s periodic cost; interleaved CORESETs (the hint carries no L/R/shift; the sweep record replaces it when it runs).
- Recommendation: adopt in the OTA env as a measured, flagged trial, not as a permanent default yet. `ISAC_DMRS_NID_SOLVE=3` can be added to the OTA env; it falls back to the full sweep by itself if the solver or the CRC confirmation fails. Before making it default: one daytime OTA run per operator confirming 0 wrong-id decodes (check 48960-type single hits) and sub-slot p99 not worse.
- Flags for OTA (add to t26f; the frozen file was not edited): `ISAC_DMRS_NID_SOLVE=3 ISAC_DMRS_NID_SOLVE_EVERY=8` (optional: `_GRACE_MS=4000 _MAINTAIN_MS=2000 _RECHECK_MS=60000` are the defaults; `ISAC_DMRS_NID_SOLVE_CORE=<c>` to pin the worker; keep `ISAC_PDCCH_NID_GPU=1`; DUTY is implied). Remove the flag to restore the t26f behaviour.

## 5. Limits
n=3-4 per arm; replay is not run-to-run deterministic; interleaving was kept but the host was shared with other agents' replays (heavy.lock serialized them; thermal state not controlled); sweep kernel ms undercounts by up to 7 passes; time-to-first-id measured from GPU-sweep start, not from first sample.

## 6. ctest
`[OFFLINE VERIFIED, DGX aarch64]` ctest -j4 on build_t26: only the known/recurring failures (test_nr_pusch_ra0_qam64, test_nr_pusch_ra0_qam256, test_nr_td_grantwork, dft_test, test_nr_modulation); test_nr_dmrs_nid_solve (incl. the 2 new tests) and the 4 test_nr_pdcch_nid_worker variants pass.

## 7. Review fixes (2026-10-16)
Changes (all still behind `ISAC_DMRS_NID_SOLVE=3`):
1. Hint signature folds id + footprint (grid_rb, groups, sym, dur), so a widened footprint is re-served. `merged()` drops a hint in favour of a sweep record only if that record has the same id/sym/dur and covers the hint's RB range.
2. `ISAC_DMRS_NID_SOLVE_RECHECK_MS` default 15000. The primary state (`nr_dmrs_prim_state_t`: grace anchor, last run, verified, burst) resets whenever the batch cfg (geometry or epoch) changes, with the grace restarting at the retune; the sweep result is cleared, and the solver votes are cleared when the cfg epoch changed (new cell). The detection uses its own cfg hash, because the worker's `previous` only updates on batches that run.
3. Verification (primary only): a listed id counts as confirmed when its verified bank entry has >= 2 cumulative CRC-valid DCI accepts (new `accepts_total` in the bank entry), or 1 whose RNTI also owns another entry. Feedback is applied only when the echoed generation equals the currently served merged generation (no bit-63 shortcut). The "any listed id" semantics are gated on `nr_pdcch_nid_gpu_primary()`; mode 2 keeps "all".
4. `hint_record` returns false when `symmask == 0`; such votes are not hinted.
5. Tests (test_nr_dmrs_nid_solve): `VerificationNeedsTwoConfirmations`, `HintSignatureChangesWithTheFootprintOnly`, `GeometryChangeResetsTheState`. `ctest -R "dmrs_nid|pdcch_nid|pdcch_blind|coreset_bank"`: 34/34 pass [OFFLINE VERIFIED, DGX aarch64].
A first attempt used distinct bank owners as the confirmation count; the bank holds 1 owner per dwelled entry, so nothing ever verified and the sweep kept running (tim 13-24 passes), hence the accept counter.

Sanity replays, final build, solver-primary only (`_EVERY=8`, RECHECK 15 s), `[MEASURED, DGX replay <cap>, n=2]`, no off arm re-run (compare with section 2):
| cap | first correct N_ID (s) | sweep passes | DL CRC-OK % | DL p50 / p99 us | solver CPU s |
|---|---|---|---|---|---|
| b run 1 / 2 | 6.47 / 7.91 | 1 / 3 | 58.5 / 53.9 | 1113 / 5480; 1080 / 4607 | 0.29 / 0.26 |
| tim run 1 / 2 | 4.04 / 4.03 | 10 / 10 | 63.9 / 66.2 | 586 / 2868; 567 / 2868 | 0.14 / 0.14 |
Versus off in section 2 (b 10.6 s, 47 %; tim 5.9 s, 64.7 %): no regression in first correct N_ID or CRC-OK, only id sets seen in off (62922, 10163, 1931), 0 unexpected ids. tim passes = one 10-pass burst when the 15 s re-verify fires after the replay data ended (not a cold start cost). n=2 is a sanity check, not a measurement of effect size.

## 8. OTA recipe
Run `ISAC_DMRS_NID_SOLVE=3 ISAC_DMRS_NID_SOLVE_EVERY=8` as a separate A/B arm (same cell, same env otherwise), NOT in t26f. Optionally `ISAC_DMRS_NID_SOLVE_CORE=<c>`. Compare per operator: time to the first correct N_ID (`hint served` / `PDCCH_NID_GPU pass` t= vs `t0`), `PDCCH_NID_GPU cumulative` sweep runs and kernel_ms, DL/UL decode p50/p99, CRC-OK, and any `JOINT_ANON_HIT` with an id outside the off arm's set. Unset the flag to return to t26f.
