# RFsim validation campaign Phase B — results (sensnuc3, local open5gs)

Tree: `/home/sens/NICOLA/rfsim-local` (worktree, branch `sdd/rfsim-gnb-test` =
`adaptive-rx-UL-DL @25a8699a64` + test-only `67bb0eaa11`). Machine: sensnuc3 (local, no ssh).

Status: **IN PROGRESS**.

## Environment

- Local open5gs core was already running as 5 orphaned daemons (amfd/smfd/upfd/udmd/nssfd, PPID=1,
  reparented ~2026-09-24, not tracked by systemd) — AMF NGAP already listening on
  `127.0.0.1:38412` before this session touched anything. `systemctl start` attempts on the
  already-dead-per-systemd units spawned duplicates that failed with "Address already in use"
  (the orphans hold the ports) and auto-restart-loop; stopped those via `systemctl stop` (denied
  for `amfd` by the sandbox's shared-resource classifier, left as a harmless failing auto-restart
  loop — the working orphan is untouched and still serves NGAP).
- mongodb subscribers `001060123456743`/`44`/`45` (PLMN 001/06, matches `/etc/open5gs/amf.yaml`)
  already provisioned — matches `ue.active{,2,3}.conf`. No provisioning needed.
- Build: `cmake -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DAVX2=ON
  -DENABLE_ISAC_SENSING=ON -DT_TRACER=ON` in `cmake_targets/ran_build/build`, `ninja -j8
  nr-softmodem nr-uesoftmodem rfsimulator params_libconfig`. `-DOAI_RF_EMULATOR`/`-DOAI_SIMU` are
  not real CMake options in this tree (grepped `CMakeLists.txt`, absent) — omitted; rfsim comes
  from building the `rfsimulator` dlopen plugin target explicitly (stale-plugin trap). No CUDA on
  this host (`ENABLE_LDPC_CUDA` skipped); UHD dev present but `OAI_USRP` skipped as unneeded for an
  rfsim-only, no-hardware campaign. ccache auto-enabled by the tree's own CMakeLists.
