# Large-grant first-pass tail, 2026-10-17

Sonnet agent; saved by the orchestrator. Branch csd/int16-gprio, commit 47e0fc794b.
Verdict: PARTIAL. ISAC_PDSCH_GATHER_FAST=1 (default off, bit-identical) lowers large-grant (>= 4 CB) first-pass p50/p90 on both captures; tot p99 gain (b -2 %, tim -5 %) is within run spread.

## Where a large first pass goes [MEASURED, DGX replay b/tim, env t26cE, nsys -t cuda graph-trace=node, n=1/cap]
Large = LDPC grid >= 28 (b n=54, tim n=377), us p50/p90:
| phase | b | tim |
|---|---|---|
| chest kernels | 116/478 | 68/261 |
| chest end -> demod launch (host) | 300/535 | 287/661 |
| demod kernels | 210/308 | 223/332 |
| demod end -> LDPC launch (host) | 284/483 | 397/688 |
| LDPC kernel | 155/248 | 156/237 |
| total | 1355/1956 | 1369/2096 |
Kernels sum to ~460 us of a ~1300 us span; the gaps are host CPU work (no sync/D2H waits).
Host timestamps (b, cb 4-15, instrumented build, n=1): seg-path data-ordered gather (dst[(off0+i)%N]=src[(off0+gidx[i])%N] per RE x antenna x symbol) 220/446 us (cb 4-15), 200/360 (cb >= 16) vs 30/119 for cb 1-3.
Second cost (not fixed): LLR stats + unscramble 62 us (cb >= 16: 145; unscramble alone 51/132), pre-LDPC prelude 33/81, serial per-CB prep_segment ~100 (cb >= 16 ~250). Chest GPU call 204 us (cb 4-15) vs 78 small: k_delay_exact scales with rows (159 us at 448 rows).

## Fix
ISAC_PDSCH_GATHER_FAST=1: nr_prb_gather_copy() copies the gather as memcpy runs (ring wrap handled). Same bytes as the per-RE loop (unit test GatherCopyEqualsPerReLoopIncludingRingWrap). Gather 220 -> 39 us p50 (cb 4-15), 200 -> 47 (cb >= 16); nsys tim chest-end -> demod gap 287 -> 113 us p50 (661 -> 172 p90). Tests: test_nr_pdsch_gpu_ldpc (2/nc/mix/pick/cl2/cl4/sm), test_nr_gpu_pass (+discrete), test_nr_pdsch_prb_set pass.

## A/B [MEASURED, DGX replay <cap>, t26cE vs + flag, n=9 per arm (3x3 interleaved, one batch on-first)], median [min-max]
| cap | arm | tot p50 | tot p99 | tot p99.9 | cb4-15 first p50 | p90 | cb>=16 first p50 |
|---|---|---|---|---|---|---|---|
| b | off | 1448 [1286-1675] | 4626 [3735-5952] | 7574 [6401-13203] | 1352 [1121-1652] | 2330 [2059-2516] | 2591 [1910-4275] |
| b | on | 1493 [1401-1670] | 4524 [3537-5727] | 7417 [5520-13261] | 988 [880-1394] | 1665 [1545-2587] | 1755 [1345-3276] |
| tim | off | 1054 [998-1068] | 2749 [2524-2884] | 5102 [4021-6634] | 1271 [1214-1364] | 2025 [1849-2168] | 1921 [1722-2072] |
| tim | on | 1041 [1009-1068] | 2616 [2377-2718] | 6007 [4154-6934] | 1166 [1046-1264] | 1796 [1650-1889] | 1738 [1569-1843] |
crc_q b 94.0-96.9 off vs 95.4-97.1 on; tim 70.7-76.6 vs 72.4-77.8. Drops 0 except one b on run 0.7 %. n_real swings 1673-5497 (discovery timing).

## Next
Cut the demod-end -> LDPC host work: descramble in the GPU demod kernel (pass path has it, off in t26cE), NEON sign-mask unscramble, per-CB SIMD/parallel prep_segment.
