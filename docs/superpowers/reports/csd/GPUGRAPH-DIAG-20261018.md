# GPUGRAPH diagnosis: why DL p99 does not fall as grants move onto the GPU pass (2026-10-18)

Diagnosis only. No receiver code is changed or committed. Branch csd/int16-gpupath, binary 6c3037439d (build_t26).
Host: DGX GB10 (sm_121), replay. Results from different captures are never pooled.

Arms:
- OFF = t26gE.
- ON = t26gE + `ISAC_PDSCH_GPU_GRAPH=1 ISAC_GPU_PASS_RETRY_OLD=1 ISAC_GPU_PASS_POLL_SLEEP_US=600 _COVER=1 _SPLIT=1 _RETRY=1 _DMRS2=1`.
- Env-only lever arms = ON plus one setting:
  - SPIN: `ISAC_GPU_PASS_POLL_SLEEP_US=100000`, i.e. pure spin for every pass wait.
  - FP32: `ISAC_GPU_PASS_FP32=1`, the FP32 demod and EVM instantiation of the pass.
  - PRIO: `ISAC_PDSCH_GPU_PRIO=1`, probes and retries on low-priority streams.
  - PR3: `ISAC_PDSCH_GPU_GRAPH_PROBE_RESERVE=3`, so probes take an instance only when all 4 are free.
- The instance count cannot be set by env: `NR_GPU_PASS_RING` is the compile-time 4 (nr_pdsch_gpu_fep.h:71).

Data:
- **Plain replays (p99, crc_q):** batch `envab.idx` has OFF, ON, SPIN, FP32, PRIO and PR3, n=3 per capture, interleaved. Batch `envab2.idx` has ON and FP32, n=3 more.
- **nsys replays:** batch `ns.idx` has OFF, ON and FP32, n=2 per capture. The trace options were `-t cuda,nvtx --cuda-graph-trace=node --gpu-metrics-devices=0`.
- **Temporary NVTX instrumentation for the nsys runs:** `instr.patch`. It adds per-grant `G` marks carrying `e2e=`, dec/body ranges, pprep/pwait inside the pass, scan ranges per slot and the Phase-2 lock wait. It was built, used, reverted, and the build was redone clean; the binary has no NVTX strings. It is not committed as code.
- **nsys inflates latency:** ON b p99 is 3493-3540 under nsys against 3047-3682 without it. The nsys runs are used for the decomposition only. p99 and crc_q come from the plain replays.
- Scripts and indexes are in `docs/superpowers/reports/csd/gpugraph_diag_20261018/`.

## Answer

**The pass does not lower p99 because the GPU is not the bottleneck, and the pass makes each grant's GPU work larger.**

1. **The GPU is far from saturated.**
   - SMs Active averages 6.9-8.4 % over the run. During the decode of near-p99 grants it is 48-64 %.
   - Launch-to-first-kernel queue waits inside the pass are small. Near-p99 medians are C 15-26 us, D 5-6 us and L 2-3 us.
   - So capacity and queueing are not what sets the tail.

   [MEASURED, DGX replay, nsys, b/tim x OFF/ON/FP32, n=2 each]
2. **The pass increases DL GPU work per real grant.** This is the sum of kernel time on the PDSCH threads, including probes and retries:

   | capture | OFF (us) | ON (us) | change |
   |---|---|---|---|
   | b | 674 | 1062 | +58 % |
   | tim | 553 | 972 | +76 % |

   [MEASURED, nsys, n=2 each] The extra work comes from three places:
   - **FP64 demod and EVM on GB10.** On tim, k_evm1 goes 105 -> 154 us and k_tail 57 -> 129 us.
   - **A per-body k_fep_exact.** It is added on top of the per-slot group FEP that `pre` still runs: k_fep_exact goes 122 -> 164 us on b and 88 -> 127 us on tim.
   - **Stages moved from the CPU into the graph:** k_rdm, k_chest_post, k_crc and k_seg_compose, about 120-135 us per grant.
