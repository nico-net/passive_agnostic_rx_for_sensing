# NR_UE_ISAC — OAI-UE ISAC / passive-radar sensing pipeline

A faithful port of the srsUE sensing pipeline (`repos/srs-ue-isac-dmd`,
`srsue/src/phy/nr/sensing/*`) into the OAI UE. The real-time NR DL procedures tap the
per-RE channel-frequency-response (Ĥ = Y/X) at reference REs and hand it — best-effort,
off the RT critical path — to a dedicated engine that produces a range-velocity map and
emits DetectionReports compliant with the central node (`repos/isac`,
`crates/isac-core/src/report.rs`).

## Pipeline (matches the srsUE stages 1:1)

1. **CFR extraction** (RT tap): Ĥ at CSI-RS LS positions, PDSCH DM-RS positions, or every
   allocated subcarrier of the DM-RS-interpolated estimate (data-aided density).
2. **CPI accumulation**: each contributing real slot = one slow-time row on a **full per-subcarrier
   grid** (column = absolute subcarrier relative to CRB0), so several sources (`sensing.sources`)
   fuse onto one grid before the transform. Rows are absolute-slot-indexed; two sources landing in
   the same slot merge into one denser row (last-write-wins per subcarrier). See
   `sensing_engine::accumulate_cpi` and `CFR_FUSION_HANDOVER.md`.
3. **Stage 4b interpolation**: linear fill of comb gaps in frequency + resampling of the
   non-uniform (bursty) slow-time onto a uniform grid.
4. **Range-Doppler DSP** (`range_doppler`): clutter removal (per-subcarrier slow-time mean
   subtraction, or opt-in **ECA/ECA+** — see below) → Hann-windowed range IFFT →
   Hann-windowed Doppler FFT (fftshift) → |·|² → zero-Doppler/zero-range notch.
5. **Detection**: 2D CA-CFAR (integral-image accelerated) + greedy non-max suppression.
6. **Outputs**: `<out_path>_detections.csv`, optional `<out_path>_rvm_<n>.f32` raster, and
   `<out_path>_reports.jsonl` / a ZeroMQ PUB bus of DetectionReport JSON (isac-compliant).

The off-RT DSP uses a self-contained arbitrary-length DFT (`isac_fft`, radix-2 + Bluestein)
because OAI's built-in DFT only supports the fixed OFDM sizes.

## Files

| file | role |
|------|------|
| `nr_isac.h` / `nr_isac.cc` | C API (RT taps + lifecycle), `[sensing]` config parsing, global engine |
| `defs_nr_UE_ISAC.h` | internal C++ structs (args, snapshot, RVM, detection) |
| `isac_fft.{h,cc}` | arbitrary-N complex DFT |
| `eca_clutter.{h,cc}` | ECA/ECA+ CFR-domain clutter cancellation (opt-in `clutter_removal = "eca+"`) |
| `range_doppler.{h,cc}` | clutter removal, range/Doppler transforms, CA-CFAR, NMS, self-test injection |
| `sensing_engine.{h,cc}` | consumer thread, CPI accumulation, interpolation, outputs, ZeroMQ |
| `detection_report.{h,cc}` | DetectionReport JSON serialiser (isac wire contract) |
| `isac_aoa.{h,cc}` | receive-array angle of arrival per detection cell (opt-in `aoa_enable`) |
| `isac_sync.{h,cc}` | OTA STO/CFO/SFO/closed-loop sync correction (see below) |

Hooked from `SCHED_NR_UE/phy_procedures_nr_ue.c` (PDSCH DM-RS / data-aided) and
`PHY/NR_UE_TRANSPORT/csi_rx.c` (CSI-RS); lifecycle from `executables/nr-uesoftmodem.c`.

### UE-agnostic CSI-RS collection (`csirs_monitor`)

The CSI-RS hook above fires only for the receiver's *own* RRC-configured CSI-RS. To also
collect **cell-common / other-UE CSI-RS** (e.g. the srsRAN tracking NZP-CSI-RS) for passive
sensing, configure `sensing.csirs_monitor` with the resources' parameters (known from the
cell config in `gnb_remote_logs/`). `PHY/NR_UE_TRANSPORT/nr_csirs_monitor.{h,c}` parses the
list and answers per-slot which resources occur; `phy_procedures_nr_ue.c` then runs each due
resource through `nr_ue_csi_rs_sensing_capture()` (csi_rx.c) — a lean path that estimates Ĥ
and feeds the ISAC engine but performs **no** RI/PMI/CQI measurement and emits **no** CSI
report to MAC/gNB. This makes CSI-RS collection independent of what the receiver's own MAC
schedules, without disturbing the RRC/MAC state. NZP CSI-RS only (the type the PHY estimation
path handles). See the `csirs_monitor` block in `nrue.uicc.conf` and runbook §4.2.

