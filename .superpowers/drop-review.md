# Review: twomodes/single1-drop (f5ba41e43d, 673bf3220c) -- CFR ring drop warm-up
Verdict: APPROVE (0 Critical, 3 Important, 3 Minor). Code review only; nothing built or run.

## Checks
1. Bit-identity / cache: OK. warm_step calls sp_crit (nr_sense_quality.c:287-292), the same function and key tuple as production (:699, :705); same 16-way set (:246-271). 1194 keys over 256 sets x 16 (mean 4.7/set): overflow/RR eviction of a warmed key is negligible, and existing test SenseQualityRound3.ThresholdCachesDoNotThrash... already shows 0 misses over a similar set. Warm uses n_ant = frame_ant (nr_sense_api.c:401 = receiver_antennas, nr_isac.cc:344), the real frame n_ant is p[17] = min(nof_ant, cfr_max_ant) (:209): equal for csirs/pdsch frames at 4 RX.
2. Coverage: window eta grid (22, identical to :480) x N 1..30 upper; QAM 16/64/256 x 1..168 upper and 3..12 lower. Matches calls at :699/:705. Cold left: gcrit (see I2), window rows with tot[p] > 30, 1024QAM (hpar 10), n_ant != frame_ant.
3. Threads: caches are __thread heap; frame_quality (api.c:340) and job_quality (:307) both run in run_writer, same thread as warm_step. warm_step runs with mu released (api.c:267), so no producer/RX/scan blocks on it; no new lock. Lost-wakeup safe (predicate rechecked under mu after relock).
4. Idle: gated on frame_count==0 && record_count==0, one key per iteration. See I1.
5. Traffic mode: untouched (requires nr_isac_mode()==SENSING, api.c:264).
6. No env/CLI/-D flag, no fixed core; only locals (eta[] is a local const array, warm_more/warm_cur locals); static_audit clean per report.
7. Tests: D1 is meaningful (one build per step, 8 probe keys bit-equal, all hits afterwards). D2 pins zero misses on hot shapes.

## Important
- I1 nr_sense_api.c:264-268: a warm step is unbounded by the queue: a burst that arrives during a step waits for it (heavy keys, e.g. QAM N~168 na=4 -> M=32, are the ~2-5 ms class). Ring is 32 frames at 14 us spacing = 0.45 ms, so a step can cause the very stall it prevents. Effect limited to the ~0.2-0.3 s warm-up (+ idle-only stretching), but order keys cheap-first (small na*N first, heavy last) or warm the heaviest first right at session open before traffic. Not measured.
- I2 nr_sense_quality.c:90/698/704: gcrit (Gamma/F bisection, 40 ftail evals) is not warmed; it is used only when nu_s/nu_r > 0 (estimated reference) and keys are continuous in (kT,nu) (quantised 1/16 and x1.25 steps), so it fills slowly. Its cold cost is unmeasured (report open issue 3); expected well below sp (no 2M+1 complex atom loop) but not proven sub-ms. Measure with nr_sense_quality_gcrit_stats before calling the cause closed.
- I3 Evidence: the cause is [MEASURED] only in one un-warmed replay arm (d2) and OTA offline burst structure; replay A/B shows 0 drops in all arms, so the fix is unproven to reduce drops. Needs the 240 s OTA arm on 415 (open issue 1) before merge claims; state it as [UNVERIFIED OTA].

## Minor
- M1 nr_sense_quality.c:277-279 / report: key count is 1194 (test asserts 22*30+3*178), report/comment say 1158.
- M2 tests/nr_sense_quality_test.cc (HotShapeCostAfterWarmUp): EXPECT_LT(worst, 2000 us) is a wall-clock assert; flaky on a loaded host (CLAUDE.md shared-host). Prefer asserting only misses==0.
- M3 Warm-up repeats per cell writer (0.2-0.3 s CPU each); acceptable, note for multi-PCI startup.

## Multi-PCI 415 cfr_drop 1.6-4.6k per arm
Not plausibly covered. Single-PCI delta was 71 -> 252 (cold-build stalls, one-off at ~7 s and during new RE counts); 1.6-4.6k is 10-20x larger and persists, which points to steady-state ring sizing (32 frames vs 16-frame csirs batches x 2-3 slots, 48 frames) and/or writer throughput/CPU contention among several cell writers, not a one-time cache fill. Warm-up gives at most a fraction. Check per-cell writer busy % and ring_full vs time (steady vs initial) first; remedies are release-before-quality or ring sized from rate x batch time (report open issue 2).
