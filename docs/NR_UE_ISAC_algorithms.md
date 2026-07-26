# NR_UE_ISAC: Range/Doppler Estimation and Sync-Compensation Algorithms

Detailed reference for the two DSP subsystems in `openair1/PHY/NR_UE_ISAC/`:

1. **Range-Doppler estimation** (`range_doppler.{h,cc}`) — turns one CPI's accumulated channel
   estimate into a range-velocity map and a list of CFAR detections.
2. **Sync compensation** (`isac_sync.{h,cc}`) — the STO/CFO/SFO/closed-loop-LOS tracking and
   correction chain that runs on the raw per-subcarrier grid before stage 1.

Both operate on the same input: a coherent-processing-interval (CPI) matrix of channel estimates
`Ĥ[n][c]` (slow-time row `n` × subcarrier `c`), produced upstream by dividing received samples by
the known reference signal (`Ĥ = Y/X`) at CSI-RS/DM-RS/data REs and accumulated across the CPI by
`sensing_engine`. Everything below assumes that grid already exists; neither module touches the raw
IQ or the channel-estimation step itself.

Code cross-references use `file.cc:function` / constant names verbatim so this doc can be diffed
against the source as it evolves. Where a design decision has a documented rationale in the source
comments, that rationale is repeated here rather than re-derived.

---

## Part 1 — Range-Doppler estimation (`range_doppler.cc`)

### 1.0 Pipeline shape

```
Ĥ[n][c]  →  (self-test injection, optional)
         →  clutter removal (mean-subtraction | ECA+)
         →  range axis: windowed IFFT over c, per-row  →  CIR[range][n]
            de-aliasing taper
         →  Doppler axis: windowed FFT over n, fftshift →  RVM[range][doppler] (power)
         →  zero-range / zero-Doppler notch
         →  2D CA-CFAR (integral-image) + greedy NMS
         →  conjugate-image (mirror-ghost) rejection
         →  detections[] {range_bin, doppler_bin, range_m, vel_mps, snr_db}
```

All of this runs once per CPI, off the real-time path, in `sensing_engine`'s consumer thread.
`range_doppler::process()` is the single entry point; `ensure_plans()` lazily (re)builds the two
`isac_fft` plans (range IFFT of length `nof_range`, Doppler FFT of length `nof_slow`) whenever the
CPI's dimensions change, so steady-state operation is allocation-free.

### 1.1 Axis geometry

For the fused per-subcarrier grid (comb spacing 1), `nof_range = nof_subc` and
`nof_dopp = nof_slow`. Let `df_comb = SCS · comb_spacing` and `t_slow = period_slots · slot_dur`
(the reference resource's slow-time sampling period). Then:

```
range_res_m = c / (2 · nof_range · df_comb)
range_max_m = c / (2 · df_comb)
vel_res_mps = c / (2 · fc · nof_dopp · t_slow)
vel_max_mps = c / (4 · fc · t_slow)
```

`range_res_m · nof_range = c/(2·df_comb)` is invariant — widening the CPI's captured bandwidth (more
subcarriers) only grows `range_max`, never changes `range_res`. This identity matters when reasoning
about fused sources with different native combs (see §1.3).

### 1.2 Clutter removal

Two interchangeable methods, selected once at construction (`args.clutter_removal`):

**`"mean"` (default).** For every subcarrier column `c`, subtract the slow-time mean:

```
mean[c] = (1/N) Σ_n Ĥ[n][c]
Ĥ'[n][c] = Ĥ[n][c] − mean[c]
```

This suppresses a perfectly static (zero-Doppler) component — the LOS/direct-path tap and any
static clutter — but leaves a **real-valued residual** at low-but-nonzero Doppler (a static/slow
scatterer's contribution isn't perfectly captured by a single DC term once STO/CFO/SFO residual or a
slowly-varying scatterer is present). A range IFFT of a signal with any real-valued component
produces energy at *both* a range bin `r` and its conjugate mirror `nof_range−1−r` — this is the
"mirror ghost" §1.6 partially cleans up after detection, and what ECA+ removes at the source.

