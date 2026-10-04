# R2b — R2 review fixes

Host: DGX aarch64 (`spark-74c3`). Branch: `rr/reconfig-robustness`.
Scope: R2 review on top of committed R3; no subsequent plan tasks.

## Changes and decisions

- [OFFLINE VERIFIED] `nr_pdcch_dci_length_sweep.{c,h}` starts cold contexts at
  the existing minimum through 63. A complete unseeded round without a lock
  enables the remaining range through 140 on the next round. Stride phases
  and budget resumes finish the same range/order; preferred-length-only rounds
  do not qualify as a full cold round. A valid explicit `ISAC_DCI_LEN_MAX` or
  `note_seen` above 63 bypasses the cold limit. New hints apply at the next
  round boundary if a round is already suspended. R3 SUSPECT sweeps retain
  their old-length-first full-range policy.
- [OFFLINE VERIFIED] The same engine serves primary, lookahead, anonymous,
  DL and UL contexts. `nr_pdcch_blind_monitor_rt.c` uses the active maximum
  for the UL supported-length uniqueness check (snapshotted before feeding),
  and passes it to existing primary DL/UL prefill calls. No GPU implementation
  or kernel was changed; GPU execution remains unvalidated.
- [OFFLINE VERIFIED] Ascending order is the default even with cell-seen hints.
  `ISAC_RECONF=1` enables distance-to-seen ordering; `pthread_once` reads the
  environment once. Flag tests use fresh subprocesses so shuffle order cannot
  change the cached configuration. The 100-bit synthetic lock and all six R3
  state/re-lock tests pass.
- [OFFLINE VERIFIED] `nr_polar_defs.h`, `nr_polar_init.c`,
  `nr_polar_encoder.c`, and `nr_polar_decoding_tools.c` allocate lookup rows
  from K and the pruned decoder tree/operation list from the actual N and
  information-bit pattern. The third lookup word exists only for K > 128.
  The <=64 encoder's eight-row read retains zero-filled padding. Decoder
  storage retains 32-byte alignment. Cleanup frees all owned buffers.
- [OFFLINE VERIFIED] DCI checkout/return uses direct `[A][log2(AL)]` idle lists,
  with constant-time pop/push under `PolarListMutex`; it never walks the global
  entry list. Busy consumers still own separate mutable trees. Initialization
  remains outside the mutex; busy entries prevent cleanup; cleanup clears the
  index. Tests exercise reuse, four simultaneous owners and cleanup/recreation.
  The non-DCI lookup retains its existing behavior.
- [IMPLEMENTED, NOT VALIDATED with captured IQ] `nr_passive_replay_capture.c`
  now writes v3 (successful DL controls) or v4 (also failed controls), replacing
  v1/v2. A stable header-prefix check reports the unsupported version and asks
  for recapture, even if the old file is smaller than the new header. Existing
  header/job/geometry checks remain in force.
- [OFFLINE VERIFIED] `nr_dci_bits.h` provides a shared hex formatter, covered by
  `nr_dci_bits_test.cc`. REPLAY-RAW-UL, FULLCRC, DCI_INTERPRET, UL raw sample,
  the DCI 0_1 ULDCIGT dump, and DL raw evidence use it in the replay, blind
  monitor/runtime and UL discovery sources. Widths <=64 retain their previous
  padded or unpadded lowercase hex byte-for-byte; widths >64 include every
  occupied word, high word first. Live emission is compile-verified only.

## OFFLINE VERIFIED computations — memory and round cost

[OFFLINE VERIFIED] `BlindPdcchTest.PolarAllocationSizesFollowShape` prints the
following owned-byte calculations in `/tmp/rr-r2b-blind.log`. Counts exclude
allocator metadata and shared immutable G_N/Q_0 tables. The unchanged owned
vectors (including CRC matrix and rate-matching lookup) are included in totals.
PBCH uses its fixed N=512 shape; UCI uses A=32, aggregation parameter 4
(E=64); all DCI rows use AL4, N=512.

| Entry | K | Before R2b bytes | After R2b bytes |
|---|---:|---:|---:|
| PBCH A32 | 56 | 290090 | 46122 |
| UCI A32 | 43 | 285662 | 27246 |
| DCI A32 | 56 | 289886 | 45918 |
| DCI A47 | 71 | 290426 | 69994 |
| DCI A100 | 124 | 292334 | 111758 |
| DCI A140 | 164 | 293774 | 183550 |

