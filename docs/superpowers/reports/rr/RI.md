# RI — integrate levers and reconfiguration robustness

Host: DGX Spark, aarch64; worktree `/home/nicola/NICOLA/wt/rr-int`, branch `rr/integration`.

[CODE-READ] Merge direction: first parent `4d98184411` (`adaptive-rx-UL-DL`, merged levers), second parent `7703e9cf02c1462d07df372d91ba9bbeca89148d` (`rr/reconfig-robustness`). All integration fixes are included in the merge commit; no refactoring, next-phase work, push, or edits to other worktrees/specifications/plans.

## Conflict resolutions

All paths below are under `openair1/PHY/NR_UE_TRANSPORT/` unless qualified.

| Conflict | Resolution and evidence |
|---|---|
| `CMakeLists.txt` | [CODE-READ] Keep all levers sources and targets plus the epoch library, rr sources/tests, pin/bootstrap dependencies. Add the epoch link to levers-only standalone targets compiling the sweep/history (`test_nr_dci_history`, `test_nr_td_legal`, `test_nr_td_cb0_wire`, `nr_td_sim`, `nr_td_sim_test`). Register three flag-on RI fixtures. |
| `executables/nr-ue.c` | [CODE-READ] Retain the levers fieldbook discontinuity call and all rr includes/identity hooks. Add the R8c signed-sample gap notification behind the existing passive/reconf guard. The local fieldbook epoch remains independent: R11 owns central mirroring, so K48 remains open. Unset/0 retains the exact levers discontinuity action; the central epoch classifies short/long/backward gaps as in R8c. |
| `nr_passive_metrics.c` | [CODE-READ] Collect levers census, fieldbook, CB0 and GPU counters together with all rr queue/inline epoch and CSI-RS counters; retain `pdschq_stale_after_decode` separately from `pdschq_drop_epoch`. Enlarge the emit buffer for the combined schema. |
| `nr_passive_metrics.h` | [CODE-READ] Union of both parents' fields; no removals or renames. |
| `nr_passive_metrics_json.c` | [CODE-READ] Union of format strings and matching argument order, retaining the CB0 object and rr variable-length CSI-RS resource array. Keep mutable `w` for array appends. The CSI-RS observer fixture also needs the emitter's 8192-byte buffer (its rr-only 2048-byte buffer no longer fits the union). Static key-set comparison: 89 distinct literal keys on levers, 59 on rr, 115 in the union, zero missing. |
| `nr_pdsch_config_sweep.c` | [CODE-READ] Retain levers `k0 = k0_plausible` and rr provenance checks before and after the observation mutex; retain every levers mode and rr feedback guard. |
| `nr_pdsch_passive_queue.c` | [CODE-READ] Union includes and counters. Keep K33 IQ lifetime checks plus R9/R8b epoch checks and R18b in-place slot-group compaction gated by `nr_cfg_reconf_enabled()`. Retain GrantWork/CB0 execution and their cleanup. |
| `nr_pdsch_passive_queue.h` | [CODE-READ] Keep both `dci_abs_slot` and `config_epoch`, and both `stale_after_decode` and `dropped_epoch`. |

## Semantic integration