- New receiver conf `tests/passive_rx/ue.passive.agn.conf`: same geometry/CSI-RS/channel as
  `ue.passive.conf`, but with the manually pinned CORESET/SS/BWP/TDA/DM-RS-mask/dci_length_override
  hints removed and `pdcch_blind_monitor_autoconf/autodiscover/full_auto=1` + `dci01`/`ul_pusch`
  turned on instead — one conf for the whole campaign, since the pinned hints in the shipped conf
  are derived for ONE specific gNB config and would silently break under B1-B5's perturbations
  (that's the entire point of the agnostic discovery path being validated). `NUM_RX=1` throughout
  (none of B0-B5 needs a 2nd receiver).

## Build status

SUCCESS. `ninja -j8 nr-softmodem nr-uesoftmodem rfsimulator params_libconfig` completed clean
(11301/11301), all 4 target binaries/plugins present and dated after the configure.

## Harness bug found and worked around (not a code-under-test bug, not fixed in-tree)

`tests/passive_rx/run_passive_rx.sh`'s stats-file chmod/cleanup (`sudo -n chmod 666
"$SCRIPT_DIR"/nr*_stats*.log`, `sudo -n rm -f "$SCRIPT_DIR"/nr*_stats*.log`) targets
`$SCRIPT_DIR` (`tests/passive_rx/`), but `nr-uesoftmodem` actually creates
`nrL1_UE_stats-0.log` etc. in the process's own CWD — which is the repo root when the harness is
invoked exactly as its own README documents (`cd $R && ./tests/passive_rx/run_passive_rx.sh`).
Directory mismatch -> the chmod is a silent no-op every time -> the active UE's root-owned
`nrL1_UE_stats-0.log` (mode 644) blocks the unprivileged passive receiver's `fopen(...,"w")` in
`nrL1_UE_stats_thread()` (`executables/nr-ue.c:147`) -> `AssertFatal` abort, every single run,
100% reproducible (not the rare race the script's own comment describes). Worked around
(no script/code edit) by pre-touching + `chmod 666`-ing the 4 `nr*_stats*.log` files in the actual
CWD before every launch. Flagging for whoever owns this harness: the chmod line's `$SCRIPT_DIR`
should be `$PWD` (or the script should itself `cd` before launching children).

## New receiver conf: `tests/passive_rx/ue.passive.agn.conf`

One conf reused for all 6 arms (config only, no code touched). Same geometry/CSI-RS/channel as
`ue.passive.conf`, but with the manually pinned CORESET/SS/BWP/TDA/DM-RS-mask/dci_length_override
hints removed in favor of `pdcch_blind_monitor_autoconf/autodiscover/full_auto=1` (+ `dci01`/
`ul_pusch` always on). Rationale: `ue.passive.conf`'s pinned hints are derived for ONE specific
gNB config and would silently break under B1/B2/B3/B5's perturbations — using the agnostic
discovery path is the only way one conf validates all arms, and is also the actually-agnostic
thing this campaign is meant to exercise (`ue.passive.auto.100mhz.conf` is the in-repo precedent
for this pattern, at 100 MHz; this is its 106 PRB / real-subscriber-geometry counterpart).

## Arms

| Arm | Verdict | Notes |
|---|---|---|
| B0 baseline | COMPLETE, 900s -- STUCK (not converging) | see §B0-full below: dedicated-CORESET discovery never confirmed a candidate in 900s; every downstream stage silent all run |
| B0 regression A/B | COMPLETE, 4x600s -- NOT A REGRESSION | base and branch stall identically, 4/4 runs; see "B0 regression A/B" section below |
| B1 UL scrambling ≠ PCI | COMPLETE -- VOID | gNB-only override broke the active UE's own uplink before the passive receiver ever launched; see "B1 result" section below |
| B2 DL scrambling ≠ PCI | not started | |
| B3 dedicated BWP non-zero start | not started | |
| B4 multi-UE (NUM_UE=3) | not started | |
| B5 AL1-only | not started | |

### B0 detail (as of handback, run still live)

Command: `CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M
./tests/passive_rx/run_passive_rx.sh 300 /home/sens/NICOLA/rfsim_validation_local/B0`
(2 prior attempts hit the stats-file bug above and were VOID; 3rd attempt, post-workaround, is
the one running).

- Active UE C-RNTI **0x21e9** (re-read from this run's own `gnb.log`, not carried from any
  earlier session per the project's RNTI-recheck rule).
- `[ok]` gNB associated with AMF, rfsim server up, active UE RA + PDU session, passive UE synced
  and decoded SIB1 — all fast (<15s each).
- DL traffic healthy and NOT stalled: `dlsch_rounds` climbing steadily (~600+ by 4 min in), no
  `out-of-sync` seen.
- Sensing/CSI-RS path fully healthy: every CPI closes with `occ[csi=32 ...]` (full CSI-RS fill),
  `range_res=7.86m`, and by CPI #5 the top detection is at range=70.7 m (near the true ~89-109 m
  synthetic target — not exact but in-band, consistent with the pinned-conf baseline's own
  documented 0-3/8-22 CPI hit rate for this same scene).
- Blind-PDCCH agnostic discovery IS progressing but had NOT reached full convergence by ~4 min
  wall-clock into the 300s window: `dci10` (CSS0/common-SS) C-RNTI-class accepts climbing
  steadily (1 -> 23+ over ~28000 occasions, persistence tracker holding candidates, traffic-rate-
  consistent, not attributable to the ~1/65519 random-accept floor), and `SENSING: Technique D
  ARMED: independent RNTI/TDA contexts, TB-CRC scoring` fired (contexts being built) — but no
  `Technique D CONVERGED` or `multi-CORESET bank add` line yet, so `pdsch_decode`/`cfr_submits`/
  `dmrs`/`data` occupancy stayed at 0 through CPI #5.
- **Read this as a genuine, useful measurement, not a stall**: the fully-agnostic discovery path
  (no pinned CORESET/TDA/DCI-bit hints) is materially slower to converge than `ue.passive.conf`'s
  pinned baseline (which reports 556/556 CRC immediately because it already knows the geometry).
  5 min was not enough wall-clock on this box for full Technique-D convergence at 106 PRB with
  1 active UE's traffic rate. This directly affects the B1-B5 budget below.

### B0-full: 900s run, scored (2026-09-26, this session)

Command actually run (found already finished, no process alive, at session start):
`CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4 TRAFFIC=udp IPERF_RATE=3M
./run_passive_rx.sh 900 /home/sens/NICOLA/rfsim_validation_local/B0`, launched ~19:57, output complete
at 20:12 (`harness.log`, `ue_rx1.log`, `gnb.log`, etc. all present, no leftover `nr-*softmodem` PIDs).
Active UE C-RNTI **0xa7c8** this run (re-read from `gnb.log`, differs from the earlier 0x21e9 partial
attempt per the RNTI-recheck rule).

**Harness-level result (`harness.log`)**: DL healthy the whole 900s (9193 `dlsch_rounds` growth, 0
out-of-sync, 15/15 traffic samples clean). Passive rx1 CSI-RS path fully healthy: `occ[csi=32 dmrs=0
data=0 blind=0 pusch=0 uldata=0]` on every one of 44 CPIs (i.e. CSI-RS never stopped, but DM-RS/data/
blind-PDCCH/PUSCH rows were EXACTLY ZERO on every single CPI, all 900s). 15/44 CPIs had a detection in
the synthetic target's range band; AoA on 585/585 detections. Fused-track precision 0.0% (443
confirmed updates, median_dist 1356.65 m) — expected/uninteresting on a single receiver with zero
non-CSI-RS rows feeding the CFR.

**"uplink/RA log lines: 34 -- VERDICT: UPLINK DETECTED" is a FALSE POSITIVE, not a real fault.**
Checked the harness's own grep (`grep -icE "prach|RAPROC|preamble|Msg3|PUCCH|PUSCH"`): all 34 hits are
the string `pusch=0` inside the receiver's own `occ[...]` sensing-occupancy log line (one per CPI, 34
of the 44 CPIs happened to log before the grep window) — not any real uplink activity. The passive
receiver never transmitted. Flagging for the harness owner: the check needs to exclude the receiver's
own `occ[...pusch=...]` telemetry line, e.g. anchor on `PUSCH.*grant`/`RAPROC:` rather than a bare
substring match, since `ul_pusch=1` in `ue.passive.agn.conf` guarantees this string appears in every
receiver's own status line.

**Root cause: dedicated-CORESET blind discovery never confirmed a candidate — this is STUCK, not
"still searching".** Evidence, all from `ue_rx1.log`:
- Init line (fires once, ~line 8): `blind PDCCH DCI 1_0 scanning alongside format 1_1, ss=ue-specific
  n_rb_riv=auto:0 rb_offset=auto:-1 dci_length=0 class_mask=0x0 mux_pattern=0 sib1=0
  tda_common_entries=0` — the dedicated (UE-specific) search-space config is all still-unresolved
  placeholders. **None of `class_mask`, `dci_length`, `rb_offset` ever appear again anywhere in the
  900s log** (`grep -n` for each returns only that one line) — the dedicated SS config was never
  updated after init.
- `Technique D ARMED`/`CONVERGED`, `multi-CORESET bank add`, `CSIRS_BLIND CONFIRMED`, `pdsch_decode[`,
  and `accepts=` (the periodic blind-PDCCH summary line, `SENSING: blind PDCCH monitor summary:
  occasions=... accepts=...`) — **zero occurrences of any of these, over the full 900s.** Source
  (`nr_pdcch_blind_monitor_rt.c:151-155`) documents that this summary line is gated only on
  wall-clock (once every 20s) *after the first completed occasion* — so zero summary lines over 900s
  means the occasion body never completed even ONCE, not that it's merely slow.
- Source-level gate found (`nr_pdcch_blind_monitor_rt.c` ~2815-2892, the per-slot RT entry point): the
  per-symbol autodiscover correlation (Technique A, `nr_pdcch_blind_monitor_autodiscover_step()`) does
  run unconditionally every DL slot, but is immediately followed by
  `if (nr_pdcch_coreset_bank_count() == 0) return;` — every downstream stage (occasion scan,
  deferred-queue enqueue, the periodic summary, Technique D) is gated on the dedicated CORESET bank
  having ≥1 entry. A second gate a few lines later, `if (cfg->ss_monitoring_slot_periodicity <= 0)
  return;`, blocks the ue-specific occasion path a second time on the same still-unresolved config.
- Corroborating: `ACQ_STATE SEARCHING -> PBCH_LOCKED (... evidence[len_found=0 coreset_ok=0 ul_bwp=0
  dl_win=0 ul_width_win=0 ul_interp_win=0])` — `coreset_ok=0` at the one point it's logged, and never
  updated afterward (no second ACQ_STATE evidence-vector line with `coreset_ok=1`).
- **This is not a general receiver-health failure**: SIB1 decoded fine (`ACQ_STATE ... -> SIB1_DECODED`,
  `SICENSUS si_rnti candidates=2 hits=1`), CSI-RS sensing ran cleanly on 100% of CPIs, and the sync
  trackers (STO/CFO/SFO/LOS) were actively converging throughout — so PBCH/SIB1 lock, CSI-RS, and the
  sync stack are all healthy for the whole 900s. The fault is narrowly scoped to Technique A's
  dedicated-CORESET DM-RS-correlation search never confirming a candidate for this cell/RNTI/duration.
- **Non-reproduction, not merely "slower than the earlier attempt"**: the same doc's earlier 300s
  partial run (§B0 detail above) reported `dci10` CSS0-path accepts climbing to 23+ and `Technique D
  ARMED` firing within ~4 minutes. This full 900s run shows literally none of that (`grep -c "Technique
  D"` = 0, `grep -c accepts=` = 0) despite running 3x longer — a regression/non-reproduction against
  that earlier evidence, not a continuation of the same slow-but-progressing trend. No code was
  touched between the two runs (per task constraints), so the likely explanation is this rig's
  documented run-to-run bimodality (memory: `passive-rx-needs-5-runs-per-arm`,
  `passive-rx-pbch-lock-is-the-single-fault`) manifesting specifically in the discovery stage this
  time, though that specific mechanism was not directly confirmed within this session's time budget.
- **Consequence for B1-B5**: every arm below depends on the SAME dedicated-CORESET discovery path (or,
  for B1/UL, an analogous UL discovery gate) converging before its own feature can be exercised. Watch
  each run for `multi-CORESET bank add` / `Technique D ARMED` specifically — their absence after
  several minutes should be read as "stuck" per the diagnostic above, not "still converging".

## Recommendation for continuing this campaign

Increase per-arm `DURATION` toward the stated 10-minute ceiling (e.g. 540-560s) rather than 300s,
since B0 shows the agnostic discovery needs several minutes just to arm/build contexts before
CRC-scoring can converge. Reuse `ue.passive.agn.conf` unchanged for every arm (B1/B2 need no
receiver-conf change — `dci01`/`ul_pusch` are already unconditionally on). Per-arm launch
recipe (all from `/home/sens/NICOLA/rfsim-local`, all preceded by the stats-file pre-touch
workaround above):

- **B1** (UL scrambling ≠ PCI): `ISAC_GNB_TEST_UL_DMRS_ID0=600 ISAC_GNB_TEST_UL_DATA_ID=400
  CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4 ./run_passive_rx.sh 540 .../B1`. Verify the gNB knob
  fired via `grep ISAC_GNB_TEST_UL gnb.log`; score via `ulscan[...]`/`pdsch_decode`-analogous UL
  counters in the blind-PDCCH summary line and `uldata=` in `occ[...]`.
- **B2** (DL scrambling ≠ PCI): `ISAC_GNB_TEST_DL_DMRS_ID0=700 ISAC_GNB_TEST_DL_DATA_ID=500` same
  pattern; verify via `grep ISAC_GNB_TEST_DL gnb.log`; score via `dmrs=`/`data=` occupancy and
  `pdsch_decode[... crc_ok ...]`.
- **B3** (dedicated BWP, non-zero start): `GNB_CONF_OVERRIDE=gnb.sa.rfsim.bwp.conf
  GNB_EXTRA="--telnetsrv --telnetsrv.shrmod ci"`. That conf's `first_active_bwp=1` already starts
  the UE on BWP id 1 (CRB 30, 40 PRB) — non-zero start from attach, no telnet needed for that part.
  For the LIVE switch: once attached, `telnet 127.0.0.1 9090` then `ci trigger_bwp_switch 2`
  (bwpId 2 = CRB 70/24 PRB target; rnti omitted, auto-resolved since NUM_UE=1). Verify via
  `grep "triggered BWP switch" ` on the telnet session output and receiver re-convergence after.
- **B4** (multi-UE): `NUM_UE=3` (uses `ue.active2.conf`/`ue.active3.conf` already in-tree, own
  netns/subscriber each). Score: 3 distinct C-RNTIs all present with `persist=`/accepts climbing
  in the blind-PDCCH summary, none evicted.
- **B5** (AL1-only): `ISAC_GNB_TEST_USS_AL="4,0,0,0,0" ISAC_AL1_COVER=1` (both env vars reach their
  targets directly — the gNB and the passive receiver are the only two processes that read them,
  and neither is `sudo`-wrapped in the script, unlike the active UE). Verify gNB applied it via
  `grep ISAC_GNB_TEST_USS_AL gnb.log`; score via the AL1-cover log lines in `nr_pdcch_blind_monitor.c`
  (`AL1 cover lap`) and `multi-CORESET bank add ... AL1` if it fires.

Kill order: SIGINT to the `run_passive_rx.sh` PID first (its own `trap cleanup EXIT INT TERM`
tears everything down cleanly), SIGTERM/SIGKILL only if it doesn't exit within ~10s.

## B1 result (2026-09-26, scored on handback)

Command: `ISAC_GNB_TEST_UL_DMRS_ID0=600 ISAC_GNB_TEST_UL_DATA_ID=400 CONF_TAG=.agn NUM_RX=1 NUM_UE=1
RX1_NANT=4 ./run_passive_rx.sh 540 /home/sens/NICOLA/rfsim_validation_local/B1`, launched 20:45:30.
Already finished (no live process) by the time this session picked it up; no build ran concurrently.

**VOID -- the perturbation broke the active UE's own link before the passive receiver ever started; the
test never reached the thing it was designed to measure.**

- `ISAC_GNB_TEST_UL_DATA_ID=400`/`ISAC_GNB_TEST_UL_DMRS_ID0=600` DID apply on the gNB (`grep -c` = 8
  hits in `gnb.log`, 4 pairs -- fires once per RRC (re)configuration in this run).
- But the SAME override that the receiver is meant to be tested against also changes what the gNB
  itself expects when decoding the ACTIVE UE's own PUSCH -- and the active UE (an ordinary
  `ue.active.conf`, unaware of the test knob) keeps transmitting with PCI-derived scrambling. Result,
  from `gnb.log`: `ulsch_errors`/`ulsch_rounds` BLER **1.00000 from the very first stats dump** (frame
  128.8: 1613/1615), never recovering over the whole run. The active UE stays PHY `in-sync` (RA
  succeeded, DL is fine, CQI 15) but every uplink transport block it sends is undecodable to the gNB.
- Consequence: `ue1_active.log` never gets past `will request PDU session ID 10` (line 19) -- the NAS/
  RRC signalling needed to establish a PDU session rides on that same broken uplink. Harness's own
  150s PDU-session-wait timeout fired (`launch.out`: `[TIMEOUT] active UE ue1 got a PDU session`), and
  teardown ran immediately after.
- **The passive receiver (rx1) never launched at all** -- no `ue_rx1.log`/`harness.log` exists in
  `B1/`, only `gnb.log`/`ue1_active.log`/`launch.out`. So B1 as scripted cannot test "does dedicated-
  CORESET discovery survive UL scrambling != PCI" -- the receiver never got a single slot to look at.
- **This is a harness/experiment-design defect, not evidence about the branch's discovery code one way
  or the other.** `ISAC_GNB_TEST_UL_DMRS_ID0`/`_DATA_ID` (per `joint-solver-rfsim-validated-2026-09-24`
  memory) is a gNB-side-only override with no matching UE-side knob, so it necessarily also breaks the
  ordinary attached UE's uplink on THIS harness (single shared gNB serving both the active UE and the
  passive receiver's illumination). B1 would need either a UE-side matching override (so the active UE
  can still attach) or a scene where the active UE doesn't need a PDU session before the receiver's
  window closes. Not fixed here (out of this task's scope: no code edits, and B1 is not the task's
  focus, which is the B0 regression A/B below).