[OFFLINE VERIFIED] `sizeof(t_nrPolar_params)` before this fix is **284848**
bytes (red test), after **240** bytes (green test); the latter excludes the
new dynamic buffers. Pre-R2 embedded size is calculated as
`284848 - (4096-1024)*32 - (3*21-2*16)*256*8 - (1536-600)*16 = 108080`
from `088163a80c^:nr_polar_defs.h`. For A47/AL4, dynamic tables/tree/ops plus
struct total `240 + 36864 + 21600 + 5712 = 64416`; unchanged vectors add
5578, giving **69994** vs the pre-R2 **113658**. For K124 the corresponding
changed allocation is 104272, below the pre-R2 108080 embedded bytes.
These are allocation calculations, not measured RSS or timing claims.
The DCI direct index itself is `141*5*8 = 5640` process-wide bytes.

[OFFLINE VERIFIED] A cold default round previously offered `140-30+1 = 111`
lengths per candidate; stage 1 now offers `63-30+1 = 34`, matching pre-R2.
Thus before/after are **111T -> 34T** scorer calls per full round with T
candidates: **666 -> 204** at T=6, or **222 -> 68** at T=2. The new
`ColdRoundStaysNarrowAcrossStrideAndBudget` test verifies the 68 calls with
stride 3 and seven-call budget suspensions, then verifies lengths 64..140
are tried in round two. Ratio is `111/34 = 3.264705882`. At AL1, lengths
above 84 are rejected before polar decoding, so these are offered scorer
counts; for AL2 and above all lengths fit the polar rate constraint.
A true 47-bit length does not by itself imply a first-round lock: after a
round without significant evidence, widening is intentional. No live cold
acquisition latency or false-lock-rate improvement is claimed.

## Validation — DGX aarch64

[OFFLINE VERIFIED] Every build followed `pgrep -x nr-uesoftmodem` with no
receiver found, ran at nice 19 with <=8 jobs, and used
`CCACHE_DIR=/tmp/rr-r2b-ccache`. Tests run at nice 19 with <=4 workers.
`ISAC_RX_BRANCH_FO` remained unset. No receiver, simulator campaign, radio
bed, GPU build or GPU workload was launched.

[OFFLINE VERIFIED] Red tests (after correcting the subprocess test macro syntax):

```bash
B=cmake_targets/ran_build/build
CCACHE_DIR=/tmp/rr-r2b-ccache nice -n 19 ninja -C "$B" -j8 test_nr_pdcch_dci_length_sweep test_nr_pdcch_blind_monitor
nice -n 19 "$B/test_nr_pdcch_dci_length_sweep" --gtest_filter='*ColdRound*:*DefaultOrder*:*SeenOrder*'
nice -n 19 "$B/test_nr_pdcch_blind_monitor" --gtest_filter='*PolarParamsDoNot*'
```

[OFFLINE VERIFIED] Length tests failed 3/3 and the embedded-size test failed
1/1. Logs: `/tmp/rr-r2b-red-{build,length,polar}.log`.

[OFFLINE VERIFIED] Iteration build and targeted tests:

```bash
B=cmake_targets/ran_build/build
CCACHE_DIR=/tmp/rr-r2b-ccache nice -n 19 ninja -C "$B" -j8 test_nr_dci_bits test_nr_pdcch_dci_length_sweep test_nr_pdcch_blind_monitor
nice -n 19 "$B/test_nr_pdcch_dci_length_sweep"
nice -n 19 "$B/test_nr_pdcch_blind_monitor"
nice -n 19 "$B/test_nr_dci_bits"
```

[OFFLINE VERIFIED] Length 33/33, blind monitor 206 passed + 2 capture-dependent
skips, bit-vector/formatting 4/4. Existing wide polar fixtures and narrow
expectations remain unchanged. Logs: `/tmp/rr-r2b-length.log`,
`/tmp/rr-r2b-blind.log`, `/tmp/rr-r2b-bits.log`,
`/tmp/rr-r2b-target-build-final.log`.

[OFFLINE VERIFIED] Required full CPU build passed 359/359 steps:

```bash
CCACHE_DIR=/tmp/rr-r2b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
```

