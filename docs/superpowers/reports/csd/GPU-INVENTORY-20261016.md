# GPU-INVENTORY-20261016 (Phase 0 Task 3)

Binary csd/int16 (7f34aff2a3). Read-only: code reading + log greps (Sonnet agent; saved by the orchestrator).
Labels: [CODE-READ file:line]; [MEASURED-T] = [MEASURED, DGX OTA 2026-10-16, TIM, ab_aTX1_1_152702]; [MEASURED-V] = [MEASURED, DGX OTA 2026-10-16, Vodafone, ab_aVX1_1_153013]; n=1 each; V and T never pooled. Core classes: X925 = 5-9, 15-19; A725 = 0-4, 10-14 [MEASURED, AB-SPREAD-PIN-20261016].

## 0. On the GPU in this run (t26aXE)
- ON: ISAC_PDCCH_NID_GPU, ISAC_PDSCH_GPU_EXACT_FEP/DEMOD/CHEST/LDPC, ISAC_UL_GPU_FEP, ISAC_UL_GPU_LDPC (ISAC_UL_GPU_PART=rt), ISAC_CSIRS_DISC (GPU sweep), ISAC_CHEST_GPU_DELAY, ISAC_GPU_RT_ISOLATE=1 with ISAC_GPU_DISC_SMS=8 (RT partition 40 of 48 SMs) [MEASURED, both logs].
- OFF: ISAC_PDCCH_GPU_FEP (`pdcch_gpu=0`), ISAC_GPU_PASS / ISAC_UL_GPU_PASS (whole-decode graph, default off, nr_gpu_pass_dl.c:170-171), ISAC_PDSCH_GPU=0.
- Run window ~130 s (V) / ~126 s (T) [INFERRED from first/last NID pass stamps; lower bound].

## 1. Threads (core class, CPU% of one core [T], creation site)
| thread | core | CPU% | creation |
|---|---|---|---|
| UEthread_0 (RT97) | 5 X925 | 25 | nr-ue.c:2524 |
| usrpRing (FIFO95) | 16 X925 | 40 | usrp_lib.cpp:1131/1510 |
| uhd_ctrl + Tpool2 | 15 X925 | 65 | usrp_lib.cpp:1465 |
| dpdk-io x2 | 18, 19 X925 | 100 each (busy poll) | DPDK (outside tree) |
| passivePdcch0/1 | 6/7 X925 | 32/30 | nr_pdcch_passive_queue.c:259-263 |
| passivePdsch0-4 | 7,8,9 X925; 3,4 A725 | 6-8 each | nr_pdsch_passive_queue.c:2900-2917 |
| passivePusch0/1 + Cap | 0/1 A725 | 38/38 | nr_pusch_passive_queue.c:738-753 |
| dmrsScan | 2 A725 | 25 | nr_pdsch_passive_queue.c:563 |
| gpuFepWorker | 10 A725 | idle | nr_pdsch_passive_queue.c:2816 |
| NID GPU worker | 13 A725 | 34 | nr_pdcch_nid_gpu.c:336 |
| pdcchUssHash | 12 A725 | 2-14 | nr_pdcch_uss_tracker.c:379 |
| csirsDisc | 11 A725 | 18 | nr_csirs_disc_feed.c:406/505 |
| pdcchJanon | unpinned | 70 | nr_pdcch_blind_monitor_rt.c:2869 |
| dmrsIdBg x3, gpuCheckBg | unpinned | ~6 each | nr_pdsch_passive_queue.c:1346, nr_pdsch_gpu_demod_host.c:832 |
| scan2Slow, pdcchParBank | 7 | small | blind_monitor_rt.c:5286/4869 |
| pdschRecov, logAsync, actors, cuda-EvtHandlr | unpinned | 1-4 | nr_pdsch_recovery.c:256, log.c:153, actor.c:18 |
Not in the map (probably idle in this arm): ulBgDisc (nr_pusch_ul_bgdisc.c:129), dmrs_nid_solve (nr_dmrs_nid_solve_live.c:259), td_cb0_pool (nr_td_cb0_batch.c:395), UL layout-lock eval (nr_pdcch_ul_layout_lock.c:1157).

