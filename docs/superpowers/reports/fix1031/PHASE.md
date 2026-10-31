# fix1031/phase -- D1 + D2: channel time interpolation and phase tracking for all layer counts (CPU + GPU)

Branch `fix1031/phase` (off `mpci-int-1031` 9d1649f17d). Audit items D1, D2 (docs/WEAKNESS_AUDIT_20261031.md). Results labelled
`[OFFLINE VERIFIED, DGX aarch64 unit]` are unit tests run on this host (DGX Spark, aarch64) through heavy.sh; nothing was run on air or on a replay.

## What changed

### Arithmetic (one source for CPU and GPU)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_phase_track_impl.h` (new): phase-aligned interpolation primitives (`delta_phasor`, `blend_phasor`, `interp_re`), the per-RE zero-forcing solve
  (`zf`, nl <= 4, nb <= 4, ill-conditioned REs rejected), the clipped 4th-power CPE statistic (`pow2`, `term4`), the CPE decision (`cpe_decide`) and the int16 rotation (`rot_re`).
  Instantiated for double (CPU, GPU bit-exact class) and float (GPU FP32 class) by macro; only + - * and explicit FMA / DIV / SQRT, so the CPU (`-ffp-contract=off`) and the GPU
  (`--fmad=false`) evaluate the same expression tree. No trigonometry in anything that rounds to int16 (the fractional phase of the interpolation is the chord-normalised phasor
  ((1-t) + t e^{jD}) / |.|; the CPE phasor e^{j phi} comes from two complex square roots of -S/|S|).
- `nr_pdsch_phase_track.{h,c}` (new): CPU API `nr_pdsch_phase_tinterp()`, `nr_pdsch_cpe_symbol()`, `nr_pdsch_cpe_sums()`. No file-scope state (stack arrays), so nothing for the multi-PCI static audit.
- `CMakeLists.txt`: the .c joins `PHY_NR_UE_SRC` with `-ffp-contract=off`; `test_nr_pdsch_phase_track` registered.

### D1 time interpolation
- CPU: `nr_pdsch_passive_decode.c` (after the chest, at the old `nr_dlsch_chest_per_symbol = 0;`): with >= 2 DM-RS symbols, not the segmented chest, not the single-branch chest, the data symbols
  between two DM-RS symbols get `((1-t) Ha + t Hb e^{-jD}) u` (D measured per RX branch and per block of 48 subcarriers from sum_l Hb conj(Ha): per-branch drift and a phase slope across
  frequency are interpolated too), the symbols outside the outermost pair hold the nearest DM-RS estimate; `nr_dlsch_chest_per_symbol = 1` makes nr_rx_pdsch() and the GPU demod geometry read
  the estimate at the data symbol. (Replaces `get_valid_dmrs_idx_for_channel_est` hold for these grants.)
- GPU pass: `k_tinterp<R>` in `nr_pdsch_gpu_demod.cu`, launched ahead of `k_gather` in `nr_pdsch_gpu_demod_pass_dev` (and the test-only `_dev`), writes the same planes of the device estimate;
  job fields `tint`, `tint_cols`, `tint_dm` (nr_pdsch_gpu_demod.h) set by `nr_gpu_pass_dl.c`, which also sets `nr_dlsch_chest_per_symbol` for the geometry it builds. The module demod
  (`gpu_grant`, DISCRETE staging) reads the CPU-interpolated host estimate and needs no kernel.
### D2 phase tracking
- CPE per data symbol and layer, every Nl: `nr_pdsch_cpe_symbol()` called in `nr_rx_pdsch()` right after the MRC ALIGN block (nr_dlsch_demodulation.c), twin `k_cpe<R>` after `launch_align` in all
  four GPU launch sequences. Blind (decision-free) clipped 4th-power statistic of the ZF output, S = sum (x^2/P)^2 over the symbol's REs, `4 phi = arg(-S)`; applied to the layer's channel column
  when coherent (|S|^2 > 4.5 Q), significant (|phi| > 1 sigma_phi, Wiener-like shrink sigma^2/phi near the threshold) and capped at 0.5 rad; a layer without a significant estimate takes the
  pooled one of all layers. This is the *blind* variant of a decision-directed estimator (same model, no slicer: immune to the unknown amplitude scale -- the DM-RS power boost makes the
  data constellation scale unknown -- and to wrong decisions at Qm 6/8). Passive receiver only (`IS_PASSIVE_RX_MODE`, as the MRC ALIGN).