- [CODE-READ] **Admission and dequeue:** PDCCH/PDSCH/PUSCH admission preserves `nr_cfg_epoch_work_stamp()` rather than assigning a new epoch to old work. Each PDSCH group member binds its own stamp and `g_dropped_epoch`; nested synchronous work shares that owner's once-only drop count. R18b's filtering remains flag-on-only. No extra whole-group copy was introduced.
- [IMPLEMENTED, NOT VALIDATED on live traffic] **GrantWork — added:** its runtime context now retains the admitting PDSCH job's epoch. Creation refuses an already stale job; lazy geometry computation and both CB0 extraction entry points bind that original epoch and check it before work and after LLR retrieval. Main TB outcomes still reach R8b's guarded decoder/sweep consumers and the queue's completion check. The stale completion exit now ends/releases GrantWork; previously the clean merge skipped the levers cleanup on that exit. The generic immutable-buffer/refcount API is unchanged.
- [IMPLEMENTED, NOT VALIDATED on real GPU/live traffic] **CB0 CPU/GPU batches — added:** the per-thread plan stamps the owning PDSCH epoch at `wire_pre`; `wire_run` checks before building/submitting, immediately after synchronous backend return (including completion of the asynchronous GPU entry), and `wire_feed` checks again before evidence. The retained GrantWork reference is released on a batch straddle. The CPU/GPU worker implementations produce buffers/results; learned-state credit occurs on the original queue thread under its provenance scope. No stale result is relabelled on return.
- [IMPLEMENTED, NOT VALIDATED on live traffic] **CB0 elimination/decoder dominance — added:** guard `feedback_cb0` (also the empty-grant TB-decoder note) and `with_context` (adapter active-set access and elimination feed), both before and after acquiring `g_lock`. Guard the adapter's premise evaluation before it can alarm on an old result. Existing index/hypothesis revalidation and decoder dominance rules remain intact.
- [CODE-READ] **Fieldbook writes:** promotion/withdrawal/fail-open in `nr_pdsch_config_sweep_feedback` already sits behind R8b's entry and post-lock checks. [IMPLEMENTED, NOT VALIDATED on live traffic] Add another check after the optional reporter, before first-convergence prior/fieldbook publication. Selection/VERIFY also rejects stale queued PDCCH work, including a bump while allocating a new catalogue outside the sweep mutex. This is rejection of stale job evidence, not an R10 consumer reset or R11 epoch-mirroring policy.
- [IMPLEMENTED, NOT VALIDATED on live traffic] **BC9 evidence — added:** guard the RT accept hook and history insertion/confirmation, including post-history-lock checks. Guard the exclusion driver, sweep k0 certification, per-key exclusions and DCI-phase evidence, plus the queue's confirmation/certified-pass helpers and their cache writes. Both inline and queued decoding inherit the owning scan/PDSCH scope, so rejected work counts against the corresponding existing epoch-drop metric.
- [CODE-READ] **BC9 with R3/R6:** the hook still records accepted C-RNTI DL DCIs before grant drops, unconfirmed until TB CRC feedback; no accept-time hard exclusion was reintroduced. Both enqueue sites still carry the original wrapped DCI slot used by the history. R3 re-lock/second-length/scout gates, R6 low-duty discovery and occupancy handoff, and R2d bounded wide-probe/cache selection remain in place. No SA/NSA gate was added to optional evidence.
- [CODE-READ] **R3 reset meaning:** `nr_pdsch_config_sweep_reset_all()` still exists and is called on a genuine changed-length RELOCK. It invalidates all context generations without rewinding their source, clears per-RNTI priors/observations/certifications, advances the constraint-cache epoch, and additionally clears the levers fieldbook. Thus fb2/VERIFY cannot immediately reuse cleared evidence. The reset remains global, including unrelated RNTIs, as documented by R3/R6b.
- [CODE-READ] **Defaults:** GrantWork ON, CB0 elimination ON, fieldbook=2, CB0 backend auto and TB CPU while acquiring retain their main-side defaults and rules. All newly added rejection predicates use `nr_cfg_epoch_work_current()` (a no-op with `ISAC_RECONF` unset/0) or the inherited gated epoch owner. R2d's existing wide-capacity/probe behavior is retained. Offline validation does not establish live timing or byte-identical logs.

## Validation

[OFFLINE VERIFIED, DGX aarch64] Every CMake/Ninja/test invocation waits for BUSY to clear, then takes its own shared lock. The commands below use `/tmp/ri-run-locked`, whose complete body is:

