# DECODETIME-20261012 -- the 20-430 ms device-wide decode stalls: GPU page faults on pageable host buffers (branch csd/decode-time, base csd/otatest eb0fec76ff)

Evidence labels: [M-replay] = DGX GB10 aarch64, 4 RX, `timacq_replay.sh SPEED=1` via heavy.sh, binary `rr-csd-dtm/.../build_t26` (CUDA, this branch), env `ota/t26z.env` + `ISAC_PDTIM_PCT=1 ISAC_PDTIM_CLASS=1`. b = Vodafone raw_pci415_12s_b (CAP_TICK 208029025, CAPC=RXC 3680010000, SSB 1171, FO 6900); tim = raw_pci923_tim_12s_tile3 (CAP_TICK 195322029, CAPC 3731910000, RXC 3759990000, SSB 233, FO -4800). Never merged across captures. [M-nsys] = nsys 2025.3 `-t cuda` of one replay (timings under nsys are not comparable with plain replays). [CODE-READ], [INF] = inference. No OTA. Data and scripts: `decodetime_data/` (`runs.txt` host record, `runs.json`, `summary.txt`, `drive.sh`, `parse.py`, `analyze.py`, `hwm.py`, `stallwin.py`, `nsys_prep.py`, `post_nsys.sh`).

## 1. Root cause [M-nsys, b, three traces]
The PDSCH GPU kernels take plain host pointers: the chest rows write `out` into a per-thread `scratch` (`aligned_alloc`, 32 MB, segmented path) or `dl_ch_estimates`, read `pil` and `rxdataF`; the demod writes `llr`, `rxdataF_comp`, `dl_ch_mag*` and reads `virt`/`rxdataF`/estimates. On the GB10 these pageable buffers are reachable from the GPU through unified memory, but **the GPU's first touch of every new 4 KB page is a GPU page fault**, and fault handling stalls the whole device, not the faulting kernel: every consumer's kernel in flight (FEP, chest, demod, in whatever green context) waits with it.

