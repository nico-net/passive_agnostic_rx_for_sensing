# Round 3 BWP switch lane

## Bounded read-only checkpoint — 2026-09-27

Status: no code edits, build, lock, or radio run. Remote lane worktree
`sens6:/home/sens/NICOLA/agn-wt/gap-bwp` is clean on `sdd/gap-bwp` at
`239eb144acd398968d5869a75e083e540ddefa26`. Build directory is absent. OCUDU-DL remains active and this
lane did not compete for the bed.

### gNB telnet control route

- `common/utils/CMakeLists.txt` makes telnet support conditional on `ENABLE_TELNETSRV`, default `OFF`
  (SHA256 `0534d7c2d6858bd5689147affd9dc7dbe343cf6ebed943ae298cf99964818583`). When enabled,
  `common/utils/telnetsrv/CMakeLists.txt` builds `telnetsrv_ci` and puts shared libraries in the build
  root (SHA256 `fff6f6a2703a305226307d1fdacc814e75e08b19fdbf77f5d331bc9bfc99a850`). No CMakeCache or
  `libtelnetsrv_ci.so` exists in this worktree, so the support has not been built here.
- `common/utils/telnetsrv/telnetsrv_ci.c` registers `get_single_rnti`, `get_current_bwp`, and
  `trigger_bwp_switch`; the latter calls `nr_trigger_bwp_switch(rnti,bwpId)`. The server default port is
  9090 (`common/utils/telnetsrv/telnetsrv.c`). Source SHAs respectively:
  `973156e5dda957233efd487ba41337d1ac8b53cdf7dffad2ff3038c26bbf85df` and
  `4aff8646bfaebd6ea8c1201ca402fc1d2d2fa31c39fa0d09e833ad7c4093f35b`.
- `tests/passive_rx/run_passive_rx.sh` already supports `GNB_CONF_OVERRIDE` and `GNB_EXTRA`; its comment
  gives `--telnetsrv --telnetsrv.shrmod ci` as the switch route. It does not itself issue the telnet
  command. SHA256 `591404ea9e611c1369bf17870907b44c7dde73d2911e6a28153ace036798f57d`.
- `tests/passive_rx/gnb.sa.rfsim.bwp.conf` defines 106-PRB carrier, first active BWP ID 1, 40 PRB at
  CRB 30, plus target BWP ID 2, 24 PRB at CRB 70. This is validation fixture truth only; it must not
  seed passive receiver configuration. SHA256
  `6a9f0a9d32781746b5566d87d08561d258e6e3c9fccd920e2dc56751fae5b843`.

### Receiver reacquisition contract and blocker

- Passive BWP tracking is enabled by `ISAC_BWP_TRACK=1`; otherwise it follows
  `ISAC_AGNOSTIC_V2` (which defaults off). The BWP tracker in
  `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` probes unregistered DCI lengths, registers
  an entry after repeated valid RNTI evidence, discovers the BWP size/start from grant/DM-RS coherence,
  discovers a dedicated CORESET from observations, then scans resolved entries. Relevant expected
  lines are `SENSING: BWP tracking armed`, `BWP NEW`, `BWP CORESET found`, `BWP RESOLVED`, and
  `SENSING: BWP SWITCH`; target-BWP PDSCH CRC must resume after the switch.
- `nr_pdsch_passive_queue.c` performs DM-RS coherence scoring for unresolved BWP probes and feeds
  decoded target-entry TB CRCs back to the tracker. After 32 failed CRCs with no pass, it un-resolves
  and rescans that geometry. It is a recovery path, not evidence that the live RRC switch is already
  handled.
- Important validation caveat: `nr_pbwp_indicator()` has a unit test
  (`DciIndicatorSwitchUsesTheTargetBwpOnceBound`) but no production call site in this tree. Runtime
  switch logging is driven by an accepted DCI 1_1 being assigned to a different tracked entry. This
  may still reacquire from subsequent target-BWP DCI-length/DM-RS/CORESET evidence, but there is no
  source evidence that the unit-tested indicator remap is wired into the live path. G4 must prove
  reacquisition empirically; do not describe the isolated helper test as live switch handling.