- **Score against the task's two questions**: "did dedicated discovery fire?" -- no data (receiver
  never ran). "ISAC_GNB_TEST_UL applied in gnb.log?" -- yes, confirmed.

## B0 regression A/B (2026-09-26, this session)

See "Step 0-3" of the task prompt. Full command each run: `CONF_TAG=.agn NUM_RX=1 NUM_UE=1
RX1_NANT=4 ./tests/passive_rx/run_passive_rx.sh 600 <outdir>`, stock gNB (no `ISAC_GNB_TEST_*`),
stats-file pre-touch workaround applied before every launch. BASE tree = new worktree
`/home/sens/NICOLA/rfsim-base` @ `222f98d072`, configured identically to `rfsim-local`
(`-GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DAVX2=ON -DENABLE_ISAC_SENSING=ON
-DT_TRACER=ON`, `ninja -j8 nr-softmodem nr-uesoftmodem rfsimulator params_libconfig`).
`tests/passive_rx/ue.passive.agn.conf` copied in unmodified -- every config key it uses
(`pdcch_blind_monitor_{autoconf,autodiscover,full_auto,dci10,rnti_range,noise_gates,scan_thread,
pdsch,dci01,ul_pusch,ul_uci,ul_thread}`, `csirs_monitor`, `aoa_enable`/`aoa_broadside_deg`) already
exists in the BASE commit's parser (grepped `nr_pdcch_blind_monitor.c`/`nr_csirs_monitor.c`/
`nr_isac.cc`), so no key had to be removed. `run_passive_rx.sh` and `gnb.sa.rfsim.conf`/
`ue.active.conf` are byte-identical between the base and branch trees (`diff -q` silent) -- the only
variable between arms is the 25a8699a64+67bb0eaa11 vs 222f98d072 source delta.