## 2. Per-stage inventory
### 2a. DL PDCCH
| stage | thread/core | CPU/GPU | cost | P/C | CUDA to reuse | stream |
|---|---|---|---|---|---|---|
| slot read + scan job | UEthread 5 X925 | CPU | 25 % [T] | C | - | - |
| bank + CSS0 scan: FEP-cache, descramble, LLR, polar, CRC (nr_pdcch_passive_queue.c:111 -> blind_monitor_rt.c) | passivePdcch 6,7 X925 | CPU; GPU FEP variant exists, off (blind_monitor_rt.c:10860) | V bank pass 181 us x 724442, css0 38 us x 75007, fep_cache hit 531k/miss 243k; SCAN_E2E V wait p99<=2250, run p50<=500/p99<=2400 us; T wait p99<=2450, run p50<=500/p99<=2500 | P | nr_pdcch_gpu_fep.cu (k_gold/k_llr/k_ls_pdcch), nr_polar_sc_cuda.cu | RT green, greatest prio (nr_pdcch_gpu_fep.cu:212) |
| joint-anon search | pdcchJanon unpinned | CPU (ASYNC) | 70 % [T]; V done 256k / DROPPED 282k of 539k; T done 187k / dropped 8.7k of 196k | P | PDCCH FEP + polar | - |
| NID sweep | NID worker 13 A725 | GPU + CPU orchestration | 34 % [T]; kernels max ~1.9 ms, stage1 594-691 ms | P | already GPU | disc partition, least prio (nid_gpu.cu:576) |
| USS tracker | 12 A725 | CPU | 2-14 % | C | - | - |

### 2b. DL PDSCH
| stage | CPU/GPU | cost | P/C | CUDA / notes |
|---|---|---|---|---|
| slot group / dequeue / spread (queue.c:1460) | CPU | qwait = top p99 item | C | policy fix |
| DM-RS/TDRA solver, ID scan (dmrsScan core 2 A725 + dmrsIdBg) | CPU, GPU check only | V 186 full scans mean 95 ms, 3534 checks 87 us; T 810 scans 70.7 ms, 4636 checks 368 us | P | nr_gpu_pass_graph.cu k_unscr/k_pick |
| FEP | GPU | PDTIM mean V 89 us (cpu 69), T 76 (cpu 57); p99<=576/512 us | P | DIRECT reads host ring in place; DISCRETE H2D (gpu_fep.cu:1233-1286) |
| chest | GPU (chunked >2048 rows) | V 157 (cpu 113), T 122 (cpu 94); refused=0 | P | RT green |
| demod/MMSE | GPU | V 189 (cpu 125), T 158 (cpu 112) | P | RT green |
| LDPC prep `prep_segment` (nr_pdsch_gpu_ldpc_host.c:115) | CPU serial | 18.9 us/CB bench; 0.3-0.38 ms at CB16+ on air | P | cb0_dematch_kernel (nr_td_cb0_batch.cu) |
| LDPC decode | GPU (cluster; nc for C>=8) | V 312k CBs mean 223 us (gpu 168), T 212k CBs 162 (gpu 129) | P | RT green (ldpc.cu:638) |
| CRC / hand-off / EVM | CPU | small | C | - |
Note (orchestrator, CODE-READ nr_pdsch_gpu_ldpc_host.c:178-181): `fallback err` counts TBs whose GPU run returned an error and were decoded on the CPU instead: DL V 2257, T 1501 of 106000 TBs (T, 1.4 %); UL V 18560, T 11798 (~10 % of UL TBs). Each is a CPU LDPC decode on the latency path.