**`"eca+"` (opt-in, `eca_clutter.{h,cc}`).** A CFR-domain oblique-projection clutter canceller
(Extensive Cancellation Algorithm). Because the sensing sources are already `Ĥ = Y/X` (an ideal
reference divided out), the disturbance subspace spanned by static/slow clutter over a bounded
delay×Doppler removal region factorizes into a delay-domain projector `P_Φ` and a Doppler-domain
projector `P_Ψ`:

```
H_clean = H − P_Φ · H · P_Ψ
```

so no large `N×N` projection matrix is ever built — this is the reason ECA+ is cheap enough to run
per-CPI. Mean subtraction is the exact special case `P_Φ = I`, Ψ = DC-only (bit-for-bit equivalent,
regression-tested). Configurable via `eca_delay_max_m` (removal window in delay; ≤0 = full range) and
`eca_doppler_max_mps` (removal half-band in Doppler — this is the ECA blind-speed / minimum
detectable velocity, and its effect in *bins* is `band / vel_res_mps`, so it must be re-tuned whenever
`vel_res_mps` changes). Full derivation: `ECA_CLUTTER_HANDOVER.md`.

### 1.3 Range axis — windowed IFFT + per-row de-aliasing

For each slow-time row `n`, the occupied subcarriers are windowed and IFFT'd (via `isac_fft`,
arbitrary-length, normalized `1/√N`, since row width varies with source/comb):

```
range_in[c]  = Ĥ'[n][c] · window[c]
range_out    = IFFT_{nof_range}(range_in)        // → CIR for row n
```

**Window choice** (`args.range_window`, resolved once per grid size and cached in `freq_hann`):

- **`"hann"` (default)**: `window[c] = 0.5·(1 − cos(2π·c / (nof_subc−1)))`. Standard raised-cosine
  taper; ~−31 dB peak sidelobe, but the *far* sidelobes decay quickly (asymptotically faster than an
  equiripple window).
