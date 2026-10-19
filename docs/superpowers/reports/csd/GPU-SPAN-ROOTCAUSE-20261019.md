# DL GPU span: root cause (p50 and p99), 2026-10-19

Labels: [MEASURED source, run] = on-air data, daytime 2026-10-19, merge19 + measurement commit d2dcce42d1, FROZEN_20261018.env. [BENCH] = synthetic unit test, not on air, never used as proof. [INFERRED] = reasoning. Cells are never pooled. All values in µs.

## 0. Runs and tools

| run | cell | what |
|---|---|---|
| gsT1 = ab_gsT1_1_165236 | TIM | unprofiled, 150 s, 23 323 real grants on the pass, EVMGRANT `gq=` / `li=` |
| gsT2N = ab_gsT2N_1_173659 + nsys_gsT2N | TIM | nsys `-t cuda,nvtx --cuda-graph-trace=node -y 90 -d 20 --kill=none`; 5 966 passes, all joined to their log line by NVTX seq |
| gsV2N = ab_gsV2N_1_174103 + nsys_gsV2N | Vodafone | same; 7 302 passes |

The Vodafone unprofiled runs crashed (PDCCH SEGV, handled elsewhere). Vodafone numbers are therefore from the profiled run only. nsys inflates absolute times; GPU-side ordering and durations are what is used from it.

Tools are in `tests/passive_rx/gpuspan/`. passes.py rebuilds each pass from the trace: C/D/L, the conditional-body kernels (attributed by graphNodeId), and each other kernel classified by the thread that issued it. passrep.py joins passes to EVMGRANT by seq and reports per-pass timelines, gaps, stretch and queue. gqrep.py reads the gq/li fields. p50attr.py, gapnext.py, tailattr.py, gapdl.py, launchblk.py and refine.py (same directory) produce §1–§2.

## 1. p50: what a typical real grant's GPU span is made of

### Example typical real pass

[MEASURED gsT2N, seq 31304, TIM]: CRC fail, TBS 5376, 16 RB, 1 code block (li: ncb 1, at the iteration limit, 86 µs). Span 620.

| t (µs) | event |
|---|---|
| 0 | C: k_fep_exact (69 µs) |
| 69 → 251 | **182 µs gap**: only CSI-RS discovery ran on the GPU |
| 251 → 307 | memcpy, k_ls, k_delay_exact 33, k_fir 8 |
| | D: chest_post 13, gate_d 7 |
| 334 → 378 | **IF-D body launch, 45 µs, nothing of its own** |
| 378 → 456 | demod + EVM chain + LLR post, 78 µs |
| | D→L boundary 7.5, gate_l 2 |
| | **IF-L body launch, 21 µs** |
| 486 → 619 | rdm 20, LDPC 88, CRC/pick 25 |

### Typical real passes (span p45–p55), mean per pass

Source: p50attr.py and gapnext.py on the joined nsys passes.

| component | TIM gsT2N (span 624) | Vodafone gsV2N (span 609) |
|---|---|---|
| own kernels at their solo speed (solo = p50 of the same kernel and grid with nothing overlapping) | 339 (54 %) | 342 (56 %) |
| kernel slow-down while others ran: other DL pass instances | 46 + 11 (with DL non-pass) | 35 + 8 |
| … while only CSI-RS discovery ran (separate SM partition) | 71 | 22 |
| … DL non-pass, PDCCH-NID, UL | 16 | 12 |
| … with nothing overlapping (data-dependent spread, unattributed) | 24 | 41 |
| gaps: **IF-D body launch** | 50 (p50 47, p90 61) | 49 (p50 46, p90 64) |
| gaps: **IF-L body launch** | 24 (p50 21) | 24 (p50 21) |
| gaps: C→D and D→L graph boundaries | 23 | 29 |
| gaps: node-to-node inside graphs (~27 gaps of ~0.5–2 µs) | 35 | 48 |
| gaps: memcpy nodes | 8 | 8 |

