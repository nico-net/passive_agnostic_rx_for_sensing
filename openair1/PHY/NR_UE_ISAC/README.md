# Native passive-sensing pipeline

This directory is a native C++17 transcription of the validated Python pipeline in
`sionna-rk/.worktrees/multi-object-case/experiments/passive_sensing/single_rx_validation`.
It does not embed Python, start Python, import Python modules, or exchange decisions with a
Python process. The OAI PHY still owns CFR extraction (`H = Y/X`) and submits measured CFR rows
through `nr_isac_submit_cfr_multi`; every operation after that boundary is implemented here.

## Processing contract

1. Keep measured row timestamps, occupancy, source bits, and sub-slot fractions. DL and UL CFRs
   are retained as independent illuminator views; their complex phase histories are never averaged.
   No slow-time interpolation or fabricated REs are introduced.
2. If supplied, reorder the four receive channels and apply fixed per-chain gain, phase, and group-
   delay correction. Then estimate and apply statistically selected STO/SFO and per-row LOS phase
   using the current CPI.
3. Align identical scheduler/allocation families and subtract their per-antenna family mean.
4. Estimate the current-CPI homoscedastic variance from within-family first differences.
5. Form the masked, irregular-slow-time likelihood noncoherently over all receive antennas.
6. Run eight iterations of sequential CLEAN with continuous profile-likelihood refinement.
7. Collapse measured-lattice multipath components, then apply CUT-excluded local log-MAD CFAR.
   The two-bin Doppler guard excludes training cells around the CUT; it never zeros Doppler zero.
8. Apply independent causal clutter/confirmed-track feedback to the DL and UL range/rate trackers,
   using global Hungarian association inside each detector stream. UL motion may extend DL coasting
   and confirm a new DL birth, but without UL AoA it cannot reject an individual DL candidate.
9. Estimate 3-D receive direction from the surveyed rank-two 2x2 UPA. Reject clipped directions,
   poor single-manifold fits, excessive component-to-component angular spread, and physically
   impossible temporal jumps before the hierarchical ENU EKF can create or update a global track.
   With UL AoA enabled, matching receive bearings provide repeated candidate-specific confirmation;
   an admitted UL bearing may fill a DL angular fade using a bearing-only update with an explicit
   cross-leg covariance floor. UL remains an independent range/rate tracker, and its bistatic
   range/rate never enters the DL EKF because its transmitter position is unknown. No oracle UE
   position is present. `aoa_enable` is the master switch and `aoa_ul_enable` is subordinate. With
   master on and UL off, DL AoA remains active and UL AoA is not evaluated.
10. Emit detections, native range-Doppler maps, and stage-1/global tracks as JSONL/ZeroMQ reports.

CFR submission, CPI formation, and detector/tracker processing are deliberately separated. The
real-time-facing snapshot queue is drained by a lightweight accumulator while one complete CPI is
processed. CPI formation is causal: the next plan is created only after the preceding detector and
tracker update, matching the Python plan -> detect -> update loop. Processing failures are counted
as `dropped_cpis`. The unplanned-row backlog is capped by `pending_row_budget_mib`; reaching it
skips one explicit oldest interval and reports `discarded_pending_rows`/`intervals`. Validation
runs require all three loss counters to remain zero.

The old OAI detector (FFT/CA-CFAR, global zero-Doppler notch, ECA variants, greedy tracker, and
legacy CLEAN) is intentionally absent.

## Array calibration and AoA admission

`sensing.rx_array_calibration` accepts four semicolon-separated tuples in physical-element order:

```
observed_channel,gain_correction,phase_correction_rad,delay_correction_ns;...
```

The correction at baseband offset `f` is
`gain * exp(j * (phase + 2*pi*f*delay))`. An empty option is the identity correction. The matching
Python replay option is `--array-calibration calibration.json`; both paths apply the correction
before synchronization, detector alignment, and AoA. Calibration is target-blind and must come
from a separate target-absent/cabled measurement. A fixed single LOS cannot identify wiring order
when arbitrary cable phase and delay are unknown, so known SDR serialization is retained unless
multiple independently surveyed look directions identify the permutation.