```bash
#!/bin/bash
set -eu
while [ -e /home/nicola/NICOLA/wt/rr-orchestration/BUSY ]; do sleep 5; done
exec flock -s -w 7200 /tmp/td_measure.lock nice -n 19 "$@"
```

[CODE-READ] The orchestration warm-build status file existed (`rc=1`) before the first RI Ninja. The warm builder had reached integration test linkage; RI added the missing epoch dependencies listed above. CMake regeneration initially could not create a lock in the read-only user CPM cache. Copying that existing cache into `/tmp/rr-ri-cpm` and resetting its cached CPM directory references allowed offline regeneration (no repository build-system workaround):

```bash
cp -a /home/nicola/.cache/cpm /tmp/rr-ri-cpm
/tmp/ri-run-locked env CCACHE_DIR=/tmp/rr-ri-ccache cmake -S . \
  -B cmake_targets/ran_build/build -U 'CPM_*' -DCPM_SOURCE_CACHE=/tmp/rr-ri-cpm
```

[OFFLINE VERIFIED] Red fixtures, before the sweep/wire guards: `PdschEpoch.LeversFeedbackDropsOnceAcrossBump` failed (stale CB0/callback/certification accepted); both `Cb0Wire.Epoch*` fixtures failed (batch ran or credited across a bump). The existing reset contract fixture passed. Logs: `/tmp/rr-ri/red-{build,sweep,wire}.log`.

```bash
/tmp/ri-run-locked env CCACHE_DIR=/tmp/rr-ri-ccache ninja -C cmake_targets/ran_build/build -j8 \
  test_nr_pdsch_config_sweep test_nr_td_cb0_wire
/tmp/ri-run-locked env ISAC_RECONF=1 cmake_targets/ran_build/build/test_nr_pdsch_config_sweep --gtest_filter='PdschEpoch.*'
/tmp/ri-run-locked env ISAC_RECONF=1 cmake_targets/ran_build/build/test_nr_td_cb0_wire --gtest_filter='Cb0Wire.Epoch*'
```

[OFFLINE VERIFIED] Green focused CTest: 4/4 registrations passed, including metrics and all three RI registrations (six RI fixtures: stale sweep/fieldbook, fieldbook reset, catalogue-allocation straddle, pre-batch drop, GPU-backend straddle, stale DCI history). `/tmp/rr-ri/focused.log`:

```bash
/tmp/ri-run-locked env CCACHE_DIR=/tmp/rr-ri-ccache ninja -C cmake_targets/ran_build/build -j8 \
  test_nr_pdsch_config_sweep test_nr_td_cb0_wire test_nr_dci_history test_nr_passive_metrics
/tmp/ri-run-locked ctest --test-dir cmake_targets/ran_build/build -j4 \
  -R 'test_nr_(levers_epoch_.*|passive_metrics)$' --output-on-failure
```

[OFFLINE VERIFIED, DGX aarch64] Required target build succeeded (9218 warm-rebuild steps, followed by 26 incremental steps for the last audit changes). CPU-only, `ENABLE_ISAC_SENSING=ON`; existing unrelated warnings remain. Logs: `/tmp/rr-ri/full-build.log`, `/tmp/rr-ri/full-build-final.log`.

```bash
/tmp/ri-run-locked env CCACHE_DIR=/tmp/rr-ri-ccache ninja -C cmake_targets/ran_build/build -j8 \
  nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
/tmp/ri-run-locked ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
```