* GPUALLOC's `RL` trace (t26z, csd/otatest 8b7ceae5ad) [M-nsys]: all 6 episodes with a victim kernel > 20 ms contain a `k_fir` (chest FIR) launch that itself ran 20-44 ms; 23,108 `k_fir` launches, median 9 us, max 44 ms; at 1744 rows the launches took 10.9 / 43.4 / 41.8 / 9.8 ms with no discovery kernel resident in two of them (`stallwin.py`).
* Per-thread high-water-mark test (`hwm.py`, both this branch's TRIM trace and RL): a `k_fir` launch whose row count exceeds the thread's previous maximum (first write to new `scratch` rows) is slow; one that does not is not:
  | trace | class | n | p50 us | p90 us | max us | > 5 ms |
  |---|---|---|---|---|---|---|
  | RL (t26z) | new high-water mark | 43 | 6936 | 36813 | 44448 | 26 |
  | RL (t26z) | not | 23065 | 9 | 23 | 6703 | 2 |
  | TRIM (CHEST_TRIM=1, this build) | new high-water mark | 42 | 6769 | 31479 | 69661 | 25 |
  | TRIM | not | 7942 | 9 | 21 | 5583 | 3 |
  `k_ls` (same rows, writes device memory) shows no such pattern (new-hwm p50 23-29 us); `k_delay_exact` neither. Cost ~12 us per new 4 KB page (1744 rows x 16 KB = 28 MB touched over a run; 880 -> 1744 rows = 13.8 MB = 40 ms).
* `ISAC_PDSCH_GPU_CHEST_TRIM=1` (DECTAIL: compute only `12 nb_rb + 16` outputs) does not change it (table above): the cost is the page, not the arithmetic.
* The same mechanism, smaller, hits the grow-only `llr` buffer (re-malloc'd on growth: fresh pages) and the first grant of each thread (estimates, comp/mag, rxdataF); the 150-430 ms demod maxima of QUIETTIMING arms A/C are consistent with first touches of a large fresh `llr`/comp region [INF, not isolated per event].
* Not the cause (this trace set): SM contention from discovery kernels (present, adds 1.6-2.5x to RT kernels, GPUSHARE), stream serialisation, cudaMalloc/cuFFT/green-context creation (all < 1 s after start), clocks (same kernel, same size: 174 us vs 36.8 ms), host locks (wall == CPU: the threads spin in `cudaStreamQuery` while the device is stalled).

## 2. Fix [IMPLEMENTED] -- `ISAC_PDSCH_GPU_HOST_PIN=1` (default off; flag off = byte-identical path)
`cudaHostRegister` (pinned, mapped once) of every host buffer the kernels touch, done **at consumer start** (`nr_pdsch_passive_decode_pin_prewarm()` from the PDSCH queue consumer), so no decode pays the registration:
* `nr_pdsch_gpu_fep.cu`: `nr_pdsch_gpu_exact_pin/unpin` (cudaHostRegister/Unregister, exported). `nr_pdsch_gpu_exact.c/.h`: `nr_pdsch_gpu_host_pin()`, `_unpin()`, `_pin_enabled()` (flag + dlsym once, pthread_once; off -> -1 without touching the module).
* `nr_pdsch_passive_decode.c`: the five constant-shape per-thread buffers (`pdsch_dl_ch_estimates`, `rxdataF_comp`, `dl_ch_mag/magb/magr`) and the LLR buffer moved to file-scope TLS handles (`decode_bufs_prepare()`, `llr_prepare()`); with the flag on the LLR buffer is allocated once at the carrier's largest G (`N_RB_DL*12*14*8*4`, 2.9 MB at 273 RB), because every regrowth cost a `cudaHostUnregister` (first pin build, nsys: 42 calls, 22 ms mean, 186 ms max) and a re-pin; `virt` (segmented demod input) pinned on allocation.
* `nr_dl_channel_estimation.c`: `pil` and `scratch` (file-scope TLS, `gch_bufs()`), pinned; `nr_pdsch_gpu_chest_pin_prewarm()`.
* `nr_pdsch_passive_queue.c`: consumer `rxdataF` pinned after allocation; prewarm call at consumer start.
* Bit-exactness: no arithmetic or data-flow change (same pointers, same kernels); CHECK modes unchanged. [M-replay] nsys run `nP` (CHECK=64): `GPU_EXACT_CHEST calls=16000 check_bad=0`, `GPU_EXACT_DEMOD calls=16000 check_bad=0 evm_far=0`. (Section 4: CHECK=1 run.)
* Static TLS: 2.34 MB (`readelf -lW nr-uesoftmodem`, TLS MemSiz 0x256bd8; unchanged, only pointer-sized TLS added). Not touched: discovery, solver, queue order, cores.
* Tooling: `PDTIMCLSST` line (next to `PDTIMCLS`, needs `ISAC_PDTIM_CLASS=1`): per class x stage, ms of stage wall time held by bodies < 1 / 1-5 / 5-20 / > 20 ms -- the stage x duration table QUIETTIMING asked for.

### nsys before/after on this build [M-nsys, b, one run each, under nsys]
| trace | RT kernels > 10 ms | stall episodes (victim > 20 ms) | `k_fir` new-hwm p50 / max | total RT excess (DECTAIL attrib) | real mean / p99 / >20 ms | chest mean |
|---|---|---|---|---|---|---|
| TRIM (`CHEST_TRIM=1`, no pin) | 17 (k_fir to 69.7 ms, k_gather 26, k_evm1 28, k_fep_exact 14) | 4 (span 21-123 ms, up to 39 victims, all 5 consumers) | 6.8 ms / 69.7 ms | 5878 ms | 903 us / 9355 / 9 (n 2487) | 229 us |
| `HOST_PIN=1` (first build, pin on the decode path) | 0 | 0 | 62 us / 259 us | 746 ms | 644 us / 3125 / 3 (n 3249) | 123 us |
The remaining > 20 ms bodies of the pin trace were the registrations themselves (cudaHostRegister 2.6 ms mean / 52 ms max, cudaHostUnregister 22 / 186 ms, on the decode threads): moved to consumer start and removed (LLR at max size) in the committed build.

## 3. Validation: A = t26z vs P = t26z + `ISAC_PDSCH_GPU_HOST_PIN=1` [M-replay, interleaved, one replay at a time, 6+6 runs on b, 3+3 on tim]
Protocol as QUIETTIMING (`drive.sh`): quiet-host gate (effective load = loadavg - 3 D-state tasks < 2, no receiver), order b-A, b-P, ..., tim pairs after every second b pair; every run finished by itself (rc 0, TMO 200 not hit). Deviation: other agents' replays ran between my runs on the same heavy.lock; `other_recv=1` in every row is the receiver's own parent/child PID pair (pgrep -c counts both), as in QUIETTIMING. GPU idle clock at start 208 MHz except b-P2 (1072) and b-P4 (2411). Full tables: `decodetime_data/summary.txt`, raw `runs.json`.

### Vodafone b (mean +- sd over 6 runs; us unless noted)
| metric | A t26z | P +HOST_PIN | diff | Welch p / MWU p / bootstrap 95 % CI |
|---|---|---|---|---|
| **real-grant mean** | **819 +- 55** | **582 +- 46** | **-237 (-29 %)** | **0.000 / 0.002 / [-289, -186]** |
| real p50 | 422 +- 8 | 422 +- 16 | 0 | 1.0 / 0.87 |
| real p90 | 935 +- 60 | 883 +- 83 | -52 (-6 %) | 0.25 / 0.17 / [-128, +20] |
| **real p99** | 7373 +- 1992 | **2747 +- 810** | **-4627 (-63 %)** | **0.001 / 0.002 / [-6227, -3087]** |
| real max (ms) | 78 +- 20 | 35 +- 11 | -43 | 0.002 / 0.004 |
| real > 5 ms share | 1.3 % | 0.4 % | -69 % | 0.000 / 0.002 |
| real > 20 ms count / run | 13.2 +- 5.6 | 1.2 +- 0.8 | -91 % | 0.003 / 0.005 |
| real mean excl. > 20 ms bodies | 622 +- 24 | 571 +- 50 | -52 (-8 %) | 0.055 / 0.065 / [-93, -12] |
| real mean excl. > 5 ms bodies | 544 +- 10 | 543 +- 43 | 0 | -- |
| all bodies mean (real+retry+probe) | 610 +- 22 | 508 +- 46 | -101 (-17 %) | 0.002 / 0.002 |
| probe mean / p99 | 620 / 3435 | 428 / 1805 | -31 % / -47 % | 0.000 / 0.002 |
| retry mean | 516 | 515 | 0 | 0.97 |
| per-decode sum p50 / p90 / p99 | 372 / 745 / 3103 | 375 / 762 / 2252 | p99 -27 % | p99: 0.034 / 0.026 |
| stage mean fep / chest / demod / ldpc | 107 / 159 / 178 / 166 | 76 / 115 / 147 / 170 | -29 / -27 / -17 / +2 % | 0.002 / 0.001 / 0.007 / -- |
| stage max ms fep / chest / demod / ldpc | 67 / 76 / 246 / 13 | 4.3 / 29 / 100 / 4.1 | | |
| stage sd fep / chest / demod | 1117 / 1351 / 2358 | 94 / 331 / 960 | | |
| stall clusters > 20 ms / run (bodies) | 10.7 (28.5) | 3.3 (3.7) | -69 % | 0.000 / 0.005 |
| pdschq drop_stale % / scanq drop_stale | 14.4 / 649 | 8.3 / 378 | n.s. | 0.25 / 0.34 |
| CRC-OK real grants / distinct RNTIs | 1373 +- 613 / 27.2 | 1673 +- 322 / 26.3 | n.s. | 0.32 / 0.74 |
Per-stage time in bodies > 5 ms (PDTIMCLSST, real, sum over 6 runs): A fep 52 %, chest 39 %, demod 28 %, ldpc 4 % of the stage's time; P fep 2 %, chest 12 %, demod 12 %, ldpc 2 %. The FEP tail is gone (max 95 -> 4 ms, sd 1117 -> 94 us); the remaining > 20 ms bodies (3.7 per run, down from 28.5) are single-consumer events, 82 % demod (14 bodies in 6 runs, 50-230 ms), 17 % chest -- no longer clusters hitting all consumers (A: 28-37 % of clusters had >= 4 bodies; P: every cluster but one is a single body).

### TIM tile3 (3 runs/arm, noisy: real n 350-840 per run)
| metric | A | P | diff | Welch p / MWU p |
|---|---|---|---|---|
| **real mean** | 2684 +- 237 | **1286 +- 124** | -52 % | 0.003 / 0.10 |
| real p50 / p90 | 622 / 3078 | 772 / 2505 | +24 % (n.s.) / -19 % | 0.32 / 0.043 |
| real p99 | 19995 (histogram cap, all runs) | 6908 +- 551 | -65 % | 0.001 / 0.064 |
| real > 5 ms share / > 20 ms count | 6.6 % / 19 | 1.8 % / 1.7 | | 0.025 / 0.019 |
| all bodies mean | 843 | 632 | -25 % | 0.019 / 0.10 |
| stage mean fep / chest / demod / ldpc | 136 / 277 / 202 / 228 | 62 / 150 / 188 / 231 | | |
| CRC-OK real / RNTIs | 313 / 7.3 | 303 / 6.7 | n.s. | |
(MWU cannot go below 0.10 with 3 vs 3.)

**Verdict.** The device-wide stalls are removed (the one fix the data asked for): -29 % real mean on b, -52 % on TIM, p99 -63/-65 %, > 20 ms bodies -91 %, all with p <= 0.005 on b. **The target mean < 0.5 ms is not met: 582 +- 46 us on b** (best run 508). What is left is not stalls: the mean without > 5 ms bodies was 544 in A and is 543 in P, p50 is 422 in both, and the 1-5 ms band now holds 21 % of real time (PDTIMCLSST: chest 758, demod 692, ldpc 901 ms of 10.9 s) -- large grants whose GPU work legitimately takes 1-3 ms (LDPC of many code blocks, 2-4-layer demod on > 100 RB) plus RT-vs-RT queueing.

## 4. Bit-exactness [M-replay, b, run `ebK1_002927`, every grant checked]
`ISAC_PDSCH_GPU_HOST_PIN=1` + `EXACT_CHEST_CHECK=1 EXACT_DEMOD_CHECK=1 EXACT_CHECK=2`: `GPU_EXACT_CHEST calls=20000 check_ok=20000 check_bad=0`, `GPU_EXACT_DEMOD calls=20000 check_ok=19998 check_bad=0 evm_far=0 evm_maxrel=1.26e-05`, `GPU_EXACT_FEP calls=14000 check_ok=14000 check_bad=0 ... delay_checked=12963 delay_bad=0`, no MISMATCH line. (Timing of that run is not comparable: the CPU references cost 4.3 ms mean per body; a CPU reference of a large grant costs up to 36 ms, which is why `CHECK=64` arms carry a few > 20 ms demod/chest bodies of their own, see section 5.) The 6 P runs at CHECK=64 (t26z default): check_bad=0 everywhere.

## 5. Can real-grant p99 go below 0.5 ms? (coordinator question, 2026-10-13) -- no with this kernel set; here is the floor
Honest status on b with `HOST_PIN=1` (8 runs): real p50 422, **p90 ~0.85 ms, p99 2.3-3.7 ms**; per-decode sum p50/p90/p99 375 / 762 / 2252 us. Even the p90 is above 0.5 ms, so p99 < 0.5 ms would need the slowest 10 % of grants to lose 40-80 % of their time.

**Where that time is [M-nsys, b, pin trace nP, 15,023 bodies after t = 15 s, body = kernels of one thread up to and including its `k_ldpc4`]:**
| quantity per body | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| GPU kernel execution alone (sum of the body's kernels) | 222 us | 361 | **1165** | 276 |
| launch API time (cudaLaunchKernel etc., ~17 launches/body) | 111 | 147 | 212 | 116 |
| first launch -> LDPC end (wall) | 495 | 870 | 2795 | 652 |
5.1 % of bodies need more than 0.5 ms of **pure kernel execution**, 7.6 % more than 0.4 ms. The p99 body is a large grant (2-4 layers on > 100 RB, many LDPC code blocks): its kernels run 1.2 ms on an idle partition. Per kernel (same trace): `k_ldpc4` exec p50/p90/p99 73/106/465 us, `k_delay_exact` 30/59/153, `k_fep_exact` 37/48/74, `k_tail` 16/33/117, `k_evm1` 10/26/116; queue (launch -> start) is now small on the critical-path kernels (p99 <= 103 us for FEP/delay/LDPC; `k_fir` p50 23 us is in-stream order behind `k_ls`/`k_delay`, not contention); the 1-5 ms band of the EVM kernels (`k_evm_fold` queue sum 1.2 s) is on the deferred EVM stream, off the decode path. Of 2,265 launch waits > 200 us, 492 (22 %) had a discovery/NID kernel resident, 1,773 were RT-vs-RT or launch-side.
So after the fix the tail is **work, not waiting**: (a) ~17 launches x 5-15 us of launch API per body (0.11 ms p50, paid on the host, serial), (b) kernel execution that scales with grant size (LDPC code blocks, layers x RB in demod, 4096-point delay transform per chest row), (c) a little RT-vs-RT SM sharing (5 consumers on 40 SMs).

**Existing levers, measured on top of HOST_PIN [M-replay, b, 3 runs each vs 2 P runs of the same series; all n.s.]:**
| arm | real mean | p50 | p90 | p99 | > 20 ms | stall clusters |
|---|---|---|---|---|---|---|
| P (pin) | 574 +- 15 | 415 | 850 | 2950 +- 941 | 1.0 | 5.0 |
| B = P + `ISAC_PDSCH_GPU_BATCH=1` | 572 +- 25 | 412 | 825 | 2682 +- 318 | 0.7 | 2.0 |
| C = P + `ISAC_GPU_RT_PARTS=2` | 546 +- 46 | 418 | 792 | 2402 +- 593 | 0.0 | 3.7 |
Welch p >= 0.41 on every quantity (mean, p50, p90, p99, sum p99); CI of the mean difference +-25-70 us. Neither the admission gate nor the SM split moves the tail, consistent with queueing no longer being the tail's substance. The `pdschq`/`scanq` stale drops vary 1-32 % run to run in every arm (not arm-related).

**What p99 < 0.5 ms would need (not done tonight; estimates [INF] from the nsys numbers):**
1. One launch per stage, or one CUDA graph per grant shape: 17 launches -> 4-5 saves ~70-90 us per body on the host (p50 launch API 111 us). The CPU steps between stages (geometry, rate-dematching setup) have to move or overlap (GPUFUSE 3.4).
2. Faster large-grant kernels: LDPC p99 465 us (code blocks decoded in sequence per TB -> batch the CBs of one TB across blocks), demod `k_tail`/`k_evm1` p99 ~115 us, `k_delay_exact` 153 us (a 4096-point transform per (symbol, layer, antenna) row even for 2-RB segments: a trimmed transform or one per PRG group). Together these set the 1.2 ms p99 kernel floor; p99 < 0.5 ms needs it ~3x lower.
3. Then concurrency: with 5 consumers the RT-vs-RT share (1.9-2.5x median exec, GPUSHARE) still applies; `RT_PARTS`/`BATCH` would start to matter only once (1)-(2) are done.
A realistic near-term target with (1)+(2) is p90 ~0.5 ms and p99 ~1 ms; p99 < 0.5 ms is not reachable by scheduling alone.

### The remaining > 20 ms bodies are the CHECK=64 CPU references [M-replay, b, Z = P + `EXACT_CHEST_CHECK=0 EXACT_DEMOD_CHECK=0 EXACT_CHECK=0`, 3 Z vs 2 P interleaved]
| metric | P (CHECK=64, t26z) | Z (CHECK=0) | diff | Welch p / MWU p / CI |
|---|---|---|---|---|
| real mean | 568 +- 24 | **514 +- 21** (runs 538, 504, 499) | -54 (-10 %) | 0.12 / 0.20 / [-84, -24] |
| real p50 / p90 | 425 / 885 | 388 / 828 | -9 % / -6 % | p90 0.18 |
| real p99 | 2345 | 2382 | 0 | 0.90 |
| real max ms / > 20 ms count | 39.7 / 1.0 | 5.7 / 0 | | 0.000 (count) |
| probe mean / p99 | 399 / 1475 | 347 / 898 | -13 % / -39 % | |
| stage chest max ms | 13.9 | 2.1 | | |
With the pin in place the real-grant > 20 ms bodies disappear entirely when the every-64th-grant CPU reference (chest + demod, up to 36 ms on a large grant, section 4) is off, and the mean drops below 0.5 ms in 2 of 3 runs. This is a diagnostic arm: CHECK=0 removes the on-air bit-exactness guard, so it is not proposed as such; `CHECK=512`/`1024` would keep ~30-60 references per 150 s OTA run at 1/8-1/16 of the cost [INF, not measured]. p99 is untouched by it (work, not checks).

## 6. Conclusions
1. Root cause of the systemic 20-430 ms pauses: GPU page faults on the GPU's first touch of pageable host buffers (chest `scratch` rows above the thread's previous maximum, LLR regrowth, first grant per thread), each fault stalling the whole device. Fixed by pinning at consumer start (`ISAC_PDSCH_GPU_HOST_PIN=1`, default off, flag off byte-identical). b: real mean 819 -> 582 us (-29 %, p < 0.001), p99 7.4 -> 2.7 ms, > 20 ms bodies -91 %, FEP tail gone; TIM: 2684 -> 1286 us (-52 %). Bit-exact (every-grant CHECK: 0 bad of 20,000 chest, 20,000 demod, 14,000 FEP). No CRC regression (CRC-OK counts and RNTIs n.s., same direction up).
2. Target mean < 0.5 ms: **not met** -- 582 +- 46 us with the pin (best 508), 514 +- 21 with the pin and no CPU references (2 of 3 runs < 0.5 ms). Target p99 < 0.5 ms: not reachable with this kernel set; the p99 body's kernel execution alone is 1.2 ms plus 0.11 ms of launch API. `GPU_BATCH=1`, `RT_PARTS=2` do nothing further (n.s., sections 3/5). What would: one launch (graph) per stage, LDPC code blocks of a TB in one launch, trimmed per-row delay transform; then concurrency control.
3. `ISAC_PDSCH_GPU_CHEST_TRIM=1`: not the lever (page cost, not arithmetic); harmless, still default off and unmeasured for speed.
4. Tooling: `PDTIMCLSST` per class x stage x duration band (used above); `decodetime_data/hwm.py` (per-thread high-water-mark test on an nsys sqlite), `stallwin.py` (what ran during a > 20 ms victim window), `post_nsys.sh`.
5. Host notes: the quiet gate (effective load < 2, no receiver) held at every start, but other agents' compilers (`cc1`, `cc1plus`, `asn1c`), a `test_nr_pdcch_b*` test and python ran during 20 of my 32 runs (`hogs_*.txt`); the heavy.lock serialised the replays themselves. Both arms saw the same conditions (interleaved), so the comparisons stand; absolute levels are a touch pessimistic.

## OTA env delta (for the orchestrator to verify on air; replay-validated, [IMPLEMENTED, NOT VALIDATED] OTA)
Add one setting to `ota/t26z.env` (call it `t26zH.env`): `ISAC_PDSCH_GPU_HOST_PIN=1`. Expected log line once at start: `SENSING: ISAC_PDSCH_GPU_HOST_PIN=1: GPU-touched host buffers pinned at allocation (ok)`; a `GPU_HOST_PIN: cudaHostRegister(...) failed rc=` warning means a buffer stayed pageable (fallback = old behaviour). Memory: ~45 MB pinned per consumer (32 MB chest scratch + 3.7 MB estimates + 4 x 1 MB comp/mag + 2.9 MB LLR + 1 MB rxdataF), 5 consumers ~225 MB. Rollback = drop the setting. Keep `CHECK=64`; a `CHECK=512` trial is optional and separate. Read per run: PDTIMCLS real mean/p99/gt20ms, PDTIMCLSST (> 20 ms bands should be ~0 for fep/chest), PDTIMSLOW count, `GPU_EXACT_* check_bad=0`.
Spec lines (TAG:ENV:CFG:FREQ:SSB:FO; cfg `sd_t26ul` as the 2026-10-12 OTA; >= 3 each, interleaved, 150 s):
```
H0a:t26z:sd_t26ul:3680010000:1171:6900
H1a:t26zH:sd_t26ul:3680010000:1171:6900
H0b:t26z:sd_t26ul:3680010000:1171:6900
H1b:t26zH:sd_t26ul:3680010000:1171:6900
H0c:t26z:sd_t26ul:3680010000:1171:6900
H1c:t26zH:sd_t26ul:3680010000:1171:6900
T0a:t26z:sd_t26ul:3731910000:1169:-4800     # TIM PCI 923, the T1 live arguments (timacq_replay.sh header); if the orchestrator's TIM spec differs, use that
T1a:t26zH:sd_t26ul:3731910000:1169:-4800
```
Success = real-grant mean and p99 lower than the paired t26z run with `check_bad=0` and CRC-OK RNTIs not lower; on air the class-A 0.9 s freezes (STALLS) are a different mechanism and should not move.

## Files
Code (commit 11b0bef449): `nr_pdsch_gpu_fep.cu`, `nr_pdsch_gpu_exact.c/.h`, `nr_pdsch_passive_decode.c/.h`, `nr_pdsch_passive_queue.c`, `nr_dl_channel_estimation.c`. Data: `decodetime_data/` (`runs.txt` A/P series, `runs2.txt` K/B/C/P, `runs3.txt` Z/P, `summary*.txt`, `runs*.json`, `hogs_*.txt`, drivers and parsers). nsys traces (not committed, 0.5-1 GB): `replays/replay_nT_225248/nsysT.*` (TRIM, no pin), `replays/replay_nP_230730/nsysP.*` (first pin build).
