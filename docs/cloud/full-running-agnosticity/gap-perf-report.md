# Lane perf — Technique D convergence on the phy-test bed

Worktree `sens6:/home/sens/NICOLA/agn-wt/gap-perf`, branch `sdd/gap-perf` (from `sdd/validation` @ c295fa18f1).
Bed: `agn-wt/gnbtest` nr-softmodem `--phy-test -m 9 -n 0 -M 106 -l 1 -D 0xff`, receiver conf
`rfsim_validation/phyA2/ue.passive.q.agn.conf` (fully agnostic, `pdcch_blind_monitor_pdsch = "2:0:0:1:16"`
= in-line decode on the single scan consumer `scan_thread = "1:8:5"`). Runner: `rfsim_validation/perf/run_arm.sh`.

## Progress log

### 1. Measurement (val binary c295fa18f1, 150 s, `ISAC_PDCCH_TIMING=1`, logs `rfsim_validation/perf/base_timing/`)

- `Technique D ARMED` logged **265238 times** in 150 s (also in every `valphytest/A0_*` log). Each occasion runs
  the bank pass, then the further-CORESET discovery pass on the root cfg; that pass calls
  `pdsch_sweep_maybe_enable()` / `dl_discovery_invalidate()` with a different identity / not-ready and
  `nr_pdsch_config_sweep_reset_all()` wipes every context and every per-RNTI observation. No Technique-D context
  outlived one occasion → no accumulation → 0 CONVERGED. (The "new per-RNTI context" line is capped at 20, which is
  why this was invisible.)
- Scan consumer: BTIM TOTAL 133.2 s busy of 150 s; **dlsweep 102.9 s (77 %)** = the discovery pass's DL
  dci_length sweep on non-existent further CORESETs; in-line PDSCH decode only 9.9 s (PDTIM per decode: fep 19 us,
  chest 60 us, demod 12 us, ldpc 187 us ≈ 300 us). `scanq drop_full=190876 / queued=468343` (41 %) — occasions
  dropped at the scan ring, the bank's real grants included.
- gNB (phy-test, no UE feedback): `dlsch_rounds 76827/76822/76822/76822` — every TB sent 4× (rv 0,2,3,1) on DTX,
  so only 1/4 of grants are rv 0; `rv0_only=1` correctly skips the rest. `pdsch_decode try=33084` (~220/s),
  crc_ok 0.
- Code finding: the in-line path (this conf) and the normal-deferred path decoded every k0 hypothesis on the
  DCI's own slot, so each k0=1 catalog entry is an exact twin of its k0=0 sibling; the sweep correctly refuses to
  separate indistinguishable hypotheses → could never converge even with persistent contexts. Only the fast
  deferred path honoured k0.
- Code finding (progress.md Task 6 minor): prunes re-index `st->hyp[]` without a generation bump, so in-flight
  tickets credit whatever moved onto their index.

### 2. Increment 1 — commit fe96d0c4b6

Fixes: no context wipe on identity change / discovery invalidate; discovery pass yields to scan backlog
(floor 1 in 16, `nr_pdcch_blind_monitor_discovery_pass_due()`); k0 honoured on all decode paths
(`nr_pdsch_k0_slot()`, in-line waits off-RT for the producer to pass the slot, else skips without scoring);
prune/probation restore retire outstanding tickets; Qm oracle on the in-line path; `SWEEPSTAT` census.
Tests (TDD, red first): `PdschConfigSweepOracle.TicketIssuedBeforeAPruneCannotScoreAfterIt`,
`PdschConfigSweepQm.TicketIssuedBeforeATablePruneCannotScoreAfterIt` (both red on the old code),
`PdschConfigSweepK0.TargetSlotWrapsFrameAndSfn`, `DiscoveryGates.BackgroundDiscoveryPassYieldsToABacklogButCannotStarve`.
`TypeB.ObservingATypeBOnlyMask...` updated to look through a fresh ticket after the prune (its old ticket is now,
correctly, retired). ctest 11/11 relevant suites pass (sweep 43/43, blind monitor 155 + 2 pre-existing skips).

### 3. fix1_r1 (fe96d0c4b6, 150 s, VALID load 3.0-5.1, `ISAC_PDCCH_TIMING=1`)

`pdsch_decode try=57154 crc_ok=1148 (2.0 %)` (baseline 33084 / 0), `scanq drop_full=790 / 397186` (0.2 %, baseline
41 %), dlsweep 71 s (yielded 240001 / ran 128161). **Still 0 CONVERGED**: SWEEPSTAT shows `created=214` contexts for
only 3 offered layouts (3/7/11), each context ≤1807 outcomes; the true hypothesis S=1 L=13 k0=0 tbl=0 is the leader
wherever it has trials (65/66) but 499 others stay unrefuted. Two causes:
- the bank pass's Technique-D identity folded in `autodiscover_generation()`, which bumps on every discovery-cursor
  move → the bank's contexts (and its layout pin) were re-keyed ~every second (20/20 capped ARMED lines carry
  distinct identities);
- in-line decode path never ran the DM-RS symbol oracle, so contexts stay at the full catalog (500 after Qm).

### 4. Increment 2 (built, under test): bank identity without the discovery generation (`t_pass_kind == PASS_BANK`);
`nr_pdsch_passive_oracle_inline()` runs the consumer's DM-RS oracle on the in-line path after the trial's feedback
(off the RT thread only).

