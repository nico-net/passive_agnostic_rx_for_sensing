# MPCI-FIX — multi-PCI fixes, 2026-11-02/03 (branch fix1102/mpci-pbch, from mpci-main 531488c715; not merged, not pushed)
Host DGX Spark + X410 34B2AF1, 4 RX, sensing cfg ue.passive.ota.sense.dgx.cfg, MPCI_20261025.env + ISAC_SIB1_CACHE=0. Labels per PROJECT_MEMORY §0.1; numbers are per cell, never pooled.
OTA tonight = night load (22:15-05:30), on the operator's request; grant counts vary 5x run to run at this hour (same cell, minutes apart), so compare locks / CORESET / CRC, not counts.

## Status in one table (OTA, night/early morning load)
Final campaign 06:24-07:16 on HEAD f47cf4925b [MEASURED]:
| run | cell | locks / LOCK_REJECT / re-init | CORESET banked at | nID passes (longest run) | grants (CRC) | >128 PRB n (CRC) | decoded PRB | overruns |
|---|---|---|---|---|---|---|---|---|
| FB1 415+923 | 415 | 3 / 0 / 0 | 23.4 s | 92 (4.3 s) | 1607 (92.7) | 345 (86.1) | 87k | 5 |
| | 923 | 4 / 0 / 0 | 6.7 s | 136 (5.0 s) | 9523 (82.7) | 59 (0.0) | 126k | |
| FB2 415+923 | 415 | 21 / 0 / 0 | 3.6 s | 111 (3.9 s) | 5459 (90.7) | 382 (52.1) | 130k | 39 (29 in the first 20 % of the run) |
| | 923 | 20 / 0 / 0 | 0.0 s | 85 (7.5 s) | 25469 (86.8) | 694 (48.3) | 530k | |
| FV27 415+27 | 415 | 1 / 0 / 0 | 16.7 s | 112 (7.9 s) | 12302 (80.4) | 1555 (55.7) | 368k | |
| | 27 | 1 / 0 / 0 | 3.3 s | 254 | 15931 (48.9) | - (51 PRB) | 239k | |
| FVL 415+403, FLW 403+508 | 403 | never found (17-18 searches, x2 each) | - | - | - | - | - | 403 not on air (3630 MHz carries PCI 27, barred) |
| | 415 / 508 | lock / 0 / 0 | 415 MISSED in FVL (38 passes beside a cell stuck in initial sync) | | | | | |
| solo FS415 | 415 | 1 / 0 / 0 | 42.9 s | | | 11 (45.5) | 2k | 0 |
| solo FS923 | 923 | 1 / 0 / 0 | 3.4 s | | | 941 (12.1) | 272k | 0 |
All 21 415+923 runs of the night (01:26-05:08, earlier builds) and the 4+2 of 06:03-06:58: 0 LOCK_REJECT, 0 device re-init. The 415 CORESET history: banked 3/3 before 03:00,
3/18 between 03:00 and 05:10 (before the lost-wakeup fix), 4/5 after it (LB2, LB3, FB1, FB2; LB1 0 candidates).

## Fixes (commits, each test-first)
1. 3b383fe6d7 initial sync: a search pinned to the cell's PCI (multi-cell receiver only) gets up to 3 PSS peaks of ONE N_ID_2; after the SSS ranking the best one was redone only if
   its N_ID_2 differed from the last tried -> the PBCH was decoded off another beam's SSB symbols while timing came from the best one: initial PBCH CRC ok, tracking PBCH 0/50,
   lock rejected forever. Root cause of "one cell works, two do not". [OFFLINE VERIFIED, DGX replay, same channelized samples]: single-cell receiver beam 4 first 3/3, multi-cell beams 2/5
   9/9 never tracking; after the fix 0 rejects in 6 replays. [MEASURED OTA] 0 LOCK_REJECT in every pair run since (beam 5 now tracks with two cells).
2. 0ef5347e2b lock verify: a relock between two census windows was not seen -> the old beam was rejected again (logs: "LOCK_REJECT ssb_index=5" while locked on 2). Lock generation passed in.
3. 99be10cda0 nID sweep: it was fed the cell epoch, bumped by every SOFT RX_RECOVERY (~3 s): each bump restarted the 10-batch window (~5 s) -> 0 passes. Now the dedicated epoch.
   [OFFLINE VERIFIED, DGX replay] 0 passes / MISSED 3/3 -> 10-30 passes, banked 3/3.
4. dda0cfcddb GPU budget: the process-wide discovery budget was single-cell (one pct share for all cells, an exclusive owner needing a device free of RT work, held across the
   ticket wait). [MEASURED OTA, diagnostic build] per 150 s 27466 slices / 9 s GPU per cell in a pair vs 122653 / 28 s solo, 48 s admission spin + 57 s ticket wait; yields 174-234 M.
   Now with k >= 2 discovering cells: budget/pacing min(100, k*pct), admission = pacing + budget, the G4 ticket is the single flight. One cell unchanged (unit tested).
   [MEASURED OTA qB2] nID passes 70/65 (were 26-52), yields 219 k, discovery share 15.3 % (solo 14.4 %), 923 dec p50/p99 747/3348 us, e2e 1828/6413, async drop 0.2 %.
