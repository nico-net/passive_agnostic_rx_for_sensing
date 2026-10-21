# FLOAT-LANE-POOL-20261021: should the float lane move into the core pool?

Branch csd/int16-floatpool (csd/int16-grantsize + merge of csd/int16-disc-nonblock 582a1a90e9). Host: DGX GB10 aarch64, quiet (no receiver, bench via heavy.sh, nice 19).
Labels: [CODE-READ], [MEASURED, DGX replay] (older reports), [MEASURED, DGX OTA] (tonight's run.log CORE_POOL lines), [Bench, DGX] (synthetic spin jobs, NOT the receiver), [INFERRED].
Nothing here was run on air. No receiver code was changed (design only, see section 6).

## Verdict
**Not dramatic on the DGX or any host with >= 7-8 non-RT cpus; dramatic below ~5.** [Bench, DGX]
- Decode wait p99 with float jobs leasing the same pool (merged) is within noise of the dedicated lane (sep) at T=8 and T=11 (DGX layout): +0.0-0.3 ms; scan/UL/DL misses <= 0.6 %, no drops, no hang, float jobs get 93-98 % of their offered CPU.
- It is NOT a decode-latency gain either (and it does not need to be): the benefit is portability (no fixed cores 10-14) and 3 fewer reserved cores. Risk is concentrated in one mechanism: age promotion (below).
- At T <= 5 cpus (pool 3-4) the unprotected merge breaks decode (DL p99 +2.5 ms at pool 3 / scale 0.5, scan miss 8 %, UL miss 57 % at overload, 42658 drops vs 7268). Cause: DISC (20 ms) and HOUSE (100 ms) waiters are promoted by age and take the reserve token; with 8 float threads waiting this happens constantly (T4 scale 0.5: 2867 DISC promotions in 20 s). Setting age_ns[DISC]=age_ns[HOUSE]=0 (never promote) removes it: decode back within +0.5 ms of the dedicated lane at T4/T5, but discovery is then shed almost completely under overload (float duty 0.01-0.41 of 1.83).
- Recommendation: DGX stays as is for the combined A/B (no float-lane change in the build); sens6 and small hosts use a host-derived layout, section 5.

## 1. Inventory of the float lane [CODE-READ unless marked]
The "float lane" is not a thread group: ISAC_FLOAT_CORES=10-14 (set by --passive-rx, nr-uesoftmodem.c:295; effective only with no core plan) moves every thread that has no explicit pin to 10-14 and makes every later nr_core_plan_create(core=-1) thread inherit it (nr_core_plan.c:361-470). Env pins that stay outside the pool: ISAC_GPU_WORKER_CORE=10, ISAC_PDCCH_USS_CORE=12, ISAC_PDCCH_NID_GPU_CORE=13, ISAC_GPU_CHECK_CORES=10-14 (bg workers). CPU % = one core, [MEASURED, DGX replay] from CORE-PLAN-20261018 section 2 / SYSAN-20261015 section 3 (b = Vodafone, tim = TIM); on-air per-thread numbers for the lane do not exist in tonight's logs (only CORE_POOL lines), so these are replay (half the air load per SYSAN).

| thread | file | work shape | CPU % (replay) | latency sensitivity | GPU wait inside the job |
|---|---|---|---|---|---|
| pdcchJanon | nr_pdcch_blind_monitor_rt.c:2352 | queue (cond wait), CPU DCI sweep per item, items 1-few ms; has stale-drop | 20-43 | low: speculative DCI search, items older than JANON_FRESH_NS dropped | no |
| nID GPU sweep (unnamed) | nr_pdcch_nid_gpu.c:58 | cond wait, upload + GPU run + publish, duty-cycled (ACQUIRE/MAINTAIN) | 24-32 | low (acquisition, then 250 ms recheck) | yes (run() waits for the kernels) |
| pdcchUssHash | nr_pdcch_uss_tracker.c:353 | queue, 65535-RNTI hash scoring per geometry | 6-40 | low-medium (RNTI knowledge feeds the scan) | no |
| dmrsScan | nr_pdsch_passive_queue.c:307 (async DM-RS scan) | queue | 10-17 | low | no |
| dmrsIdBg / gpuCheckBg | nr_pdsch_gpu_demod_host.c:713-770 | nice-19 queue worker, ISAC_GPU_CHECK_CORES | 3-57 | none (estimator / verifier) | no (CPU reference) |
| csirsDisc | nr_csirs_disc_feed.c:355 | 5 ms timed wait, drain + hook per batch | 3-6 | low, drop/defer already built in | the hook launches GPU discovery (chunked, disc-nonblock) |
| ulBgDisc | nr_pusch_ul_bgdisc.c:47 | 20 ms timed wait, one run = 113-287 ms, not chunked, nice 10 | 0.1-45 | none (background) | no |
| UL NID bg | nr_ul_nid_bg.c:565 | SCHED_IDLE, 10 ms poll, jobs | 3 | none | no |
| pdschRecov | nr_pdsch_recovery.c:258 | ALREADY RETRY lease in the pool (:203) | 4-9 | medium | n/a |
| pdcchDisc | nr_pdcch_blind_monitor_rt.c:4314 | ALREADY DISC lease in the pool | n/a | low | n/a |
| OAI actors DL__/UL__/SYNC__ (RR 97), ITTI, logAsync, pdcp_timer, time source, L1 stats, CUDA helper/EvtHandlr, main | actor.c, log.c, ... | tiny, timer driven, 3000+ wakeups/s | 8-12 + 5 + 2 + 1-3 (about 0.25-0.4 core total) | actors and timers: yes (wake latency), RR 97 | CUDA callbacks |
| dashboard | nr_dashboard.c:42 | SCHED_IDLE, core 2 | small | none | no |
| gpuFepWorker | no create site found in this tree | legacy env key ISAC_GPU_WORKER_CORE only used by the plan/recovery busy list | <0.5 | n/a | n/a |

Sum of the movable discovery/background CPU: about 1.8-1.9 cores (janon .30, nID .30, USS .25, dmrsScan .15, dmrsId .30, csirs .05, ulBgDisc .45, UL NID .03) [INFERRED from the table, mixed b/tim samples; SYSAN gives "speculative CPU 1.2-1.8 cores"]. Tonight on air the pool itself already carries discovery: DISC class n=0.5-2.6 M leases per run, wait p99 <= 256-1024 us, max 20-22 ms; nested 0.29-0.99 M (inline discovery under SCAN) [MEASURED, DGX OTA, ab_pTP1/pTP2/pVP1/pVP2 CORE_POOL]. The DISC max of 20 ms equals the DISC promotion age: promotion is already active on the DGX.

## 2. Classification
(a) latency-critical, keep a core or top pool class: UEthread_0 (slot thread, core 5), usrp ring, UHD ctrl, DPDK, the scan/PDSCH/PUSCH consumers (already pool classes). Nothing in the float lane is in this group.
(a') tiny but wake-sensitive, "unleasable": OAI actors (RR 97, must not be preempted by a leased spinner), CUDA callback threads, ITTI, timers, log. They need a HOME set that no lessee uses; they are about 0.3-0.4 core in total. These are the only threads that truly need a reserved lane.
(b) GPU-wait threads: nID GPU sweep (host wait inside run()), csirsDisc (hook). They are mostly waiting; a lease around the CPU parts only is possible for nID (upload / publish) but not around run() (pool report: a GPU wait that holds a lock or has a deadline cannot give the token). Bench models them as spin jobs, i.e. the worst case (token held for the whole wait).
(c) background/best effort: janon, USS, dmrsScan, dmrsIdBg, csirsDisc, ulBgDisc, UL NID bg, dashboard. Classes: DISC for the first group (janon, nID, USS, dmrsScan, dmrsId, csirs), HOUSE for ulBgDisc / UL NID bg (already nice 10/19, SCHED_IDLE). No FIFO/RR anywhere in this plan.

## 3. Minimal design [design only, nothing implemented]
1. Layout is host-derived in nr_pool site_init (the 'site core map' keys stay accepted as overrides): W = allowed online cpus minus the RT set (ISAC_UE_RT_CORE, ring, UHD ctrl cores, which today come from env keys; sens6 audit section 4 already proposes nr_core_machine_read for these). Home set H (unleasable threads) = 1 cpu for W < 12, else 2, taken from the small/efficiency cores when cpu_capacity is known (DGX: 13,14). Pool = W - H. ISAC_FLOAT_CORES becomes H (the lane shrinks from 5 to 1-2 cpus); ISAC_GPU_CHECK_CORES, USS/nID/GPU-worker core keys are no longer needed (workers stop pinning themselves; they lease).
2. Each float worker leases per job around CPU work, never across a cond wait: pattern already in nr_pdcch_blind_monitor_rt.c:4314 (acquire(DISC, 0, enq_ns) ... release()). Sites: janon_worker item loop, tracker_worker job, dmrs scan worker, nr_bgq_run_one, csirs disc hook (per batch), nID worker (upload+publish; run() only if the pool bench says the held-token cost is small), ulBgDisc g_run (must call nr_pool_yield between its steps, otherwise a 113-287 ms lease: with chunking the bench shows no effect, without chunking also no effect at T>=8, but it removes a token for 0.2 s at small N). HOUSE class for ulBgDisc and UL NID bg. The workers' self-pin code (affinity to env core) is deleted; they start on the home mask (nr_pool_site_home_attr already does this for children of lessees).
3. Degraded mode (pool tokens N < 6, i.e. W < 7): DISC and HOUSE age promotion off (cfg.age_ns[DISC]=age_ns[HOUSE]=0). Shed order is then automatic from the class order: HOUSE (ulBgDisc, UL NID bg) first, then DISC; scan reserve and decode reserve are never taken. Work already has drop-oldest or expiry (janon stale drop, pdcchDisc queue 8, ulBgDisc g_max_age, nID duty cycle, csirs defer), so a shed job is dropped, not queued forever. Dashboard stays SCHED_IDLE on the home cpu. DL/UL decode is never starved by construction (caps low <= N-2, decode reserve).
   Alternative for W <= 5: keep the old dedicated 1-cpu lane (mode sep). Bench: it keeps decode at none level and gives floats 1.2 of 1.83 cores.
4. Counters: CORE_POOL line already reports disc/house waits and promotions; add nothing else.
Edit size [INFERRED]: about 8 lease sites (3-6 lines each), site_init layout (about 40 lines), removal of 5 self-pins, a unit test for the layout function and the no-promotion policy. Not done: it touches RT-adjacent threads and cannot be validated without replay/OTA, and the bench shows no decode gain on the DGX, so shipping it into the combined A/B would only add risk.

## 4. Bench [Bench, DGX]
common/utils/tests/bench_nr_core_pool.cpp extended (env BENCH_FLOAT=sep|merged|none, BENCH_POOL/BENCH_LANE cpu lists, BENCH_PROD, BENCH_ULBG_CHUNK_MS, BENCH_NOAGE). Same decode mix as CORE-POOL-20261020 (scan 225 us/slot, DL 600 us x1.2, UL 400 us x0.4, retry 1.5 ms x0.1, 2 pool-side discovery threads) at load scale 0.5 and 1.0 (offered 1.7 / 3.4 cores), 20 s, 3 repeats, medians. Float jobs: 8 threads with the duty cycles of section 1 (total 1.83 core; janon .30/2 ms, nID .30/3 ms, USS .25/1 ms, dmrsScan .15/2 ms, dmrsId .30/1 ms nice 19, csirs .05/0.5 ms, ulBgDisc .45/150 ms nice 10, UL NID .03/2 ms) plus an unleasable 0.25-core burst thread on the lane/home cpus in both modes. Float jobs spin for their whole duration (GPU-wait threads: worst case). Modes: none = pool only; sep = pool + unleased floats on the lane (status quo); merged = floats lease DISC/HOUSE, lane = home cpus only. T = cpus used (pool + lane/home), producer on cpu 5 not counted; every case uses the same T for sep and merged. Real cores: pool 6 7 8 9 3 4 (+10..12 when merged), lane 10-14 = the DGX layout (T11). Host noise: "none" T11 scale 1 shows p99 0.47 ms vs 0.36 sep, i.e. +-30 % is noise; max waits of 3-13 ms appear in every mode including none (nice-19 bench on a shared host).
Medians, wait = arrival to start, us; miss % of the 2 ms (DL/UL) / 500 us (scan) deadline. Full table: float-lane-pool-bench-20261021/summary_medians.txt, raw/ per run, summ.py.

| T (pool + lane or home) | scale | mode | scan p99 | DL p99 / miss % | UL p99 / miss % | retry p99 | float CPU got / 1.83 | float start-wait p99 (worst class) | drops |
|---|---|---|---|---|---|---|---|---|---|
| 4 (3+1) | 0.5 | none / sep / merged / merged no-promo | 115 / 105 / 1681 / 221 | 1306 / 1322 / 3780 (12 %) / 1829 | 1154 / 1164 / 3360 (6.8 %) / 1601 | 7983 / 7872 / 14410 / 8978 | - / 1.25 / 0.43 / 0.34 | sep 0 (own cpu), merged 100 ms, no-promo 20 s (shed) | 0 |
| 4 | 1.0 | none / sep / merged / no-promo | 295 / 298 / 2615 / 244 | saturated in all (1.3 s / 1.3 s / 6.1 s / 0.9 s) | 2595 / 2583 / 11149 (57 %) / 2506 | saturated | - / 1.23 / 0.38 / 0.01 | | 7682 / 7268 / 42658 / 5191 |
| 5 (4+1) | 0.5 | none / sep / merged / no-promo | 96 / 105 / 105 / 103 | 645 / 668 / 1147 / 1131 | 697 / 690 / 1364 / 1328 | 3335 / 3333 / 4930 / 5334 | - / 1.23 / 1.02 / 1.02 | merged 100 ms | 0 |
| 5 | 1.0 | none / sep / merged / no-promo | 95 / 100 / 273 / 131 | 1477 / 1592 / 2931 (7.9 %) / 1857 | 1190 / 1278 / 2635 / 1659 | 11823 / 11951 / 22268 / 16392 | - / 1.23 / 0.48 / 0.41 | | 0 |
| 6 (5+1 / 4+2) | 0.5 | sep / merged / no-promo | 98 / 97 / 118 | 662 / 804 / 821 | 685 / 912 / 960 | 3338 / 3849 / 3914 | 1.66 / 1.53 / 1.52 | merged 30 ms | 0 |
| 6 | 1.0 | sep / merged / no-promo | 108 / 110 / 117 | 1463 / 1182 / 1130 | 1242 / 1390 / 1385 | 12860 / 8199 / 8092 | 1.66 / 1.03 / 0.99 | merged 100 ms | 0 |
| 7 (6+1 / 5+2) | 0.5 | sep / merged | 116 / 119 | 253 / 625 | 531 / 771 | 2567 / 2949 | 1.66 / 1.71 | 5.7 ms | 0 |
| 7 | 1.0 | sep / merged | 107 / 120 | 721 / 846 | 778 / 1069 | 4945 / 5829 | 1.65 / 1.46 | 35 ms | 0 |
| 8 (6+2 / 7+1) | 0.5 | none / sep / merged | 124 / 110 / 130 | 266 / 174 / 348 | 465 / 456 / 609 | 2245 / 2203 / 2494 | - / 1.65 / 1.78 | 2.4 ms | 0 |
| 8 | 1.0 | none / sep / merged | 106 / 123 / 110 | 376 / 399 / 647 | 640 / 661 / 815 | 3605 / 3895 / 4353 | - / 1.67 / 1.70 | 8.3 ms | 0 |
| 11 DGX (6+5 / 9+2) | 0.5 | none / sep / merged | 115 / 142 / 137 | 225 / 195 / 181 | 484 / 483 / 455 | 2190 / 2203 / 2117 | - / 1.83 / 1.80 | 0.65 ms | 0 |
| 11 DGX | 1.0 | none / sep / merged | 253 / 105 / 156 | 469 / 357 / 379 | 681 / 645 / 611 | 3891 / 3793 / 3703 | - / 1.83 / 1.79 | 1.9 ms | 0 |
| 11 (8+3) sep only | 0.5 / 1.0 | sep | 125 / 136 | 171 / 305 | 411 / 535 | 1947 / 3152 | 1.79 / 1.79 | | 0 |
| 8 and 11, ulBgDisc UNCHUNKED (150 ms lease) | 0.5 / 1.0 | merged | 106 / 159 (T8), 138 / 124 (T11) | 330 / 685 (T8), 177 / 334 (T11) | 622 / 835, 452 / 579 | | 1.80 / 1.70, 1.80 / 1.80 | | 0 |

Reading (DL/UL decode "extra" = merged minus sep, p99):
- T = 8 and 11: +0.17 to +0.25 ms DL at T8, -0.02 to +0.02 ms at the DGX layout; UL +0.15 / -0.03 ms. Below the 1 ms line, no starvation, no hang, floats keep 93-98 % of their CPU; their own start wait p99 is 0.6-8 ms (they queue behind each other, fine for background work).
- T = 7: +0.3-0.4 ms DL, +0.1-0.3 UL; floats 0.80-0.9 of offered at scale 1 (start wait 35 ms: shed).
- T = 6: +0.14 ms (0.5), -0.28 (1.0, noise level); floats shed to 0.55 of offered at scale 1, ulBgDisc job takes 1.1 s instead of 0.17 s.
- T = 5: DL +0.5 ms (0.5) / +1.3 ms (1.0, over the line), UL +0.7 / +1.4 ms; floats 0.4-0.55. No-promotion policy: DL +0.46 / +0.27 ms vs sep.
- T = 4: DL +2.5 ms and scan miss 8 %, UL miss 57 % at overload: dramatic. No-promotion policy: DL +0.5 ms, scan 0.8 % miss, drops back to the sep level, floats shed to 0.01-0.34.
- Mechanism: with the pool full, 8 float waiters sit in the queue; the DISC age (20 ms) and HOUSE age (100 ms) promote them into the reserve token. The per-run CORE_POOL line shows disc promo 2549-2867 per run at T4/T5 vs 0 at T8.
- Not modelled: GPU contention between float and decode kernels (same in sep and merged), big/little capacity differences (the DGX merged pool takes little cores 10-12 for DL leases: pick_token prefers big cores when cpu_capacity is known, and the DGX numbers above came from real DGX cores 10-12 being little, no extra DL cost visible), RT preemption of the unleasable actors (not simulated, no FIFO in the bench), real GPU-wait token holding (modelled as full spin = worst case).

## 5. Recommended design
DGX (20 cpus; W = 15 after RT set): no change for the next A/B. Optional follow-up only after the A/B shows the pool is clean: merge, H = 13,14, pool 9 (6 7 8 9 3 4 + 10 11 12), keep age promotion (bench T11 fine). Saves 3 cpus; zero measured decode cost.
sens6 and small hosts (host-derived, no flags): W = allowed online cpus - RT set.
- W >= 8: merged (pool = W - 1 or 2), promotion as is.
- 6 <= W <= 7: merged with promotion off for DISC and HOUSE (bench T6/T7: <= +0.4 ms); floats shed under overload.
- W <= 5: keep a dedicated lane of 1 cpu (sep) with floats at nice 19 / SCHED_IDLE; or merged + no promotion (T4/T5 no-promo: +0.5 ms, floats near zero). Sep is preferred: same decode, floats keep 1.2 cores.
- Whatever the case: DL/UL decode never wait for DISC/HOUSE (caps), shed order = ulBgDisc and UL NID bg, then dmrsId/dmrsScan/csirs, then janon/USS/nID.
- Needs the sens6 audit items (cpus 2-3 isolated, dashboard core 2, FLOAT_CORES=10-14 intersect online not allowed) fixed first.

## 6. Prototype
Not implemented. The only DGX-relevant question was "dramatic?": the answer is no, and the change has no measured decode benefit there; the sites include nID (GPU waits) and ulBgDisc (needs chunking) and cannot be validated without replay/OTA. The bench knobs are the only code change (bench_nr_core_pool.cpp, test target unchanged; test_nr_core_pool and test_nr_core_plan pass, 2/2 [ctest, DGX]).

## 7. Requested OTA A/B (orchestrator)
Combined A/B as planned: main vs csd/int16-floatpool tip. This branch contains no float-lane change, so: build targets nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests in cmake_targets/ran_build/build_t26 (merge of disc-nonblock 582a1a90e9 built OK for nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem), env POOL_20261021.env as is (FROZEN minus ISAC_PDSCH_CORES / ISAC_PUSCH_CORES / ISAC_UL_SNAP_CORE); ISAC_FLOAT_CORES=10-14, ISAC_GPU_CHECK_CORES=10-14, ISAC_GPU_WORKER_CORE=10, ISAC_PDCCH_USS_CORE=12, ISAC_PDCCH_NID_GPU_CORE=13 stay set (the float lane is unchanged). For a later float-merge arm (not built): remove those five keys, lane = 13,14, pool = 6 7 8 9 3 4 10 11 12; pass = DL/UL p99 within +10 % of the pool arm, CORE_POOL disc/house promo and waits, nested, UL drops, discovery counters (USS_TRACK, PDCCH_NID_GPU passes, CSIRS_DISC, UL_NID_BG) not lower than 80 % of the pool arm.
