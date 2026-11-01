# PHASEF: PDSCH phase-tracker GPU cost (fix1101/phasef, base mpci-int-1031 a6e8f12e52)

## Finding: the production path is already float
The brief assumed the double instantiations run in production. They do not [OFFLINE VERIFIED, code read + ota_runs logs]:
- `nr_gpu_pass_graph.cu:1437` `g_fp32 = 1`, exported as `nr_gpu_pass_fp32_default()`; `nr_gpu_pass_dl.c:490` reads it into `G.fp32`, `:747` sets `dj.demod_fp32 = G.fp32`, `:848` puts it in the graph key
  (`NR_GPU_PASS_FP32_DEMOD`). `launch_tinterp`, `launch_cpe`, the IRC finalize and `k_align`/`k_mmse3_prep` select `<float>` on `jj.demod_fp32`.
- 351 of the 367 recorded `ota_runs/*/run.log` print "demod FP32 (sign/CRC shadow checks)" (the 16 "FP64 (bit-exact)" ones are older); the last runs (a1S508, a1S403, a2S923) are FP32.
- The double rows of `bench_nr_pdsch_phase_gpu` are the CPU-exact reference class used by the bit-exact unit tests only. So no default changed and no flag was added; the +72 us of the
  integration build is the FP32 cost itself (float bench sum 84.8 us at 217 PRB Nl2), and the work was to make the float kernels cheaper.

## What changed
1. `k_cpe_pass1/pass2` (`nr_pdsch_gpu_demod.cu`): thread per RE instead of thread per 12-RE chunk (block = 16 chunks = 192 threads); the chunk sums are then formed in RE order from shared
   memory, so the partials are the CPU's bit for bit in double (the existing bit-exact test passes unchanged). Pass 1 solves the ZF through `cpe_zf_re<NL, NB, R>` with the branch count as a
   literal (switch 1..4): the per-RE matrices stay in registers (STACK 160-480 B -> 0).
2. `k_irc_final` body (`irc_final_body`) now runs as the block z = 0 of the `k_tinterp` launch (grid z = TI_MAXD + 1; only block (0,0,0) works), beside the interpolation, not ahead of it
   (independent: the whitening is first read by the mode-3 solve). One launch less in the graph, its chain hidden behind the interpolation. `nr_irc_final_t` switches on nb (2/3/4) to literal
   branch counts; `nr_irc_final` is `NR_MMSE_FN_INL` (force-inline, CUDA only) so it unrolls (stack 544 B -> 0). CPU function unchanged.
3. `nrch::k_irc` (`nr_pdsch_gpu_fep.cu`, DM-RS covariance sums of the chest, integer, order independent): template on NB (v[33] int64 in registers instead of local memory) and a one-warp block
   (a group has <= 24 LS values; 128 threads left 3 warps idle and reduced 33 sums over all of them). `launch_irc` dispatches.
4. Bench `bench_nr_pdsch_phase_gpu`: extra column `prod(us)` = fused tinterp+irc_final + cpe + irc (the production launch set).
5. Test `PhaseTrackGpu.Fp32ClassMatchesDoubleClassRandomGrants` (`tests/nr_pdsch_phase_track_gpu_test.cc`).

## Precision test [OFFLINE VERIFIED, DGX aarch64 unit]
48 random 4-RX grants: Nl 1-4, 51/79/106/160/217/273 PRB, QPSK/16/64/256QAM, CFO +-400 Hz plus per-branch drift +-80 Hz, 20-35 dB, float class vs double class of the same module:
- CPE angle: 471 tracked layer-symbols, max |dphi| 4.3e-6 rad (tolerance 1e-4 rad = 0.8 LSB on a 2^13 channel).
- Rotated channel rows (17.3 M values): 0 differ by more than 1 LSB; interpolated estimate planes (20.8 M values): 0 differ by more than 1 LSB (the int16 rounding boundary is the only source).
- LLRs of the production (launch-only, interpolating) path, 12.8 M: 0.228 % differ at all, max |dLLR| = 2, none > 8 (bound = slope of 256QAM, 2^(Qm/2-1) LSB per LSB of rounded z), 1 sign
  difference in 12.8 M (gate asserted <= 0.1 %, the production `nr_gpu_pass_fp32_shadow` gate).
- Host-run path (not used for float in production): the whole float demod differs from the double one by 0.13 % sign flips with the tracker on AND off (16746 vs 16800), i.e. independent of the tracker;
  asserted: tracker on does not add sign flips (> 0.001 %) to the tracker-off number. Pre-existing, noted, not analysed.
Hard decisions = LLR signs, covered by the sign counts. IRC: `nr_irc_final_f` is exercised by the pass tests below (bit-equal sums, same finalize); no separate float-vs-double IRC LLR test.

## Bench, microseconds per launch, back-to-back (launch floor ~4 us), DGX GB10, 4 RX, float [MEASURED, DGX bench_nr_pdsch_phase_gpu 200, under flock]
Before = tinterp + cpe + irc + irc_final of base. After = production launch set (fused tinterp+irc_final, cpe, irc).

