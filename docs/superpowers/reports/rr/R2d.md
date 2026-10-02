# R2d — bounded wide DCI probing

Host: DGX aarch64. Branch: `rr/reconfig-robustness`; starting HEAD `e10ddbd237`.
Scope: the measured R2b/R2c blind-PDCCH cost regression; no live measurements run here.

## Regression evidence supplied by the orchestrator

[DGX aarch64, rfsim 106 PRB, CPU decoder unless stated, flags off, quiet machine,
1 x 150 s each, measured by orchestrator; SIM VERIFIED on the listed revisions]
These are supplied measurements, not new R2d results.

| Revision / arm | drop_full jobs | drop_full | PDCCH occasions |
|---|---:|---:|---:|
| pre-R2 `26a217bbf2` | 144–161 | 0.04% | ~583–647k |
| Phase 1 `d1e3e79c3e`, default | 6265–15053 | 1.7–3.7% | ~471k |
| Phase 1 + `ISAC_DCI_LEN_MAX=63` | 116–369 | 0.03–0.10% | ~604–634k |
| Phase 1, `NR_GPU_POLAR=1` | not supplied | ~9% | not supplied |
| pre-R2, GPU polar | not supplied | 1.1–1.5% | not supplied |

[KNOWN ISSUE, orchestrator evidence plus source inspection] Unresolved RNTI/geometry
contexts exhausted 256 trials at each stage-1 length, then permanently searched
111 lengths instead of 34 (`111/34 = 3.2647`). DL, UL and lookahead batching paid
that cost. The live acceptance gate remains DGX drop_full <=1%; R2 CPU cold-sweep
latency (first C-RNTI to first bank add) must stay <=1.25x pre-R2.

## Changes and decisions

[OFFLINE VERIFIED, tests below] `nr_pdcch_dci_length_sweep.{c,h}` replaces the
exhaustion-induced `wide_range` latch with `stage_one_exhausted`. The contiguous
active range remains 30..63. Exhausted contexts append at most one round-robin
length from 64..140 on a probe occasion. Wide results accumulate the same payload,
RNTI recurrence, bootstrap and statistical evidence as narrow results. A passing
normal gate calls `note_seen`, enabling ordinary wide searches thereafter. No
polar payload representation, encoding, decoding or acceptance thresholds changed.

[OFFLINE VERIFIED] `ISAC_DCI_WIDE_PROBE_EVERY` is cached with the other length
settings; default **1**, valid positive integers through INT_MAX, malformed/zero/
negative values fall back to 1. It is a minimum interval, increased automatically
for rotated or preferred-only sweeps. With W narrow lengths, stride S clamped to
1..W, let B=floor(W/S); conservatively B=1 for preferred/exclusion sweeps.
The effective interval is `N=max(configured_N, ceil(20/B))`. With equal candidate
counts the added decode fraction is <=`1/(N*B)` <=5% (amortized over probe periods).
For the default W=34, S=1: **35T versus 34T = +2.9412%**, even though one probe
runs on every exhausted occasion. At S=8, N=5: +8/(34*5)=4.7059%; at S=34, N=20:
+5%. GPU prefill offers 34 lengths plus zero/one, so its added item fraction is
at most 1/34 per occasion. This choice avoids waiting thousands of occasions
within a geometry's existing 500-round dwell. Setting N=32 is supported but slows
wide discovery. These are work-count bounds, not CPU-time measurements; wider
polar shapes can cost more per decode.

[OFFLINE VERIFIED] Probes run before narrow work to prevent deadline starvation.
They respect the existing scorer-call/deadline budget and leave narrow rotation
and resume cursors intact. A partially processed probe does not carry deferred
work into another occasion; subsequent probe occasions advance round-robin.
Seen-wide hints, explicit `ISAC_DCI_LEN_MAX`, and R3 SUSPECT re-lock retain full
widening (a suspended round finishes first, as in R2c). Runtime SUSPECT remains
behind `ISAC_RECONF=1` and its existing re-lock budget is unchanged.

[OFFLINE VERIFIED for shared selection; runtime integration compile-verified]
`nr_pdcch_blind_monitor_rt.c` uses the same pure batch-length selector for DL/UL
prefill and lookahead batches; sparse lane-cache columns retain O(1) lookup.
Lane preparation now selects the actual per-RNTI/anonymous geometry context,
instead of the legacy lane state that phase B did not necessarily score.
The selected RNTI is carried into phase B. Other consumers can advance a context
between preparation and scoring; cache misses retain the existing CPU fallback.
UL passes the configured capacity to feed and includes accumulated probe evidence
in its uniqueness check. The existing summary gains **`dci_wide_probes`**, a
process-cumulative atomic count of context probe occasions with at least one
scorer call (including cached results). No existing key is renamed.