- The existing `tests/passive_rx/ue.passive.conf` is not eligible for this blind validation: it pins
  CORESET geometry, SearchSpace, BWP `0:106`, DCI length `45`, field widths, and DM-RS assumptions from
  the default gNB's known configuration. Its comments explicitly say those values were derived from
  that gNB. SHA256 `236a48800678eb684f0ed1ae5997c5efd8321ae70f0661fa250412d7d7771c7d`.
  No fully agnostic BWP receiver conf is tracked in `gap-bwp`; this is the current procedure blocker.
  Do not run this fixture as-is and do not test the sensing pipeline.

### Fail-closed live plan (not executed)

Before any later live run, stage an independently reviewed blind receiver config/build and exact-pin
all binaries/configs/scripts. Build gNB with `ENABLE_TELNETSRV=ON` and confirm the CI module is present;
start `run_passive_rx.sh` with the BWP gNB config, one active UE, one passive UE, and telnet CI module.
Enable only the passive monitor (`ISAC_AGNOSTIC_V2=1` or explicit `ISAC_BWP_TRACK=1`), without copying
gNB geometry/ID truth into its configuration. Use one exact-UE telnet session on a checked-free port:

1. Poll `get_single_rnti` and `get_current_bwp`; before trigger, require the active UE to be at DL/UL
   BWP ID 1 and require blind C-RNTI accepts plus PDSCH CRC > 0.
2. In switch arms call `trigger_bwp_switch 2 <rnti>` (the single-UE route is also accepted); poll
   `get_current_bwp` until the gNB reports ID 2. Treat this only as validation truth.
3. Require the passive receiver to discover/resolved the new entry and CORESET, emit a switch entry
   event for the same blind C-RNTI, and resume positive PDSCH CRC on post-switch grants. Preserve
   separate pre-/post-switch attempts, CRC, C-RNTI accepts, and run logs. A timeout/no switch line or
   no post-switch CRC is a failing/inconclusive arm, never a pass.
4. Run three baseline/no-trigger and three switch arms, alternated B/S/B/S/B/S with identical
   300-second measured dwell per arm, fresh run directories, pre-run no-build/quiescence/load checks,
   and the sensnuc3 `radio_bed.lock`. Baseline must retain C-RNTI CRC; every switch arm must recover
   after the gNB's reported transition. Do not overlap builds/captures.

No RED/GREEN or G1-G6 result exists; this checkpoint only establishes source support, existing test
coverage, and the configuration blocker. No receiver edit or commit.

## Coordinator review fixes — fixture-only checkpoint, 2026-09-27

Updated only the owned lane runner and its offline test file on sens6; the blind/no-sensing config is
unchanged. Worktree remains at `239eb144acd398968d5869a75e083e540ddefa26` with exactly three untracked
lane files:

- `tests/passive_rx/run_bwp_matrix.py`, SHA256
  `51aa64b64d9fc2f25c260f343225fe73d553066a432d47e38287fb70b239524e`
- `tests/passive_rx/test_bwp_runner.py`, SHA256
  `83bd49aac8aff3e84f6cacc02cb6509edbc6b3c7ab4fffbe7070323aad1b392d`
- `tests/passive_rx/ue.passive.bwp.agn.conf`, unchanged SHA256
  `301486494ed6f168d551a5cb21384768979787253aef6c1acc08d78e3179d577`

Coordinator review findings are addressed in the scaffolding:

- Telnet sends the registered module namespace (`ci <command>`), sends before waiting (the server has
  no initial greeting), requires the actual `softmodem_<function>>` prompt, strips optional echoed
  commands/prompts, and parses result lines independently of them.