| PRB | Nl | before: tinterp / cpe / irc / irc_final = sum | after: tinterp+irc_final / cpe / irc = prod | saved |
|---|---|---|---|---|
| 106 | 1 | 8.2 / 22.6 / 8.2 / 12.1 = 51.1 | 10.0 / 16.4 / 4.4 = 30.8 | 20.3 (40 %) |
| 106 | 2 | 10.5 / 31.3 / 16.3 / 12.1 = 70.2 | 10.3 / 18.5 / 4.1 = 32.9 | 37.3 (53 %) |
| 106 | 3 | 11.1 / 51.2 / 14.3 / 12.1 = 88.8 | 12.3 / 23.8 / 4.1 = 40.2 | 48.6 (55 %) |
| 106 | 4 | 12.3 / 71.1 / 16.4 / 12.1 = 111.9 | 13.6 / 26.5 / 4.1 = 44.2 | 67.7 (61 %) |
| 217 | 1 | 10.3 / 30.2 / 12.4 / 12.1 = 64.9 | 12.3 / 26.4 / 4.1 = 42.8 | 22.1 (34 %) |
| 217 | 2 | 12.3 / 43.9 / 16.4 / 12.1 = 84.8 | 16.2 / 26.6 / 4.1 = 46.9 | 37.9 (45 %) |
| 217 | 3 | 16.7 / 68.9 / 23.0 / 12.1 = 120.7 | 16.3 / 28.7 / 4.9 = 49.9 | 70.8 (59 %) |
| 217 | 4 | 18.4 / 97.3 / 29.4 / 12.1 = 157.3 | 18.5 / 35.2 / 6.1 = 59.8 | 97.5 (62 %) |
| 273 | 1 | 10.7 / 33.6 / 13.7 / 12.1 = 70.2 | 12.3 / 24.7 / 4.1 = 41.1 | 29.1 (41 %) |
| 273 | 2 | 14.3 / 48.6 / 18.4 / 12.1 = 93.4 | 15.9 / 28.7 / 4.1 = 48.7 | 44.7 (48 %) |
| 273 | 3 | 16.4 / 77.4 / 26.7 / 12.0 = 132.6 | 18.5 / 31.9 / 6.1 = 56.5 | 76.1 (57 %) |
| 273 | 4 | 20.4 / 111.9 / 45.8 / 11.8 = 190.0 | 20.5 / 39.3 / 6.1 = 65.9 | 124.1 (65 %) |

217 PRB Nl2 (the controller's case): 84.8 -> 46.9 us (-38 us). In a CUDA graph the launch floor is lower than in this back-to-back loop, so the in-graph saving is smaller than the table for the
fused launch (about the 9 us the separate irc_final took), and larger-than-table for nothing. The per-grant gp[6] effect is `[HYPOTHESIS]`: roughly -25..-35 us of the +72 us at Nl2 (CPE 17 us, irc 12 us, irc_final ~9 us,
tinterp unchanged or +2 us from the extra z layer of empty blocks); not verified on air or in a replay (not allowed here).

## Other per-grant GPU work on the critical path (code read + nsys on `test_nr_gpu_pass` GpuPassDl.*, double class, mixed shapes, so only relative) [MEASURED, DGX unit, not production shapes]
| kernel | avg us | note |
|---|---|---|
| `nrdm::k_mmse3_prep<double>` | 53 | mode-3 prep (MMSE3GPU, not fix1031); float instantiation is what production runs, not separated here; candidate for a float-only measurement |
| `gpupass::k_rdm` | 44 | rate dematch, not touched by fix1031 |
| `nrdm::k_tinterp<double>` | 21.6 | (double, test shapes) float production figure is in the table |
| `nrdm::k_gather` | 15 | one launch per grant, independent of the phase work |
| `nrpass::k_unscr` + `k_llr_stats` (single block 1024) + `k_norm_sum` (single block 1024) + `k_norm_apply` | 7.8 + 7.7 + 3.4 + 5.1 | four serial launches after the demod; `k_llr_stats` and `k_norm_sum` both scan the LLRs on ONE SM: fuse them (one pass computing both) or run `k_llr_stats` on the EVM side stream; ~8 us, not done (touches the LLR-norm contract, needs gpu_pass bit tests) |
| `cudaMemsetAsync` of `irc_acc` (9 KB) per multi-layer pass | n/a | could be folded into the finalize (zero after use) but an aborted/gated pass would leave stale sums; not done |
| `k_level`, `k_mask_pw` | 8, 5 | pre-existing |
PTRS and RM (fix1031/ptrs, rm) add no GPU kernel per grant (host-side tables / slot-phase offsets); the phase/IRC set above is the whole fix1031 addition.

## Tests [OFFLINE VERIFIED, DGX aarch64 unit]
Build: `ninja -j6 -k 0 nr-uesoftmodem pdsch_gpu ldpc_cuda tests` clean (13120 steps). `ctest -R "gpu_pass|irc|phase|llr_norm|pdsch_gpu"`: 35/36 pass; `test_nr_gpu_pass_cells` fails identically
at the base (rr-int-1031 build, `GpuPassCells.HarqSlabsAreSeparate`), pre-existing. A first version of k_irc (32 threads for 33 sums) was caught by `test_nr_gpu_pass` /
`deq_direct` (bit-exact DM-RS covariance) and fixed. `tools/mpci/static_audit.py --strict` and `--check`: UNCLASSIFIED=0 (no new file-scope state).

## Not verified / risks
- No replay, OTA or radio. The in-graph timing (gp[6], dec p50, queue-wait) is untested.
- Floating results of the float class changed only by reduction order in none of the kernels (chunk sums keep RE order); the double class is bit-identical (existing bit-exact tests pass).
- The tinterp grid grew by one z layer (+25 % mostly-empty blocks): bench shows tinterp +0..2 us, covered by the fused irc_final.
- Files touched by other fixes: `nr_pdsch_gpu_demod.cu` (k_tinterp signature changed: any new caller must pass `(acc, w, sh)` or nullptrs), `nr_mmse_exact.h/_body.h` (new `NR_MMSE_FN_INL`), `nr_pdsch_gpu_fep.cu` (k_irc).
- Log fields: `GPUPASS` gp[] stage spans (gp[6] decode span), dec p50, queue wait; IRC counters `irc_active`/`irc_gain_db` unchanged semantics.