5. f47cf4925b GPU ticket: a waiter never sleeps unbounded. [MEASURED, gdb on the stalled receiver] the ticket was FREE (no holder, no RT, pacing expired) and the 4 bulk
   waiters (nID + CSI-RS of both cells) slept untimed for up to 107 s: a lost wakeup stopped all discovery (pB2, bB1, gC2, gD2). Recheck every ms; how the wakeup was lost
   is not established. [MEASURED] longest nID run 3.9-7.9 s in 7 runs since (was 20-107 s).
6. 80e3de06f8, eff313dcea logging only: nID push-reject causes / batch states on CORESET_DISC; DM-RS-gated grants show the gate SNR (they looked like "never demodulated").
Earlier: 02e01e8ad2 (reject a never-tracking lock, per-cell recovery, no device re-init: B done on air), 3d5e2371c9 (never shed a CORESET-less nID sweep), iq_chan_offline tool.
Tried and reverted (no measured effect): ticket priority for the acquiring sweep, nID worker CPU float, nID stream priority, core-pool resize (replay).

## Per-run OTA results (night)
| run (time) | cell | locks/rej | nID passes | CORESET | grants (CRC) | >128 PRB n (CRC) | decoded PRB |
|---|---|---|---|---|---|---|---|
| nB1 (01:26, 99be10cda0+logs) | 415 | 7/0 | 66 | banked | 4578 (93.1) | 606 (78.5) | 165k |
| | 923 | 6/0 | 67 | banked | 29669 (83.8) | 2030 (68.1) | 677k |
| nB2 (01:56) | 415 | 3/0 | 71 | banked | 2055 (90.7) | 246 (72.4) | 78k |
| | 923 | 3/0 | 50 | banked | 37133 (87.6) | 3319 (59.0) | 948k |
| xB3 (02:35) | 415 | 6/0 | 66 | banked at 78 s | 586 (89.9) | 204 (92.6) | 50k |
| | 923 | 7/0 | 57 | banked | 34809 (87.8) | 1991 (59.8) | 741k |
| xV27a (02:41) | 415 | 1/0 | 65 | banked | 11859 (94.3) | 5883 (91.3) | 1.31M |
| | 27 | 1/0 | 53 | banked | 1356 (23.7) | - (51 PRB) | 4k |
| nVL1 (01:32, 403 absent) | 415 | 1/0 | 32 | banked | 13413 (88.7) | 4129 (89.1) | 936k |
| qB2 (04:56, dda0cfcddb) | 415 | 3/0 | 70 | MISSED (0 candidates) | 1 | 0 | 0 |
| | 923 | 4/0 | 65 | banked at 6.7 s | 23922 (91.8) | 29 (34.5) | 362k |
| qB3 (05:04) | 415 | -/0 | 81 | MISSED (14 candidate passes, none verified) | | | |
| qV27 (05:00) | 415 | -/0 | 156 | banked at 67 s | | | |
| solo nS415 (02:14) | 415 | 1/0 | - | banked | 14310 (96.3) | 3747 (90.4) | 960k |
| solo nS923 (02:18) | 923 | 1/0 | - | banked | 18432 (73.3) | 3013 (44.9) | 498k |
| solo zS415 (04:35) | 415 | 1/0 | 155 | banked at 36.7 s | | | |
923's big-grant CRC is our link SNR (median 10 dB at 16QAM vs 12.5 on 415; 256QAM 0 % on every cell, SNR 10-20 dB, EVM 20-35 %): physics, not a receiver defect.

## Open
- 415's dedicated CORESET after 03:00. Interleaved A/B at 05:09-05:22 [MEASURED]: solo 415 did not bank either (bS1/bS2: 119/153 passes, 11/0 with candidates), while
  PDSCH-like energy not explained by any grant was present in every run (window_ungranted_pdsch_like_rbslots 1.1-2.8e5), so traffic was there: 415's CORESET discovery is
  unreliable at night even solo (zS415 at 04:35 banked at 37 s), and a pair makes it worse (fewer passes: bB1 2, bB2 28 vs 119-155 solo; with 923 the passes find 0-14
  candidates). Next to the light PCI 27 it banks (qV27: 156 passes, 128 with candidates, banked at 67 s). (UL DCI counts are no traffic proxy: they need the CORESET.)
- Watch: FB2 had 39 SHMIQ_OVERRUN (reader behind its channelizer ring -> resync), 29 of them at start-up; other runs of dda0cfcddb/f47cf4925b 5-11, HEAD before 4-11.
  dda0cfcddb lets discovery run beside RT GPU work with >=2 cells: daytime A/B against eff313dcea on overruns and decode p99 before merging.
- A partner stuck in initial sync (403 absent) slowed 415's sweep to 38 passes and 415 missed (FVL): the searching cell's acquisition load is not bounded.
- 403/508 pair: re-test in daytime (403 off air at night).
- ctest failures beyond the known ARM list also fail on mpci-main 531488c715 (test_nr_ssb_rate_match_prod, test_nr_gpu_pass_cells, test_nr_pusch_ul_disc,
  test_ldpc_cuda_pool_cpu); nr_td_sim_test timed out (not compared); test_nr_gpu_pass_geo* and test_iq_chan flaky under -j4, pass alone.

## OTA test for the operator (daytime)
HEAD 08f5839939 (code f47cf4925b), 300 s, pairs 415+923, 415+403, 403+508 x2 each + solos; plus the A/B of dda0cfcddb vs eff313dcea on overruns and decode p99. Look at: LOCK_REJECT = 0, device re-init = 0, per cell "CORESET_DISC state=LOCKED" and its
armed_for_s, nid_passes, per cell >128 PRB CRC and decoded PRB vs solo.