[OFFLINE VERIFIED] `tests/nr_pdcch_dci_length_sweep_test.cc` adapts the stage-1
exhaustion expectation and covers the four requested tests:

- `ExhaustedStage1ProbesWideAtLowDuty`: K=20*34*77 occasions, strides 1/8/34,
  decode count <=1.05x narrow-only; length 140 is reached and active_max stays 63.
- `WideProbeFindsLength100`: no seen hint or bootstrap; two candidates/occasion,
  varying payloads, five distinct-occasion RNTI votes. Stage 1 takes 128 occasions;
  100 is probe 37, repeated every 77. Bound **128+37+4*77=473 occasions**, within
  the existing 500-round dwell. No contiguous widening occurs before the lock.
- `SeenWideLengthWidensImmediately`: the existing wide-hint fixture is renamed
  to the requested name and still exercises immediate cold wide scoring.
- `LanesNeverBatchFullWideRangeWithoutEvidence`: exercises the production batch
  selector for all 77 wide lengths; every scorer request is in its snapshot,
  which contains only 34 or 35 lengths.

[OFFLINE VERIFIED] Additional tests cover N=32, the probe counter, and seven-call
budget suspensions with stride 3. Existing R2 wide/narrow real-polar fixtures,
R2b/R2c range/order tests, and R3/R4 context tests pass.

## Validation

[OFFLINE VERIFIED, DGX aarch64, 2026-10-02] Every build/test invocation first ran:

```bash
while [ -e /home/nicola/NICOLA/wt/rr-orchestration/HOLD ] || [ -e /home/nicola/NICOLA/wt/rr-orchestration/BUSY ]; do sleep 60; done
```

[OFFLINE VERIFIED] Red: built `test_nr_pdcch_dci_length_sweep` and ran
`--gtest_filter='*ExhaustedStage1ProbesWideAtLowDuty:*WideProbeFindsLength100'`.
One failed (old cost 3.2647x), one passed (old full-wide search could lock 100).
Logs `/tmp/rr-r2d-red-build.log`, `/tmp/rr-r2d-red.log`.

[OFFLINE VERIFIED] Commands (each preceded by the gate above):

```bash
B=cmake_targets/ran_build/build
CCACHE_DIR=/tmp/rr-r2d-ccache nice -n 19 ninja -C "$B" -j8 test_nr_pdcch_dci_length_sweep test_nr_pdcch_blind_monitor
CCACHE_DIR=/tmp/rr-r2d-ccache nice -n 19 ninja -C "$B" -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
nice -n 19 "$B/test_nr_pdcch_dci_length_sweep"
nice -n 19 "$B/test_nr_pdcch_blind_monitor"
ISAC_RECONF=1 nice -n 19 "$B/test_nr_pdcch_dci_length_sweep"
ISAC_RECONF=1 nice -n 19 "$B/test_nr_pdcch_blind_monitor" --gtest_filter='BlindPdcchTest.NoisePrntiShortOnlyReservedFieldsNeverArm:Sib1Cache.*:EpochFeedback.*'
nice -n 19 ctest --test-dir "$B" -j4 --output-on-failure
```

[OFFLINE VERIFIED] Required targets built (14 steps, then 8 after the final
cadence/cache-alignment edits). Length **48/48**, blind **208 passed, 5 skipped**;
reconf length **48/48**, reconf-only blind filter **3/3**. The remaining two skips
need captured IQ. Full CTest ran **once: 127/135 passed**, 27.95 s. Eight failures
are exactly documented exceptions: ARM `test_nr_pusch_ra0_qam256`, `dft_test`,
`test_nr_modulation`; sandbox `time_management_tests`, `test_gtp`, `test_vrtsim`,
`test_vrtsim_cirdb`, `nr_cuup_functional_test`. No new failure.
Logs: `/tmp/rr-r2d-{target-build,full-build,full-build-final,length-final,blind,length-reconf,blind-reconf,ctest}.log`.

[OFFLINE VERIFIED] From the build directory, both touched test binaries ran
sequentially with `nice -n 19 ./<binary> --gtest_shuffle --gtest_random_seed=<seed>`
for seeds **1/3/5**, gated before each invocation. **6/6 invocations passed**:
length 48 and blind 208 passed + 5 skipped per seed; total **768 passed, 15 skipped,
zero failed**. Logs `/tmp/rr-r2d-shuffle-<binary>-<seed>.log`.
`git diff --check` and the sens6 frozen gate passed before commit. No push.

## DEFERRED MEASUREMENTS

[PLANNED] Orchestrator only, outside the sandbox in a reserved quiet DGX slot.
No rfsim, OTA, simulator campaign, GPU build/test or benchmark ran in this task.
Prepare one GPU-capable R2d build; both CPU and GPU-polar arms use that binary
with CPU LDPC. Before **each** configure/build/test, use the HOLD/BUSY gate above.
The orchestrator owns BUSY during each live bed and releases it after the bed;
do not run this task's builds/tests concurrently with those measurements.

