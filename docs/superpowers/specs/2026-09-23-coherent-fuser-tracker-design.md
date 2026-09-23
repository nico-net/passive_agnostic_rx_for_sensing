# Coherent Fuser + Tracker — Design Spec (2026-09-23)

Branch: `feature/multirx-clean-adaptive` (sens6 worktree `/home/sens/NICOLA/multirx-clean-adaptive`).
Approved in conversation section by section (Sections 1–7 below). Nothing here deletes a file.

## 0. Intent and constraints (from the user)

- Build a coherent fuser and a tracker that treat the 4 X410 RX channels (one LO, one sample clock) as
  one sparse phase-coherent array, replacing per-receiver stages 1–6 at runtime and stages 7–10
  (Python tail) in the launcher. **No existing file is removed**; stages 7–10 are only unwired.
- Scene: 4 antennas on a 5 m or 10 m square, checkerboard heights 0.5 m / 3.5 m, identical cables, one
  X410. gNB = lab cell, surveyed with a measuring tape (±5–10 cm). Surveillance volume ~30×30 m,
  altitude 0–30 m (configurable).
- Targets: people, cars, a small drone (all ≤ ~40 m/s). Output: 3D tracks (position, velocity,
  confidence). **No object-type labels.**
- **Manual receiver configuration** (`tests/passive_rx/ota/sensing_ota_manual.conf.template`), DL only
  for now: `gate_require_ul = 0`, `sources = "pdsch_dmrs_blind,pdsch_data"`.
- **UL path is built but disabled** (`coherent_ul_enable = 0`) until the user asks.
- **Golden rule — no calibrated or tuned constants.** Every quantity is estimated online from the data
  or derived from physics / the actual allocation. Declared inputs only: the tape survey (+ its
  declared accuracy `coherent_survey_sigma_m`), the surveillance volume, the existing
  `maximum_target_speed_mps` and `false_object_intensity_per_s`, and conventional statistical
  quantiles (χ² 99 % gate, SPRT α = 1 %, β = 10 %). Accept some accuracy loss for generality.
- **Allocation-aware:** resolutions, grid spacings, Doppler span and measurement noise derive from the
  PRBs/REs and rows actually present in each CPI — never from 273-PRB / 75 ms constants.
- **Real time:** every CPI processed within the 75 ms CPI period, zero silent drops.
- **Logs are never overwritten or truncated.**
- UI: detector and tracker output wired to the monitor.

## 1. Architecture and data flow

```
receiver (unchanged): single decode → X̂ → nr_isac_submit_cfr_multi(4 antennas)
SensingEngine (unchanged up to CPI formation): flow gate → CPI accumulation / variable_cpi
   │ CfrWindow (DL + UL session windows), antenna-major [a][row][subcarrier], raw phase
   ▼ coherent_enable = 1 ?  ── no ─→ existing per-receiver stages 1–6 (untouched)
CoherentPipeline (NEW; GPU-resident per CPI; CPU reference = fallback + test oracle)
  C1 upload once   C2 common-mode sync   C3 per-channel range-Doppler
  C4 per-channel calibration   C5 focusing (envelope → coherent refinement)   C6 detection
   ▼
CoherentTracker (NEW, C++ CPU): 3D CV Kalman + GNN + SPRT existence, DL (+UL when enabled)
   ▼
JSONL: coherent_reports, coherent_tracks, coherence (+ decimated images) → monitor
```

New files (all under `openair1/PHY/NR_UE_ISAC/` unless stated): `coherent_types.h`,
`coherent_core.{h,cc}` (CPU reference C2–C6), `coherent_cuda.{h,cu}` + `coherent_cuda_stub.cc`,
`coherent_tracker.{h,cc}`, `coherent_autofocus.{h,cc}`, `coherent_ul.{h,cc}`,
`coherent_pipeline.{h,cc}`, `coherent_report.{h,cc}`, tests under `tests/`, generator
`tools/make_coherent_scene.py`, monitor `tests/passive_rx/monitor/coherent_view.py` +
`coherent.html`. Modified (additive only): `CMakeLists.txt`, `nr_isac.cc` (config keys),
`sensing_engine.{h,cc}` (switch), `pipeline_types.h` (config struct member),
`tests/passive_rx/run_sensing.sh`, `tests/passive_rx/monitor/monitor.py`, the manual template.