- Before each arm, exact `/proc` process identity and the two harness ports are checked. Each child is
  started in its own session/process group. Cleanup verifies leader PID/start tick, session/group
  identity, and allowed executable names (including the sudo UE child), signals only that owned
  group, and continues through all children while aggregating cleanup failures.
- Binary/config/helper pins now include `libtelnetsrv.so` and `libtelnetsrv_ci.so`; the build root is
  added to gNB `LD_LIBRARY_PATH`, and all pins are rechecked during/after the arm.
- Passive evidence scoring requires target-C-RNTI BWP NEW plus CORESET/RESOLVED markers; switch arms
  additionally require post-trigger BWP NEW and RESOLVED evidence for the target entry plus a
  matching BWP SWITCH for the same returned C-RNTI. Both arms require positive pre-transition
  per-RNTI PDSCH CRC/decoded counts and a positive post-midpoint CRC and decoded delta. The gNB
  telnet BWP report remains validation-only and is not written into receiver configuration.
- The six-arm `baseline,switch` alternation (three each), exact 300-second dwell, fully agnostic
  receiver controls, and both sensing `enable=0` settings are preserved.

Test-first evidence: the initial expanded suite failed with 6 review-contract errors (12 tests
total), captured at sens6 `/tmp/gap-bwp-review-red.log`, SHA256
`fed8f1c384292491706f9c410454b495fc03c2a743d1026b12dd1fd6fd2912d0`. After the fixes, the same
suite passes 12/12 at `/tmp/gap-bwp-review-green.log`, SHA256
`dbf2044da74b70a25a1be3a6f6f780f16caf162ce87f7ed45f0a036d60b4de3b`; `py_compile` of runner and
tests passes. `git diff --check` passes. No receiver source, handover, progress, build, lock, radio,
or commit was touched.

Correction to experiment trail: the first post-edit upload had a syntax typo in the `/proc/stat`
parser (`parse_proc_stat`, then line 153), so that intermediate unittest/`py_compile` attempt did not
execute tests. The typo was fixed before the recorded final GREEN run above; it is not counted as a
test failure or pass.

This is only offline runner/scoring scaffolding: mocked process ownership and synthetic passive logs
do not establish live sudo process-group behavior, telnet integration, BWP re-acquisition, or CRC
recovery. G1-G6 and live B/S x3 remain outstanding; do not build/run without coordinator authorization.

### Source SHA256 index

- `nr_pdcch_blind_monitor_rt.c`: `91cbc770ce051d671d455ab38b986d024567f132b8765afa250253d64131e9ed`
- `nr_passive_bwp.c`: `a06196a9b39178ec89bd3d60fafa7d10e7d111d048db440015c67835db591568`
- `nr_passive_bwp.h`: `42fd3cf2b0bf2549ec26bc21f87857ed9e4975b8edd1122d559a9a8fb4f4cbb0`
- `nr_agnostic_v2.h`: `5d55a98f1380b8acf262d98a2c338273ef2c57a8aedef5b17d06b7a5ed4daa55`
- `nr_pdsch_passive_queue.c`: `5d0b6348ed4f3fc63011be8c276f15926c7bbcb5f12300cec8d2444d9de89ddc`
- `tests/nr_passive_bwp_test.cc`: `03f2285517f093d282dca0c79f42dd57611745a170de79641ea699c7eac79efc`
- `openair2/LAYER2/NR_MAC_gNB/config.c`: `c3749a507462af4c80521c08cb1e1ca8f1de7b651cddfc31774a39169101f1a2`
- `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_primitives.c`: `f3e36c8bcce036ef92d0fd5abd1835cd2808dd094ece2252a7b30f59fa12f9c0`

## Fixture-only staging update — 2026-09-27

