[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, b6e5fb27ac (+fix 1d6cbdf5c3)] -- gtest, TSAN
[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, b6e5fb27ac (+fix 1d6cbdf5c3)] -- rfsim TSAN oracle, A/B, gate

Task A7: thread-safe blind-PDCCH scan -> N consumers. All binaries built from the tree committed as b6e5fb27ac (round-1 fix: 1d6cbdf5c3), together
with this file. Host: 4 cores, so every consumer is UNPINNED (scan_thread core field -1: queue_start passes -1 to
every consumer; affinity >= 0 would pin consumer i to core + i).

## 1. Unit test (TDD, RED -> GREEN)
Phase2Concurrent.FourThreadsSameRntiLoseNoAcceptAndNoSighting / .EnergyFloorCountsEverySampleFromFourThreads
(openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc), driving the new lean module
nr_pdcch_blind_phase2.c (Phase-2 lock, RNTI-persistence ring, energy floor) from 4 threads x 10 000 accepts.
- RED (module moved verbatim, unsynchronised), plain build: accepts 25279/40000, energy nseen 27423/40000.
  TSAN: 13 data races (g_recent*, accept counter, energy floor) -> tsan_gtest_concurrent_before.txt
- GREEN: both pass, TSAN 0 warnings -> tsan_gtest_concurrent_after.txt
- Fix round 1 (review minor): the test now drives the PRODUCTION accept gate nr_pdcch_blind_dl_accept_gate() (the
  function rt.c's DCI 1_1 path calls; the duplicated EMA line is gone) and asserts the production sightings counter
  (guarded by g_phase2_mu). RED re-check with g_phase2_mu lock/unlock compiled out: sightings 14802/40000 -> FAIL;
  restored: pass, TSAN 0 warnings (file above now holds this run). Full gtest + seeds 1/3/5: 197 + 2 skips;
  ctest -R pdcch 13/13.
- Full gtest + shuffle seeds 1/3/5: 197 passed + 2 skipped each (195 + 2 new) -> gtest_shuffle_1_3_5.txt
- test_nr_passive_obs under TSAN (A3 reviewer request): 8/8 passed, 0 warnings -> tsan_test_nr_passive_obs.txt
- ctest: 126/128; failures = env-only baseline set {test_thread-pool, nr_cuup_functional_test} -> ctest_summary.txt
  (`ninja` of the whole tree also fails to LINK nr_psbchsim / nr_srssim / nr_ulsim / nr_ulsim_mu_mimo on undefined
  nr_isac_* / nr_ue_diag_* / nr_passive_acq_note_sib1 symbols from untouched files: pre-existing, not in ctest.)

## 2. TSAN oracle on the real occasion path (rfsim 106 PRB, scan_thread "2:16:-1", 300 s, receiver only instrumented)
| run | instrumented | stage reached | reports | passivePdcch0 <-> passivePdcch1 |
|---|---|---|---|---|
| tsan_rfsim_full_before.txt | whole receiver, Debug, HEAD 0d04761969 | discovery only (TSAN too slow) | 54 | 37 |
| tsan_rfsim_full_after.txt | whole receiver, Debug, round-1 fix | discovery only | 7 | 0 |
| tsan_rfsim_partial_round1.txt | blind-PDCCH sources only (compiler launcher), round-1 fix | TRACKING (bank, Technique D converged) | 46 | 26 |
| tsan_rfsim_partial_round2.txt | blind-PDCCH sources only, final code | TRACKING (bank, conv 162 s) | 24 | 0 |
Round 1 fixed the research list plus what the full-TSAN run found (dci_nr.c DM-RS probe, SS registry, sweep identity,
pbwp extract opts, lane length state, ...). Round 2 fixed what only the tracking stage reaches: RNTI bootstrap table
(no lock at all), PDSCH producer-side pending batch, discovery generation/verified flags.
REMAINING in round 2 (not consumer<->consumer, so not introduced by N>1): 13 UEthread_0 <-> passivePdcchN races in
nr_pdcch_blind_monitor.c (autodiscover_step on the receive thread vs note_rnti_for_windows / extent advance on a
consumer) -- present with ONE consumer too; 4 passivePdsch pool races (nr_pdsch_passive_queue_thread); rfsim/exit.

## 3. rfsim A/B, 106 PRB, 150 s, alternating S/M/S/M/S/M (ab/), plus two depth controls
S = scan_thread "1:8:-1", M = "2:16:-1". acc/occ = pdcch_accepts/pdcch_occasions over the DL_CONVERGED ISAC_METRICS
lines; CPU from tests/passive_rx/dgx/thrprof.sh over 20 s at t+75 s (tracking), 100 % = one core.
| arm | crc % | drop_full % | ttc s | conv | acc/occ | scanq max_lag | passivePdcch CPU % (0 / 1 / sum) | UEthread_0 % |
|---|---|---|---|---|---|---|---|---|
| S1 | 95.04 | 1.634 | 4.78 | 2 | 0.475 | 8 | 71.6 / - / 71.6 | 103.3 |
| M1 | 97.78 | 0.640 | 2.59 | 2 | 0.470 | 16 | 40.2 / 40.4 / 80.6 | 103.8 |
| S2 | 95.38 | 1.167 | 4.25 | 2 | 0.458 | 8 | 71.0 / - / 71.0 | 104.3 |
| M2 | 94.76 | 0.555 | 4.28 | 2 | 0.443 | 16 | 36.5 / 36.5 / 73.0 | 105.1 |
| S3 | 96.37 | 1.565 | 2.45 | 2 | 0.449 | 8 | 72.5 / - / 72.5 | 103.7 |
| M3 | 96.65 | 0.467 | 2.55 | 2 | 0.446 | 16 | 42.1 / 42.1 / 84.2 | 106.4 |
| S mean | 95.60 | 1.455 | 3.83 | 2 | 0.461 | 8 | 71.7 | 103.8 |
| M mean | 96.40 | 0.554 | 3.14 | 2 | 0.453 | 16 | 79.3 | 105.1 |
| M8 "2:8:-1" | 94.89 | 1.018 | 4.78 | 2 | 0.462 | 8 | 42.0 / 40.5 / 82.5 | - |
| S16 "1:16:-1" | 96.71 | 0.722 | 3.09 | 2 | 0.480 | 16 | 72.3 / - / 72.3 | - |
Verdict: acc/occ (S 0.449-0.475, M 0.443-0.470) and CRC (S 95.0-96.4, M 94.8-97.8) within run-to-run spread;
total passivePdcch CPU M 79 % vs S 72 % (+10 %: lock/cache overhead; each consumer ~40 %). drop_full lower with M.
scanq max_lag is 16 for M vs 8 for S, but it follows the queue DEPTH, not the consumer count: the controls give
2:8 -> 8 and 1:16 -> 16 (max_lag saturates at the depth whenever the ring fills once). Not a regression.
No speed-up claim: on 4 cores the single consumer already keeps up (drop_full 1.2-1.6 %); the A/B shows the work
splits across two threads with identical decode results, which is what A7 must establish before the 273-PRB DGX run.

## 4. Regression gate (default config, SCANTHREAD auto "1:8:-1")
GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5 tests/passive_rx/dgx/rfsim_regress.sh 1 -> PASS, crc 96.65 %, drop_full 0.75 %,
CONVERGED 2, ttc 2.19 s (HEAD baseline crc 94.9-96.9 %, drop 0.77-1.55 %) -> regress_base_r1.score.json
