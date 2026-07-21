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
4. **Range-Doppler DSP** (`range_doppler`): per-subcarrier clutter mean subtraction →
   Hann-windowed range IFFT → Hann-windowed Doppler FFT (fftshift) → |·|² →
   zero-Doppler/zero-range notch.
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
| `range_doppler.{h,cc}` | clutter removal, range/Doppler transforms, CA-CFAR, NMS, self-test injection |
| `sensing_engine.{h,cc}` | consumer thread, CPI accumulation, interpolation, outputs, ZeroMQ |
| `detection_report.{h,cc}` | DetectionReport JSON serialiser (isac wire contract) |

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

  # DSP self-test (optional): inject synthetic echoes into the live CFR
  # selftest         = 1;
  # selftest_targets = "3.0:20:0.6,6.0:-15:0.4";  # DELAY_US:DOPPLER_HZ:GAIN

  # outputs
  out_path        = "/tmp/oaiue_sensing";  # <out_path>_detections.csv / _rvm_*.f32 / _reports.jsonl
  # report_path    = "";                    # override JSON-lines path
  # report_endpoint = "tcp://127.0.0.1:5556"; # live ZeroMQ PUB bus (SUB-connect from isac-track bus)

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