Supersedes the earlier statement that no fully agnostic BWP receiver configuration is available:
the authenticated standalone `rfsim-val/tests/passive_rx/ue.passive.agn.conf` (SHA256
`892f9f0f4a3771856a0eb249390d9dd34bd83a7b643526db4c3e9f7822555f1e`) was copied into this owned
lane as `tests/passive_rx/ue.passive.bwp.agn.conf`. Only `[sensing_channel].enable` and
`[sensing].enable` were set to `0`, plus a comment correction; new SHA256
`301486494ed6f168d551a5cb21384768979787253aef6c1acc08d78e3179d577`. The blind scan/autoconf/
autodiscover/full-auto controls remain enabled. No effective pinned CORESET/SS/BWP/TDA/DMRS/DCI
geometry fields were added. `nr_pdcch_blind_monitor_rt.c` explicitly decouples the blind PDCCH
monitor from `nr_isac_enabled()` at lines 2685-2700; CSI-RS and other ISAC-source paths remain gated.

Added fixture-only `tests/passive_rx/run_bwp_matrix.py` (SHA256
`9fa5049fbc218a8d308dcf0acc1ad85f6ec0b0c273904e87e1e8eeef7046d4fa`) and stubbed structural tests
`tests/passive_rx/test_bwp_runner.py` (SHA256
`47e7a4b3e74022afdead096f29cc416659fb466e0c9b80aadecb444424c4ee15`). The runner plans exactly
`baseline,switch` repeated three times, fixes each capture window at 300 seconds, requires explicit
`--live`, self-flocks the shared radio-bed lock, pins binary/config/helper paths and SHA256s before
launch, records BWP state, obtains the exact single RNTI from CI telnet for control commands, and
only terminates its recorded child PIDs when `/proc/<pid>/stat` start ticks still match. It does not
use `pkill`, create tmux sessions, or modify receiver production code. It is scaffolding, not live-
validated process control.

Test-first evidence: the initial test invocation failed because `run_bwp_matrix.py` did not yet
exist; preserved at `gap-bwp-red.log` (SHA256
`b2ac82c0813ff35f03185f68aa2b8d8b23f2002c3064334fe7c65f7700276ba4`). An intermediate 6/7 result
was a test-regex defect (`.` did not span lines), not an implementation failure; that assertion was
corrected. Current offline suite passes 7/7, and `py_compile` passes; log at `gap-bwp-green.log`
(SHA256 `fd1abcb5043a07cfa087d028d86284c063bfab9f1006c00b718ccd8d44496dcd`). Worktree status is
only those three new lane files; no tracked base file changed. No build, lock acquisition, or radio
run occurred. G1-G6 and live n=3 baseline/switch evidence remain outstanding; do not run until the
coordinator authorizes the BWP lane.

## G5 review follow-up — monitored dwell and trigger-correlated evidence — 2026-09-27

Implemented the two requested harness-only review fixes in the existing isolated lane; no receiver
production file, shared handover/progress ledger, build, lock, or radio was touched. Worktree remains
`/home/sens/NICOLA/agn-wt/gap-bwp` at `239eb144acd398968d5869a75e083e540ddefa26`, with only the same
three untracked lane files and no tracked base-file changes.

- `tests/passive_rx/run_bwp_matrix.py`, SHA256
  `feee3b662b9c2351ab8ecaad68602cd0c86978e0f6daa8e56b4a431472bc53f4`: replaced remaining-dwell
  sleep with an absolute `time.monotonic()` deadline and bounded polling. Every poll verifies each
  owned process is alive with matching PID/start tick, leader membership, and allowed executable
  membership; the passive log byte count and aggregate `PDSCHQ queued/decoded` census must both
  advance within bounded stall intervals. Failures create `VOID.json` before owned-only cleanup.
  The elapsed active interval is explicitly checked against the exact 300-second minimum. UDP
  traffic helpers now start immediately before the measured interval and have 330-second lifetimes,
  so all owned process groups are expected to survive the dwell.
- For switch arms, `TelnetCI.command` records the passive-log byte length immediately before sending
  the accepted `ci trigger_bwp_switch ...` command. Scoring examines only the suffix after that byte
  boundary and requires CORESET, target NEW+RESOLVED, same-returned-RNTI SWITCH, and positive
  post-trigger CRC and decoded deltas. Baseline uses whole-arm BWP events and the midpoint-to-end
  CRC interval. Partial first lines at a byte boundary are discarded rather than misattributed.
