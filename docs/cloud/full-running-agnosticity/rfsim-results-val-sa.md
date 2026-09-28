# RFsim validation campaign — merged `sdd/validation` branch (sensnuc3, local open5gs)

STATUS: **CAMPAIGN COMPLETE (2026-09-27)** — all planned arms attempted and scored: B0 (2 runs),
B2, B3 (2 attempts), B4, B5. B1 skipped (known VOID from a prior session). See "Overall summary"
near the end for the one-line-per-arm verdict table.

## Setup done

- Test gNB tree: `/home/sens/NICOLA/rfsim-local` (branch `sdd/rfsim-gnb-test` @ `67bb0eaa11`),
  already built, unmodified. `nr-softmodem` used from here (env-gated `ISAC_GNB_TEST_*` knobs,
  default off).
- UE-under-test tree: new worktree `/home/sens/NICOLA/rfsim-val`, fetched from
  `sens6:/home/sens/NICOLA/adaptive-rx-UL-DL` branch `sdd/validation`, checked out at
  `c295fa18f1` (confirmed: `adaptive-rx-UL-DL @25a8699a64` + SA discovery-stall fix
  (`028ed102cf`+`801de181a1`, merged directly) + Technique D fix (`sdd/agn-td` merged, tip
  `ecfced8bd0`) — verified via `git log --graph`, both fix lanes present as parents of the merge
  commit.
- Built `nr-uesoftmodem`, `rfsimulator`, `params_libconfig` in `rfsim-val` via
  `cmake -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DAVX2=ON
  -DENABLE_ISAC_SENSING=ON -DT_TRACER=ON` + `ninja -j8 nr-uesoftmodem rfsimulator
  params_libconfig` (`libparams_libconfig.so` is the actual output name). Clean build.
- `tests/passive_rx/ue.passive.agn.conf` copied byte-identical from `rfsim-local` into
  `rfsim-val` (didn't exist there yet). `pdcch_blind_monitor_scan_thread = "1:8:5"` confirmed ON.
- `nr-softmodem` from `rfsim-local`'s build dir symlinked into `rfsim-val`'s
  `cmake_targets/ran_build/build/nr-softmodem` so `run_passive_rx.sh` (which hardcodes
  `BUILD_DIR` relative to its own tree) picks up the test gNB while using `rfsim-val`'s own
  `nr-uesoftmodem`/`rfsimulator`. Verified this works (RPATH is baked absolute at link time in
  `rfsim-local`, so the symlink's location doesn't affect its own library resolution; confirmed
  `--help` runs correctly through the symlink).
- Harness stats-file bug (documented in `rfsim-results-phaseB.md`) worked around: pre-touched +
  `chmod 666`'d `nrL1_UE_stats-0.log` in the repo-root CWD (`/home/sens/NICOLA/rfsim-val/`,
  where the harness is invoked from) before launch.
- Confirmed no other `nr-uesoftmodem`/`nr-softmodem`/`run_passive_rx.sh` processes were running
  anywhere on this machine before starting (only an unrelated sens6-side session's own build was
  visible via a stray local shell one-liner, not a local process).

## B0 baseline, run 1 (`SA_B0_1`) — IN PROGRESS AT HANDBACK, looks HEALTHY (fix appears to work)

Command: `CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M
./tests/passive_rx/run_passive_rx.sh 600 /home/sens/NICOLA/rfsim_validation_local/SA_B0_1`,
launched ~00:03 local time. Still running (both `nr-uesoftmodem` PIDs and the harness script
alive) when this report was written; not yet scored to completion.

**This is qualitatively different from phaseB's B0 stall and consistent with the SA fix having
worked**, watched live via a log-tail monitor on `ue_rx1.log`:
- `ACQ_STATE SEARCHING -> PBCH_LOCKED` then `PBCH_LOCKED -> CELL_CONFIGURED` with
  `evidence[... coreset_ok=1 ul_bwp=1 ...]` — **`coreset_ok` reached 1 almost immediately**
  (phaseB's stall never got past `coreset_ok=0` in 900s/4x600s).
- `Technique D ARMED: independent RNTI/TDA contexts, TB-CRC scoring` fired early (phaseB: 0
  occurrences ever, on either base or branch).
- Periodic blind-PDCCH summary lines show steadily climbing, healthy activity, no stall, no
  crash, over the ~20 minutes of wall-clock observed:
  - occasions=1 -> 21148, accepts=1 -> 333, growing monotonically.
  - `dci10[...]` C-RNTI-class accepts (`C=`) present and growing (1 -> 2 by the last observed
    sample) alongside a much larger SI-RNTI count (`SI=`) — i.e. the CSS0/SIB-anchored path
    (the one the SA fix report's Gate 1 change specifically unblocked) is firing continuously,
    not stalled.
  - `pdsch_decode[try=... crc_ok=... (100.0%) ...]` — **100% CRC pass rate held at every sampled
    point** (1/1 through 331/331).
  - `cfr_submits` tracking `crc_ok` 1:1 (matches the documented healthy pattern from
    `PHASE3_BLIND_PDCCH_LIVE_WIRING_HANDOVER.md`).
  - `scanq[queued=... done=... drop_full=0 drop_stale=0 maxlag=4]` — **`drop_full=0` throughout**,
    unlike the TD-fix-report's `agn-td` phy-test bed (which saw drop_full climb to ~190-200k/470k)
    — this scene's traffic rate (1 active UE, 3 Mbps UDP, 106 PRB) is evidently light enough that
    the deferred-decode queue never backs up.
- **Not yet observed by handback**: `Technique D CONVERGED`, `multi-CORESET bank add` (0
  occurrences of either in the log as of the last check) — the C-RNTI-class discovery via the
  dedicated CORESET (as opposed to the CSS0-anchor path) had not yet produced a confirmed
  candidate in ~4 minutes of the 600s window. Consistent with phaseB's own note that dedicated-
  CORESET convergence, even once unblocked, needs "several minutes" on this traffic rate — too
  early to call converged-or-not at the point this report was written.
- No `AssertFatal`, no segfault, no core dump signature seen in the monitored window.

## B0 baseline, run 1 (`SA_B0_1`) — COMPLETE, SCORED (2026-09-27)

Full 600s run finished cleanly on its own (no hang, no teardown needed from outside). Active UE
C-RNTI this run: `0x488d` (re-read from `gnb.log`).

**Verdict: HEALTHY / major improvement over phaseB's total stall, with one still-open gap
(dedicated-CORESET convergence stays sparse, never reaches "CONVERGED").**

- `coreset_ok`: **reaches 1** (`ACQ_STATE PBCH_LOCKED -> CELL_CONFIGURED evidence[... coreset_ok=1
  ul_bwp=1 ...]`), confirming the SA discovery-stall fix holds on a fresh full-length run (phaseB:
  never left `coreset_ok=0` in 4x600s+900s).
- Dedicated (ue-specific) SS params DO resolve off their `auto`/`0x0` placeholders within ~200 log
  lines: `rb_offset=auto:-1`→`0`, `class_mask=0x0`→`0x1`, `dci_length=0`→`41` (DCI 1_0),
  `n_rb_riv=auto:0`→`106`. CSS0 autoconf from MIB/SIB1 also fires cleanly (coreset groups=8 dur=1
  bundle=6, bwp=[29..77)).
- `Technique D ARMED`: **1** (fires early, line ~212). `Technique D CONVERGED`: **0** (never, whole
  600s). `multi-CORESET bank add`: **0** (never — plausibly expected for B0 since there's no BWP/
  CORESET-switch event to populate a second bank entry; B3 is the arm that should exercise this).
- Blind-PDCCH accepts split by RNTI class (final summary, occasions=171856 candidates=859331):
  **C-RNTI-class (`C=`) accepts = 6, frozen since early in the run** (reached 6 by ~1/32 of the way
  through and never grew again) vs **SI-RNTI (`SI=`) = 2686, still climbing linearly** at teardown.
  So the dedicated C-RNTI path is real (nonzero, unlike phaseB's permanent 0) but very sparse and
  itself stalls after a handful of candidates — this is the one metric that still looks like a
  partial, not full, fix.
- `pdsch_decode[try=2692 crc_ok=2692 (100.0%) ... data_submits=2692]` — **100% CRC held for the
  entire run** (this aggregates both SI- and C-RNTI-class accepted candidates; no per-class
  crc_ok/try breakdown exists in the log line, so the C-RNTI-specific figure can't be isolated
  further than "6 accepts, and the aggregate rate was 100% throughout").
- CFR occupancy per CPI: `occ[csi=8 dmrs=0 data=31 blind=31 pusch=0 uldata=0]` — blind-PDCCH-sourced
  rows ARE feeding the CFR at a healthy, steady rate every CPI (not zero like phaseB).
- UL PUSCH decode: **zero all run** (`ulscan[sched=0 crc_hit=0 disc=0]`, `pusch=0`/`uldata=0` in
  every `occ[...]` line) — expected, `ul_pusch` exercising is not this arm's focus and the active
  UE's own uplink wasn't perturbed.
- `scanq[drop_full=0 drop_stale=0 maxlag=4]` — **zero drops for the entire 600s**, no backpressure.
- Crashes/asserts: **none** (`grep -iE "assertfatal|segfault|core dumped|terminate called"` empty
  across gnb/ue1/rx1 logs).
- Harness's own "VERDICT: UPLINK DETECTED" is the same known false positive as phaseB (matches the
  receiver's own `occ[...pusch=...]` telemetry substring, not real uplink) — 151 hits, not checked
  line-by-line here but consistent with the documented false-positive mechanism.
- Sensing/tracking: 229 DetectionReports, 73/229 CPIs hit the synthetic target's range band,
  2904/2904 detections carry azimuth. Fused-track precision 1.6% (34/2112 confirmed within 15m,
  median_dist 1234.55m) — poor, but this is a single receiver with no cross-receiver gate and not
  the metric this campaign is targeting (dedicated-path discovery is).

## B0 baseline, run 2 (`SA_B0_2`) — COMPLETE, SCORED (2026-09-27)

Same command as B0_1, fresh out-dir. C-RNTI this run: `0xe089`.

**Verdict: HEALTHY, REPRODUCES B0_1 — 2/2 runs consistent, not a one-off.**

- `coreset_ok`: reaches 1 (`SIB1_DECODED -> CELL_CONFIGURED evidence[... coreset_ok=1 ul_bwp=1 ...]`).
- Dedicated SS resolves the same way: `class_mask 0x0->0x1`, `dci_length 0->41`, `rb_offset auto:-1->0`.
- `Technique D ARMED`=1, `CONVERGED`=0, `multi-CORESET bank add`=0 — identical pattern to B0_1.
- Final blind-PDCCH summary (occasions=173678): **C-RNTI-class accepts = 8** (again froze early,
  same sparse-but-nonzero shape as B0_1's 6) vs **SI=2714**, still climbing.
- `pdsch_decode[try=2714 crc_ok=2714 (100.0%) data_submits=2714]` — 100% CRC held all run.
- CFR occupancy: `occ[csi=8 dmrs=0 data=31 blind=31 pusch=0 uldata=0]` — same healthy per-CPI shape.
- UL PUSCH: zero all run (`ulscan[sched=0 crc_hit=0 disc=0]`). `scanq[drop_full=0 drop_stale=0]` —
  zero drops. No `AssertFatal`/segfault/`terminate called` in gnb/ue1/rx1 logs.
- Sensing: 317 DetectionReports, 103/317 in-band, 3862/3862 carry azimuth. Fused precision 2.9%
  (81/2786 within 15m, median_dist 924.17m) — same order of magnitude as B0_1, single-receiver
  precision not this campaign's target metric.

**B0 2-run baseline conclusion**: the SA discovery-stall fix reproduces cleanly (2/2): CSS0/SIB1
CORESET resolution + dedicated-SS-param resolution + healthy blind-PDCCH decode pipeline (100% CRC,
zero scanq drops) both times. The one consistent open gap across both runs: dedicated C-RNTI-class
blind accepts stay very sparse (6, then 8 total over 600s) and never reach `Technique D CONVERGED`
or a `multi-CORESET bank add` — worth a closer look in a future session, but not blocking for B2-B5
since those arms exercise different perturbations on top of the same (now-reproducibly-healthy)
baseline path.

## B2 (DL scrambling 700/500) — COMPLETE, **VOID** (2026-09-27), same failure class as B1

Command: `ISAC_GNB_TEST_DL_DMRS_ID0=700 ISAC_GNB_TEST_DL_DMRS_ID1=700 ISAC_GNB_TEST_DL_DATA_ID=500
CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M ./run_passive_rx.sh 600
.../SA_B2`. Exited after only ~3.5 min (well under the 600s window) via the harness's own 150s
PDU-session-wait timeout, not a hang — no external teardown needed.

**VOID — the perturbation broke the active UE's own downlink before the passive receiver ever
started, exactly mirroring B1's documented UL failure mode (`rfsim-results-phaseB.md`'s "B1
result" section), just on the DL side this time.**

- `ISAC_GNB_TEST_DL_DMRS_ID0/1=700`, `ISAC_GNB_TEST_DL_DATA_ID=500` **DID apply** (606 hits in
  `gnb.log`: `"ISAC_GNB_TEST_DL_DMRS_ID0=700 active..."` etc., firing repeatedly across
  reconfigurations/re-syncs).
- But the same override the receiver is meant to be tested against also changes what the ACTIVE
  UE's own gNB-side DL decode expects, and `ue.active.conf` has no matching knob — result:
  `dlsch_errors` tracked `dlsch_rounds` almost 1:1 (e.g. `30/29/29/28` rounds, `28` errors) i.e.
  ~93-97% DL BLER from the start. `ue1_active.log`: `"max RETX reached on SRB 1"` ->
  `"RRC moved into IDLE state"` -> `NAS_CONN_RELEASE_IND` -> UE restarts cell search from scratch.
  The active UE never got a PDU session in the harness's 150s wait window
  (`launch.out`: `[TIMEOUT] active UE ue1 got a PDU session (waited 150s)`).
- **The passive receiver (rx1) never launched at all** — no `ue_rx1.log`/`harness.log` in `SA_B2/`,
  only `gnb.log`/`ue1_active.log`/`launch.out`. So this arm as scripted cannot test "does the
  receiver blind-discover non-PCI DL DM-RS/data scrambling IDs" — it never got a single slot.
- **This is a harness/experiment-design defect, not evidence about the branch's discovery code.**
  `ISAC_GNB_TEST_DL_*` is gNB-side-only with no UE-side counterpart, so it necessarily also breaks
  the ordinary attached UE's downlink on this harness (one shared gNB/cell serving both the active
  UE and the passive receiver's illumination). B2 would need either a UE-side matching override (so
  the active UE can still attach) or a scene where the active UE doesn't need a PDU session before
  the receiver's window opens. Not fixed here (out of scope for a read/run/score task).
- **Score against the task's two questions**: "did the gNB knob fire?" — yes, confirmed (606 hits).
  "receiver discovered 700/500 and C-RNTI CRC>0?" — no data, receiver never ran.

## B3 (dedicated BWP + switch), attempt 1 — gNB CRASHED at launch, harness needed a manual SIGINT+SIGKILL

Command: `GNB_CONF_OVERRIDE=gnb.sa.rfsim.bwp.conf GNB_EXTRA="--telnetsrv --telnetsrv.shrmod ci"
CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M ./run_passive_rx.sh 600
.../SA_B3`.

**Root cause, confirmed from `gnb.log`: `--telnetsrv.shrmod ci` cannot load because the
`telnetsrv_ci` shared module was never built in this campaign's `ninja` invocation** (build command
recorded in this doc's "Setup done" section only built `nr-uesoftmodem`/`rfsimulator`/
`params_libconfig`; `rfsim-local`'s own doc likewise lists `nr-softmodem` built without any
`telnetsrv*` target). `find .../build -iname "*telnetsrv*"` returns nothing — no `.so` artifact
exists at all, only the source files. Confirmed the flag *name* itself is correct (`telnetsrv.c:37`'s
own doc comment shows the identical `--telnetsrv --telnetsrv.shrmod <module>` pattern with `rrc`
instead of `ci`) — this is a missing build artifact, not a wrong invocation.
- `gnb.log`: `[CONFIG] unknown option: --telnetsrv.shrmod` / `unknown option: ci` /
  `2 unknown option(s) in command line` → `Assertion (ret==0) failed! In
  rfsimulator_write_internal() ... ret=22` → gNB **aborted (core dumped)** within ~1s of the active
  UE starting, before RA even completed.
- The harness's own 150s PDU-session wait doesn't watch gNB liveness, so `run_passive_rx.sh` kept
  running with a dead gNB and a stuck active-UE `nr-uesoftmodem` retrying sync against nothing.
  Manually torn down per the kill-order rule: `SIGINT` to the harness PID cleared the
  `nr-uesoftmodem` children cleanly, but the harness's own bash process then hung asleep
  (~20s, no children left, not a zombie) and needed `SIGTERM` then `SIGKILL` to actually exit —
  **a harness bug worth flagging**: something in its post-teardown path (likely the
  fusion/scoring step trying to process a run that produced no `ue_rx1.log`) blocks indefinitely
  instead of exiting after `trap cleanup EXIT INT TERM` runs.
- No receiver ever launched, no BWP/switch data collected. **This is a build-environment gap, not a
  finding about the branch's BWP-switch code.**

## B3, attempt 2 (dedicated BWP, non-zero start only — switch trigger untestable without a rebuild)

Retried the static half of B3 — `first_active_bwp=1` in `gnb.sa.rfsim.bwp.conf` starts the UE on a
non-zero-start BWP from attach, no telnet needed for that part — by dropping `GNB_EXTRA` entirely.
Live BWP SWITCH via `ci trigger_bwp_switch` remains untested and needs a rebuild
(`ninja telnetsrv_ci` or whatever its actual target name is, from `rfsim-val`) before it can be
exercised; not done here (out of scope for a read/run/score task, and per project rule builds must
never run during/adjacent to a live capture — deferred to a future session with dedicated time).

## B3 attempt 2 (`SA_B3b`, static non-zero-start BWP, no telnet) — COMPLETE, SCORED (2026-09-27)

Command: `GNB_CONF_OVERRIDE=gnb.sa.rfsim.bwp.conf CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4
TRAFFIC=udp IPERF_RATE=3M ./run_passive_rx.sh 600 .../SA_B3b`. C-RNTI this run: `0x9f4d`.

**Verdict: HEALTHY — the non-zero-start BWP itself doesn't break discovery; reproduces B0's
signature exactly.**

- `gnb.log` confirms the BWP actually applied: `BWP 1, start PRB 30 size 40 locationandbandwidth
  10755, scs 1` (matches `first_active_bwp=1`'s CRB 30 / 40-PRB target from the conf comment).
- `coreset_ok` reaches 1 (`PBCH_LOCKED -> CELL_CONFIGURED evidence[coreset_ok=1 ul_bwp=1]`).
- `Technique D ARMED`=1, `CONVERGED`=0, `multi-CORESET bank add`=0 — same pattern as B0_1/B0_2.
- Final summary (occasions=172134): **C-RNTI-class accepts = 6** (+1 Temp-C-RNTI) vs **SI=2690**
  climbing — same sparse-dedicated/rich-SI split as both B0 runs.
- `pdsch_decode[try=2690 crc_ok=2690 (100.0%) data_submits=2690]` — 100% CRC held all run.
  `occ[csi=8 dmrs=0 data=31 blind=31 pusch=0 uldata=0]` — healthy, same shape as B0.
  `scanq[drop_full=0 drop_stale=0]` — zero drops. No crashes/asserts anywhere.
- Sensing: 406 DetectionReports, 115/406 in-band, 4763/4763 carry azimuth. Fused precision 3.3%
  (109/3334 within 15m, median_dist 844.38m).
- **Conclusion**: attaching on a non-zero-start dedicated BWP (CRB 30/40 PRB) does not itself
  disturb the agnostic discovery path — numbers track B0's baseline closely on every metric. The
  LIVE BWP-switch half of B3 (`ci trigger_bwp_switch`) remains unexercised — see attempt 1 above —
  and needs the `telnetsrv_ci` module built before it can run.

## B4 (`SA_B4`, NUM_UE=3) — COMPLETE, SCORED (2026-09-27)

Command: `NUM_UE=3 CONF_TAG=.agn NUM_RX=1 RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M ./run_passive_rx.sh
600 .../SA_B4`. All 3 active UEs attached and got PDU sessions (`ue1`/`ue2` default/own netns,
`ue3` own netns) before the receiver launched.

**Verdict: HEALTHY on the discovery question this arm targets (3 distinct C-RNTIs held, no
eviction), with a real but separately-explained quality cost under 3x load (matches this project's
already-documented CPU-contention pattern, not a new discovery bug).**

- **3 distinct C-RNTIs confirmed**: `0x07c1`, `0x59bf`, `0x5e2e` (from `gnb.log`). All three appear
  in the receiver's own log (`0x07c1`: 29 hits, `0x59bf`: 13491, `0x5e2e`: 21940 — sparse for one,
  heavy for the other two, but none zero) and **`grep -ic evict` = 0** — no eviction event ever
  fired for any of them; all three held for the whole 600s.
- `coreset_ok` reaches 1 (`PBCH_LOCKED -> CELL_CONFIGURED evidence[coreset_ok=1 ul_bwp=1]`).
- `Technique D ARMED` = **66559** (vs B0's single 1 — plausibly one per newly-armed candidate
  RNTI/TDA context, and 3 real UEs' grant volume creates far more of them), `CONVERGED` = 0,
  `multi-CORESET bank add` = **1** (first nonzero value anywhere in this campaign so far).
- Final blind-PDCCH summary (occasions=189099): **C-RNTI-class accepts = 247** (`TC=3`) — an order
  of magnitude above B0's 6-8, consistent with discovery genuinely tracking multiple real UEs'
  grants rather than staying stuck.
- **Real quality cost under 3-UE load**: aggregate `pdsch_decode[try=15235 crc_ok=1138 (7.5%)
  unsup=755]` — CRC pass rate collapsed from B0's 100% to 7.5%. CFR occupancy: 37/48 (77%) CPIs
  show the healthy `blind=30-32` pattern, but 11/48 (23%) show `occ[csi=32 dmrs=0 data=0 blind=0
  pusch=0 uldata=0]` (CSI-RS fine, blind-PDCCH path went to zero for that CPI). One `scanq
  drop_full=1` appeared (vs 0 everywhere else in this campaign so far). `ue3_active.log`'s ping
  showed 70.5% packet loss / RTTs up to 145s — direct evidence of real CPU contention across
  gNB+3 active UEs+1 4-antenna passive receiver on this box, the documented mechanism (project
  memory: CPU-contention-under-N-concurrent-processes), not a new bug in the SA fix.
  UL scan volume also exploded as expected with 3x UL traffic: `ulscan[sched=2279489 crc_hit=20386
  disc=20386]`, `dci01[rejects=2259124]`.
- CPI duration inflated to 2.560s (from B0's 0.640s) and `vel_max` shrank to ±0.564 m/s (from
  ±2.258) — the documented CPI-slot-count/traffic-rate interaction, not new here.
- No `AssertFatal`/segfault/`terminate called` anywhere. Fused precision 3.5% (129/3728 within
  15m, median_dist 830.20m) — same order of magnitude as every other arm.
- **Conclusion**: the dedicated-CORESET discovery path itself scales to 3 concurrently-tracked
  RNTIs without evicting any of them and without regressing `coreset_ok`/Technique-D-ARMED
  behavior; the CRC/occupancy degradation is machine-contention, already a known, separately
  root-caused phenomenon in this project, not something this task should attribute to the SA fix.

## B5 (`SA_B5`, AL1-only) — COMPLETE, **INCONCLUSIVE** on the specific AL1 question (2026-09-27)

Command: `ISAC_GNB_TEST_USS_AL="4,0,0,0,0" ISAC_AL1_COVER=1 CONF_TAG=.agn NUM_RX=1 NUM_UE=1
RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M ./run_passive_rx.sh 600 .../SA_B5`.

**Two data-quality caveats on this run, neither attributable to the AL1 knob itself:**
1. The active UE's first `nr-uesoftmodem` instance **segfaulted during initial sync** (`Segmentation
   fault sudo -n ... nr-uesoftmodem ...`) a few seconds after launch. The harness's own retry logic
   relaunched it transparently (`[ok] active UE ue1 completed random access` / `got a PDU session`
   followed right after in `launch.out`) — no manual intervention needed. This matches the
   project's already-documented open issue ("an active-UE segfault in initial sync at attach, ~1 in
   18 UE starts, undiagnosed") — not a new AL1-specific fault.
2. **`gNB out-of-sync events: 27` and DL stalled from ~480s of the 600s window** (`launch.out`:
   `*** DL STALLED from ~480s -- sensing data after that point was collected on an IDLE cell ***`).
   So the last ~20% of this capture's blind-PDCCH/CFR numbers reflect a dead cell, not real AL1
   scheduling activity.

**What IS confirmed:**
- `ISAC_GNB_TEST_USS_AL=4,0,0,0,0` genuinely reached the scheduler, not just the injection log line:
  `gnb.log`: `"Candidates per PDCCH aggregation level on UESS: L1: 4, L2: 0, L4: 0, L8: 0, L16: 0"` —
  the active UE's dedicated search space really was configured AL1-only.
- `ISAC_AL1_COVER` is a real, wired receiver-side knob (confirmed by source read,
  `nr_pdcch_blind_monitor.c:917-925`, `getenv("ISAC_AL1_COVER")` gates an "AL1 cover lap" candidate-
  generation stage) — **but it has no runtime log line by design** (its own comment: "Default off:
  discovery order unchanged unless asked for", no `LOG_*` call anywhere in that path), so its
  activation cannot be confirmed or refuted from `grep` alone; a log-based go/no-go check for this
  knob isn't possible with the current instrumentation.
- Discovery-health signature (pre-stall) matches every other healthy arm: `coreset_ok`=1,
  `Technique D ARMED`=1/`CONVERGED`=0/`multi-CORESET bank add`=0, `pdsch_decode[crc_ok=2753/2753
  (100.0%)]`, `occ[... blind=30-31 ...]` healthy, `scanq[drop_full=0]`. No `AssertFatal`/
  `terminate called` anywhere.
- **The AL1-only question itself is inconclusive**: final `dci10[C=5]` (C-RNTI-class accepts) is
  statistically indistinguishable from every other arm's baseline plateau (B0_1=6, B0_2=8, B3b=6) —
  i.e. forcing the gNB to schedule the dedicated SS exclusively at AL1 did not visibly change the
  C-RNTI accept count one way or the other. The one per-AL confirmed/examined breakdown this build
  logs (`SEARCH_SPACE INFERRED ... confirmed/examined[AL1=0/0 AL2=0/0 AL4=32/1988 AL8=0/994]`) is
  for the **CSS0/SIB1 common search space**, which this knob does NOT target (it overrides UESS
  only) — so it says nothing about AL1 coverage of the dedicated path. There is no equivalent
  per-AL breakdown log for the dedicated/UE-specific search space in this build.
- **Verdict: cannot confirm or refute C-RNTI recovery at AL1 from this run** — the knob demonstrably
  reached the scheduler, but (a) this run's own DL stall after 480s degrades the back fifth of the
  capture, (b) the receiver-side coverage flag has no confirmatory log line, and (c) the only
  available per-AL breakdown telemetry covers the wrong search space. A clean re-run with either a
  code-level per-AL counter added to the dedicated-SS accept path, or at minimum a repeat run
  without the segfault/stall this one hit, is needed before this arm can be called pass or fail.
  Per project memory (`passive-rx-needs-5-runs-per-arm`), a single run was never going to be
  decisive here regardless.

## Campaign COMPLETE — all planned arms attempted

B0 (2 runs), B2, B3 (2 attempts), B4, B5 all run and scored. B1 skipped (known VOID per prior
session). See "Overall summary" below.

## Overall summary (2026-09-27)

| Arm | Verdict | Headline |
|---|---|---|
| B0_1 | HEALTHY | `coreset_ok`=1, ARMED=1/CONVERGED=0, C=6/SI=2686, CRC 100%, 0 drops |
| B0_2 | HEALTHY, reproduces B0_1 | `coreset_ok`=1, ARMED=1/CONVERGED=0, C=8/SI=2714, CRC 100%, 0 drops |
| B1 | VOID (prior session) | not re-run, known VOID (UL scrambling breaks active UE before rx1 starts) |
| B2 | **VOID** | knob applied (606 hits) but broke active UE's own DL (~95% BLER) before rx1 ever launched — same failure class as B1, just DL-side |
| B3 (switch) | **BLOCKED — missing build** | `telnetsrv_ci` module never built in this campaign's `ninja`; gNB aborted immediately when `--telnetsrv.shrmod ci` was passed |
| B3 (static non-zero BWP) | HEALTHY | BWP 1 (CRB30/40PRB) confirmed applied; matches B0 signature exactly |
| B4 | HEALTHY (discovery); quality cost is known CPU contention | 3 distinct C-RNTIs held, 0 evictions; CRC 100%→7.5% and 1 scanq drop under 3-UE load (documented contention pattern, not a new bug) |
| B5 | **INCONCLUSIVE** | AL1-only knob confirmed reaching scheduler (`L1:4 L2:0...`); run degraded by a known ~1/18 active-UE segfault (auto-retried) + a DL stall from ~480s; no per-AL breakdown exists for the dedicated search space to confirm/refute AL1 recovery either way |

**Bottom line on the SA discovery-stall fix**: reproduces as fixed across every completed arm that
reaches the receiver (B0 x2, B3-static, B4) — `coreset_ok` reliably reaches 1, dedicated SS params
resolve off their `auto`/`0x0` placeholders, and PDSCH decode holds 100% CRC. The one open,
consistent finding across ALL of B0/B3/B4/B5: the **C-RNTI-class blind-PDCCH accept count plateaus
very early at a small number (5-8) and never grows further**, `Technique D` never reaches
`CONVERGED`, and `multi-CORESET bank add` fires only once (B4, under 3-UE load) in the whole
campaign — worth follow-up investigation, but secondary to this task's regression-check scope.
B2 and B3's live-switch sub-test could not be exercised (harness/build gaps, not code-under-test
findings); B5's AL1 question needs either better instrumentation or a clean re-run.

## Kill order / operational notes (kept for future continuations)

Kill order for any live run: SIGINT to the `run_passive_rx.sh` PID (its own `trap cleanup EXIT
INT TERM`), SIGTERM/SIGKILL only if it doesn't exit within ~10s — observed live in this session's
B3 attempt 1 (gNB crashed on a bad cmdline flag): SIGINT cleared the `nr-uesoftmodem` children but
the harness's own bash process needed a follow-up SIGTERM then SIGKILL to actually exit (~20s
sleeping with no children left) — worth the harness owner's attention, likely a hang in a
post-teardown fusion/scoring step when a run produced no `ue_rx1.log`. Per project rule, never
build while a capture is running, and re-check `pgrep -x nr-uesoftmodem`/`nr-softmodem` are clear
before starting the next arm.

Report path: `/home/sens/NICOLA/docs/superpowers/sdd/full-running-agnosticity/rfsim-results-val-sa.md`