## OTA synchronisation (STO/CFO/SFO/closed-loop LOS)

**Problem it solves**: the receiver and the illuminating gNB/UE are two independent, free-running
clocks (no shared reference — the design target is autonomous UEs, not GPSDO-disciplined ones).
That mismatch shows up as three distinct impairments in the CFR grid before anything downstream
(range-Doppler, CFAR) can trust it:

| Impairment | What it is | Effect on the CFR grid |
|---|---|---|
| **STO** (sample-timing offset) | fixed + slowly-varying sub-sample delay | rotates phase *linearly across subcarriers*, same for every subcarrier's slope |
| **CFO** (carrier-frequency offset) | residual LO mismatch (Hz) | rotates phase *uniformly across a row*, common to every subcarrier |
| **SFO** (sample-frequency offset) | sample-clock rate error (ppm) | STO's slope *itself drifts over slow-time* — a ramp, not a constant |

All three are estimated from the **direct-path (LOS) tap** — the strongest, most stable feature in
the channel — and corrected on the raw per-subcarrier CPI grid, before Stage 4b interpolation.
Not one paper's algorithm: a purpose-built pipeline (`docs/NR_UE_ISAC_sync_gap_analysis.md`,
`tasks/ota_sync_passive_ue.md`) assembled from individually well-known DSP building blocks.

### Phase 1 — STO (`cpi_sto_tracker`)

Per row: IFFT the row's occupied subcarriers into a compact CIR (channel impulse response,
Hann-windowed to reduce interpolation bias), find the power peak within a search window, then
refine to sub-bin precision with a **complex-domain Jacobsen/Candan-form ratio estimator**
(`subbin_delta()`) — the one piece of this stack that's a named, citable technique (DFT-based
fractional-peak interpolation), clamped to ±0.5 bin.

The window **walks**: row *r*'s search is centered on row *r-1*'s own found peak (±2 bins), not a
fixed nominal bin — so cumulative drift across a long CPI can be arbitrarily large as long as the
*per-row step* stays small. Row 0 seeds from a caller-supplied nominal LOS bin with a wider ±4-bin
window. A **flywheel** guards against fades: if a row's in-window peak doesn't clear a fade/SNR
gate, the walker doesn't trust it — it projects the center forward using the last cross-CPI SFO
estimate instead of trusting a spurious local maximum, and marks that row excluded from every
downstream fit.

If the CPI's fitted drift classifies as flat ("constant"), a single frequency-domain phase ramp
nulls the common fractional delay across every row. The coarse/integer bin is deliberately left
alone — Phase 1's scope is the sub-bin residual only.

**Known constraint** (found via the office bench, `tests/ota_sync_bench/`): the search window is
*not circular* — `lo = max(1, center-halfwin)`, `hi = min(M-2, center+halfwin)` — so a residual
sitting near bin 0 or bin *M-1* falls outside a window that can't wrap. Real deployments don't hit
this (the LOS tap sits at a comfortable mid-array bin), but a bench feeding a near-zero residual
needs to deliberately offset it first.

### Phase 2 — CFO (`cpi_cfo_tracker`)

Reuses Phase 1's per-row LOS-tap phase (no second CIR search). Sequentially unwraps that phase
across valid rows in time order, least-squares fits the unwrapped sequence vs. each row's own
absolute time — the slope is the residual CFO. Correction fully de-rotates each row by its own
raw observed LOS phase (not just the fitted trend), since a CFO/CPE is a uniform-across-subcarriers
rotation and the LOS tap directly measures it.

**Known constraint**: the sequential unwrap assumes the true phase step between *consecutive
valid* rows stays under π. This breaks down over very long spans with many skipped
(flywheeled/excluded) rows — cumulative unwrap error compounds. Confirmed on the bench: accurate to
<2% at CPI spans of tens of milliseconds, diverged (residual phase RMS of hundreds of radians) at a
multi-second span. Production CPIs (128–512 slots, i.e. 64–256 ms) are far below where this bites.

### Phase 3 — SFO (`cpi_sfo_tracker`)