- `tests/passive_rx/test_bwp_runner.py`, SHA256
  `87cc1c0eaa87230bda22d247ad26cdfb1e50d088d2347bf5a84c23e24ce3b5d4`: 14 offline tests include
  fake-clock short-dwell, process-death and stalled-log checks (no real wait), stale pre-trigger-only
  CORESET rejection, positive post-boundary reacquisition/CRC evidence, and a missing post-trigger
  CRC-delta rejection. Existing blind-config, alternating exact-300 matrix, telnet, ownership,
  cleanup, and pin tests remain.
- Test-first RED: the 14-test suite against the pre-fix runner failed exactly 3 tests: missing
  monitored dwell API (2 errors) and old scoring boundary API (1 error). Log on sens6
  `/tmp/gap-bwp-supervision-red.log`, SHA256
  `ad5949425dbce98c3eddc5aeffde5b2d2f592fbf3fc0ef10291661149226d85c`.
- GREEN: 14/14 pass, log on sens6 `/tmp/gap-bwp-supervision-green.log`, SHA256
  `77a06a80e073bb4f12f1b4463ebeebb61c7fe3cc93eca4531bf594fa6e58520d`. `py_compile` passes for
  runner and tests; `git diff --check` passes. Blind/no-sensing config is unchanged, SHA256
  `301486494ed6f168d551a5cb21384768979787253aef6c1acc08d78e3179d577`.

This remains scaffolding only. No live behavior is inferred from fake-clock tests; telnet/BWP
reacquisition and pre/post CRC evidence still require authorized live B/S x3. G1-G6 remain
outstanding; do not build or launch radio without coordinator authorization.

## G5 review round 2 — required executable identity — 2026-09-27

Added the required role-specific process identity check to the monitored capture path. Each owned
launch now claims an expected executable PID and `/proc` start tick within its own process group;
every dwell poll requires that exact claimed PID/start-tick/executable tuple to remain present. For
the sudo-launched active UE, the required executable is the pinned `nr-uesoftmodem` binary; a still-
alive `sudo` leader alone no longer satisfies ownership. Other groups claim their exact expected
executable as well. Missing/replaced identity raises from the monitor, and `run_arm` writes
`VOID.json` before its owned-only cleanup.

Test-first falsifier: with the sudo leader PID/start tick still valid and group membership intact,
but no claimed `nr-uesoftmodem` child, the new test failed against the prior runner because no
exception was raised. RED log on sens6 `/tmp/gap-bwp-executable-red.log`, SHA256
`ebec9deb17461284464743e233f5747dbeb7b6c4e6e2d06aad17451f3abd4966`. GREEN log
`/tmp/gap-bwp-executable-green.log`, SHA256
`cce2dee1e154446dbe0bf5a528ade725c4e086f257539939f54f4b63d464c006`: 15/15 tests pass, including
the test that verifies the failure is persisted as `VOID.json`. `py_compile` for runner and tests
and `git diff --check` pass.

Current runner SHA256 `fefcfd78a8105d9ec5e892c1a4ebff8b3f0773c905cae649c97f33a5c3fe4b9c` and test SHA256
`a107b85da43fbc1015efb1c5bb15a07024d25f1f040382f1d6f21b968cd53c12`. The no-sensing config remains
unchanged at `301486494ed6f168d551a5cb21384768979787253aef6c1acc08d78e3179d577`. The worktree is
still at `239eb144acd398968d5869a75e083e540ddefa26`, with only the three intended untracked lane
files and no tracked base changes. No build, lock, radio, commit, handover, or progress edit occurred.
This proves only the monitor's falsifiable identity contract, not live sudo behavior or BWP
reacquisition; G1-G6 and authorized live B/S x3 remain outstanding.