[OFFLINE VERIFIED] Full CTest ran **once: 122/130 passed, 8 failed**, 32.73 s.
Failures: known ARM `test_nr_pusch_ra0_qam256`, `dft_test`,
`test_nr_modulation`; known sandbox `time_management_tests`, `test_gtp`,
`test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. The intermittent
known qam64 failure did not occur. No unlisted failure. Logs:
`/tmp/rr-r2b-full-build.log`, `/tmp/rr-r2b-ctest.log`.

[OFFLINE VERIFIED] All **27/27 shuffle invocations passed**, totaling
**1032 passed, 6 skipped, 0 failed**. The command for each binary below,
for seeds 1, 3 and 5, was
`nice -n 19 <absolute-binary> --gtest_shuffle --gtest_random_seed=<seed>`,
with its parent directory as cwd. A Python ThreadPoolExecutor limited
concurrency to four. Binary paths relative to `cmake_targets/ran_build/build`:

| Binary | Passed per seed | Skipped per seed |
|---|---:|---:|
| test_nr_dci_bits | 4 | 0 |
| test_nr_pdcch_blind_monitor | 206 | 2 |
| test_nr_pdcch_dci_length_sweep | 33 | 0 |
| test_nr_pdcch_dci11_layout_sweep | 40 | 0 |
| test_nr_pdcch_dci01_layout_sweep | 14 | 0 |
| test_nr_passive_bwp | 21 | 0 |
| test_nr_pdcch_joint_solve | 14 | 0 |
| test_nr_pdcch_joint_live | 6 | 0 |
| openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_mib_blind_handoff | 6 | 0 |

[OFFLINE VERIFIED] Logs: `/tmp/rr-r2b-shuffle-<binary>-<seed>.log`,
`/tmp/rr-r2b-shuffle-summary.log`. The blind-monitor skips are the two
existing captured-IQ-dependent replay tests.

[OFFLINE VERIFIED] `git diff --check` and the frozen gate passed before commit:

```bash
git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures 'tests/passive_rx/*.conf' tests/passive_rx/sens6_host_snapshot_2026-09-30
```

[OFFLINE VERIFIED] The starting R3 commit is `de03ab9977`; no frozen files
or other worktrees were changed, and nothing was pushed.

## DEFERRED MEASUREMENTS

[PLANNED] In the orchestrator's quiet DGX slot only, run two CPU arms and
score first C-RNTI to bank-add from the raw logs (the scorer's convergence
metric ends later). Match against the same arms at the R3 baseline; the
offline round-count calculation above does not replace the R2 latency gate.

```bash
cd /home/nicola/NICOLA/wt/rr-robust
if pgrep -x nr-uesoftmodem; then exit 2; fi
env -u ISAC_RX_BRANCH_FO -u NR_GPU_FEP ISAC_RECONF=0 NR_GPU_POLAR=0 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r2b-sa-off bash tests/passive_rx/dgx/rfsim_regress.sh 2
env -u ISAC_RX_BRANCH_FO -u NR_GPU_FEP ISAC_RECONF=1 NR_GPU_POLAR=0 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r2b-sa-on bash tests/passive_rx/dgx/rfsim_regress.sh 2
```

[PLANNED] GPU compatibility check, solely in the orchestrator's GPU slot:

```bash
cd /home/nicola/NICOLA/wt/rr-robust
if pgrep -x nr-uesoftmodem; then exit 2; fi
cmake -S . -B cmake_targets/ran_build/build_gpu_r2b -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DENABLE_ISAC_SENSING=ON -DOAI_USRP=ON -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121 -DCMAKE_CUDA_ARCHITECTURES=121 -DCPM_SOURCE_CACHE=/tmp/rr-r1-cpm
CCACHE_DIR=/tmp/rr-r2b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build_gpu_r2b -j8 nr-uesoftmodem polar_gpu nr_polar_sc_cuda_test
nice -n 19 cmake_targets/ran_build/build_gpu_r2b/nr_polar_sc_cuda_test quick
```

## Open issues / limits

- [PLANNED] Matching NSA-like radio validation needs the orchestrator's
  no-SIB1 fixture; no implemented ignore-SIB1 switch exists here. The length
  tests require neither SIB1 nor CORESET#0.
- [IMPLEMENTED, NOT VALIDATED with captured IQ] Replay header rejection and
  new logs are compiled, but no captured-IQ replay or live emission was run.
- [PARTIAL] GPU lookahead batching is unchanged per task scope; no GPU cost
  or correctness result is inferred from CPU tests. Existing R3's global
  Technique D reset limitation remains unchanged.