Does **not** reuse Phase 1's estimates — SFO can walk the LOS peak by hundreds of bins over a long
CPI, far past Phase 1's narrow per-step window. Runs its own sequential tracking search (sharing
only the CIR-building step), with an ISI-contamination gate: candidate rows are excluded if their
out-of-window CIR energy is anomalous relative to a running per-comb baseline. The surviving
`{time, delay}` pairs are least-squares fit; the slope is directly the fractional sample-clock error
(ppm, no unit conversion beyond ×1e6) — a real clock error drifts *linearly*, so `r_squared` is the
built-in confidence check. Correction is a per-row frequency-domain phase ramp scaled by each row's
own elapsed time since CPI start (the intercept — the coarse/absolute delay level — is deliberately
excluded, mirroring Phase 1's scope boundary).

A cross-CPI EMA (`sfo_ppm_filtered`) damps one noisy CPI's fit from being applied at full strength;
this is what the correction actually uses, not the raw per-CPI value.

**Known constraint**: needs real elapsed *time*, not just row count, to resolve a small ppm drift —
1 ppm over a few hundred ms is a fraction of a bin, statistically unmeasurable (`r_squared` near 0,
correction correctly withheld). Confirmed accurate on the bench (<6% of true value, `r_squared >
0.999`) once given a multi-second window.

### Phase 4 — closed-loop LOS pinning (`los_baseline_tracker`)

Wraps the *existing* range-Doppler/CFAR/detection chain rather than adding a second one. After each
CPI's detections are produced, it finds the one nearest an established LOS baseline (range +
Doppler), measures the residual, and folds it into a **leaky integrator** (`bias = (1-LEAK)*bias +
KI*residual`) — a leak, not a pure integrator, so the bias state stays bounded even if the baseline
match degrades for several CPIs. That bias is applied as an upfront correction (same frequency-ramp
/ uniform-rotation primitives as Phases 1–3) at the *start* of the next CPI, before Phases 1–3 run —
a one-CPI-latency closed loop by design.

### Order and composition

`sensing_engine.cc` runs Phase 4's stored bias, then Phase 1 → 2 → 3, in that fixed order per CPI
(order among 1–3 doesn't matter mathematically — each is a distinct phase-rotation primitive that
commutes with the others). All four are gated independently under `sync_correction` (master) plus
per-phase `sync_sto`/`sync_cfo`/`sync_sfo`/`sync_los` switches, so any one can be isolated for
testing without touching the rest.

## Configuration — add a `[sensing]` section to the UE `.conf`

