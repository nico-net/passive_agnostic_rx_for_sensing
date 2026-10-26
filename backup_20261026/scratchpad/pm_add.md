
**Update 2026-10-25 evening: wide rank-4 PDSCH on the lab cell, root causes (sens6 X410 327B872, RXG 36, 4 RX).**
- Sampling-clock drift inside the slot `[MEASURED, OTA lab, cbmapcpu2_lab_20261025_155639, sfo*_lab_20261025_*]`. The receiver clock is fast by
  +3.8 ppm (timing loop +3.77..+3.84 ppm; PDSCH DM-RS phase slope +3.78 ppm uncorrected, two independent device measurements). That moved the FFT window by eps*n_l
  inside every slot. The raw DM-RS symbol-difference SNR of 273-PRB grants was 10-15 dB at the band edges (narrow grants 34 dB).
  Fix: `nr_sfo_state.{c,h}` + `nr_sfo_rot.h` (one eps provider, bit-exact integer phasor, CPU and GPU). Per-symbol correction exp(+j 2pi (k-k_ref) eps n_l/N)
  in the DL FEP (nr_slot_fep*, GPU exact FEP, GPU pass), k_ref = SSB centre (the receiver's CFO is measured there and already
  contains eps*f_ssb; a DC reference re-added 165 Hz and cost narrow CRC). Commits 6859f63e2e, 4a1e19f8ac, 176afaf312, DL FEP commit (see git).
  After it: raw aligned DM-RS SNR 32-34 dB at every bin, slope residual 0.000 ppm, DC common term +13/+51 Hz (OFF: -245/-89 Hz).
  A/B (2 ON vs 2 OFF, k_ref): 33-64 RB rank-4 CRC 0.17-0.39 vs 0.006-0.24, narrow unchanged or better; >96 RB still ~0.
- The dominant limit for >~64 RB rank 4 is the 4x4 channel conditioning at the receiver `[MEASURED, pred_lab_20261025_175817]`.
  Measured receive noise ~35 dB LSB^2 per RE, signal-proportional, ~10 dB above the empty-RE floor. With the chest's H, the predicted per-layer SINR
  (capacity-equivalent) is ZF 24.8/25.3/20.9/16.6 dB for 1-4/5-32/33-128/>128 RB. MMSE with the correct noise adds +0.0/+0.0/+0.2/+0.6 dB.
  MCS 24-25 needs ~25 dB. EVMGRANT `snr` is decision-directed and saturates at 23-26 dB for 256QAM: it never showed this.
  The remaining lever is non-linear detection (ML/K-best/iterative LDPC-aided) `[PLANNED]`.
- Slot abs%40==4 CRC 0 `[MEASURED + CODE-READ]`: 4-port NZP CSI-RS for CSI (row 4 fd-CDM2, l0 4, id 2, P/O 40/4) never confirmed. The per-port score of a CDM port
  is bounded by 1/sqrt(L) (z 3.92 vs bar 4). The ZP-CSI-RS on symbol 8 of the same slot has no candidate. Branch sens6/csirs-cdm: CDM-despread scoring
  `[IMPLEMENTED, NOT VALIDATED]`; blind ZP and CRC arbiter `[PLANNED]`.
- Side findings: the per-path online trial picks pass_dl/demod GPU or CPU run to run (5-32 RB CRC 0.30 GPU pass vs 0.66 CPU, different runs, confounded).
  Solver lock on DCI layout 43 instead of 249 voided one run (sfoon_lab_20261025_170336). test_ldpc_cb0_cuda BatchSizesAreInvariant fails on sens6 (untouched code).
