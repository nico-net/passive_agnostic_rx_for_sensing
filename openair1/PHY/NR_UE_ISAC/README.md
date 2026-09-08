# Native passive-sensing pipeline

This directory is a native C++17 transcription of the validated Python pipeline in
`sionna-rk/.worktrees/multi-object-case/experiments/passive_sensing/single_rx_validation`.
It does not embed Python, start Python, import Python modules, or exchange decisions with a
Python process. The OAI PHY still owns CFR extraction (`H = Y/X`) and submits measured CFR rows
through `nr_isac_submit_cfr_multi`; every operation after that boundary is implemented here.

## Processing contract

1. Keep measured row timestamps, occupancy, source bits, and sub-slot fractions. No slow-time
   interpolation or fabricated REs are introduced.
2. Estimate and apply statistically selected STO/SFO and per-row LOS phase using the current CPI.
3. Align identical scheduler/allocation families and subtract their per-antenna family mean.
4. Estimate the current-CPI homoscedastic variance from within-family first differences.
5. Form the masked, irregular-slow-time likelihood noncoherently over all receive antennas.
6. Run eight iterations of sequential CLEAN with continuous profile-likelihood refinement.
7. Collapse measured-lattice multipath components, then apply CUT-excluded local log-MAD CFAR.
   The two-bin Doppler guard excludes training cells around the CUT; it never zeros Doppler zero.
8. Apply causal clutter/confirmed-track feedback and update the constant-acceleration range/rate
   tracker using global Hungarian association.
9. Estimate 3-D receive direction from the surveyed rank-two 2x2 UPA and feed the hierarchical
   ENU EKF. `aoa_enable` is the master switch; `aoa_ul_enable` is subordinate. With master on and
   UL off, UL rows remain available to detection but cannot enter AoA.
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
fallback.

`test_nr_isac_cuda_detector_parity` compares map support, component decisions, local statistics,
and energies with the CPU implementation. `benchmark_nr_isac_cuda_detector` exercises a
deterministic 4x192x3276 CPI with eight CLEAN passes and fails when its five-run steady-state median
is not below 200 ms (override only for diagnostics with `NR_ISAC_CUDA_BENCHMARK_MAX_MS`). Setting
`NR_ISAC_DETECTOR_TIMING=1` prints per-iteration map/refinement/subtraction timings.

`test_nr_isac_cuda_family_processing` compares aligned tensors, diagnostics, within-family
variance, and the raw-power fallback against the CPU path. The deterministic
`benchmark_nr_isac_cuda_family_processing` exercises a 4x192x3276 CPI and requires the warmed
combined alignment/variance median to remain below 40 ms (override for diagnostics with
`NR_ISAC_CUDA_FAMILY_BENCHMARK_MAX_MS`).

`sensing.num_ues` is one variable with the supported range **1 through 4**. It does not reduce the
number of UEs in a scenario; it records and validates the deployment count while the existing
passive PDCCH/PDSCH/PUSCH paths decode the configured UE traffic. The submitted CFR contract is
UE-count agnostic.

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
