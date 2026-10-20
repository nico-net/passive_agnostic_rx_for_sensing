# GPU LDPC: tiny BG2 code blocks (K % 8 != 0) decoded on the GPU, 2026-10-16

Commit eb65a0d06d (csd/int16). Follow-up of T5-LDPC-REASONS-20261016.md. Sonnet agent; saved by the orchestrator.
Labels: [MEASURED, DGX replay <cap>, t26aXE, TMO=90, n=1]; b = raw_pci415_12s_b, tim = raw_pci923_tim_12s_tile3. Replay only. Baseline = T5 NC8 run 1.

## Change
- Cause (T5): DL PARAM refusals of BG2 R=15 Z in {7,11,15} (K=70/110/150, K%8=6) at the (j.K & 7) check (byte-packed K>>3 snapshot).
- Scalar kernel (Z%4 != 0) writes (K+7)>>3 snapshot bytes and zeroes the tail bits; Z%4 kernel unchanged (K%8 == 0 there).
- Refusal removed in nr_pdsch_gpu_ldpc_run, pass_set, nr_pusch_gpu_ldpc_run; host snapshot copies (K+7)>>3 bytes.
- UL: the same restriction existed host-side (decode_impl: G->ul && (K & 7) -> CPU, counted in fb_refuse); removed. Decoded-byte copy stays K>>3 like the CPU path; check_crc sees the full last byte.
- Not changed: GPUPASS entry points (nr_gpu_pass_dl.c, nr_gpu_pass_ul.c, nr_gpu_pass_graph.cu) still refuse Z&3 / K&7.

## Tests (test_nr_pdsch_gpu_ldpc 3 <mode>)
New BG2 Z=15/11/7 cases (3 BG2 rates, noise and junk, failing CRC included), GPU vs CPU by K bits (tail masked). Modes 2, nc, ul, mix: snapshots=3492 mismatches=0 stop_cases=571 stop_mismatches=0 cpu-fallback=0 each. Reason-code tests pass (PARAM now K=8449). Not run red first; not confirmed that the new cases reach the CRC-pass path.

## Replay
- DL fallback_err[param]: 0 on both caps (T5: tim 182, b 30); all reasons 0. check_bad=0, no MISMATCH.
- UL (b): fallback_err all 0; fallback[refused]=31 of 2000 TBs.
- crc_ok/decoded: tim 37.9 % vs 37.0 %; b 20.3 % vs 20.8 % (all decodes incl. probes). Unchanged.
- LDPC wall us p50/p99 (n), after vs baseline: tim CB 4-15 541/1041 (185) vs 549/1156 (225); CB 16+ 746/3210 (70) vs 769/2756 (56); b CB 4-15 435/1067 (415) vs 431/1238 (335); CB 16+ 909/1576 (12) vs 855/1519 (14). Stage p99 tim 1015 vs 915, b 815 vs 795.
- Reading: n=1; "not worse" plausible, not demonstrated.

## Open
GPUPASS entry points still refuse Z&3/K&7 (needed by the all-GPU pipeline).
