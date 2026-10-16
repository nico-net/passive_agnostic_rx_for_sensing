# T14 cluster-2/4 LDPC + smem-resident variant, 2026-10-17

Sonnet agent (author-only review); saved by the orchestrator. Branch csd/int16-cldpc: 90d014ae5f (bench header), 6318363b78 (cluster-2/4 family, env-gated, bit-exact vs nc), 65704d972f (bench env print, sm mode), 71d4bc01d3 (ISAC_GPU_LDPC_SM smem-resident hard-decision state). CMake: 4 new add_test lines (pick, cl2, cl4, sm). Defaults off.
Verdict: R13 32 CB <= 200 us NOT met: best 369 us (SM=1) vs 441 us default. Cluster-2/4 helps C=4..20 (~2.2x at C=4..10, ~1.25x at C=12..16).

## Tests [MEASURED, GB10, build_t26]
Default, nc, mix, ul, pick, cl2 (cb2=408), cl4 (cb4=408), sm (366 smem launches; also under ISAC_GPU_FORCE_DISCRETE=1, functional only), ul with CL_MIN_CB=16: mismatches=0. Byte-for-byte clab A/B (BG1 Z=384, R13/23/89, C 1..32): nc vs cl2/cl4/cl8/SM=1 identical. Fail-first step skipped for Tasks 2 and 4.

## G0 baseline
sm_count 48, smem opt-in 99 KB, 128 regs -> 1 block/SM. nc C=32 run-wall p50 441.0/445.2/440.3 us; cluster-8 C=32 1231.6 us. 40-SM RT partition inferred.

## Task 3 [MEASURED, GB10 bench, RT_ISOLATE=1 DISC_SMS=8 POLL=200, 3x100 reps, it=8] R13 decodable run-wall p50 [p99 max] us
| C | nc | cl2 | cl4 | sel (CL_MIN_CB=4, NC_MIN_CB=8) |
|---|---|---|---|---|
| 1 | 348 [350] | 276 | 143 | 113 (cluster-8) |
| 4 | 350 [422] | 278 | 152 | 152 [155] |
| 8 | 353 [425] | 280 | 164 | 164 [201] |
| 10 | 354 [426] | 282 | 164 | 162 [282] |
| 12 | 356 [358] | 283 | 283 | 284 [285] |
| 16 | 359 [574] | 287 | 358 | 287 [432] |
| 24 | 364 [581] | 436 | 508 | 364 [509] |
| 32 | 441 [730] | 514 | 586 | 442 [658] |
R23: C4 129 -> 77, C16 140 -> 110, C32 158 -> 162; R89: C4 122 -> 97, C16 137 -> 122, C32 159 -> 160. sel beats nc for C 4..20 at all rates (p50 and p99). C 21..32 unchanged.

## Task 4 smem (SM=1, Z%4==0 single-block kernel only)
R13 C=1..16 unchanged; C=32 441 [730] -> 369 [512] (-16 %), noise 443 -> 371; R23/R89 -6 to -15 %; worst regression +0.1 %. Level 2 not built (kernel latency-bound, likely c2v loads 121 KB > 99 KB opt-in [INFERRED]).

## Recommended (daytime OTA A/B before default)
ISAC_GPU_LDPC_CL_MIN_CB=4 ISAC_GPU_LDPC_NC_MIN_CB=8 ISAC_GPU_LDPC_SM=1.
Unverified: on-air contention (cl2/cl4 SM pairs under 5 consumers; nc fallback untested under load, g_cl_fallback not yet printed), sm_89, A725 timing. cl_stats is now uint64_t o[5]. Raw: rr-orchestration/replays/clldpc_{base,bench,smem}/.