The default AoA admission limits are manifold residual `0.25`, phase-fit RMS `pi/4`, and `45`
degrees standard uncertainty on each angle. They are configurable with
`sensing.aoa_max_manifold_residual`, `sensing.aoa_max_phase_fit_rms_rad`,
`sensing.aoa_max_azimuth_stddev_deg`, and `sensing.aoa_max_elevation_stddev_deg`.

For a collapsed extended target, significant CLEAN components inside the dominant peak's two-bin
range/Doppler NMS ball are reprojected independently. Their power-weighted receive directions are
combined, and their between-component spread is added to the angular covariance without division
by component count. The ENU tracker also limits one-CPI direction changes using the declared
50 m/s maximum tangential speed plus three standard deviations of measured angular uncertainty.
Neither gate reads target class, target position, scorer output, or UE position.

Cross-leg bearing association uses a 99% two-angle chi-square gate and a 3-degree systematic
standard-deviation floor. One matching CPI labels evidence but does not delete alternatives; a new
global track requires two candidate-specific matches in the latest three observations. UL without
AoA supplies scene-level birth/coast support only. Existing DL tracks accept UL bearing-only EKF
updates only when DL AoA is absent or quality/temporal rejected. A valid but jointly inconsistent
DL AoA is not silently overridden by UL.

## CUDA detector backend

With `ENABLE_CHANNEL_SIM_CUDA=ON`, the same native detector control flow uses a CUDA workspace for
the operations implemented by `gpu_pipeline.py`: complex64 batched range IFFTs, the precomputed
irregular slow-time coherent projection, continuous-refinement sufficient statistics, and
device-resident sequential CLEAN subtraction. The map copied into `initial_likelihood`, every
component, the CUT-excluded local statistic, localization covariance, and multipath collapse are
still produced through the same public `detect_clean` result contract. CLEAN remains sequential,
uses all configured components and rows, and retains continuous Newton refinement.

`SensingEngine::start()` performs the one-time CUDA/cuFFT warm-up before it admits CFR snapshots.
If CUDA was not built, no device is present, or `NR_ISAC_CUDA_DETECTOR=0`, the unchanged CPU path is
used. A CUDA failure before the first map may fall back to CPU; a failure after device-side CLEAN
has changed the residual fails the CPI rather than mixing stale host and device state.
Campaigns must set `NR_ISAC_REQUIRE_CUDA=1`: startup then fails before CFR admission when the CUDA
device/warm-up is unavailable, and backend initialization or first-map failures cannot silently
fall back to CPU.

The allocation-family alignment/static-reference subtraction and current-CPI variance estimator
also use a reusable CUDA workspace. Family keys and row order are still constructed by the shared
host implementation, while CUDA preserves the sequential phase unwrap, weighted delay fit,
complex-gain normalization, exact positive-sample filtering, and exact median decision. A failed
optional alignment download is staged and cannot partially overwrite the host window before CPU
fallback. Per-column phase/weight preparation is parallel; unwrap and every decision-bearing sum
retain their original order.

When RDM capture is enabled, the primary map is the DL detector input and `dl_rvm_blob` is an exact
backward-compatible copy. `ul_rvm_blob` is generated by a separate detector invocation. A malformed
UL view is reported as invalid and cannot suppress the valid DL report or update either tracker.

`test_nr_isac_cuda_detector_parity` compares map support, component decisions, local statistics,
and energies with the CPU implementation. `benchmark_nr_isac_cuda_detector` exercises a
deterministic 4x192x3276 CPI with eight CLEAN passes and fails when its five-run steady-state median
is not below 200 ms (override only for diagnostics with `NR_ISAC_CUDA_BENCHMARK_MAX_MS`). Setting
`NR_ISAC_DETECTOR_TIMING=1` prints per-iteration map/refinement/subtraction timings.
The same benchmark also exercises mixed incoming UL/DL submissions, independent family alignment
and detector views, and dual-map JSON; its full-path median must remain below 200 ms (override
with `NR_ISAC_CUDA_FULL_CPI_MAX_MS`). `NR_ISAC_BENCHMARK_FORCE_CPU=1` disables all CUDA paths and
runs one full-size CPU comparison sample without a warm-up.