## 2. Sync, calibration, coherence diagnostic (C2, C4)

- **Delay reference = each channel's own measured LOS.** Excess bistatic delay of voxel x on channel i:
  `Δτ_i(x) = (|x−tx| + |x−rx_i| − |tx−rx_i|) / c`. Referencing each channel's delay axis to its measured
  gNB direct-path peak cancels the common STO and per-cable delay; survey error enters only through
  distance differences.
- **LOS detection:** earliest range bin whose zero-Doppler power exceeds the noise-derived threshold
  (robust median/MAD of the channel's range profile, χ² quantile from the declared false-alarm rate),
  not the strongest (a wall reflection of the gNB must not bias calibration). Sub-bin delay by parabolic
  interpolation on the magnitude; applied as a phase ramp over subcarriers.
- **Common CFO/SFO:** row-to-row phase progression and delay drift of the LOS tap, averaged over
  channels, applied identically to all channels.
- **Per-channel calibration:** `g_i = LOS_tap_i` (after LOS referencing), relative to channel 0.
  Smoothed with a scalar Kalman recursion whose process noise is estimated by covariance matching
  from the innovations (no forgetting constant). `σ_i²` = measured innovation variance.
  Combining weights `w_i = ĝ_i* / σ_i²` (normalised) — a decohering channel fades out automatically.
- **Coherence diagnostic (per CPI, own JSONL):** `g_i` (|·|, ∠), phase jitter vs the SNR bound
  `1/√(2·SNR)`, LOS coherent gain `G = |Σ w_i H_i|² / Σ |w_i H_i|²` ∈ [1, 4], and
  `ρ = (G − 1)/3` ∈ [0, 1].
- Honest limit: tape survey error is exact-cancelled only along the LOS direction; autofocus (C5.3)
  reduces it online.

## 3. Range-Doppler, focusing, detection (C3, C5, C6)

- **C3:** range IFFT over the observed subcarriers (zero-filled, per-row normalised by its observed
  count); `B_eff` = median over rows of the observed bandwidth; delay step `1/(N_fft·Δf)`; range crop to
  the largest `Δτ_i(x)` over the volume. Doppler = non-uniform DFT over the rows' actual timestamps
  with a Hann window; frequency step `1/T_cpi`, span `±1/(2·median Δt_row)`. Static clutter removed by
  subtracting each channel's slow-time mean (after calibration is measured); zero-Doppler notch = the
  Hann mainlobe (±2 bins).
- **C5.1 envelope:** voxel spacing `c/(4·B_eff)` over the volume; statistic
  `E(x,v) = Σ_i |w_i|²·|RD_i(Δτ_i(x), v)|²` (linear interpolation in delay). Robust to tape error.
- **C5.2 coherent refinement (per detection):** fine grid spacing = half the fringe width
  `λ·R/(2·D)` (R = distance from array centroid, D = max baseline), extent ± one envelope spacing;
  statistic `C(x,v) = |Σ_i w_i·RD_i(Δτ_i(x), v)·e^{+j2π f_c Δτ_i(x)}|²`. Output position =
  `ρ·x_coh + (1−ρ)·x_env`.
- **C5.3 autofocus:** for detections associated to confirmed tracks, per-channel phase residuals at the
  focused voxel feed a recursive weighted least-squares refinement of antenna positions (linear
  phase-sensitivity model, weights from SNR, prior = survey with `coherent_survey_sigma_m`).
- **C6 detection:** per Doppler bin, noise = median of E over voxels (GPU radix select); threshold =
  χ²₂ₙ quantile at `P_fa = false_object_intensity_per_s · T_cpi / (n_voxels · n_dopp_tested)`,
  scaled by the χ²₂ₙ median; local maxima with NMS radius = 2 envelope spacings; **harmonic merge**
  (same NMS neighbourhood, Doppler ratio within one bin of an integer k ≥ 2 → keep the strongest);
  **z < 0 rejected**; zero-Doppler notch bins not tested.
- **UL (built, off):** UE position from its earliest tap per channel (TDOA grid search over the volume;
  σ from the χ² +1 contour), then focusing with `tx = UE` and per-channel Doppler referenced to that
  channel's UE direct-path Doppler.

## 4. Tracker

State `[x y z vx vy vz]`, constant velocity. Process noise: white-acceleration PSD, adaptive by
covariance matching per track; start value `q0 = (maximum_target_speed_mps / 1 s)²` (existing
declared bound, same convention as stage 10). Measurements: 3D position (σ per axis = focus width /
√(2·SNR), floor = grid spacing/√12) — linear update; bistatic range-rate (σ = (λ/T_cpi)/√(2·SNR),
floor = Doppler bin/√12) — scalar update with gradient `u_tx→x + u_x→rx`. Joseph form + symmetrise.
Association: Hungarian (GNN) on Mahalanobis distance, gate = χ²₃ 99 %. Existence: SPRT on the
log-likelihood ratio (α 1 %, β 10 %), `P_D` estimated online from confirmed-track hit ratios, clutter
density from `false_object_intensity_per_s` over the volume. Births from unassociated detections
(velocity σ = `maximum_target_speed_mps`). Output per CPI: id, position, velocity, σ diag,
existence probability, hits, age, confirmed flag.

## 5. UI

Monitor gains a coherent page (existing views untouched): live rotatable 3D scene (antennas, gNB,
volume box, current detections sized by SNR with hover, track trails + velocity arrows coloured by
existence), top-view envelope heatmap with detections, 4 per-channel RD maps, coherence panel
(∠g_i, jitter vs bound, G/ρ over time), health strip (CPI time vs 75 ms, GPU/CPU, drop/backlog
counters, gate). Images decimated and rate-limited by the engine (existing `rvm_period_s`).

## 6. Real time, threading, failure handling, config, logs

- One upload per CPI into pinned memory; C2–C6 on one CUDA stream; only detections, calibration,
  per-detection channel terms and (rate-limited) decimated images come back.
- Coherent worker thread with a depth-2 queue (double buffering); `submit` never drops silently —
  waits are counted and reported. Per-stage CUDA-event timers per CPI; overruns (> T_cpi) counted.
- Failures: no LOS in a CPI → calibration predicts (σ grows, ρ falls, envelope-only); a decohered
  channel's weight fades; no CUDA → CPU reference (flagged; `NR_ISAC_REQUIRE_CUDA=1` fails closed as
  today).