- Per-RX-branch phase drift for Nl >= 2: part of D1 (the pair phase D is per branch); for the data symbols beyond the outermost DM-RS pair the common part is the CPE above.
### Counters (additive, EVMGRANT line)
- `cpe=` mean |CPE| applied per (data symbol, layer) in degrees (zeros included), `cpes=` fraction of (symbol, layer) pairs rotated, `evsd=` std (percentage points) of the per-symbol EVM across the data
  symbols (-1 = n/a). Fields `nr_pdsch_evm_acc.cpe_*`, `nr_pdsch_passive_decode_result_t.cpe_deg/cpe_frac/evsd_pct`; GPU counters via `nr_gpu_demod_job_t.cpe_phi` (Shared.cpe).

## Tests added and results
All `[OFFLINE VERIFIED, DGX aarch64 unit]` (GB10, via heavy.sh, ninja -j6).
- `test_nr_pdsch_phase_track` (CPU, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_phase_track_test.cc): synthetic slot, DM-RS on symbols 2/11, 100 PRB, 4 RX, Nl 1/2, Qm 2/6/8, CFO 0/50/150/300 Hz plus per-branch
  LO drift (0/+60/-80/+30 Hz), 4 independent slots per cell, data SNR 18/24/30 dB (the OTA EVM floor is ~ -16 dB). Results: worst-symbol post-ZF EVM of the old hold demodulator -17.7 / -10.9 / -5.3 dB
  (Qm 2, f = 50/150/300, grows with the distance from the DM-RS) against -22.1 dB flat for the tracker (Qm 2, Nl 1); Qm 6 Nl 1: hold -19.1/-11.1/-5.4 dB, tracker -26.6/-27.1/-26.1 dB; Qm 8 Nl 2: hold
  -17.3/-10.8/-5.3, tracker -22.2/-21.0/-21.8 dB. Mean (tracker - an estimate whose phase is known exactly) is <= 0 dB in every cell (the interpolation also averages two noisy estimates). Zero CFO:
  mean (tracker - hold) EVM <= 0 dB in every cell (no regression); CPE rotated on 0..15 of 40 symbols with mean |phi| <= 0.012 rad (the noise of the estimator, shrunk). Noise-only symbols: false
  rotation on 3 of 200. CPE alone: a known 0.13 rad extra rotation is recovered to 0.107..0.14 rad (Nl 1) and 0.12/0.16 (Nl 2, one symbol of 100 PRB; std ~0.015 rad per layer at 64QAM).
- `test_nr_pdsch_phase_track_gpu` (libpdsch_gpu.so, shared tier): `k_cpe` equals `nr_pdsch_cpe_symbol` bit for bit on 1,296,000 channel values (Nl 2/3/4, CFO 0/150/400 Hz, 110 layer rotations
  applied) and its counters match to 1e-5; `k_tinterp` equals `nr_pdsch_phase_tinterp` bit for bit (Nl 1/2/4). Both use the double (bit-exact class) instantiation.