### 2c. UL PUSCH
| stage | CPU/GPU | cost | P/C | CUDA |
|---|---|---|---|---|
| IQ capture (core 0 A725) | CPU | - | C | snapshot pool 36 |
| FEP | GPU | V 168k calls 93 us (over_cap 690), T 148k 89 us | P | RT partition |
| LDPC | GPU | V 128k CBs 198 us (gpu 158), fallback err 18560; T 138k CBs 193 (gpu 147), err 11798 | P | - |
| solver, equalise, layout/ID search, retry (nr_pusch_passive_decode.c:3143), cores 0,1 A725 | CPU | UL_E2E cpu p50/p99 V 1005/21737 us (n=73789), T 1364/20516 (n=57846); tot p50/p99 V 7265/53119, T 3180/51309 | P (1024-id sweep) | ISAC_UL_GPU_PASS graph (off) |
| background UL discovery (ulBgDisc) | CPU | not mapped | P | - |

### 2d. Discovery / background
| stage | CPU/GPU | cost | CUDA |
|---|---|---|---|
| CSI-RS discovery (core 11 A725) | GPU sweep + CPU control | 18 % [T] | nr_csirs_disc_gpu.cu/_sweep.cu (managed memory) |
| DM-RS ID background (dmrsIdBg x3) | CPU + GPU check | ~6 % each | - |
| radio: ring, uhd, dpdk-io | CPU | 40 / 65 / 2x100 % | KEEP-CPU |

## 3. CUDA modules (.cu total 12.4k lines)
| module | kernels | alloc | green stream | nr_gpu_model() |
|---|---|---|---|---|
| nr_pdcch_gpu_fep.cu | k_gold/k_llr/k_ls_pdcch | 5 DEVICE, 2 STAGING, 1 SHARED_RW | RT greatest (:212) | none |
| nr_pdsch_gpu_fep.cu | gather, rotate, fir, gold, hb, llr, ls, sigma, smid | 19 DEVICE, 11 STAGING, 2 SHARED_RW, 2 hostpin | RT (:283,:747,:805); disc (:289) | DIRECT/DISCRETE (:1233-1286) |
| nr_pdsch_gpu_demod.cu | align, evm1-3, evm_fold/out, gather, level, mask_pw, mmse3_prep, tail | 7 devmalloc, 3 hostpin | RT (:1460,:1481) | none |
| nr_pdsch_gpu_ldpc.cu | k_ldpc_t, k_ldpc4_t (cluster, nc) | 5 devmalloc, 8 hostpin | RT (:638); UL disc/lowest (:957-973) | none |
| nr_gpu_pass_graph.cu | k_norm_*, k_pick, k_seg_compose, k_unscr | graph | RT (:753) | 6 branches (:330-1118); default off |
| nr_pdcch_nid_gpu.cu | ~20 sweep kernels | 2 hostpin, 1 devmalloc | disc, least (:576) | none |
| nr_csirs_disc_gpu.cu, _sweep.cu | sweep, fep_extract, fft_*, lattice_* | 1 managed, 1 hostpin, 11 devmalloc | disc | none (managed = unified-only assumption) |
| nr_td_cb0_batch.cu, ldpc_cb0.cu, ldpc_decoder.cu, nr_polar_sc_cuda.cu | cb0_dematch, cb0_*, update_cn/vn, sc_decode | DEVICE/STAGING/SHARED_RW | none | via platform layer |
- Only FEP and the pass graph have explicit DIRECT/DISCRETE branches; demod, LDPC and PDCCH FEP rely on mapped/pinned host memory (sens6 discrete path unverified). csirs_disc uses cudaMallocManaged with no discrete path. Each translation unit makes its own green contexts over the same split (nr_gpu_green.h).