[OFFLINE VERIFIED] One full CTest run completed in 1615.05 seconds: **140/150 passed, 10 failed**. Nine are the documented host failures: ARM `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`; sandbox sockets `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. The tenth, the CSI-RS fixture capacity mismatch, was fixed and rerun successfully below; **only the nine known failures remain**. The full suite was not repeated. Complete log: `/tmp/rr-ri/ctest.log`.

[OFFLINE VERIFIED] The full run exposed `CsirsObserver.CounterPlumbing`'s rr-only 2048-byte JSON buffer. A standalone build of the **unchanged rr parent** fixture, observer and serializer passed that case (1/1), establishing an integration failure. Increased only the fixture buffer to 8192, matching the merged runtime emitter; targeted CTest then passed 1/1 (10 cases). The maximum-width merged-schema fixture also passes, with all 16 CSI-RS resource entries. Logs: `/tmp/rr-ri/observer-{baseline,build,green}.log`. Baseline files were extracted with `git show rr/reconfig-robustness:openair1/PHY/NR_UE_TRANSPORT/<path>` into `/tmp/rr-ri/observer-baseline`; exact build/run commands:

```bash
/tmp/ri-run-locked cc -I /tmp/rr-ri/observer-baseline -c /tmp/rr-ri/observer-baseline/nr_csirs_observer.c -o /tmp/rr-ri/observer-baseline/observer.o
/tmp/ri-run-locked cc -I /tmp/rr-ri/observer-baseline -c /tmp/rr-ri/observer-baseline/nr_passive_metrics_json.c -o /tmp/rr-ri/observer-baseline/json.o
/tmp/ri-run-locked c++ -I /tmp/rr-ri/observer-baseline \
  -I /tmp/rr-ri-cpm/googletest/a36d6fcbd7356d396ea479e6b11f4b9760ad0c04/googletest/include \
  /tmp/rr-ri/observer-baseline/nr_csirs_observer_test.cc /tmp/rr-ri/observer-baseline/observer.o \
  /tmp/rr-ri/observer-baseline/json.o cmake_targets/ran_build/build/lib/libgtest_main.a \
  cmake_targets/ran_build/build/lib/libgtest.a -pthread -o /tmp/rr-ri/observer-baseline/test_observer
/tmp/ri-run-locked /tmp/rr-ri/observer-baseline/test_observer --gtest_filter=CsirsObserver.CounterPlumbing
/tmp/ri-run-locked env CCACHE_DIR=/tmp/rr-ri-ccache ninja -C cmake_targets/ran_build/build -j8 test_nr_csirs_observer
/tmp/ri-run-locked ctest --test-dir cmake_targets/ran_build/build -j4 -R '^test_nr_csirs_observer$' --output-on-failure
```

[OFFLINE VERIFIED] **111/111 shuffle invocations passed, zero failed**, across 32 binaries plus five additional flag-on invocations per seed. Seeds **1 / 3 / 5** each passed **37/37 invocations, 1085 cases**, with **15 skips**: total **3255 case passes, 45 skips**. Each seed's skips comprise eight flag-on-only cases in the default-off runs (all exercised by the additional flag-on runs), three CUDA-only CB0 cases, two optional replay/capture cases, the reset timing benchmark, and the external v1 simulator reference comparison. Golden simulator fixtures still run; no benchmark or missing capture was substituted. Logs: `/tmp/rr-ri/shuffle-<binary>-<seed>[-reconf].log`; complete ledger `/tmp/rr-ri/shuffle-results.json`, summary `/tmp/rr-ri/shuffle.log`.

Exact shuffle runner: `python3 /tmp/rr-ri/shuffle.py > /tmp/rr-ri/shuffle.log 2>&1`.
The script below is its complete content; each child waits for BUSY and takes its own shared lock through the wrapper above. Three children maximum, plus the one remaining full-CTest registration, keep total test concurrency at four. These are unit tests (including `nr_td_sim_test`), not simulator campaigns. CUDA-only binaries are not available in this CPU build.

```python
import concurrent.futures, json, os, pathlib, re, subprocess
root = pathlib.Path('/home/nicola/NICOLA/wt/rr-int')
build = root/'cmake_targets/ran_build/build'
out = pathlib.Path('/tmp/rr-ri')
binaries = '''test_nr_dci_bits test_nr_pdcch_blind_monitor test_nr_pdcch_dci_length_sweep test_nr_pdcch_coreset_bank test_nr_pdcch_blind_rnti_bootstrap test_nr_pdcch_dci11_layout_sweep test_nr_pdcch_dci01_layout_sweep test_nr_passive_bwp test_nr_pdcch_joint_solve test_nr_pdcch_joint_live test_nr_passive_cfg_epoch test_nr_passive_job_epoch test_nr_passive_metrics test_nr_csirs_observer test_nr_csirs_blind_search test_nr_dci_history test_nr_passive_acq_state test_nr_passive_sample_lifetime test_nr_pdsch_chest_key test_nr_pdsch_config_sweep test_nr_td_gate test_nr_td_order test_nr_td_legal test_nr_td_grantwork test_nr_td_fieldbook test_nr_td_cb0_batch test_nr_td_cb0_wire test_nr_llr_norm test_nr_tdd_pattern nr_td_sim_test openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_mib_blind_handoff openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_ra_procedures'''.split()
extra = [
 ('test_nr_pdsch_config_sweep', 'PdschEpoch.*'),
 ('test_nr_td_cb0_wire', 'Cb0Wire.Epoch*'),
 ('test_nr_dci_history', 'DciHistoryEpoch.*'),
 ('test_nr_pdcch_dci_length_sweep', '*'),
 ('test_nr_pdcch_blind_monitor', 'Sib1Cache.*:EpochFeedback.*:BlindPdcchTest.NoisePrntiShortOnly*:BlindPdcchTest.Dci10PRntiShortMessageOnly*')]
