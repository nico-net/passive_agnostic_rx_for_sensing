# GPUPASS: why ON is not faster than OFF (2026-10-16)

Scope: read-only analysis of the Task 8 logs (replays/replay_gp_{b,tim}_{OFF,ON,ONREC}_1..6, commit 4c0a11a07e) plus code read. No replay, build or radio was run. Helper scripts used: tests/passive_rx/sysan_e2e.py, p99_stats.py (python3 -I), plus two throw-away aggregators (EVMGRANT e2e fields, all runs of one capture and arm pooled; captures never merged). Labels: [MEASURED, DGX replay <cap>, n=6, Task 8 logs] or [CODE READ]. Causal attributions marked HYPOTHESIS were not isolated by a stamp or an A/B.

## 1. What "whole call" measures [CODE READ]

openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c: `dec_t0` is taken right before `nr_pdsch_passive_decode()`, `dec_t1` right after it; EVMGRANT `e2e=` field 4 (`dec`) is dec_t1 - dec_t0, field 5 (`first_body`) is the wall of the first `passive_decode_body()` only (t_dec_first_ns, nr_pdsch_passive_decode.c).

- Included: every body of the grant (first pass, inline recovery arms: S/TDRA/CSI/CDM/wide-MRC retries, RV/LBRM), the CSI-FULL hook, CPU PSS/SSS FFT on SSB-candidate slots, GPU pass host prep + launch + wait + commit, LDPC host path.
- Excluded: PDCCH scan to enqueue (`arr2enq`), queue wait, dequeue-to-start (`pre`: IQ copy, stale check, GPU FEP group), EVM line, solver feedback, DM-RS id accumulation and everything after dec_t1, async recovery bodies (ONREC).
- Not stamped inside the call: host prep before the graph launch, launch-to-done, host post-processing after it. Only the pass's host wait is histogrammed (power-of-two bucket, p50 <= 512 us).
- Under ON the PDTIM stage timers (fep/chest/demod/ldpc, PDTIMCLS "real") read 5 us p50: the stages are inside the graph, so they are not comparable with OFF.

## 2. Per-stage numbers

Real grants (probe=0), pooled over 6 runs per arm [MEASURED, DGX replay <cap>, n=6]. Times in us; "first" = first body.

| cap arm | grants | bodies/grant | first p50 | share of decode time in first body | CRC-fail grants: share of time | whole p50 |
|---|---|---|---|---|---|---|
| b OFF | 24041 | 1.76 | 582 (<=96 PRB) | 59.6 % | 73 % | 1102 |
| b ON | 26734 | 1.72 | 637 | 60.5 % | 72 % | 1182 |
| b ONREC | 24967 | 1.02 | 628 | 97.4 % | 68 % | 639 |
| tim OFF | 30910 | 1.10 | 570 | 91.1 % | 48 % | 586 |
| tim ON | 29625 | 1.10 | 652 | 91.0 % | 46 % | 674 |
| tim ONREC | 26874 | 1.00 | 650 | 99.5 % | 43 % | 654 |

(whole p50 column = grants <= 96 PRB.)

Single-body CRC-OK grants <= 96 PRB, first body p50 split by SSB-candidate slot (slot%20 in 0-3,10-13) vs other slots:

| cap arm | SSB slots p50/p90 | other slots p50/p90 |
|---|---|---|
| b OFF | 653 / 1545 | 433 / 1004 |
| b ON | 782 / 1579 | 476 / 999 |
| tim OFF | 698 / 1094 | 494 / 624 |
| tim ON | 805 / 1148 | 571 / 871 |

Old path stage sum (OFF, PDTIMPCT p50): b fep 65 + chest 85 + demod 85 + ldpc 105 = 355 us; tim 405 us. Hence the OFF first body (433-494 us on non-SSB slots) is already ~stage sum + 80-140 us host overhead.

## 3. Answer

The pass replaces a ~355-405 us old GPU chain (already exact-GPU FEP/chest/demod/LDPC, each a short wait) with one graph whose host wait is itself in the 256-512 us bucket. There was never ~300 us to save on the first body; the median whole call is not the first body anyway on Vodafone (b): it is dominated by retries and, on both operators, by non-pass work. ON adds +45..+130 us to first bodies and +7..16 % to whole-call p50.

## 4. Ranked contributors

1. **Recovery / retry bodies (b: 39.5 % of decode time ON, 62 % of real grants have >= 2 bodies, CRC-fail grants hold 72 % of the time).** [MEASURED, DGX replay b, n=6] ON whole p50 1246 -> ONREC 678 (-46 %); first body unchanged (637 vs 628). tim: 91 % single-body, retries 9 %, so this is Vodafone-specific. Also retry bodies got slower under ON: retry_dec p50 758 vs 653 OFF (run 1 of each) [MEASURED, b run 1]. Launches 7151 vs 5015 grants in b ON_1 suggests each retry body re-enters the full pass (host prep + wait + LLR D2H) instead of re-running only the affected stage (INFERRED from counters, not stamped). Smallest fix: make ISAC_PDSCH_RECOVERY_ASYNC=1 part of the ON recipe (already built); gain b whole p50 -42 %, p99 -39 %, tim >96 PRB p99 21.8 -> 6.6 ms is seen with ON alone. Caveats from Task 8: full-discovery b CRC-OK -1.04 pp (n=4 vs 3) and 2-4 % lost handoffs. Classification: flag adoption = Stage 1 follow-up (needs an OTA CRC check); making the recovery lane a prioritised background lane with admission (so recovery never runs on a critical consumer and discovery is gated while critical work is pending) = Stage 3.