### 5. Status at handback #2
- Increment 2 is built in gap-perf and not committed yet: `nr_pdcch_blind_monitor_rt.c`, `nr_pdsch_passive_queue.[ch]` are modified vs fe96d0c4b6.
- fix2_r1 is queued behind the gap-rank4 lane's capture. Command:
  `rfsim_validation/perf/quiet_run.sh fix2_r1 <gap-perf nr-uesoftmodem> 150`, detached. The result will land in
  `perf/fix2_r1{.verdict,/rx/rx.log}`.
- Scoring: `rfsim_validation/perf/score.py <arm_dir>...` prints, per run:
  - CONVERGED count;
  - time-to-converge, from the first `rnti_seen ... 0x1234` to the utc_ns of the last rnti_seen line before the CONVERGED line;
  - crc_ok/try and drop_full;
  - the VALID/VOID verdict;
  - a mean±sd summary.

  Baseline and fix1 rescored with it: base_timing 0 CONVERGED, 0/33084, drop 40.8 %; fix1_r1 0 CONVERGED, 1148/57154,
  drop 0.2 %.
- Next steps:
  1. Score fix2_r1. If CONVERGED, rerun ctest (11 suites) and commit increment 2.
  2. Run n=5 per arm, alternating FIXED (gap-perf) and val (`agn-wt/val`), with quiet_run.sh, then `score.py`.
  3. If fix2 does not converge, read SWEEPSTAT: n_hyp after the oracle, the created count, and unrefuted.

### 6. fix2_r1 and the increment 2 commit (ccf3b7a2c6)

fix2_r1 (150 s, VALID, load 5.9–8.3) was the first run to converge:

- **CONVERGED = 2:** tda=0 S=1 L=13 mask 0x804, and tda=2 S=1 L=5 mask 0x4. These are the gNB's own TDRA entries 0 and 2.
- **Time to converge:** 1.3 s (wall clock) after the first `rnti_seen 0x1234`, which spans SFN 578 to 796 (218 frames on air). Wall time is shorter than air time because rfsim runs faster than real time.
- **Decoding:** 5 contexts, crc_ok 66132/66896 (98.9 %), scanq drop_full 1000/462533 (0.2 %).
- **Final SWEEPSTAT:** n_hyp = 12 per context, leader 93/93, lo = 0.755 against max_other_hi = 0.538, 0 hypotheses unrefuted.

Offline results:

- ctest passes 11/11 relevant suites.
- `test_nr_pdsch_config_sweep` (43 tests) and `test_nr_pdcch_blind_monitor` (155 tests) pass under `--gtest_shuffle` with seeds 17, 34 and 51.
- Increment 2 changes only `rt.c` and the queue consumer. Both need a live `PHY_VARS_NR_UE`, so the change has no unit test. The fix2_r1 run above is its evidence.

Increment 2 was committed as ccf3b7a2c6.

### 7. n=5 A/B (running)

`perf/batch.sh` alternates `ab_fix_r1..5` (gap-perf ccf3b7a2c6) with `ab_val_r1..5` (agn-wt/val c295fa18f1). It then runs two condition arms with the gap-perf binary:

- `cond_al1_fix`: gNB `ISAC_GNB_TEST_USS_AL=4,0,0,0,0`, receiver `ISAC_AL1_COVER=1`.
- `cond_qam256_fix`: gNB `-m 20 -n 1`.

Every run goes through `quiet_run.sh`, so it gets a VALID/VOID verdict. Score with `./score.py ab_fix_r* ; ./score.py ab_val_r*`. The batch writes `batch.done` when it finishes.
- ab_fix_r1 (VALID, load 0.7–4.7): CONVERGED 2, ttc 1.3 s, crc_ok 67222/67988 (98.9 %), drop_full 0.4 %.
- ab_val_r1 (VALID, load 4.3): CONVERGED 0, crc_ok 0/32513, drop_full 41.2 %.
- ab_fix_r2    conv=2 ttc=1.3s crc_ok/try=66658/67414 (98.9%) drop_full=1425/472858 (0.3%) VALID
- ab_val_r2    conv=0 ttc=- crc_ok/try=1/32673 (0.0%) drop_full=191580/467741 (41.0%) VALID
- ab_fix_r3    conv=2 ttc=1.3s crc_ok/try=66584/67375 (98.8%) drop_full=1948/468229 (0.4%) VALID
- ab_val_r3    conv=0 ttc=- crc_ok/try=0/31696 (0.0%) drop_full=198342/469366 (42.3%) VALID
- ab_fix_r4    conv=2 ttc=1.3s crc_ok/try=67320/68067 (98.9%) drop_full=1337/471266 (0.3%) VALID
- ab_val_r4    conv=0 ttc=- crc_ok/try=2/32540 (0.0%) drop_full=193712/470255 (41.2%) VALID
- ab_fix_r5    conv=2 ttc=1.3s crc_ok/try=67204/67966 (98.9%) drop_full=1351/470595 (0.3%) VALID
- ab_val_r5    conv=0 ttc=- crc_ok/try=0/32707 (0.0%) drop_full=192081/468415 (41.0%) VALID
- SUMMARY n=5 converged_runs=5 ttc=1.31±0.04 crc_ok=66997.60±347.60 try=67762.00±337.86 drop_full=1551.40±264.72
- SUMMARY n=5 converged_runs=0 ttc=- crc_ok=0.60±0.89 try=32425.80±416.36 drop_full=193863.20±2671.17
- cond_al1_fix conv=2 ttc=1.4s crc_ok/try=66346/67109 (98.9%) drop_full=1421/471244 (0.3%) VALID