## 4. Classification
| stage | class | reason |
|---|---|---|
| PDCCH bank scan | PORT | data-parallel; kernels + polar SC exist; a2e floor 0.6 ms |
| pdcchJanon | PORT | 70 % unpinned; same LLR chain; V drops 52 % of submissions |
| NID worker host side | PORT-PARTIAL | kernels on GPU already; 34 % is staging/polling |
| USS tracker | KEEP-CPU | tiny control |
| dmrsScan, dmrsIdBg | PORT | 70-95 ms per hypothesis sweep |
| PDSCH slot grouping / dequeue | KEEP-CPU | scheduling policy |
| PDSCH LDPC prep | PORT | 19 us/CB serial; dematch kernel exists |
| PDSCH FEP/chest/demod/LDPC | already GPU | only CPU fallbacks remain |
| PUSCH solver / ID search | PORT-PARTIAL | 1024-id sweep parallel; control stays CPU |
| PUSCH FEP / LDPC | already GPU | ~10 % CPU fallbacks |
| CSI-RS discovery host | PORT-PARTIAL | GPU sweep exists; 18 % is control |
| UEthread, usrpRing, uhd_ctrl, dpdk-io | KEEP-CPU | real-time I/O (dpdk-io = 2 cores busy poll) |
| logAsync, stats, actors | KEEP-CPU | 1-4 % |

## 5. Top CPU consumers
T [MEASURED-T live map]: dpdk-io x2 200 (X925) > pdcchJanon 70 > uhd_ctrl+Tpool2 65 (X925) > usrpRing 40 (X925) > passivePusch0/1 38/38 (A725) > NID worker 34 (A725) > passivePdcch0/1 32/30 (X925) > UEthread 25 (X925) = dmrsScan 25 (A725); then csirsDisc 18, PDSCH consumers ~36 together. Decode/search work (janon + PUSCH + PDCCH scan + dmrsScan) ~3.1 cores; radio/I-O ~3.05.
V: no per-thread map (needs `top -H` sampling); log proxies [MEASURED-V counters, window INFERRED]: PDCCH bank passes 131 s (~1.0 core), css0 2.9 s, DM-RS scans 17.7 s wall (0.14 core), PDSCH consumers (PDTIM cpu) 58 s (0.44 core), PUSCH >= ~74 s (0.57 core, INFERRED). T proxies: DM-RS scans 57 s wall (0.45), PDSCH 44 s (0.35), PUSCH 79 s (0.62, INFERRED).

## 6. Port candidates ranked by gain / effort
| # | candidate | gain | effort | reuse |
|---|---|---|---|---|
| 1 | Lower ISAC_GPU_LDPC_NC_MIN_CB below 8 (~4) and use nc for UL | CB16+ GPU wait halved at 8; bench nc 2.8-4x faster at C=32 | small | nc kernels |
| 2 | Parallel / GPU LDPC prep (rate-dematch) | 0.3-0.6 ms serial at CB16-32 | medium | cb0_dematch_kernel (needs DL rate-match variants) |
| 3 | Scheduling: small first in slot + heavy-grant lane | removes pre/qwait, the largest p99 driver | small-medium | none |
| 4 | PDCCH bank scan fully on GPU (FEP + polar + CRC + candidate loop) | a2e floor 0.6 ms; frees ~0.6 core (+ janon 0.7); removes core-class sensitivity | large | nr_pdcch_gpu_fep.cu, nr_polar_sc_cuda.cu |
| 5 | pdcchJanon on the same GPU chain | 70 % core; V drops 52 % | medium after #4 | #4 |
| 6 | PUSCH ID search on GPU / ISAC_UL_GPU_PASS | 2 x 38 % cores + 1.0-1.4 ms CPU per grant; G2 CRC is ID-limited | medium-large | nr_gpu_pass_graph.cu, nr_gpu_pass_ul.c |
| 7 | DM-RS scan / ID hypotheses on GPU | 25 % + dmrsIdBg | medium | k_unscr/k_pick |
| 8 | Core placement (latency threads on X925, helpers on A725; idle gpuFepWorker) | dedicated cores, no code | small | - |
| 9 | NID worker / csirsDisc host control | ~0.5 core together | small-medium | already GPU |
Discrete note: #4-#7 need a discrete branch for sens6; today only FEP and the pass graph have one; csirs_disc has none.

Not checked: dpdk-io creation site; V per-thread map; ulBgDisc / dmrs_nid_solve / td_cb0_pool costs; discrete behaviour of demod/LDPC on sens6; exact run window. Core shares from mean x count are order-of-magnitude.
