# fix1031/irc — D4: DM-RS covariance whitening (IRC) in front of the mode-3 MMSE

Branch `fix1031/irc` off `mpci-int-1031`. Evidence labels: PROJECT_MEMORY §0.1. Host: DGX aarch64 (GB10), no receiver/replay/OTA was run.

## What it is

Per RB group g (8 PRB = 96 estimate subcarriers from the grant's first RB, `nr_irc_group_of`: a data RE of a DM-RS-compressed symbol is scaled back to its estimate index; the same index on the CPU chest and the GPU chest rows, whatever band the estimate covers) the spatial covariance of the DM-RS least-squares residual
across the RX branches (the residual the chest's `nvar` is made of: LS of the pilot pair minus the final estimate, one vector per LS value, summed over
all DM-RS symbols and layers) is turned into a whitening matrix W_g = scale * L^-1:

* exact int64 sums `nr_irc_sum_t` (order independent, so the CPU chest and the GPU kernel agree bit for bit);
* only the SHAPE of S is used (trace-normalised): the LS residual is smoothed/pair-averaged and sits ~20 dB under the data-domain noise (MMSE3CAL);
* shrinkage to the scalar model with the Ledoit-Wolf rule and the closed-form estimation error of a complex Gaussian sample covariance,
  `beta = min(1, (tr S)^2 / n_eff / ||S - mu I||_F^2)`, `n_eff = n/2` (neighbouring LS values share the FIR fit): white noise -> beta ~ 1, W ~ I; a real interferer -> beta ~ 0. No tuned constant;
* diagonal load 0.05 (bounds the amplification of quiet directions), `scale <= 1` (whitened h never exceeds raw h, so the int16 headroom of `nr_mmse3_gain` is unchanged);
* fallback to the scalar model (identity, `w[20] = 0`) when n < 2 nrx, the trace is not positive, a Cholesky pivot is not positive or the pivot spread exceeds 400.

Both passes of mode 3 (the decision-free residual that gives s2 when nrx > nl, and the solve) run on the whitened samples; the unbiased scaling
`w_l = (1 - s2 [A^-1]_ll) / [A^-1]_ll * gain` is unchanged (it is evaluated in the whitened domain with the matching s2). For nrx == nl the nvar-based s2
is multiplied by the groups' mean `noise_fac` (the whitened noise per dimension, from the model R = mu R~).

## Where (file:line at the head commit)

* `nr_mmse_exact_body.h`: `nr_mmse_exact_core` (R-typed inputs; `nr_mmse_exact_re` is now its int16 wrapper, same arithmetic), `nr_irc_final`, `nr_irc_whiten_re`,
  `nr_mmse_exact_re_w`, `nr_mmse3_re_w` (double and float instantiations).
* `nr_mmse_exact.h`: `nr_irc_sum_t`, group map `nr_irc_group_of` / `nr_irc_groups_of_re`, `nr_mmse3_symbol_w(..., wg, nre_full)` (`nr_mmse3_symbol` = it with no whitening, bit-identical).
* CPU: `nr_dl_channel_estimation.c: nr_pdsch_irc_cov()` (covariance from `rxdataF` + the final estimate, after the CPU chest, the GPU-exact chest or a cache hit);
  hook in `nr_pdsch_passive_decode.c` right after the chest-cache store; `nr_dlsch_demodulation.c: nr_dlsch_mmse` uses `nr_irc_grant` (`nr_irc_cov.h`, thread-local, cleared at the start of every decode).
* GPU host-run demod (`nr_pdsch_gpu_demod_host.c` build_job -> `nr_pdsch_gpu_demod.cu` `irc_w`/`irc_ng` job fields, H2D of the matrices): `k_mmse3_prep` / `re_mmse3` whiten per group.
* GPU graph pass (default DL path): `nr_pdsch_gpu_fep.cu: k_irc` + `nr_pdsch_gpu_chest_irc_at` (adds the residual of every chest chunk after `k_fir`, reading the LS scratch and the estimate rows),
  `nr_gpu_pass_graph.cu` (zero + accumulate in the C sub-graph, contiguous grants, nl >= 2), `nr_pdsch_gpu_demod.cu: k_irc_final` (head of the D sub-graph, the instance's context; float or double by `demod_fp32`).
* Log: `EVMGRANT ... irc=<gain_db>,<groups>` (additive, last field). `-99.0,0` = scalar model.
* `tools/mpci/mpci_statics.tsv`: one TLS row (`nr_irc_grant`).

## Tests

`[OFFLINE VERIFIED, DGX aarch64 unit]` `test_nr_irc` (new, 5 tests), 4 RX, per-group sums built like the chest (int16-quantised residual vectors):
* one co-channel interferer (INR 10 dB per RX, fixed spatial signature per group): SINR gain over the scalar MMSE 4.40 dB (nl 2), 3.17 dB (nl 3), 0.80 dB (nl 4 square, nrx == nl); estimate 6.0 dB;
* white noise: loss 0.000-0.001 dB at n = 24/48/96 residual vectors per group (nl 2, 3, 4);
* fallback (no samples, zero covariance, n < 2 nrx, one branch) = identity; an inactive group reproduces `nr_mmse3_symbol` bit for bit.

`[OFFLINE VERIFIED, DGX aarch64 unit]` `test_nr_llr_norm` `MmseExact.GpuMode3IrcBitExact` (new) and `GpuMode3BitExact` (existing): GPU mode 3 with per-group whitening (3 groups, one inactive) vs `nr_mmse3_symbol_w`:
0 of 1078272 int16 outputs differ, and the test fails if the whitening changes nothing.

`[OFFLINE VERIFIED, DGX aarch64 unit]` graph pass == old path: `test_nr_gpu_pass` 77 tests, 0 failures with IRC on in the graph (including `GpuGraphCover.ShareWidenedChestEqualsStagePath`, `GpuGraphSplit.*`, `GpuGraphDmrs2.Type2EqualsStagePath`,
`GpuPassSized.*`); the fixture's stage path now computes its own covariance on the host from the chest rows (independent of `k_irc`) and feeds it through `nr_pdsch_gpu_demod_dev`. Also passing: `test_nr_pdsch_gpu_demod(_discrete/_cpu/_model)`, `test_nr_gpu_pass_discrete/_deq_*/_oom/_compat/_quota/_geo(_discrete)`, `test_nr_gpu_pass_cpu`, `test_nr_mrc_weights`, `test_nr_llr_norm` (16 tests).

`[OFFLINE VERIFIED, DGX aarch64 bench]` link-level bench (`nr_linklevel_bench ml n4q4,n3q4 c95,fc95`, 60 trials/SNR, white noise, the product graph pass): TB failures with IRC on == IRC off at every SNR (n4q4 c95 38/21/16/9/9/7/7 of 60 at 18..30 dB, n3q4 c95 7/1/0/0 at 18..24) -- the covariance of white noise gives W ~ I, no regression.
(The off arm was a local, uncommitted seam in `prepare()`; the committed code has no switch.) The bench has no interferer arm; the interference gain is the unit test.

Pre-existing failures on the base `mpci-int-1031` (same result with its own binaries, not caused by this change): `GpuPassCells.HarqSlabsAreSeparate` (`test_nr_gpu_pass_cells`), `static_audit_check` / `test_static_audit_scanner` (rows NEW from other commits; this change adds its own TLS row and no new one).

## NOT verified

* The covariance model uses the chest's own LS residual (pair-averaged, smoothed). Its spatial shape equals the interference+noise shape only if the interference is stationary over the pair and the FIR window; a fast-fading or wideband-varying interferer is
  estimated worse. `[HYPOTHESIS]`
* Nl = 1 (the noise-weighted MRC, `t_mrc_wq` / `align`) is NOT changed: rank-1 is not a mode-3 path on either side, and a whitened rank-1 would have to replace the per-branch noise weights and the branch-mask logic of both implementations. Left for a follow-up; the Nl 2..4 mode-3 path (production at 4 RX) is covered.
* Segmented (PRG) grants and non-contiguous allocations keep the scalar model (the sums stay zero / `nr_pdsch_irc_cov` refuses); the slot-share chest band is handled (band base = grant base).
* No on-air or replay run (rule).

## Expected on-air effect `[HYPOTHESIS]`

At an interference-limited position (415, co-channel PCIs 380/249; D4 estimates ~2 dB at Nl 2 Qm 6) the whitened mode 3 gains of the order of the unit-test gain scaled by the real interferer-to-noise ratio; at a noise-limited position
`irc=` reads ~0 dB and the output equals the scalar path. Look at `EVMGRANT irc=` (gain estimate, usable groups), `evm=`/`snr_l=` per layer, and CRC per MCS/Nl against the same cell's `irc=-99` decodes.

## Risks for other fixes

* `nr_pdsch_gpu_demod.h` (job: `irc_w`, `irc_ng`, outputs `irc_gain_db`, `irc_active`), `nr_pdsch_gpu_demod.cu` (Shared, Ctx), `nr_gpu_pass_graph.cu` (C sub-graph), `nr_pdsch_passive_decode.c/.h` (hook, result fields), `nr_pdsch_passive_queue.c` (EVMGRANT format), `nr_mmse_exact*.h`.
* A fix that changes the DM-RS/chest geometry (phase, ptrs, solver) must keep `nr_pdsch_irc_cov` and `k_irc` aligned with `chest_ls_type1/2` (LS index k = estimate index k, group = k / 96).