## G5 review round 3 — end-to-end VOID test linkage — 2026-09-28

Replaced the prior direct-helper VOID assertion with an end-to-end mocked `run_arm("switch", 2,
300, ...)` test. It launches five fake owned roles, keeps the sudo leader's PID/start tick and
process-group membership valid while omitting its claimed `nr-uesoftmodem` child, then lets the real
`MonitoredDwell` callback invoke `assert_owned_processes_alive`. The test asserts `run_arm` itself
raises, writes `rep2_switch/VOID.json` with the required-executable failure, and runs the real
`cleanup_owned` loop with `terminate_owned` mocked to record all five owned groups. This closes the
test-linkage gap: the test no longer calls `write_void_record` itself and cannot pass solely because
that helper works.

The mocked integration test passes, and the full offline suite is 15/15. Log on sens6
`/tmp/gap-bwp-runarm-void-green.log`, SHA256
`77997021c4742aeeba12da61775be85c56f0b6e7282719f89d57aed6954c1101`. `py_compile` for runner and
tests plus `git diff --check` pass. Runner remains unchanged, SHA256
`fefcfd78a8105d9ec5e892c1a4ebff8b3f0773c905cae649c97f33a5c3fe4b9c`; test SHA256 is now
`fd88e71e08eb1e5661916fa83db5702ff851baea2e44b9eecb5bd9c429753c8a`. Worktree HEAD remains
`239eb144acd398968d5869a75e083e540ddefa26` with only the three intended untracked lane files. No
build, lock, radio, commit, receiver edit, or shared-ledger edit occurred; live behavior remains
unverified.

## ROUND 3b thin wrapper — 2026-09-28 (Claude)

ROUND 3b forbids the custom supervision runner: `run_bwp_matrix.py` + `test_bwp_runner.py` (untracked,
never committed) moved out of the worktree to `sens6:/tmp/gap-bwp-retired/`. Replaced by
`tests/passive_rx/run_bwp_switch.sh` (~60 lines bash): exports `GNB_CONF_OVERRIDE=gnb.sa.rfsim.bwp.conf`,
`CONF_TAG=.bwp.agn`, `NUM_RX=1 NUM_UE=1`, `ISAC_BWP_TRACK=1`, `GNB_EXTRA+=--telnetsrv --telnetsrv.listenport
$PORT --telnetsrv.shrmod ci`; runs the UNCHANGED `run_passive_rx.sh`; at `TRIGGER_AT` records the passive
log byte offset, `ci get_current_bwp`, (switch mode) `ci trigger_bwp_switch 2`, `ci get_current_bwp`
(telnet via bash /dev/tcp, one command per connection); then greps pre/post evidence into
`bwp_summary.txt` (switch/new/resolved post-trigger counts, crc_ok pre/post, per-RNTI census pre/post).
Test `tests/passive_rx/test_run_bwp_switch.py` (fake runner + fake telnet server): RED 2 failures
(wrapper absent, `/tmp/gap-bwp-thin-red.log`), then RED 1 failure for the added `ISAC_BWP_TRACK=1`
assertion (`/tmp/gap-bwp-thin-red2.log`), GREEN 3/3 (`/tmp/gap-bwp-thin-green2.log`). SHA256: wrapper
7f950a01…, test b0410ed6…, conf unchanged 30148649….

Bed finding (source): a `--phy-test` gNB CANNOT execute a telnet BWP switch. `nr_trigger_bwp_switch()` ->
`nr_mac_trigger_reconfiguration()` calls `du_get_f1_ue_data(rnti)`, which dereferences the hashtable
entry with no NULL check (`f1ap_ids.c:142`); the phy-test UE is added by `nr_mac_add_test_ue()` with no F1
UE data, and the switch is only applied after an RRC Reconfiguration Complete the phantom UE never sends.
So live G4 needs the OAI SA bed with a core (sensnuc3, `run_passive_rx.sh`), not sens6 phy-test.