`test_nr_isac_cuda_family_processing` compares aligned tensors, diagnostics, within-family
variance, and the raw-power fallback against the CPU path. The deterministic
`benchmark_nr_isac_cuda_family_processing` exercises a 4x192x3276 CPI and requires the warmed
combined alignment/variance median to remain below 40 ms (override for diagnostics with
`NR_ISAC_CUDA_FAMILY_BENCHMARK_MAX_MS`).

`sensing.num_ues` remains one variable with the deployment range **1 through 4** for DL decoding.
The current CFR submission ABI does not carry a UE identity, so UL sensing is rejected when
`num_ues != 1`; otherwise measurements from different UE geometries would be silently mixed. The
validation campaign therefore uses `num_ues=1`. Supporting 2--4 UL illuminators requires adding a
UE identifier to every CFR submission and maintaining one UL detector/tracker view per UE.

## Deferred CPU optimization

CUDA is the validated campaign backend. A later CPU-only optimization should add reusable FFT and
projection workspaces, cache steering matrices by CPI geometry, vectorize map accumulation, and
parallelize independent range bins while preserving decision order and CPU/CUDA golden parity.

## Debugging the chain block by block

Set `NR_ISAC_DEBUG_DIR=<dir>` (the launcher does this with `--debug`). Every artifact maps to one block:

| block | artifact | view with |
|---|---|---|
| input (ABI) | `cfr_rows.bin` — every admitted row, per antenna, plus gate closes | `isac_replay -O <conf> --rows cfr_rows.bin` |
| gate | `SENSING_GATE open/close/stats` receiver log lines | monitor Pipeline tab |
| per-channel CFO | `BRANCHFO d_vs_br0=[...]` log line | monitor Pipeline tab |
| [1] sync | `seqN_rxI_raw.bin` → `seqN_rxI_sync.bin`[^sync-dump]; report `spatial_receivers[i].sync` | `show_block.py` |
| [2] families + clutter | `seqN_rxI_pre.bin` → `seqN_rxI_post.bin`; report `causal_clutter` | `show_block.py` |
| [3] CPI formation | report `cpi_plan`, `dropped_cpis`, `discarded_pending_*` | monitor Pipeline tab |
| [4]/[5] RD map, CLEAN, CFAR | report `rvm_blob` (pre-CLEAN), `rvm_final_blob`, components, `detections` (capture = 1) | monitor DL/UL tabs |
| [6] long dwells | report `long_dwells` | report JSON |
| UL per UE | `seqN_rxI_ul<rnti>_raw/post.bin`; report `uplink_sessions[]` | `show_block.py`, monitor UL tab |
| [7]–[10] | `realtime_chain.py --debug-dir`: `stage8.jsonl`, `localiser.jsonl`, `stage9.jsonl`, `tracks.jsonl` | monitor 3D tab |

[^sync-dump]: the `sync` dump is only emitted when `sync_enable` is on and the window has >= 3
  rows -- a `raw` file with no matching `sync` sibling means that CPI skipped sync correction, not
  a bug.

Offline: record once over the air, then replay deterministically as often as needed.

## Reference identity

The source snapshot used for the transcription is branch `feature/multi-object-relax`, commit
`5a49a8618856895f8794c19beff7a62973a081b5`, including its working-tree versions. SHA-256 values:

| Reference | SHA-256 |
|---|---|
| `python_pipeline.txt` | `f15fc53` (prefix) |
| `optimization/optimized_pipeline.py` | `a5a8eb` (prefix) |
| `optimization/gpu_pipeline.py` | `9f9730` (prefix) |
| `optimization/adaptive_clutter_map.py` | `f3527b` (prefix) |
| `optimization/adaptive_threshold.py` | `5bce3b` (prefix) |
| `optimization/motion_tracker.py` | `df3f05` (prefix) |
| `optimization/enu_tracker.py` | `6a939b` (prefix) |
| `optimization/hierarchical_tracker.py` | `2d57e` (prefix) |
| `evaluate_run_optimized.py` | `60fdbb` (prefix) |

`tests/python_parity_test.cc` contains deterministic golden cases generated by those Python
modules. A build of this directory has no Python headers or libraries and `nr-uesoftmodem` never
executes Python.