3. **Tail grants are burst slots with concurrent bodies.**
   - In grants per slot: tail grants have 2.5-10.3 against 1.3-2.0 for all grants. The busiest slots cluster on b.
   - In PRB per slot: 151-375 against 31-70.

   [MEASURED, gg_d2 logs, n=3-6]

   - During a near-p99 grant's decode, 2 other real grants are in flight (p90 4-5). Over all grants the median is 0-1 [MEASURED, nsys].
   - A pass body near p99 has a GPU span of 0.9-1.6 ms, of which only 0.55-0.80 ms is its own kernel time. The rest is co-running kernels interleaving with its serial chain of about 27 small, latency-bound kernels.
4. **The stage bodies that remain are about half of the near-p99 band, and they get slower.**
   - They are large segmented grants (median 112-201 PRB).
   - Under the added pass load their own GPU busy rises from 448-801 us (OFF) to 967-1065 us (ON). Their host gaps stay at 0.8-1.3 ms.
5. **a2e (0.5-1.0 ms) does not change.** It is the PDCCH scan running until the DCI is enqueued: scan_to_enq is 0.38-1.07 ms in the band. It is not queueing: scan_wait is 0.06-0.16 ms and the Phase-2 lock wait is about 0.
6. **The poll sleep is not a cost.**
   - It spins for 600 us and then nanosleeps 20 us per poll [CODE-READ nr_gpu_poll.h:14-31].
   - The measured gap from the last kernel's end to the end of the wait is 7-24 us (median, near-p99 pass bodies).
   - Pure spin (SPIN) gives no gain.

Net:
- The pass removes host gaps for small and medium grants. Their p50 is not better, though: +70-100 us at <= 48 PRB [MEASURED, gg_d2].
- In the burst slots that make the tail, its extra GPU work and the slower remaining large stage grants cancel the gain. tim gets slightly worse; b is within noise.
- SEG128 moves the largest grants in as well, holds instances longer and adds more work, so p99 rises 32-39 % [MEASURED, GPUGRAPH-DMRS2].

## Decomposition of grants at p99 (near-p99 band = tot rank p98-p99.5)

[MEASURED, DGX replay, nsys + DIAGTMP NVTX, n=2 runs per arm and capture; medians over the band, per run "run1 / run2". Band size is 39-78 grants per run.]

| part (us) | b OFF | b ON | b FP32 | tim OFF | tim ON | tim FP32 |
|---|---|---|---|---|---|---|
| p99 tot (under nsys) | 3151 / 3832 | 3540 / 3493 | 3897 / 3447 | 2380 / 2398 | 2757 / 2502 | 2368 / 2570 |
| band tot | 3018 / 3657 | 3267 / 3231 | 3688 / 3288 | 2281 / 2296 | 2606 / 2427 | 2287 / 2451 |
| a2e: arrival -> DCI enqueued | 913 / 961 | 982 / 951 | 1252 / 871 | 526 / 567 | 520 / 519 | 493 / 503 |
| of which scan wait (slot read -> scan start) | 138 / 133 | 62 / 134 | 159 / 74 | 84 / 137 | 111 / 130 | 81 / 87 |
| of which scan start -> enqueue | 742 / 850 | 883 / 785 | 1065 / 756 | 391 / 436 | 424 / 398 | 379 / 387 |
| of which Phase-2 lock wait (tail median) | 1 | 1 | 1-2 | 1 | 1 | 1 |
| qw (enqueue -> consumer takes it) | 9 / 15 | 22 / 47 | 9 / 84 | 14 / 9 | 11 / 21 | 28 / 31 |
| pre (IQ copy, group GPU FEP) | 43 / 44 | 47 / 44 | 50 / 45 | 44 / 43 | 43 / 42 | 43 / 43 |
| dec (decode call, bodies = 1) | 2054 / 2300 | 1823 / 2045 | 1978 / 1972 | 1645 / 1650 | 1961 / 1784 | 1656 / 1757 |
| band share decoded by the pass | 0 | 52 % / 43 % | 57 % / 44 % | 0 | 39 % / 40 % | 39 % / 34 % |
| **pass bodies:** dec | - | 1517 / 1876 | 1338 / 1521 | - | 1624 / 1525 | 764 / 1426 |
| host before the first graph launch (geometry, job, prepare) | - | 111 / 165 | 73 / 148 | - | 97 / 115 | 132 / 104 |
| C: queue wait / exec span | - | 26/183, 20/291 | 15/171, 18/213 | - | 22/164, 23/278 | 18/136, 18/161 |
| D: queue wait / exec span | - | 5/233, 6/348 | 5/151, 5/187 | - | 5/475, 5/371 | 5/166, 5/203 |
| L: queue wait / exec span | - | 3/164, 3/262 | 3/123, 3/120 | - | 3/204, 3/196 | 3/138, 2/144 |
| poll latency (last kernel end -> wait exit) | - | 16 / 23 | 8 / 16 | - | 22 / 17 | 7 / 7 |
| host after the wait (commit) | - | 60 / 90 | 51 / 72 | - | 64 / 81 | 81 / 93 |
| own kernel busy / other kernels busy in own span | - | 548/882, 799/1274 | 463/585, 518/906 | - | 800/1217, 789/1191 | 437/517, 651/1023 |
| **stage bodies:** dec / own busy / host gaps | 2054/448/1464, 2300/575/1278 | 2024/967/1055, 2216/1059/1029 | 2528/1088/1307, 2378/1391/1024 | 1645/801/740, 1650/604/854 | 2054/1065/923, 1848/1004/818 | 1749/914/820, 1812/1001/847 |
| SMs Active during the band's decode (%) | 37 / 56 | 56 / 64 | 50 / 62 | 51 / 48 | 56 / 58 | 48 / 53 |