```bash
R=/home/nicola/NICOLA/wt/rr-robust
G="$R/cmake_targets/ran_build/build_gpu_r2d"
# Gate, configure; gate, build; gate, quick correctness (separate invocations).
cmake -S "$R" -B "$G" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON -DENABLE_ISAC_SENSING=ON -DOAI_USRP=ON \
  -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121 -DCMAKE_CUDA_ARCHITECTURES=121 \
  -DCPM_SOURCE_CACHE=/tmp/rr-r1-cpm
CCACHE_DIR=/tmp/rr-r2d-ccache nice -n 19 ninja -C "$G" -j8 \
  nr-uesoftmodem nr-softmodem rfsimulator params_libconfig polar_gpu nr_polar_sc_cuda_test
(cd "$G" && nice -n 19 ./nr_polar_sc_cuda_test quick)

# CPU: two 150-second, cold, 106-PRB arms with the default probe policy.
env -u ISAC_RX_BRANCH_FO -u ISAC_DCI_LEN_MAX -u ISAC_DCI_LEN_MIN \
  -u ISAC_DCI_WIDE_PROBE_EVERY -u ISAC_DCI_SWEEP_STRIDE -u NR_GPU_FEP \
  ISAC_RECONF=0 NR_GPU_POLAR=0 BUILD="$G" \
  LD_LIBRARY_PATH="$G${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r2d-after-cpu \
  bash "$R/tests/passive_rx/dgx/rfsim_regress.sh" 2

# GPU polar: same two arms, same binary and CPU LDPC.
env -u ISAC_RX_BRANCH_FO -u ISAC_DCI_LEN_MAX -u ISAC_DCI_LEN_MIN \
  -u ISAC_DCI_WIDE_PROBE_EVERY -u ISAC_DCI_SWEEP_STRIDE -u NR_GPU_FEP \
  ISAC_RECONF=0 NR_GPU_POLAR=1 BUILD="$G" \
  LD_LIBRARY_PATH="$G${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r2d-after-gpu \
  bash "$R/tests/passive_rx/dgx/rfsim_regress.sh" 2
```

[PLANNED] Repeat those exact two arm commands using the orchestrator's isolated
GPU-capable **pre-R2 `26a217bbf2` build** for G and output directories
`/tmp/rr-r2d-before-cpu` and `/tmp/rr-r2d-before-gpu`. Keep all other controls,
core assignments and configs identical. Preserve build commit IDs and raw logs;
verify the GPU plugin actually loads in GPU arms. The harness already scores
CONVERGED, CRC >=98%, and drop_full <=1%. Compare cold sweeps separately:

```bash
python3 - <<'PY'
import json, re, statistics, sys
from pathlib import Path
sys.path.insert(0, '/home/nicola/NICOLA/wt/rr-robust/tests/passive_rx/dgx')
from score_rx import score, ANSI
means = {}
for revision in ('before', 'after'):
    for backend in ('cpu', 'gpu'):
        times = []
        for run in (1, 2):
            arm = Path(f'/tmp/rr-r2d-{revision}-{backend}/base_r{run}')
            s = score(str(arm))
            lines = [ANSI.sub('', x) for x in (arm/'rx/rx.log').read_text().splitlines()]
            bank = next((float(x.split()[0]) for x in lines
                         if re.search(r'bank add .*len=\d+', x)), None)
            first = s['first_crnti_s']
            assert first is not None and bank is not None and bank >= first, arm
            times.append(bank-first)
            print(json.dumps(dict(arm=str(arm), first_crnti_s=first,
                                  bank_add_s=bank, cold_s=bank-first, score=s)))
            summaries = [x for x in lines if 'blind PDCCH monitor summary:' in x]
            print(summaries[-1] if summaries else 'MISSING SUMMARY')
        means[revision, backend] = statistics.mean(times)
for backend in ('cpu', 'gpu'):
    assert means['before', backend] > 0, 'need finer timestamps for zero baseline'
    ratio = means['after', backend] / means['before', backend]
    print(backend, 'cold_s after/before', ratio)
    assert ratio <= 1.25 if backend == 'cpu' else ratio < 1.0
PY
```

## Open limits

[IMPLEMENTED, NOT VALIDATED on live traffic/GPU] The live drop_full and cold-time
gates remain unmeasured for R2d. Batch selection is offline-tested; actual GPU
execution and CPU/GPU cache handoff require the deferred arms. Synthetic 473
occasions assumes two candidates with real varying 100-bit evidence on every
probe; intermittent traffic, larger configured N, preferred-length scheduling,
rotation, deadlines or context eviction can exceed the existing discovery dwell.
No universal RF acquisition-time guarantee or new OTA result is claimed.
