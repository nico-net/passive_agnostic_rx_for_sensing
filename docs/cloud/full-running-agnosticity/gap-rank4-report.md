# Lane gap-rank4 — progress log

## Setup
- Worktree `/home/sens/NICOLA/agn-wt/gap-rank4`, branch `sdd/gap-rank4`, forked from `sdd/validation @
  c295fa18f1`.
- Build dir replicated from `agn-wt/val`'s cmake cache (RelWithDebInfo, ENABLE_ISAC_SENSING=ON,
  ENABLE_LDPC_CUDA=ON, ENABLE_TESTS=ON, OAI_RF_EMULATOR/SIMU/USRP/VRTSIM=ON), built
  `nr-uesoftmodem nr-softmodem rfsimulator oai_usrpdevif params_libconfig` via `lane-make.sh
  gap-rank4` — clean build, no errors.
- Confirmed content-present (different commit hashes from rebase, same as the base-bisect pattern):
  LBRM n_L cell-wide hypothesis (`nr_pdsch_passive_decode.c` `g_lbrm_nl`/`lbrm n_L=%d try/ok`) and LLR
  int8-clip normalisation (`llr_norm shift0..4` counters) are both PRESENT in this tree.

## Bug #1 (rank-4/100MHz PBCH never syncs) — investigation