Unprofiled confirmation [MEASURED gsT1, gqrep], for the typical grant (gspan 912; this run's mix is 4-layer and ~5 code blocks):
- C 208 + D 225 + L 425.
- L = gate_l→first LDPC block 42 + LDPC 285 + after-LDPC 79. The 42 µs is IF-L launch + rdm (~20) + memset, so the IF-L launch latency is ~20 µs without the profiler too.
- gate_d→gate_l (IF-D launch + D body) = 193.

**p50 cause (proven by the per-grant reconstruction):** there is no single waiting resource. The span is:

- **(a) ~55 % the grant's own GPU work at solo speed (~340 µs).** The work is fixed per grant: a full-slot FEP for every grant (k_fep_exact 69–75 µs plus delay/FIR ~45), the demod/EVM chain (~80–110), and an LDPC launch sized by the class (~90–110 for 1–5 blocks). For the p50 grant (1 code block, 16 RB) this is size-independent work.
- **(b) ~20–25 % slow-down of those kernels by work running at the same time.** Mostly other DL pass instances sharing the DL SMs. There is also an association with discovery-only overlap in TIM (71 µs). That part is not causal for p50: the CSI-RS-off A/B did not move p50.
- **(c) ~22–26 % idle gaps.** Of these, **~74 µs are the two conditional-node (Fix A+D gate) body launches**. They have a fixed latency, p90/p50 ≈ 1.3, and occur with the GPU otherwise idle for this pass. The rest is graph boundaries and node dependencies (~60–75).
- The earlier "~170 µs kernel compute" does not hold. The own-kernel work at solo speed is ~340 µs per typical real grant [MEASURED gsT2N, gsV2N].

## 2. p99: proven causes

### 2.1 Extreme tail (≥ 10 ms): launches blocked until a long CSI-RS discovery kernel ends

Passes at span ≥ p99 [MEASURED tailattr.py]:

| | TIM | Vodafone |
|---|---|---|
| mean span | 21.0 ms | 49.2 ms |
| own kernel busy | 3.8 ms | 9.3 ms |
| idle gaps, of which > 96 % lay under one long (> 1 ms) discovery kernel | ~17 ms | ~40 ms |
| main kernels under those gaps | segment 12.0, sweep 2.0, lattice_screen 1.9 ms | sweep 21.3, refine_kernel 13.0, segment 4.3 ms |

During pass gaps ≥ 10 ms [MEASURED gapdl.py]: other DL/UL work covered only 5 % (TIM) / 21 % (Vodafone) of the gap time, while CSI-RS discovery covered 100 %. The DL SMs were idle and the pass still did not run.

The mechanism [MEASURED refine.py, launchblk.py, gsV2N]:
- In the 5 episodes with refine_kernel (258–652 ms), csirsDisc sat in `cudaEventSynchronize` on that kernel.
- In each episode, **`cudaGraphLaunch` calls of 2 to 5 DL and UL decode threads (passivePdsch*, passivePusch*) blocked inside the driver**. Every one returned 0.2–1.1 ms after refine_kernel ended.
- The same happened with segment, lattice_screen and sweep. Launches > 5 ms occurred only while a long discovery kernel ran, and 20 of 23 (Vodafone C) and 5 of 7 (TIM C) returned within 2 ms of that kernel's end. No launch > 5 ms occurred outside such kernels.

The resource is therefore **host-side launch submission into the driver**, not SMs. The same pattern stalls the device-side IF-body launches (tail ifD p50 295 µs / 1.9 ms).

Without the profiler the stalls are also there [MEASURED gsT1]: 2.5 % of real grants have gspan > 10 ms. The tail grants have a normal LDPC span (kspan p50 290) but C/D/L spans of ms to hundreds of ms.

### 2.2 Moderate tail (p90–p99, 2.5–5.5 ms): SM sharing with other DL/UL work + large grants

- Gaps of 0.2–10 ms are 52–76 % covered by other DL/UL kernels [MEASURED gapdl.py].
- p90–p99 passes: own busy 1.4 ms (TIM) / 2.5 ms (Vodafone), gaps 1.1 / 3.0 ms [MEASURED tailattr.py].
- Large grants carry the LDPC cost (§2.3).

### 2.3 LDPC behaviour on the GPU path

Source [MEASURED gsT1 and gsT2N li=]:
- Every code block is decoded to its own stop rule. There is no abort after the first failure (code: k_ldpc4_t; k_pick applies OAI's rule afterwards).
- Failing blocks run to the iteration limit. Converged blocks take 2 snapshots p50 and 6 at p99.
- **One failing block sets the TB's LDPC time.** Exactly-1-limit vs all-limit, ncb ≤ 8: 282 vs 277 µs (TIM T2N); ncb ≤ 32: 716 vs 850.
- **But the LDPC time is dominated by the number of blocks in the launch, not by iterations.** kspan for converged-only vs ≥ 1 block at the limit: ncb ≤ 8 281 vs 277; ncb ≤ 16 432 vs 570; ncb = 1 30 vs 69.
- **Padding blocks of the class (next power of two) run to the limit** in 55 % of ncb 2–8 failing grants and 87 % of ncb 9–16 grants (dpad 291 ≈ dlast 298) [MEASURED gsT1]. In conv-only ncb 2–8 grants they set the kernel time: kspan 169 vs dconv 89. On air this contradicts the bench, where zero-LLR padding converges [BENCH]. **Why is unproven.**
- Blocks wait for SM slots in large launches: swait p50 292 µs at ncb 9–16 [MEASURED gsT1].

### 2.4 Queueing

Source [MEASURED passrep queue section]:
- For grants at slot→result ≥ p99, qwait p50 is 145 µs (TIM) and **6 061 µs** (Vodafone). During the wait, on average 2.1–2.2 of the other 4 decode threads were inside a GPU pass.
- The worst Vodafone grants waited behind passes that were themselves stalled for 21–53 ms (§2.1). Example: seq 23777 waited 33.8 ms in `pre` behind its slot-group mate seq 23775, whose span was 31.6 ms.
- So the 1–5 ms (and beyond) of queue is **head-of-line blocking behind stalled passes**. The resource is the decode threads and pass instances, which are held while their launch is blocked.

## 3. Ruled out

| candidate | ruled out by |
|---|---|
| Discovery stretching DL kernels via its SM partition, as the p50 driver | p50 A/B (CSI-RS discovery off): no change |
| Failing-block iterations as the p50 driver | at fixed block count, kspan with and without limit blocks is the same for ncb ≤ 8 |
| Host prep / launch-call cost at p50 | gp prep 25, launch 26 µs p50 |
| The IF launch latency as an nsys artefact | unprofiled g2l = 42 µs (IF + rdm + memset) |

## 4. Open, NOT proven

1. **Why this binary stalls so much more than the morning runs.** Real grants with gspan > 10 ms: gsT1 2.5 %, gsT2N 1.8 %, gsV2N 10.5 %, against mmTM1/TM2/VM1/VM2 ≤ 0.01 % at 15:31–15:56, with the same discovery activity. UL also degraded: ULSTALL 641–800 vs 43–97 per run.
   - Candidates: (i) measurement commit d2dcce42d1 (2 extra cudaEventRecord per pass, NVTX mark, globaltimer stores); (ii) the functional commits 66a913319f and 438890da3c, which the gs binary includes and the mm binary did not; (iii) external GPU load on the shared host.
   - Needed: same-hour on-air A/B, 438890da3c vs d2dcce42d1, TIM, 150 s each.
2. **Why the driver blocks `cudaGraphLaunch` until the other context's kernel ends.** The data shows that it does, not why.
3. Why the padding blocks do not converge on air.

**No fix is proposed.** The p50 composition is proven, but it is a sum of work, sharing and launch latency, not one removable cause. The p99 stall mechanism inside the driver, and its trigger in this binary, are not proven (§4.1–4.2).