2. **CPU PSS/SSS FFT on SSB-candidate slots (about half of single-body grants: 2854/5636 b, 3754/10834 tim).** [MEASURED] first body +220 us (b OFF) / +306 (b ON) / +204 (tim OFF) / +234 (tim ON) vs other slots at p50. The graph pass does not touch it (it runs in passive_decode_body_core before the pass, SYSAN section 2: 150-250 us at p50, ~450 us at p90). ISAC_SSB_FEP_GPU=1 exists in code and was measured at -8..-20 % in SYSAN but is NOT in t26f.env and so not in any Task 8 arm. Smallest fix: enable it in the ON recipe (one env var); expected -150..-250 us on those slots, i.e. whole-call p50 -80..-120 us on tim, less on b (retries dominate). Stage 1 follow-up; V on air -20 % already seen under SYSAN, so only a regression OTA check.

3. **Pass floor is a host wait of 256-512 us plus unstamped host prep/post; the 110 us kernel estimate is not what the host sees.** [MEASURED] wait_us_p50 <= 512 in all ON runs; non-SSB first body ON 476 (b) / 571 (tim) vs OFF 433 / 494. Candidate small cause (HYPOTHESIS, CODE READ): t26f.env sets ISAC_GPU_POLL_SLEEP_US=200; the graph wait (nr_gpu_pass_graph.cu wait_done -> nr_gpu_poll_relax) spins 200 us then nanosleeps 20 us per poll (about 70 us wake-up with default timer slack). The old path's many short waits never cross 200 us; the single long graph wait always does. Expected +30..+70 us, which matches the ON-OFF first-body gap. Smallest fix: raise the threshold (e.g. 600) for the pass wait only, or pass a per-call spin budget; expected -30..-70 us. Also add four monotonic stamps in nr_gpu_pass_dl_run (entry, before launch, done, after commit) and log p50/p99 in the GPUPASS census, so the remainder (host prep: chest_pass_inputs, demod_pass_job geometry, gold cache, segmentation; post: commit, LLR D2H, EVM) is measured rather than guessed. Stage 1 follow-up; needs no OTA for the stamps, an OTA check for the poll threshold (RT starvation risk behind that setting, see nr_gpu_poll.h).

4. **CRC-fail grants pay the pass and then the retries (b 53 %, tim 35 % of real grants).** [MEASURED] first body of a failing grant p50 792 vs 584 for a passing one (b ON). 74 % of b ON pass launches end in tb_fail. The pass is taken (fallback <0.3 %) but gives no early-out gain on a failing TB: the LLRs are D2H-copied for the CPU retry arms. Gates are not the issue: gate[probe]=3.8k (layout probes, old path by design, ~35 % of eligible bodies, p50 ~300 us each but on separate queue slots, qwait stays 7-11 us), gate[seg]=382 (> 16 segments, ~5 %), the others 0. Segmented classes are 52 % of launches (seg_launches 3751/7151) and carry more rows. Fix: not a pass change; reduce the number of retry passes per failing grant (prior-ranked arm order, skip arms that the S-toggle history says never rescue on this cell; SYSAN: S-toggle = 99 % of rescues). Stage 3 (background lane/admission for low-prior arms) with a Stage 1 follow-up for the ordering; OTA check needed (CRC-OK must not drop).

5. **Large grants (> 96 PRB, 10 % of b grants, 4 % of tim) set the tail, not the median.** [MEASURED] first p50 1.5-1.8 ms ON and OFF alike; b p99 whole 8.5 ms ON; they hold 20 % (b) of decode time. Kernel time itself exceeds 500 us there (SYSAN: k_ldpc4 p99 394 us), so ISAC_GPU_PASS_LDPC_CBPAR (not implemented) is the only lever and needs per-stage GPU timing first (contributor 3 stamps). Stage 1 follow-up.

## 5. GPU contention: not the explanation

- Pass instance contention: busy fallbacks 0.0-0.29 % of passes (4 instances) [MEASURED].
- Discovery partition: gpu_budget_used_pct 18.7-22.4 % in ON and OFF alike [MEASURED]; discovery has its own 8-SM partition and SYSAN measured PDSCH kernels start within 53 us p99 on an idle stream [MEASURED, SYSAN]. Whole-call p50 does not depend on it. DRAM bandwidth sharing on the unified-memory GB10 is not excluded; it needs an nsys run with the pass on (not allowed now).
- ISAC_GPU_BUDGET rt_begin refuses a job older than 250 us from job_start (counted in the unsupported fallback, 0 in the census): not active.

## 6. Not the cause, but note

- End-to-end is larger than the decode call: arr2enq (PDCCH scan to enqueue) p50 796-815 us on b and 415-421 us on tim, in every arm; tot p50 b 1.6-2.2 ms. A 300 us whole call cannot give a sub-slot total while the scan stage is 400-800 us.
- OFF and ON runs of b differ in grant count 2x (late acquisition mode, Task 8 note), so ON-OFF differences of 5-10 % at p50 are near the run-to-run spread; the conclusion is "no gain", not "ON is 10 % slower".

## 7. Recommended order

1. ISAC_SSB_FEP_GPU=1 + ISAC_PDSCH_RECOVERY_ASYNC=1 in the ON recipe (env only; re-run the A/B on tim and b; then OTA CRC check).
2. Four stamps in nr_gpu_pass_dl_run and the poll-threshold test (ISAC_GPU_POLL_SLEEP_US=600 A/B), then decide on CBPAR and on a leaner retry (stage-reuse) path.
3. Stage 3: recovery/low-prior arms as a prioritised background lane with admission, discovery gated while critical work is pending.