- Regression: `test_nr_pdsch_gpu_demod`, `test_nr_pdsch_gpu_demod_{cpu,discrete}`, `test_nr_gpu_pass{,_discrete,_deq_direct,_deq_discrete,_oom,_compat,_cpu}`, `test_nr_llr_norm` pass. The existing fixtures
  build their demod job directly (cpe = tint = 0), so they are unchanged by construction. `test_nr_gpu_pass_cells` fails in HarqSlabsAreSeparate on the BASE build too (rr-int-1031: "HARQ slab outside the
  elastic GPU headroom", shared-GPU memory), not touched. `static_audit --check`: the four NEW rows (g_ul_dmrs_pin_ns, g_tp_spec_refuted, g_phs, g_ul_pin_seen_epoch) are on the base as well; this branch
  adds none (no file-scope state; TI_MAXD is a macro).
- Link-level bench (`nr_linklevel_bench`, product chain GPU FEP -> chest -> demod -> int8 LDPC, flat static 4-RX channel, new bench args `dj_cpe dj_tint cfo_hz`; variant A, TB failures / TBs,
  `trial_scale 0.4`, 1 dB steps; q8m configs crash in the bench as before):

  | cfg | SNR dB | cfo 0, old | cfo 0, new | cfo 150 Hz, old | cfo 150 Hz, new |
  |---|---|---|---|---|---|
  | q6m2 (64QAM, 100 PRB, Nl 2, R 567) | 13 | 48/132 | 17/132 | 114/132 | 24/132 |
  | q6m2 | 14 | 0/132 | 0/132 | 29/132 | 0/132 |
  | q6m2 | 15 | 0/132 | 0/132 | 2/132 | 0/132 |
  | m16 (16QAM, 100 PRB, Nl 1) | 9 | 100/100 | 98/100 | 100/100 | 99/100 |
  | m16 | 10 | 0/100 | 0/100 | 13/100 | 0/100 |

  No SNR point is worse with the tracker; with 150 Hz the 10 % BLER point moves by > 1 dB. The cfo 0 gain of q6m2 (48 -> 17 at 13 dB) is not explained by a CFO: the fixture's old demodulator holds
  the nearest DM-RS (symbol < 7 -> first, else second) and the time interpolation also averages the two noisy estimates. Raw lines: /tmp/claude-1001/phase_bench_saved.txt (not in git).

## NOT verified
- No replay, no OTA, no radio, nothing on a real capture. The FP32 class (float kernels on the RTX 4060 Ti of sens6, `demod_fp32`) and the DISCRETE tier (staged `nr_pdsch_gpu_demod_run`) are compiled
  and the DISCRETE unit tests pass, but k_cpe / k_tinterp were only run bit-exactly on the shared tier in double. The float instantiation is the same source with float operators.
- The CPE is blind (4th-power), not decision-directed as the task text says: the same signal model, no slicer. Reason: the data constellation amplitude is unknown (DM-RS boost) and decisions are wrong at Qm 6/8
  precisely where the CPE matters. Not compared against a decision-directed variant.
- The estimator std is ~0.012-0.017 rad per symbol for 100 PRB 64QAM (about 1/sqrt(N)); on 12-24 PRB grants most symbols fall below the 1 sigma gate and nothing is applied (by design).
- PT-RS grants: PT-RS REs inside the data symbols stay in the CPE statistic (a few % outliers, clipped); the per-RNTI PT-RS handling (task `ptrs`) should replace/feed this CPE for those grants.
- CPU cost: the interpolation is ~15 flops per RE x layers x branches x data symbols (the CPU path is the fallback); not timed. GPU: two extra kernels per pass (k_tinterp early-outs for < 2 DM-RS symbols),
  not timed on air.

## Expected on-air effect `[HYPOTHESIS]`
- EVM flat across the slot instead of growing with the distance from the DM-RS: EVMGRANT `evsd=` (std of the per-symbol EVM, percentage points) should drop, `evm=` / `snr_l=` of MCS >= 20 (Qm 6/8) and
  big Nl 2 grants should improve by the share of the ~15 % flat floor that is residual phase (the model predicts up to ~5 dB at 150 Hz residual for the farthest symbols, ~0 dB for symbols next to a DM-RS),
  first-pass CRC of Qm 6/8 should rise accordingly; zero effect expected on QPSK/16QAM near DM-RS and with 1 DM-RS symbol (no interpolation, CPE only).
- `cpe=` mean |CPE| in degrees (expect < 1 deg on a stable cell; > 2 deg flags oscillator phase noise or a CFO the FEP did not remove), `cpes=` fraction of (symbol, layer) rotated (expect 0.2-0.5).
  A cell where `cpe=` stays ~0 and `evsd=` is already small gets nothing from D2; a cell with large `evsd=` after this change has an error that is not a phase (timing/SFO, interference).

## Risks for the other fixes touching the same files
- `nr_dlsch_demodulation.c` / `nr_pdsch_passive_decode.c`: `ptrs` (PT-RS), `irc`, `rm`, `solver` edit nr_rx_pdsch() and the chest block. My hooks: one block after the MRC ALIGN (before the IRC prototype) and one
  block replacing `nr_dlsch_chest_per_symbol = 0;`. IRC: the CPE rotates `chFext` before the IRC covariance is formed, which is the intended order. `nr_pdsch_gpu_demod.cu`: new kernels, a Shared field (`cpe`),
  four launch sites (`launch_cpe`), one (`launch_tinterp`) in pass_dev / dev; the job struct gains fields in the kernel-visible part (before `evm`). `nr_gpu_pass_dl.c`: sets `nr_dlsch_chest_per_symbol` and the job's
  tint fields right after `nr_pdsch_gpu_demod_pass_job`. `nr_pdsch_evm_acc.h`: three counters appended. EVMGRANT: three tokens appended at the end of the line.
- Anything that reads `pdsch_dl_ch_estimates` at a data symbol and assumes it is stale/empty now sees the interpolated estimate when `nr_dlsch_chest_per_symbol == 1`.