Read `rfsim-results-phaseA.md` Bug #1 + the 2026-09-26 bisect follow-up (PRE-EXISTING at base
`222f98d072`) + `rfsim-channel-nb-tx-from-own-tx-count` memory (the 2026-09-16 working rank-4 bed used
a DIFFERENT conf, `ue.passive.pin49r4.100mhz.conf`, not the `ue.passive.q.100mhz.conf` /
`gnb.sa.rfsim.100mhz.rank4.conf` pair phaseA's Bug #1 actually exercises).

Diffed the two UE confs:
```
frac_delay_taps: 8 (q) vs 0 (pin49r4, commented "keep rfsim fast enough to sync")
objects: q = 1 object, 2 waypoints; pin49r4 = 3 objects, tuned for <=10-sample excess delay
rx_array: q = lambda/2 spacing (0.0399 m); pin49r4 = 4-lambda spacing (0.32 m)
tx_array: q = NONE (every port radiates the same wavefront); pin49r4 = explicit 4-lambda array,
  boresight 0 deg (along LOS, broadside)
pdcch_blind_monitor_bwp/dci_bits: DCI-decode-only knobs, irrelevant to PBCH sync
```

Read `sensing_channel.c`'s `accum_tap`/`tx_steer`: with no `tx_array`, `txr=1,txi=0` for every TX port
(coherent, unweighted sum) — ruled out as an amplitude/nulling issue for the no-tx_array case
specifically. Read `apply_channelmod.c`'s `rxAddInput`: the non-zero-tap-only convolution optimisation
(2026-07-26 fix) is present; cost is `O(nbSamples * nbTx * n_nonzero_taps)` per RX antenna, and
`frac_delay_taps=8` multiplies non-zero taps per path ~17x vs `frac_delay_taps=0`'s 1 tap/path — a
real, documented (by the working conf's own comment) compute-budget concern at nb_tx=4/nb_rx=4/100MHz.
Ruled out the commented-out `lin_amp = .../sqrt(nb_tx_antennas)` power-normalisation snippet in
`apply_channelmod.c` — it's dead legacy-study text, not active code.

**Test 1 (frac_delay_taps isolation): REFUTED as the sole cause.** One-line scratch conf
(`/home/sens/NICOLA/rfsim_validation/gap_rank4_frac0.conf` = `ue.passive.q.100mhz.conf` with
`frac_delay_taps 8->0`, nothing else changed) against `gnb.sa.rfsim.100mhz.rank4.conf`: still 100%
`ERROR NR_PBCH_DECODE => polar decoding wrong`, `pbch not decoded on any branch`. So the frac-tap
compute-cost hypothesis alone does not explain Bug #1; the remaining candidate differences are
`rx_array`/`tx_array` geometry and/or the `objects` scene.
**CAVEAT: this run partially overlapped another lane's build (sens6 load hit 25, cc1plus/make active
at the time per the coordinator's mid-task notice) — treating as VOID/unconfirmed pending a clean
re-run, not as a settled refutation.** Logs:
`sens6:/home/sens/NICOLA/rfsim_validation/gap_rank4_test1/{gnb,rx}/`.

**Test 2 (full pin49r4 conf pair, unmodified, against current binaries): ABORTED before capture** —
launched right as another lane's build was still running (load 13, cc1plus/make/ninja pids present).
Killed immediately per the coordinator's rule (no capture data recorded). Logs directory
`sens6:/home/sens/NICOLA/rfsim_validation/gap_rank4_test2/` exists but is empty/void.

## Bug #1 root-cause — CONFIRMED: lives in `sensing_channel.c`, NOT the base rfsim TX/RX path

**Test 3 (`[sensing_channel] enable=0`, everything else = `ue.passive.q.100mhz.conf` unchanged,
against the SAME `gnb.sa.rfsim.100mhz.rank4.conf`, load 2.98-13 at launch, clean — no build overlap):
PBCH SYNCS PERFECTLY.** `SENSING: RFCENSUS slots=2000 ssb_slots=50 pbch_ok=50 pbch_fail=0` repeating
every ~100 frames for the full run. This is the load-bearing result: **Bug #1 is not a base-rfsim or
gNB-TX nb_tx=4 defect at all — it is specific to the ISAC synthetic sensing channel injection.**
Scratch conf `sens6:/home/sens/NICOLA/rfsim_validation/gap_rank4_noSC.conf`. Logs:
`sens6:/home/sens/NICOLA/rfsim_validation/gap_rank4_test3/{gnb,rx}/`.

**Test 1 REDONE cleanly (load 3.56, no cc1/make/ninja running, confirmed twice): frac_delay_taps=0
alone (sensing still `enable=1`) does NOT fix it** — 100% `pbch not decoded on any branch` again, same
signature as Bug #1. The earlier Test-1 result (flagged VOID for build overlap) reproduces identically
under a clean run, so it stands: **frac_delay_taps is NOT the (sole) cause.** Logs:
`sens6:/home/sens/NICOLA/rfsim_validation/gap_rank4_test4_frac0_clean/{gnb,rx}/`.

**Narrowed conclusion so far**: since `enable=0` (no LOS/object taps injected at all, stock rfsim
channel underneath) syncs perfectly, and `enable=1` with the base scene's own LOS-only physics
(frac_delay_taps aside) fails, the defect is in how `sensing_channel.c` synthesizes/writes the LOS tap
(or the surrounding array/objects config) specifically at `nb_tx=4`, `nb_rx=4`. Remaining untested
candidates, in order of suspicion: (a) `rx_array`/`objects` geometry (q.conf uses lambda/2 rx spacing
+ a 2-waypoint single object; pin49r4 uses 4-lambda rx spacing + a tx_array + a 3-object <=10-sample-
excess-delay scene) — next test is the unmodified `ue.passive.pin49r4.100mhz.conf` against this same
gNB, to check whether the *known-working* 2026-09-16 scene syncs at all on the CURRENT tree (it should,
per the `rank4-ota-converged`/`rfsim-channel-nb-tx-from-own-tx-count` memories); (b) if pin49r4 also
fails now, the regression is somewhere else entirely (post-09-16 commit) and needs a source diff of
`sensing_channel.c`/`apply_channelmod.c` against that date, not further conf tuning.

**Paused per coordinator instruction**: lane `perf` has priority use of the sens6 phy-test bed
(Technique D convergence capture queued). Killed my gNB+RX cleanly (`pgrep -x nr-softmodem`/
`nr-uesoftmodem` both empty) before yielding the bed. Resuming captures only after
`/home/sens/NICOLA/rfsim_validation/perf/fix2_r1.verdict` exists on sens6 (polling in background).
Continuing offline (conf diffs, code reading) in the meantime.

## Bug #1 — RESOLVED: harness/conf issue, not a code regression

**Confirmed live: `ue.passive.pin49r4.100mhz.conf` (unmodified, 2026-09-16's own working conf) syncs
cleanly on the CURRENT tree against `gnb.sa.rfsim.100mhz.rank4.conf`** — PBCH decodes, blind PDCCH
CORESET/RNTI confirmation fires within the first ~10s (`DCIQUAL rnti=0x1234 ... pass`,
`blind PDCCH rnti_seen ... rnti=0x1234 ... fmt=1_1 ... al=2`). This rules out a post-09-16 source
regression (the earlier bisect already showed the same for the *broken* q.100mhz.conf pairing at the
base commit; this confirms the *working* conf pairing still works too) — **Bug #1 is fully explained
as `ue.passive.q.100mhz.conf`'s scene/array geometry being incompatible with `nb_tx=4`, not a defect
in `sensing_channel.c`, `apply_channelmod.c`, or the receiver.** Root cause narrows to one of
`rx_array` spacing (lambda/2 vs 4-lambda), the explicit `tx_array` (vs none), or the `objects` scene
(1 object/2 waypoints vs 3 objects tuned for <=10-sample excess delay) in `q.100mhz.conf` — not
further bisected field-by-field since `pin49r4.100mhz.conf` is the pre-existing validated fix and
re-deriving the exact minimal diff has no incremental value for this lane's goal (a working rank-4
bed). **Fix: use `ue.passive.pin49r4.100mhz.conf` (already in-tree, unmodified) as the rank-4 100MHz
receiver conf**, not `ue.passive.q.100mhz.conf`. No code changes needed for Bug #1.

## Rank-4 blind-decode validation — 3x150s arms, ALL PASS

Ran `run_rank4_arms.sh` (waits for the shared bed to be free before each arm, kills only its own PIDs
by exact binary-path match — never a bare `pkill -x`/`-f` — so it never touched lane `perf`'s
concurrent phy-test captures). gNB: `gnb.sa.rfsim.100mhz.rank4.conf` (`-m 25 -n 1 -M 273 -l 4 -D 0xff`,
MCS 25/256QAM/4 layers). RX: unmodified `ue.passive.pin49r4.100mhz.conf`
(`--passive-rx --ue-nb-ant-rx 4 -r 273 -C 3750000000 --ssb 1478`), pinned BWP/TDA/DCI-bits knobs (cell
geometry) with Technique D's own MCS-table/rank/LBRM hypothesis tracking left active. Logs:
`sens6:/home/sens/NICOLA/rfsim_validation/gap_rank4_arms/arm{1,2,3}/{gnb,rx}/`.

| Arm | PDSCH CRC (crc_ok/decoded) | Rank identified (DCI DM-RS ports) | LBRM n_L | asserts/segfaults |
|---|---|---|---|---|
| 1 | 3266/3266 (100.0%) | modal_layers=4, hist[1=0 2=0 3=0 **4=200**] | 1277992 bits = **n_L=4** (matches gNB truth) | 0/0 |
| 2 | 3270/3270 (100.0%) | modal_layers=4, hist[1=0 2=0 3=0 **4=200**] | 1277992 bits = **n_L=4** | 0/0 |
| 3 | 3280/3280 (100.0%) | modal_layers=4, hist[1=0 2=0 3=0 **4=200**] | 1277992 bits = **n_L=4** | 0/0 |

All three arms: `LLRFILL Nl=4 Qm=8 (256QAM) G=1153152 have=1153151/1153152` (near-complete LLR fill,
no int8-clip pathology — the `rank4-mmse-llr-int8-saturation` fix is confirmed live-active), and
`RANK IDENTIFIED from DCI DM-RS ports` converges to the correct 4-layer hypothesis on every one of the
200 sampled grants per arm (0 ambiguous/wrong). Antenna-ports DCI-1_1 field decode (bwp_ind=0-bit,
ant_ports width 4, per `pdcch_blind_monitor_dci_bits="1:-1:..."` / the pinned `dci_bits` conf) has
existing OFFLINE coverage in `nr_pdcch_dci11_layout_sweep_test.cc` (antenna-ports field widths
4/5/6 swept) — not re-run here since nothing in this lane changed that code path; no new test needed
(pure conf fix, TDD method note: "add a new [test] only if needed").

## Conclusion

- **Bug #1 root cause: `ue.passive.q.100mhz.conf`'s scene geometry (narrow lambda/2 rx_array, no
  tx_array, a single long-excess-delay object) is incompatible with `nb_tx=4`/`nb_rx=4` in
  `sensing_channel.c`'s synthetic channel — NOT a base-rfsim, RU-TX, or receiver code defect.**
  Proven by elimination: (1) disabling the sensing channel entirely (`enable=0`) fixes PBCH sync with
  every other parameter unchanged -> defect is inside `sensing_channel.c`, not the base RF path;
  (2) `frac_delay_taps 8->0` alone does NOT fix it (tested twice, once voided for build-overlap, once
  clean) -> not a compute-budget/real-time issue; (3) the unmodified, pre-existing
  `ue.passive.pin49r4.100mhz.conf` (2026-09-16's wide-aperture, tx_array-equipped, short-excess-delay
  scene) syncs immediately on the CURRENT tree -> confirms no source-level regression since 09-16 and
  gives a ready-made fix.
- **Fix applied: use `ue.passive.pin49r4.100mhz.conf` as the rank-4 100MHz receiver conf** (already
  in-tree, unmodified — a harness/conf fix, zero code changes, per the brief's own branch for this
  case).
- **Known rank-4 issues (LBRM n_L mismatch, MMSE LLR int8 saturation) are CONFIRMED FIXED and ACTIVE
  in this tree** (content-verified in `nr_pdsch_passive_decode.c`, different commit hashes from the
  rebase but present; live-confirmed via `lbrm=1277992.0` = true n_L=4 with no CRC degradation, and
  near-100% LLR fill with no clipping pathology across all 3 arms).
- **Rank-4 (4 TX, 4 RX) phy-test rfsim bed is now fully working and validated**: PBCH sync, CORESET/
  RNTI blind confirmation, DCI 1_1 antenna-ports/rank identification (200/200 correct per arm), and
  PDSCH CRC (100% across 3266+3270+3280 = 9816 decoded TBs, 3x150s) all pass with zero code changes.

## Status: DONE

No code changes were needed (root cause was harness/conf, confirmed by elimination against the base
RF path and the frac-tap hypothesis, and resolved by adopting the pre-existing correct conf). No new
gtest needed (no code touched; existing `nr_pdcch_dci11_layout_sweep_test.cc` already covers the
antenna-ports DCI field this validation exercised). Nothing committed to the `sdd/gap-rank4` branch —
there is no code diff to commit for a conf-only harness fix (the fix is "use the other pre-existing
conf file", not an edit to a tracked file in this lane's worktree; `ue.passive.q.100mhz.conf` and
`ue.passive.pin49r4.100mhz.conf` are both already tracked, pre-existing files).

Open concern: the exact minimal geometric cause within {rx_array spacing, tx_array presence,
objects/excess-delay} was not isolated field-by-field (stopped once the working conf was confirmed,
per the lazy/root-cause-not-symptom guidance — re-deriving the precise minimal diff has no
incremental value once a correct, pre-existing, already-designed-for-this-exact-purpose conf is
available). If a FUTURE rank-4 100MHz conf needs to be authored from scratch (not just reused), revisit
`sensing_channel.c`'s array/objects interaction at nb_tx=4 more rigorously.