- **`"chebyshev"` (opt-in, `range_window_sidelobe_db`, default 60 dB)**: `build_chebyshev_window()`
  implements the standard Dolph-Chebyshev frequency-sampling construction (Antoniou; same algorithm
  as `scipy.signal.windows.chebwin`), reusing `isac_fft`'s forward DFT rather than adding a
  dependency:
  1. `order = N−1`, `r = 10^(atten_db/20)`, `β = cosh(acosh(r)/order)`.
  2. Sample the Chebyshev polynomial on the unit circle: for `k = 0..N−1`,
     `x_k = β·cos(πk/N)`; `p_k = cosh(order·acosh(x_k))` if `x_k>1`, `cos(order·acos(x_k))` if
     `|x_k|≤1`, and the sign-corrected `cosh` branch if `x_k<−1`.
  3. IDFT `p` back to the time domain (odd/even-length cases handled separately — even-length needs a
     `exp(jπk/N)` pre-rotation before the transform, per the standard construction), then
     peak-normalize.

  `isac_fft`'s uniform `1/√N` scaling on every bin cancels exactly in the final peak-normalization
  step, so reusing it unmodified (rather than an unnormalized DFT) is safe. **Cross-checked
  numerically against `scipy.signal.windows.chebwin`** (same N/dB) before trusting the port.
  Trade-off, verified not assumed: Chebyshev's equiripple floor beats Hann's near sidelobes a handful
  of bins from the mainlobe, but Hann's faster asymptotic rolloff overtakes Chebyshev's flat floor
  further out — it is not a strict improvement everywhere, only in the near-sidelobe region (see
  `SYNC_NOISE_HANDOVER.md`'s LOS-skirt finding, which sits in exactly that region).

**Per-row de-aliasing.** A row whose real reference samples are comb-`N` spaced physically supports
unambiguous range only out to `nof_range/N` bins (`= c/(2·N·SCS)`) — beyond that, the row's own
range profile is aliased replicas of its near-range content ("grating lobes"). Each row's valid
span `valid = nof_range / row_comb[n]` is computed from its own native comb; bins beyond `valid` are
zeroed, with a `DEALIAS_TAPER = 4`-bin raised-cosine rolloff immediately before the cutoff (not a
hard gate) so a dense comb-1 row keeps full range while a sparse comb-12 row collapses to its true
~416 m unambiguous window, and fused CPIs keep extended range only where dense rows actually support
it.

### 1.4 Doppler axis — windowed FFT

A second Hann window (always Hann — the range-axis window choice does not extend to this axis) is
applied along slow-time per range bin, then a forward FFT of length `nof_slow`:

```
dopp_in[n]  = CIR[r][n] · hann[n]
dopp_out    = FFT_{nof_slow}(dopp_in)
power[r][d] = |dopp_out[(d+nof_slow/2) mod nof_slow]|²     // fftshift: zero-Doppler centered
```

### 1.5 Notch + CA-CFAR

**Static-clutter notch.** Zero the zero-Doppler band (`|d − nof_dopp/2| ≤ zero_doppler_guard`, all
ranges) and the near-zero-range band (`r ≤ zero_range_guard` or its symmetric upper-edge partner) —
this removes the residual LOS/direct-path and keeps the strongest clutter out of the CFAR noise
estimate, rather than relying on CFAR alone to reject it.

**2D CA-CFAR**, computed via an `(R+1)×(D+1)` integral image of `power` for O(1) rectangular-sum
queries. For each cell `(r,d)`:

```
train_cells  = (outer window of half-width guard+train) − (inner guard window)
noise        = mean(power) over train_cells
α            = train_count · (pfa^(−1/train_count) − 1)     // classical CA-CFAR threshold factor
threshold    = α · noise
detection if power[r,d] > threshold;  snr_db = 10·log10(power[r,d] / noise)
```

`cfar_guard`/`cfar_train` set the guard/training window half-widths (per side); `cfar_pfa` is the
target false-alarm probability the `α` factor is derived from.

**Greedy non-max suppression**: sort surviving detections by SNR descending; keep a detection only if
no already-kept detection lies within `(nms_range_bins, nms_doppler_bins)` of it; cap at
`max_detections`.

### 1.6 Conjugate-image (mirror-ghost) rejection

Post-NMS cleanup for whatever real-valued clutter residual survives mean-subtraction / ECA+ tuning
(§1.2): a detection at range bin `r` and one at its mirror `nof_range−1−r` (within
`conj_image_guard`) cannot both be physical. **Range, not SNR, is the discriminator** — the image
sits in the quiet far-range region where CFAR tends to assign it a *higher* SNR than the real target
buried next to the LOS skirt, so an SNR-based "keep the stronger" rule would keep the ghost. The rule
implemented: drop the detection whose partner sits at a strictly *lower* range bin (i.e., always keep
the near/physical member of a mirror pair). Governed by `args.conj_image_reject` /
`args.conj_image_guard`. Defeated only if the near partner is itself already suppressed by NMS
elsewhere (an orphaned far image) — a known, documented limitation.

---

## Part 2 — Sync compensation (`isac_sync.cc`)

Four phases, run in this order in `sensing_engine`'s CPI-close block, all before stage-4b
interpolation (interpolation should operate on already timing-corrected data):

```
Phase 4 apply_bias_correction()   (bias accumulated from PAST CPIs' residuals)
Phase 1 cpi_sto_tracker::process()   → fine STO estimate + correction
Phase 2 cpi_cfo_tracker::process()   → residual CFO estimate + correction  (reuses Phase 1's rows)
Phase 3 cpi_sfo_tracker::process()   → SFO estimate + correction           (independent walk)
   … stage-4b interpolation, range_doppler::process() …
Phase 4 update_residual()         (measures THIS CPI's residual for the NEXT CPI's bias)
```

All four are phase/frequency-domain multiplies on the existing occupied columns — none of them
change `nof_subc`, column identity, or occupied-column count, so `range_res_m` is preserved exactly
through the whole chain. All gated by a single master switch, `[sensing] sync_correction` — when
false, Phases 1-4 are skipped entirely (bit-for-bit pre-Phase-1 behaviour).

### 2.0 Shared building block: compact-CIR peak search

Every phase below searches a **compact CIR**: a row's occupied subcarriers gathered at their native
comb stride (`row_cir_builder::build()`), Hann-windowed, and IFFT'd via `isac_fft` (arbitrary-N,
since row width varies with source/comb). An un-windowed compact CIR is a raw Dirichlet-kernel
mainlobe, and naive 3-point parabolic interpolation of that shape has severe bias — windowing shapes
the mainlobe much closer to parabolic before any sub-bin estimator runs on it.