jobs = []
for seed in (1, 3, 5):
 for binary in binaries:
  jobs.append((binary, seed, False, '*'))
 for binary, filt in extra:
  jobs.append((binary, seed, True, filt))

def run(job):
 binary, seed, reconf, filt = job
 exe = build/binary
 tag = f'{exe.name}-{seed}' + ('-reconf' if reconf else '')
 log = out/f'shuffle-{tag}.log'
 envargs = ['env', '-u', 'ISAC_RX_BRANCH_FO', '-u', 'ISAC_RECONF']
 if reconf: envargs += ['ISAC_RECONF=1']
 cmd = ['/tmp/ri-run-locked'] + envargs + [str(exe), '--gtest_shuffle', f'--gtest_random_seed={seed}', '--gtest_brief=1', f'--gtest_filter={filt}']
 with log.open('w') as f:
  p = subprocess.run(cmd, cwd=exe.parent, stdout=f, stderr=subprocess.STDOUT)
 text = log.read_text()
 passed = re.findall(r'\[  PASSED  \] (\d+) tests?\.', text)
 skipped = re.findall(r'\[  SKIPPED \] (\d+) tests?', text)
 result = dict(binary=binary,seed=seed,reconf=reconf,filter=filt,rc=p.returncode,passed=int(passed[-1]) if passed else None,skipped=int(skipped[-1]) if skipped else 0,log=str(log))
 print(json.dumps(result), flush=True)
 return result

with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
 results = list(pool.map(run,jobs))