Reading:
- **a2e is 28-31 % of near-p99 tot on b and about 22 % on tim**, and it is the scan itself. The scan wait is 0.06-0.16 ms and the lock wait is about 0.
- **Inside a pass body the GPU queue waits are negligible.** Exec spans are larger than own busy: the remainder is time shared with co-running kernels.
  - FP32 cuts the D span on tim from 371-475 to 166-203 us, and the near-p99 pass dec from 1525-1624 to 764-1426 us.
- **The extreme top-1 % holds a few multi-ms events per run.** Each part is counted when it is >= 2 ms, out of 27-53 top-1 % grants per run [MEASURED, nsys]:

  | part >= 2 ms | count per run |
  |---|---|
  | a2e | 1-8 |
  | pre | 1-12 |
  | qw | 0-3 |
  | C queue | 0-3 |
  | host_pre | 0-4 |

  They are spread over parts and arms, OFF included. They shape p99.9 more than p99. The two named causes:
  - **One cudaHostRegister of 5.9 ms per run.** Lazy pinning by pdschRecov and the consumers after RX start: 11 + 5 x 3 calls per run. It blocks every graph launch and the csirsDisc memcpy for about 3.5-6 ms.
    - It overlaps 0-7 of 40-53 top-1 % grants.
    - In the near-p99 band it overlaps 0-2 grants.

    [MEASURED, nsys] It is a start-up transient that a 12 s replay over-weights.
  - **Core 7.** passivePdsch0 shares core 7 with the FIFO-50 scan consumer. It has 50 of about 120 graph-launch or launch API calls that blocked > 0.5 ms (pre-empted mid-call). The other consumers have 7-9 each. [MEASURED, nsys smoke run b ON, n=1]
- **Long csirsDisc kernels are not the cause of the GPU stalls.** refine_kernel and sweep, single-thread on the disc green context, run up to 400 ms. One was running at the C launch in 0/1 - 3/4 stalled bodies against 10-57 % of all bodies [MEASURED, nsys].

## Env-only lever A/B

[MEASURED, DGX replay, plain (no nsys), interleaved. goal_metrics tot in us. crc_q med (min).]