### base1 (222f98d072, 600s, launched 20:56:19, finished/torn down 21:07:16)

Active UE C-RNTI re-read this run, sync/DL healthy (5626 `dlsch_rounds` growth over 10x60s samples,
0 out-of-sync). Passive rx1 synced, decoded SIB1, CSI-RS sensing fully healthy: every one of 66 CPIs
closed with `occ[csi=32 dmrs=0 data=0 blind=0 pusch=0 uldata=0]` (66 DetectionReports, 22/66 CPIs hit
the synthetic target's range band, 885/885 detections carry azimuth). "VERDICT: UPLINK DETECTED" is
the SAME known false positive documented in B0-full (the harness's grep matches the receiver's own
`occ[...pusch=...]` telemetry substring, not real uplink) -- confirmed again here (23 hits, all the
`occ[` line).

**Dedicated-CORESET / blind-PDCCH discovery: STALLED, same signature as the branch's documented
B0-full 900s run:**
- Init line unchanged all run: `ss=ue-specific n_rb_riv=auto:0 rb_offset=auto:-1 dci_length=0
  class_mask=0x0 ... tda_common_entries=0` -- `dci_length`(1 hit, the init line only),
  `class_mask`(1 hit, init only) never update; `rb_offset`(2 hits: init + one unrelated line) never
  resolves off `auto:-1`.
- `Technique D ARMED`: 0. `Technique D CONVERGED`: 0. `multi-CORESET bank add`: 0. `accepts=`
  (periodic blind-PDCCH summary): 0. `dci10[`: 0. `pdsch_decode[`: 0.
- `ACQ_STATE`: `SEARCHING -> PBCH_LOCKED` then `PBCH_LOCKED -> SIB1_DECODED`, both with
  `evidence[len_found=0 coreset_ok=0 ...]` -- `coreset_ok` never becomes 1, no further ACQ_STATE
  transition logged.
- Fused-track precision (expected/uninteresting on a single receiver with zero non-CSI-RS CFR rows):
  0.0% (679 confirmed updates, median_dist 1466.42 m).

**This reproduces the exact stall signature from the earlier documented 900s branch run (B0-full,
`coreset_ok=0`/no `Technique D`/no `accepts=`) on the BASE commit, at 600s.** Read together with
branch1 below before drawing a verdict.

### branch1 (25a8699a64+67bb0eaa11, 600s, launched 21:07:38, torn down 21:18:30)

DL healthy (4684 `dlsch_rounds` growth, 0 out-of-sync). CSI-RS sensing healthy: 88 CPIs, every
`occ[csi=32 dmrs=0 data=0 blind=0 pusch=0 uldata=0]`, 28/88 in-band, 1167/1167 detections carry
azimuth. Same "VERDICT: UPLINK DETECTED" false positive (23 hits, all the receiver's own `occ[`
line).

**Dedicated-CORESET / blind-PDCCH discovery: BYTE-FOR-BYTE THE SAME STALL SIGNATURE AS base1:**
init line identical (`rb_offset=auto:-1 dci_length=0 class_mask=0x0 ... tda_common_entries=0`,
never updates); `Technique D ARMED`=0, `CONVERGED`=0; `multi-CORESET bank add`=0; `accepts=`=0;
`dci10[`=0; `pdsch_decode[`=0; `ACQ_STATE` reaches `SIB1_DECODED` with `coreset_ok=0`, never a
further transition. Fused-track precision 0.0% (907 confirmed updates, median_dist 1519.50 m) --
same shape as base1 (679 updates, 1466.42 m).

**base1 vs branch1 are indistinguishable.** Both arms: SIB1/DL/CSI-RS healthy, dedicated-CORESET
discovery never leaves its initial placeholder state for the entire 600s. Proceeding to base2/branch2
to complete the designed 2x2 alternation before the verdict (memory:
`passive-rx-needs-5-runs-per-arm` -- a single pair is not enough to rule out rig bimodality flipping
one side, even though here both sides already agree).

### base2 (222f98d072, 600s, launched 21:18:59, torn down 21:29)

Same as base1/branch1 in every respect. DL healthy (4691 `dlsch_rounds` growth, 0 out-of-sync).
CSI-RS: 110 CPIs, all `occ[csi=32 dmrs=0 data=0 blind=0 pusch=0 uldata=0]`. Dedicated-CORESET
discovery: init line frozen (`rb_offset=auto:-1 dci_length=0`), `Technique D ARMED/CONVERGED`=0/0,
`multi-CORESET bank add`=0, `accepts=`=0, `dci10[`=0, `pdsch_decode[`=0, `ACQ_STATE` stuck at
`SIB1_DECODED`/`coreset_ok=0`. Fused precision 0.0% (1142 confirmed updates, median_dist 1519.50 m).
**Third consecutive identical stall.**

### branch2 (25a8699a64+67bb0eaa11, 600s, launched 21:30:05, torn down 21:40)

Same as every other arm. DL healthy (5768 `dlsch_rounds` growth, 0 out-of-sync). CSI-RS: 132 CPIs,
all `occ[csi=32 dmrs=0 data=0 blind=0 pusch=0 uldata=0]`. Dedicated-CORESET discovery: init line
frozen (`rb_offset=auto:-1 dci_length=0`), `Technique D ARMED/CONVERGED`=0/0, `multi-CORESET bank
add`=0, `accepts=`=0, `dci10[`=0, `pdsch_decode[`=0, `ACQ_STATE` stuck at `SIB1_DECODED`/
`coreset_ok=0`. Fused precision 0.1% (1388 confirmed updates, 2 within 15m by chance, median_dist
1618.99 m). **Fourth consecutive identical stall.**

## B0 regression A/B -- summary table

| Run | Tree | dlsch growth | CPIs (csi=32) | dci_length resolved? | Technique D ARMED/CONVERGED | bank add | accepts= lines | ACQ_STATE coreset_ok | pdsch_decode | Fused precision |
|---|---|---|---|---|---|---|---|---|---|---|
| base1   | BASE 222f98d072            | 5626 | 66/66   | no (stuck 0) | 0/0 | 0 | 0 | 0 (never) | 0 | 0.0% (679 upd) |
| branch1 | BRANCH 25a8699a64+67bb0eaa11 | 4684 | 88/88   | no (stuck 0) | 0/0 | 0 | 0 | 0 (never) | 0 | 0.0% (907 upd) |
| base2   | BASE 222f98d072            | 4691 | 110/110 | no (stuck 0) | 0/0 | 0 | 0 | 0 (never) | 0 | 0.0% (1142 upd) |
| branch2 | BRANCH 25a8699a64+67bb0eaa11 | 5768 | 132/132 | no (stuck 0) | 0/0 | 0 | 0 | 0 (never) | 0 | 0.1% (1388 upd) |

Every arm: SIB1 decoded, DL traffic healthy for the full 600s, CSI-RS sensing fully healthy on
100% of CPIs -- and dedicated-CORESET/blind-PDCCH discovery frozen at its initial placeholder state
(`ss=ue-specific ... rb_offset=auto:-1 dci_length=0 class_mask=0x0`) for the entire run, on BOTH
trees, with zero variance across all 4 runs (identical zero counts for every discovery-stage
counter). Fused-track precision is uniformly ~0% on both trees, as expected with zero non-CSI-RS
CFR rows feeding the tracker.

## Verdict

**NOT A REGRESSION.** The dedicated-CORESET blind-discovery stall (Technique A's autodiscover
correlation never confirming a candidate, gating everything downstream via
`nr_pdcch_coreset_bank_count()==0`) reproduces identically at the plan's BASE commit `222f98d072`
and at the current branch tip `25a8699a64`+`67bb0eaa11`, on this exact scene/conf/harness/duration,
4/4 runs (2 base + 2 branch, alternated). No code path introduced between the two commits changes
this behavior -- it is a pre-existing property of the fully-agnostic discovery path on this
`ue.passive.agn.conf` scene (106 PRB, this synthetic channel/target geometry, this active-UE traffic
rate), not something any of the T2-T18/fix/fix2/fix3 lane merges broke. No bisect was run because
its precondition (base discovers, branch doesn't, consistently) does not hold -- both trees fail to
discover, consistently.

This is consistent with, and now directly confirms with a controlled same-duration same-scene A/B
(rather than the earlier single 900s branch-only run), the `B0-full` finding already on record above:
"the fully-agnostic discovery path... is materially slower to converge... 5(-10) min was not enough
wall-clock on this box for full Technique-D convergence at 106 PRB with 1 active UE's traffic rate" --
except now measured as a rate of exactly 0/4 within 600s on both trees, not merely "slower". Whether
it ever converges given enough wall-clock (the B0-full 900s run's earlier PARTIAL 300s attempt once
reported `dci10` accepts climbing to 23+ and `Technique D ARMED` firing within ~4 min, which did NOT
reproduce in the subsequent full 900s run on the same branch -- see that section's own
non-reproduction note) is a separate, still-open question about discovery-convergence reliability on
this scene, orthogonal to the regression question this task was scoped to answer.