```
sensing = {
  enable          = 1;          # master switch (0 = zero overhead)
  source          = "pdsch_dmrs"; # single source (legacy): csi_rs | pdsch_dmrs | pdsch_data
  # sources       = "csi_rs,pdsch_dmrs"; # CFR-fusion enabled set (overrides `source` when set);
                                #   several sources feed ONE per-subcarrier grid before the range-Doppler
                                #   transform. Fused reports carry ref_type = "fused(csi_rs+pdsch_dmrs)".
  cpi_slots       = 256;        # coherent processing interval (slow-time length)
  interpolate     = 1;          # fill comb gaps + resample non-uniform slow-time
  capture         = 0;          # also dump RVM raster + rvm_blob in the reports

  # detection
  cfar_guard      = 4;
  cfar_train      = 8;
  cfar_pfa        = 1e-3;
  zero_doppler_guard = 3;
  zero_range_guard   = 2;
  nms_range_bins     = 3;
  nms_doppler_bins   = 3;
  max_detections     = 32;

  # clutter cancellation (ECA_CLUTTER_HANDOVER.md)
  clutter_removal     = "mean";  # "mean" (default, per-subcarrier slow-time mean) | "eca+"
  # ECA/ECA+ (eca_clutter): a CFR-domain oblique projection that removes the near-zero-Doppler
  # clutter band over a bounded delay window *in the complex domain, before the range IFFT*, so the
  # static/slow residual that produces the conjugate mirror ghost never reaches the transform (the
  # fix lives upstream of detection, not as the post-CFAR conj_image_reject band-aid, which stays on
  # as a safety net). Because the sources are Ĥ = Y/X, the disturbance subspace is spanned by ideal
  # delay/Doppler atoms and the projection factorises into a delay (frequency) and a Doppler
  # (slow-time) projector: H_clean = H − P_Φ·H·P_Ψ. Mean subtraction is the exact special case
  # (eca_delay_max_m = 0 / full, eca_doppler_max_mps = 0).
  # eca_delay_max_m     = 0.0;   # delay removal window [0, x] m; <=0 => full range
  # eca_doppler_max_mps = 0.5;   # Doppler removal half-band [-x, +x] m/s around zero velocity
  #   NOTE (ECA blind speed): any real target inside [-eca_doppler_max_mps, +eca_doppler_max_mps] is
  #   cancelled along with the clutter — keep this band narrower than the slowest target of interest.
  #   IMPORTANT: this is an ABSOLUTE m/s band, but what matters is its size in BINS = band/vel_res.
  #   A long CPI / slow PRF has a very fine vel_res (e.g. ~0.04 m/s in the sensing-sim), so 0.5 m/s
  #   there removes ~12 Doppler bins and will eat a slow target. Tune it to ~zero_doppler_guard*vel_res
  #   (a handful of bins). The 0.5 default suits coarse-vel_res OTA CPIs, not fine-resolution ones.

  # DSP self-test (optional): inject synthetic echoes into the live CFR
  # selftest         = 1;
  # selftest_targets = "3.0:20:0.6,6.0:-15:0.4";  # DELAY_US:DOPPLER_HZ:GAIN

  # outputs
  out_path        = "/tmp/oaiue_sensing";  # <out_path>_detections.csv / _rvm_*.f32 / _reports.jsonl
  # report_path    = "";                    # override JSON-lines path
  # report_endpoint = "tcp://127.0.0.1:5556"; # live ZeroMQ PUB bus (SUB-connect from isac-track bus)

  # ---- receive-array AoA (PHASE3_AOA_MULTISTATIC_HANDOVER 5.4). Opt-in; costs nothing when off.
  # Needs a phase-coherent multi-channel receiver AND nr-uesoftmodem started with a matching
  # --ue-nb-ant-rx. The engine logs a loud one-shot warning if that is forgotten, because otherwise
  # every bearing is simply absent with nothing to say why.
  # aoa_enable       = 1;
  # rx_array         = "0,0;0.0439,0;0.0878,0;0.1317,0";  # element offsets (m), ARRAY frame
  # rx_array_boresight_deg = 90.0;   # rotation of that frame into ENU (deg CCW from east)
  # aoa_estimator    = "beamscan";   # beamscan | interferometry | music
  # aoa_scan_step_deg = 1.0;
  # aoa_min_snr_db   = 6.0;          # below this the azimuth is OMITTED, not guessed
  # aoa_cell_search_bins = 2;        # local re-peak around each detection cell
  # aoa_broadside_deg = -1000.0;     # which half-plane a LINEAR array scans (-1000 = its own normal)
  # aoa_selfcal      = 1;            # calibrate per-channel phase against the known-bearing LOS tap
  #
  # In simulation the SAME element list must also appear in [sensing_channel] rx_array — that section
  # is what the air actually carries, this one is what the estimator assumes, and the two disagreeing
  # is a silent-garbage failure, not a degraded one. make_scenes.py emits both from one source.
  #
  # A LINEAR array cannot separate a bearing from its mirror about the array axis (physics: the phase
  # depends only on d.u). Only a genuinely 2-D element layout removes that.

  # ---- position-anchored harmonic rejection (GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md Phase A)
  # The same test as harmonic_reject, with a strictly tighter anchor: two detections must localise
  # (range + bearing -> ray n bistatic ellipse) to the SAME reflection, rather than merely share a
  # range bin. Harmonics are one reflection mis-binned in Doppler, so they share range AND bearing;
  # two distinct targets sharing a range bin -- common -- almost never share both.
  # Runs in sensing_engine.cc AFTER the AoA block (azimuth does not exist earlier). Inert without
  # bearings. Logs its per-CPI and cumulative rejection count whenever it fires.
  # It does NOT catch an ORPHANED harmonic (fundamental undetected this CPI) -- that is Phase B, and
  # it lives in repos/isac, where a track carries position and velocity to predict the rate from.
  #
  # MEASURED 2026-07-30: a flat metre gate LOST to the existing range anchor -- the two fixes of a
  # real harmonic pair are dominated by INDEPENDENT bearing noise (~1.9 deg CRB here), so a gate tight
  # enough to reject unrelated targets was also too tight to hold real pairs together. The tangential
  # (bearing-driven) residual is therefore chi2-normalised by each detection's OWN azimuth_std_deg;
  # only the radial residual (no per-detection range sigma exists) stays a flat metre tolerance.
  # harmonic_pos_reject      = 1;
  # harmonic_pos_chi2        = 9.0;  # 1-dof chi2 on the tangential residual, 9 = 3 sigma
  # harmonic_pos_range_tol_m = 8.0;  # flat tolerance on the radial residual only

  # surveyed geometry / logical ids for the central node
  rx_id           = "rx1";
  illuminator_id  = "gnb1";
  rx_pos_x        = 0.0;  rx_pos_y = 0.0;
  tx_pos_x        = 0.0;  tx_pos_y = 0.0;
};
```

## Consuming the output with the central node (`repos/isac`)

```
# file replay:
isac-track replay /tmp/oaiue_sensing_reports.jsonl
# live bus (set report_endpoint above):
isac-track bus tcp://127.0.0.1:5556
```