| capture | arm | n | p99 median (runs) | p50 | crc_q | drops max | dec mean |
|---|---|---|---|---|---|---|---|
| b | OFF | 3 | 3192 (3192, 3098, 3246) | 1220 | 96.6 (95.4) | 0 | 636 |
| b | ON | 6 | 3516 (3380, 3047, 3660, 3682, 3604, 3427) | 1288 | 96.7 (96.3) | 0 | 702 |
| b | FP32 | 6 | 3404 (3594, 2850, 3149, 3215, 3990, 3699) | 1338 | 96.0 (95.2) | 0 | 707 |
| b | SPIN | 3 | 3361 (3361, 3668, 3221) | 1232 | 96.7 (95.6) | 0.5 | 677 |
| b | PRIO | 3 | 3289 (3755, 3289, 3251) | 1291 | 97.1 (95.3) | 0 | 709 |
| b | PR3 | 3 | 3431 (3329, **6798**, 3431) | 1319 | 96.5 (96.5) | 0.8 | 777 |
| tim | OFF | 3 | 2290 (**7110**, 2290, 2129) | 990 | 74.3 (72.5) | 0.8 | 571 |
| tim | ON | 6 | 2446 (2366, 2298, 2525, 2580, 2293, 2551) | 1058 | 74.6 (72.4) | 0 | 685 |
| tim | FP32 | 6 | **2290** (2531, 2330, 2178, 2249, 2144, 2623) | 978 | 74.5 (72.2) | 0 | **575** |
| tim | SPIN | 3 | 2476 (2476, 2473, 2678) | 1067 | 75.4 (73.7) | 1.1 | 864 |
| tim | PRIO | 3 | **16732** (2266, 16732, 18942) | 1059 | 74.4 (72.1) | 1.1 | 966 |
| tim | PR3 | 3 | 2408 (2688, 2294, 2408) | 1079 | 75.0 (74.9) | 0 | 685 |

- b n_real is bimodal (1387-4930), as in earlier reports. Between batches the same arm moves by up to ±400 us, so the b differences are within noise.
- **FP32 correctness:**
  - shadow_bad = 0, shadow_fail_bad = 0, fp32_flip = 0, need_cpu = 0 and no MISMATCH or DISABLED line in all 6 FP32 runs.
  - crc_q equals ON within spread.

  [MEASURED] This is replay-correctness-equivalent on the sampled shadows. It is not bit-exact by design.

## Ranked levers (expected p99 gain)