(out/'shuffle-results.json').write_text(json.dumps(results,indent=2)+'\n')
print('TOTAL',len(results),'PASS',sum(r['rc']==0 for r in results),'FAIL',sum(r['rc']!=0 for r in results),flush=True)
raise SystemExit(any(r['rc']!=0 for r in results))
```



[OFFLINE VERIFIED] The sens6 frozen-data gate and `git diff --cached --check` passed before the merge commit. The first parent remains merged levers main; all semantic fixes are in the single merge commit.

```bash
git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30
git diff --cached --check
```

## DEFERRED MEASUREMENTS

[PLANNED] Orchestrator only, idle DGX, exclusive lock per 420-second arm. These commands preserve the 4-RX, 106-PRB merged-main defaults, disable branch FO, and use the post-convergence gate explicitly. Repeat the pair at least five times in alternating order with distinct output directories; neither command was run by RI. The built RI directory is CPU-only, so `auto` uses its CPU fallback; a real CUDA full-combination gate requires the orchestrator's GPU-enabled build of this same commit.

```bash
cd /home/nicola/NICOLA/wt/rr-int
# Flag off: ISAC_RECONF is unset, all fastest-combination levers enabled.
flock -x -w 7200 /tmp/td_measure.lock env -u ISAC_RX_BRANCH_FO -u ISAC_RECONF \
  ISAC_TD_GRANTWORK=1 ISAC_TD_CB0_ELIM=1 ISAC_TD_FIELDBOOK=2 \
  ISAC_TD_CB0_BACKEND=auto ISAC_TD_TB_CPU_WHILE_ACQ=1 \
  BUILD=/home/nicola/NICOLA/wt/rr-int/cmake_targets/ran_build/build \
  CELL='-C 3319680000 -r 106 --ssb 516' RXEXTRA='--ue-nb-ant-rx 4' \
  GNBARGS='-m 9 -n 0 -M 106 -l 1' GATE_SECS=420 GATE_MODE=postconv \
  GATE_NCTX_MIN=2 GATE_REOPENS_MAX=0 GATE_POSTCONV_CRC_MIN=99.8 GATE_POSTCONV_MIN_DEC=5000 \
  GATE_TTC_MAX_TDA0=39 GATE_TTC_MAX_TDA2=77 GATE_CRC_FLOOR=93.4 GATE_DROP_MAX=2.5 \
  OUT=/tmp/rr-ri-4rx-off-r1 bash tests/passive_rx/dgx/rfsim_regress.sh 1

# Flag on: otherwise identical arm and thresholds.
flock -x -w 7200 /tmp/td_measure.lock env -u ISAC_RX_BRANCH_FO ISAC_RECONF=1 \
  ISAC_TD_GRANTWORK=1 ISAC_TD_CB0_ELIM=1 ISAC_TD_FIELDBOOK=2 \
  ISAC_TD_CB0_BACKEND=auto ISAC_TD_TB_CPU_WHILE_ACQ=1 \
  BUILD=/home/nicola/NICOLA/wt/rr-int/cmake_targets/ran_build/build \
  CELL='-C 3319680000 -r 106 --ssb 516' RXEXTRA='--ue-nb-ant-rx 4' \
  GNBARGS='-m 9 -n 0 -M 106 -l 1' GATE_SECS=420 GATE_MODE=postconv \
  GATE_NCTX_MIN=2 GATE_REOPENS_MAX=0 GATE_POSTCONV_CRC_MIN=99.8 GATE_POSTCONV_MIN_DEC=5000 \
  GATE_TTC_MAX_TDA0=39 GATE_TTC_MAX_TDA2=77 GATE_CRC_FLOOR=93.4 GATE_DROP_MAX=2.5 \
  OUT=/tmp/rr-ri-4rx-on-r1 bash tests/passive_rx/dgx/rfsim_regress.sh 1
```

## Remaining limits

[IMPLEMENTED, NOT VALIDATED on real GPU/live traffic] No receiver, rfsim/OTA bed, simulator campaign, GPU build or benchmark was run. The straddle fixture uses the real CPU CB0 decoder through a fake GPU backend that bumps the central epoch during its call; it does not validate CUDA execution. Production GrantWork cleanup/lazy PHY wiring is compile- and source-checked, not an RF replay. R10/R11 must handle already learned state and fieldbook mirroring; RI does not roll back evidence completed before a bump or implement those reactions.

[KNOWN ISSUE, inherited] K48's local fieldbook bump on SOFT gaps persists until R11. Rank >1 elimination runtime admissibility, sens6 x86/GPU validation, roughly three testable CB0 candidates per grant (K47), scan-queue drops (K45), and BC9c/BC10/BC11/BC12b remain unmodified and unmeasured here. R2d cost and R8c live continuity/false-trigger rates still require orchestrated measurements. No live equivalence or performance claim is inferred from unit tests.