- Config (`[sensing]`): `coherent_enable` (0), `coherent_ul_enable` (0),
  `coherent_volume_m = "xmin:xmax:ymin:ymax:zmin:zmax"`, `coherent_survey_sigma_m` (0.1, declared).
  Geometry from `spatial_rx_positions` and `tx_pos_*`.
- Logs: every engine process writes `coherent_{reports,tracks,coherence}.<utc>_<pid>.jsonl` in the
  report directory, opened append-only; nothing truncates. `run_sensing.sh` stops truncating chain
  outputs on retry and does not launch `realtime_chain.py` in coherent mode.

## 7. Testing

1. Unit tests: each C2–C6 stage vs analytic expectations; CUDA vs CPU reference parity.
2. Synthetic end-to-end (`make_coherent_scene.py` → `isac_replay`): square geometry, checkerboard
   heights, per-channel phase offsets, ±10 cm survey error, irregular grants, noise, person/car/drone,
   wall reflection, ground bounce. Pass: injected phases recovered; G≈4 coherent / ≈1 scrambled;
   all targets detected and confirmed with sub-range-cell error; no below-ground or multipath tracks;
   autofocus reduces survey error.
3. Real time: same stream via `isac_replay_rt --realtime`, pinned to the sensing cores: per-CPI p95
   < 75 ms, zero drops/overruns, same CPI count as lockstep.
4. Existing tests unchanged; `coherent_enable = 0` reproduces current behaviour.
5. OTA order (user-run): static scene coherence panel → walking person → car → drone.
