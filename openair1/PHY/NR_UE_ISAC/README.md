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