**Sub-bin estimator (`subbin_delta()`)**: a complex-domain Jacobsen/Candan-form ratio estimator,
replacing an earlier power-based parabolic fit that had ~0.09–0.11 bin residual bias even after
windowing (the squaring in a power-based fit distorts the symmetry assumption under a non-rectangular
mainlobe):

```
δ_raw = Re[ (x[k−1] − x[k+1]) / (2·x[k] − x[k−1] − x[k+1]) ]
δ     = clamp(δ_raw / HANN_ESTIMATOR_SCALE, −0.5, +0.5)
```

`HANN_ESTIMATOR_SCALE = 0.478762` is an **empirically measured** window-correction constant — a
standalone calibration harness swept a pure tone across true sub-bin offsets through this exact
Hann-windowed IFFT pipeline and least-squares-fit the scale mapping the raw ratio to the true offset
(not a literature figure taken on faith; the fitted scale is mildly N-dependent, 0.416 at N=8 rising
to 0.495 at N=128, and 0.478762 — fit at a representative N=32 — generalizes to <0.07 bin residual
bias across that whole range, versus the old estimator's 0.09–0.11 bin bias everywhere). Shared by
Phase 1 and Phase 3.

### 2.1 Phase 1 — fine STO: walking LOS tracker (`cpi_sto_tracker`)

**Why it walks.** The original design searched a small, fixed window
(`SEARCH_HALFWIN_BINS = ±4`) around a constant nominal bin, every row. `SYNC_NOISE_HANDOVER.md`
root-caused this as the actual bottleneck: once real inter-clock drift (SFO) walks the true LOS delay
outside that fixed window within a CPI, the per-row peak search starts reading noise/sidelobe instead
of the real tap — and that corruption propagates directly into Phase 2 (which fully de-rotates each
row by its own, now-garbage, observed phase). The fix: each row's search window re-centers on the
**previous locked row's own peak**, mirroring the pattern Phase 3 already used, with a narrower
`WALK_HALFWIN_BINS = ±2` (this phase only needs to absorb the *per-step* drift between consecutive
rows, not survive SFO's much larger whole-CPI excursion — that stays Phase 3's job).

**Per-row algorithm:**

```
nominal_bin = round(NOMINAL_LOS_RANGE_M / range_res_m)     // known fixed group-delay seed, ~49 m

for each row r (in time order):
  center = current_center_bin   if locked at least once this CPI, else nominal_bin
  halfwin = WALK_HALFWIN_BINS   if locked at least once this CPI, else SEARCH_HALFWIN_BINS

  build compact CIR; search [center−halfwin, center+halfwin] for the power-argmax bin `peak`

  floor = mean power OUTSIDE the search window (same row)
  if peak_power < FADE_MIN_SNR_LINEAR(4.0, ~6 dB) · floor:
      → FLYWHEEL (see below)
  else:
      → LOCK: record peak_bin=peak, frac_bin=subbin_delta(...), peak_val=CIR[peak]
              current_center_bin = last_locked_bin = peak
              absolute_drift_bins = (current_center_bin − anchor_bin_cpi_start) + frac_bin
```

**Flywheel (fade resistance).** If the in-window maximum doesn't clear the fade/SNR gate, it is not
trusted as a measurement. Instead the walk is **projected forward** from the last real lock, using
the most recently available cross-CPI-smoothed SFO estimate
(`cpi_sfo_tracker::filtered_sfo_ppm()` — necessarily the *previous* CPI's value, since Phase 1 runs
before Phase 3 within a given CPI):

```
dt_s              = time_s[row] − last_locked_time_s
predicted_delay_s = sfo_ppm_hint · 1e−6 · dt_s
current_center_bin = last_locked_bin + round(predicted_delay_s / bin_to_delay_s)
```

The row is marked `flywheeling` and `!valid` (excluded from this phase's own line fit *and* from
Phase 2's CFO fit, which filters on the same flag).

> **Implementation note (a real bug found and fixed via live testing, not caught by the offline
> synthetic tests):** the projection must be an **absolute** offset from `last_locked_bin` — a fixed
> reference captured at the moment of the last real lock — not an *incremental* `current_center_bin
> += predicted_bins` applied every flywheel row. The latter re-applies the full elapsed-time-scaled
> prediction on top of an already-shifted position every consecutive flywheel row, compounding
> quadratically over a sustained fade. A live `tests/sensing_sim` run surfaced this directly: per-CPI
> walked drift swinging ±1000+ bins where the physically expected magnitude was O(10). Fixed by
> introducing `last_locked_bin_` as the stable projection reference, separate from
> `current_center_bin_` (which the flywheel branch now *sets*, not increments).

**CPI-level fit and correction.** Across all locked (non-flywheeling) rows, a least-squares line fits
`frac_bin` vs. each row's own absolute time:

```
slope = fit slope (bins/s);  mean_frac_bin = mean(frac_bin);  drift_bins_cpi = |slope · (t_max−t_min)|
is_constant = drift_bins_cpi < DRIFT_BIN_THRESHOLD (0.25 bins)
```

If `is_constant`, a single frequency-domain phase ramp nulls the **common fractional** component
across every row's occupied columns — the row's own *integer* CIR bin (the diagnosed ~49 m / bin-6
coarse group delay, walked or not) is deliberately left untouched; correcting it is out of this
phase's scope by design:

```
phase_per_subc = 2π · mean_frac_bin / nof_subc
Ĥ[row][c] *= exp(j · phase_per_subc · c)     for every occupied c, every row
```

> **Semantic note.** Since the walker now absorbs real drift into the tracked *integer* bin instead
> of letting it leak into `frac_bin`'s linear trend, `is_constant` will typically read true even under
> substantial real drift — it no longer doubles as an implicit "SFO leaking through" detector the way
> it incidentally did in the pre-walking design (Phase 3 runs unconditionally regardless of this
> classification anyway). The real walked-distance signal is `absolute_drift_bins` /
> `total_drift_bins`, reported per-row and per-CPI (and in the `"sto"` DetectionReport JSON) but not
> yet consumed by Phase 4's closed loop.

**Known live-validated limitation.** A 2×2 `tests/sensing_sim` batch (2026-07-23, 20 runs) measured
the flywheel engaging on **~93% of rows per CPI** on that harness — `WALK_HALFWIN_BINS=±2` is almost
never wide enough for its real per-row drift, so in practice Phase 1 spends most of a CPI
dead-reckoning off a noisy per-CPI SFO estimate rather than measuring real per-row peaks. Flagged as
the concrete next tuning step (recalibrate `WALK_HALFWIN_BINS` against measured drift, same
"measure, don't guess" practice as `HANN_ESTIMATOR_SCALE`), not yet done.

### 2.2 Phase 2 — residual CFO (`cpi_cfo_tracker`)

Reuses Phase 1's already-computed per-row `peak_val` directly — no second CIR/peak search. Neither
existing frequency-offset facility (`--ue-fo-compensation`'s one-shot acquisition-time retune, or
`--cont-fo-comp`'s 20 ms PBCH-driven loop) tracks at sensing-grade rate, so this is an independent,
ISAC-path-only loop.

```
for each valid (locked) row, in time order:
  raw_phase[row] = atan2(Im(peak_val), Re(peak_val))
  unwrapped[row] = sequential unwrap of raw_phase (assumes < π jump between consecutive valid rows)

line-fit unwrapped vs time_s  →  slope (rad/s);  cfo_hz = slope / 2π
residual_phase_rms_rad = RMS(unwrapped − fitted line)
```

An alpha-beta filter (`CFO_ALPHA=0.3`, `CFO_BETA=0.05`, one CPI-tick per step) tracks
`cfo_hz_filtered`/`cfo_rate_hz_per_cpi` across CPIs — **diagnostic only**; it does not feed the
correction below.

**Correction** de-rotates each valid row by its **own raw observed phase** (not the fitted line):

```
Ĥ[row][c] *= exp(−j · raw_phase[row])     for every occupied c
```

This is deliberate: CFO/CPE is a phase rotation common to every subcarrier of a row (no ICI
modelled), so the row's own LOS-tap phase *is* that row's common-phase-error — subtracting it whole
removes both the smooth CFO trend and whatever doesn't fit the line in one step. Being a *uniform*
(not per-subcarrier-linear) rotation, it commutes with Phase 1's frequency-ramp correction and leaves
range-domain structure untouched — only row-to-row (Doppler-axis) phase coherence changes.

### 2.3 Phase 3 — SFO (`cpi_sfo_tracker`)

**Why it doesn't reuse Phase 1.** SFO can walk the LOS peak by hundreds of range bins over a
multi-second CPI (e.g. ~184 bins at 2 ppm over 5 s) — far outside Phase 1's window even with walking
enabled at `±2`. Phase 3 runs its **own** sequential tracking walk (reusing only the CIR-building
step, not the peak-finding strategy), seeded from the same `nominal_bin` for its first row, then
re-centered on the **previous accepted row's own peak** with `SFO_TRACK_HALFWIN_BINS = ±10` — wide
enough to additionally tolerate row-to-row peak wobble/noise on top of genuine per-step drift, with
cumulative drift across the CPI unbounded as long as each *step* stays within the window.

**ISI/anomaly gating.** Per candidate row, the average CIR power just *outside* its local search
window is compared against a running per-native-comb baseline (comb-12 CSI-RS and comb-1/2 PDSCH
rows have different noise floors, so combs are never compared against each other):

```
leakage_model.expected_ratio(M, |frac|)   // per-M calibration curve: synthetic single-tone sweep
                                           // through this exact Hann+IFFT pipeline at 21 |frac|
                                           // grid points, measuring out-of-window/peak-power ratio
anomaly = observed_out_of_window_ratio / expected_ratio(M, frac)   // ~1.0 nominal for a clean row
```

(Raw out-of-window energy swings 3-4 orders of magnitude as a benign function of *both* M and
`frac` — not `frac` alone, as first assumed — which is why the model is calibrated per-M, not a
single global curve.) A row is excluded from the fit if its normalized `anomaly` exceeds a **sliding
window** (last `SFO_ISI_LOCAL_WINDOW=20` *accepted* rows of the same comb) mean by more than
`SFO_ISI_SIGMA=12.0` standard deviations — a sliding, not whole-CPI-cumulative, baseline, since a
cumulative one doesn't track the legitimate slow drift in this statistic as the search window walks
across the compact CIR over a long CPI. Excluded rows don't poison the baseline and don't advance the
walk's anchor.

**Fit and correction.** Surviving `{time_s, tau_s}` pairs (`tau_s = (peak+frac)·bin_to_delay_s`) are
least-squares fit; the slope is *directly* the fractional sample-clock error (seconds of delay drift
per second elapsed ≡ ppm/1e6, no further unit conversion):

```
sfo_ppm = slope · 1e6
sfo_ppm_filtered = (1 − SFO_EMA_ALPHA)·sfo_ppm_filtered_prev + SFO_EMA_ALPHA·sfo_ppm     (α=0.3)
```

**Cross-CPI EMA smoothing.** Unlike Phase 2 (whose correction bypasses its own fit's noise entirely
by using each row's own phase), Phase 3's correction multiplies THIS CPI's fitted slope by every
row's elapsed time — so a noisy or wrong-sign single-CPI fit would otherwise inject a real error
proportional to elapsed time into every row of that CPI. Live-sim testing found `sfo_ppm` swinging
wildly and inconsistently in sign CPI to CPI (e.g. +5.0, −1.4, +7.2 ppm on consecutive CPIs — not
plausible for a real oscillator). The EMA-*filtered* value, not the raw per-CPI fit, is what the
correction actually applies:

```
tau_correct(row) = sfo_ppm_filtered · 1e−6 · time_s[row]      // intercept excluded: correction is
                                                                // zero at CPI start, removing only
                                                                // the drift accumulated SINCE start
phase_per_subc(row) = 2π · SCS · tau_correct(row)
Ĥ[row][c] *= exp(j · phase_per_subc(row) · c)     for every occupied c
```

Using a frequency-domain phase ramp (`exp(j·2π·k·SCS·τ)`) rather than a time-domain resample is exact
for *any* delay shift, sub-sample or not, and needs no extra FFT round-trip since the CFR is already
held in the frequency domain — the same technique Phase 1 uses for its smaller correction.

### 2.4 Phase 4 — closed-loop LOS pinning (`los_baseline_tracker`)

Wraps the *existing* `range_doppler` → `detection_report` chain — no second RD/CFAR/detection path.
Two entry points at different points in the per-CPI flow:

- **`update_residual()`** (late, right after `range_doppler::process()`): finds the detection nearest
  the established (or being-established) baseline, within `LOS_MATCH_MAX_RANGE_BINS`/
  `LOS_MATCH_MAX_DOPPLER_BINS` (5/5) on both axes. The first `LOS_BASELINE_INIT_CPIS=5` matched CPIs
  are averaged to set the baseline (initial guess before that: the same `nominal_bin`-derived range,
  zero Doppler, since the direct path is static once fully corrected — a physically-motivated guess,
  not an arbitrary constant). Once set, `residual = detection − baseline` on both range and
  Doppler/velocity, converted to a delay residual (`2·Δrange/c`) and a CFO-like residual
  (`2·Δvel·fc/c`).
- **`apply_bias_correction()`** (early, next CPI): applies whatever bias the loop has accumulated,
  via the same two correction primitives Phases 1-3 already established (frequency ramp for delay,
  uniform per-row rotation scaled by elapsed time for the CFO-like bias) — so it commutes with all of
  Phases 1-3 regardless of order.

**Leaky integrator**, not a pure one (bounds the bias state if the baseline match ever degrades for
several consecutive CPIs — e.g. a real target transiting the LOS's own range/Doppler cell):

```
bias = (1 − LOS_BIAS_LEAK) · bias + LOS_BIAS_KI · residual      (LEAK=0.02, KI=0.3)
```

One-CPI feedback latency (this CPI's residual informs *next* CPI's correction) is the expected shape
of a closed loop here, not an oversight.

---

## Status, as of 2026-07-23

- Range-Doppler pipeline (Part 1): production path, live-validated on B210 OTA and
  `tests/sensing_sim`. Chebyshev window (§1.3) is offline- and live-tested but opt-in.
- Sync compensation (Part 2): Phases 1-4 offline-tested (`tests/isac_sync_test.cc`, 9/9) and
  live-tested on `tests/sensing_sim` (20-run 2×2 batch). **Net result: `sync_correction` is currently
  net-negative on that harness** even with the Phase 1 walking-tracker fix, root-caused to the ~93%
  flywheel engagement rate above — not yet fixed, tracked in `SYNC_NOISE_HANDOVER.md`'s 2026-07-23
  addendum and `CLAUDE.md` §7. Phase 6b (real OTA sync validation, as opposed to sensing-sim) has not
  been run.

## Further reading

- `docs/NR_UE_ISAC_sync_gap_analysis.md` — original Phase 0-6a design record and per-impairment gap
  analysis (12 sections).
- `SYNC_NOISE_HANDOVER.md` — the sync-noise investigation, root-cause diagnosis, and the 2×2
  live-validation batch this document's §2.1 status note summarizes.
- `ECA_CLUTTER_HANDOVER.md` — full ECA/ECA+ derivation (§1.2).
- `openair1/PHY/NR_UE_ISAC/README.md` — module-level config reference (`[sensing]` fields).