| # | lever | evidence | expected p99 gain |
|---|---|---|---|
| 1 | **Cut a2e: enqueue each DCI when it is accepted, not when the occasion's scan reaches it late; or a faster scan (PDCCH on GPU, spec section 2).** | Near-p99 a2e is 0.87-1.25 ms on b and 0.49-0.57 ms on tim, almost all of it scan start -> enqueue. Scan wait and the lock are about 0 [MEASURED, nsys, n=2]. Unchanged by every DL lever. | [ESTIMATE] -0.3 to -0.6 ms on b, -0.15 to -0.3 ms on tim, if the enqueue moves to about half the scan run. The largest lever for b and the only one that reaches b < 3 ms. |
| 2 | **FP32 demod/EVM in the pass** (`ISAC_GPU_PASS_FP32=1`; already the default on sm_89 / sens6) | tim p99 2446 -> 2290 (-6 %, n=6 vs 6), dec mean 685 -> 575 (= OFF 571); DL GPU work per grant on tim 972 -> 668 us; D span on tim -55 to -65 % [MEASURED]. b -3 %, within noise. Correctness counters are clean [MEASURED]. | [MEASURED] tim -0.15 ms (back to OFF). b about 0 to -0.1 ms. Free: env only. |
| 3 | **Task 5, reframed: feed the pass from the per-slot group FEP that `pre` already runs (worker thread), and drop the per-body k_fep_exact** | The per-body FEP adds 39-42 us per real grant to the k_fep_exact total over OFF. The pass's own FEP is about 50-70 us per body. The group FEP (main thread, 3.1-3.5 s of kernel time per run) is computed anyway [MEASURED, nsys]. | [ESTIMATE] -50 to -100 us on near-p99 pass bodies, and about -10 to -15 % DL GPU work. Smaller than the plan's framing suggested. |
| 4 | Pre-register at init the host buffers that pdschRecov and the consumers pin lazily (cudaHostRegister after the fence) | One 5.9 ms call per run blocks every GPU launch for 3.5-6 ms and hits 0-7 of the top-1 % grants [MEASURED, nsys]. | [ESTIMATE] replay p99 -0 to -100 us, p99.9 larger. On air it is a once-per-start event. It also violates the "no allocation after fence" rule in spirit. |
| 5 | Take PDSCH consumer 0 off the scan core (core 7) | 50 of about 120 blocking launch calls are on consumer 0 [MEASURED, n=1]. An earlier replay move to core 17 changed nothing (GPU-CONTENTION-20261017). | [ESTIMATE] small in replay; it matters on air (core-7 body p50 +130-180 us, OTA). |
| 6 | Large residual stage grants (> 32 segments, 112-201 PRB): host gaps 0.8-1.3 ms | They are about half of the near-p99 band [MEASURED]. SEG128 made p99 worse because of added work on held instances. | [INFERRED] Retry SEG128 only after 2 and 3 have cut per-grant GPU work. Until then, remove the stage path's host gaps (spec 10.2 rows 6, 11, 15). |
| - | Poll sleep: lower it or use event-blocking sync | The poll latency is 7-24 us (near-p99 median). SPIN: b 3361, tim 2476 against ON 3516 / 2446 [MEASURED]. | none. Keep 600. |
| - | Fewer concurrent instances or probe reserve (PR3) | Queue waits are 2-26 us. PR3 shows no gain, and b had one 6.8 ms run [MEASURED]. | none |
| - | Priority to real or oldest grants (PRIO) | tim p99 16.7 and 18.9 ms in 2 of 3 runs, 1.1 % drops [MEASURED]. | **harmful: reject** |
| - | GPU capacity, more SMs for RT | SMs Active mean 7-8 %, 48-64 % during band grants [MEASURED]. | none |

## Recommendation for the next DL step

1. **Make the ON arm "ON + ISAC_GPU_PASS_FP32=1"** (FP32 is already what sens6 gets by default). Gate it with one more interleaved b/tim batch of OFF / ON+FP32, n >= 6. Confirm that it is at least at OFF on both captures and that the shadow counters stay clean.
2. **Spend the next implementation slot on a2e, not on the GPU pass.** Instrument the scan occasion to see where in its run the DCI is found, then enqueue each accepted DCI immediately (Phase-2 decides only the DCI; the PDSCH job need not wait for the rest of the occasion). Measure a2e in the near-p99 band. This is the only lever with an expected gain that brings b under 3 ms.
3. **Do Task 5 as "reuse the group FEP"** (no new S sub-graph): the pass reads the slot grid that `pre`'s worker already computes. Do it after 1 and 2. The expected gain is modest; the main benefit is GPU work and portability (sens6 has a smaller GPU).
4. **Small hygiene:** pre-register the lazily pinned host buffers before the fence.
5. **Stop:** SEG128 (until per-grant GPU work drops), PRIO, PROBE_RESERVE, the poll sleep, and ring size.

## Deviations and caveats
- **The temporary NVTX build ran inside heavy.lock** while another agent's replay (rr-csd-int16ulr, its own build dir) held the lock; the build waited for it. pgrep showed that process at the moment of the call. Both builds touched only this worktree's build_t26 and have been rebuilt clean. `git status` is clean.
- **The instance count was not varied.** That needs a rebuild with `NR_GPU_PASS_RING` changed, which is a code change. PR3 is the env proxy; queue waits show instance contention is not limiting.
- **nsys n=2 per arm.** The band medians differ run to run by up to about 300 us. Treat the per-part numbers as ±20 %.
- **a2e split.** Assumes the scan occasion of slot `a` that started last before the enqueue is the one that enqueued the grant. If a slot is scanned more than once, that is approximate.
- **GPU SM metrics are sampled at 10 kHz.** The GPC clock column reads as garbage on GB10 (negative) and is not used.
