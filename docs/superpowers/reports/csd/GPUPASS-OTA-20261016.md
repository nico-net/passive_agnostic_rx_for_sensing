# GPUPASS OTA window 2026-10-16 (night) — t26f vs t26g vs t26gR

[MEASURED, DGX OTA V (Vodafone PCI 415, 3680.01 MHz) and T (TIM PCI 923, 3759.99 MHz) LOW LOAD 2026-10-16 night, n=2]
Window 01:40-02:16 CEST, X410 34B2AF1, cfg sd_t26ul, 150 s runs, binary csd/gpupass build_t26 (3eed2b1768). Cells are never pooled. Env arms: t26f = frozen default; t26g = t26f + ISAC_GPU_PASS=1 ISAC_UL_GPU_PASS=1; t26gR = t26g + ISAC_PDSCH_RECOVERY_ASYNC=1. Run dirs: /home/nicola/NICOLA/ota_runs/ab_{F,G,GR,TF,TG,TGR}{1,2}_1_* (the 01:40-02:16 stamps; older dirs of the same tag are other windows). Per-run data: p99_stats.py --json plus EVMGRANT/PUSCHQ/GPUPASS/RECOV log parsing (scripts in the session scratchpad, not committed).

## 0. Headlines
- All 12 runs: sync=1, rfstall=0, no crash (RX_EXIT=130 = SIGINT stop), no ring overflow line; CSI-RS confirmed V 110/42/51/106/100/40 (F1/G1/GR1/G2/GR2/F2), T 4/3/1/3/2/1 (window log).
- Hard checks all 0 in all 12 runs: bad_chest, bad_demod, mismatch, GPU_EXACT_FEP check_bad, UL_GPU_FEP bad, UL_GPU_LDPC check_bad, GPUPASS shadow_bad, shadow_fail_bad; no GPUPASS MISMATCH/DISABLED line.
- V traffic collapsed in the G/GR runs (PDSCHQ queued 821 / 961 / 978 / 2105 versus 17 047 / 9 333 for F). V is NOT comparable across arms (see 1.1), so no V verdict on CRC or tails.
- T (load comparable, 22.8k-47k real grants/run): GPUPASS fires on essentially every real grant, CRC is within run spread, but the >5 ms tail count is 2-9x the t26f value in both t26g runs and the whole-call p99 is higher (5.2-5.8 ms vs 2.2-4.3 ms). Sub-slot p99 < 500 us is not met by any arm, DL or UL.
- UL: UL CRC-OK is ~0 on air in every arm including t26f; the UL cache is therefore almost never filled (section 3).

## 1. DL

Columns: dec = whole decode call (EVMGRANT dec_us, probe=0 grants) incl. retries; first-pass per-PRB split is not logged in these runs (no ISAC_E2E_TRACE), so the PRB buckets use the whole-call time (retries included); CRC = EVMGRANT probe=0 crc ok / grants (PDSCHQ crc_ok% in brackets includes probe bodies); fat = count of calls > 2 ms / > 5 ms; stale = PDSCHQ stale drops (full=0 in all runs).

### 1.1 V (Vodafone PCI 415)
| run | grants | CRC-OK % | dec p50 / p99 / max | p99 <=96 PRB (n) | p99 >96 PRB (n) | >2 ms / >5 ms | stale |
|---|---|---|---|---|---|---|---|
| F1 t26f | 15633 | 22.7 (21.3) | 2390 / 13893 us / 154 ms | 7767 (12617) | 19661 (3016) | 9211 / 2560 | 166 |
| F2 t26f | 3550 | 83.0 (30.5) | 592 / 7249 us / 42 ms | 2730 (3422) | 11667 (128) | 267 / 73 | 0 |
| G1 t26g | 743 | 92.5 (83.7) | 543 / 1529 us / 9.4 ms | 1529 (743) | - (0) | 5 / 3 | 0 |
| G2 t26g | 482 | 96.9 (47.8) | 534 / 3688 us / 8.5 ms | 2462 (477) | 3902 (5) | 11 / 3 | 0 |
| GR1 t26gR | 1010 | 99.1 (99.1) | 579 / 1267 us / 13 ms | 1267 (1010) | - (0) | 5 / 2 | 1 |
| GR2 t26gR | 552 | 62.7 (19.2) | 1542 / 8075 us / 39 ms | 4712 (261) | 10731 (291) | 229 / 52 | 10 |

V caveat: grants per run differ by 20-35x between arms (F 3.5k-15.6k vs G/GR 0.5k-1.0k, except GR2). G1/G2/GR1 are mostly MCS 0-4, Nl 1 (rnti 0xffff broadcast-like) grants, which decode ~92-99 % and are fast; F1 carries 7169 Nl2 + 2580 Nl3 grants with 8321 MCS 5-9 grants at 22 % CRC. The CRC and tail numbers differ because the traffic differs, not because of the arm. No V verdict is possible. Sub-slot: V p99 is 1.3-13.9 ms in every arm.

### 1.2 T (TIM PCI 923)
| run | grants | CRC-OK % | dec p50 / p99 / max | p99 <=96 PRB (n) | p99 >96 PRB (n) | >2 ms / >5 ms (% of grants) | stale |
|---|---|---|---|---|---|---|---|
| TF1 t26f | 44448 | 68.1 (57.4) | 628 / 4293 us / 1270 ms | 2259 (42558) | 13457 (1890) | 2124 / 293 (4.8 / 0.66) | 104 |
| TF2 t26f | 46784 | 70.2 (53.0) | 574 / 2163 us / 84 ms | 1598 (46258) | 12009 (526) | 558 / 56 (1.2 / 0.12) | 44 |
| TG1 t26g | 35300 | 61.9 (50.3) | 793 / 5765 us / 202 ms | 2970 (33404) | 11706 (1896) | 2456 / 555 (7.0 / 1.57) | 61 |
| TG2 t26g | 40788 | 67.6 (39.4) | 603 / 5172 us / 73 ms | 2314 (38912) | 11490 (1876) | 2143 / 457 (5.3 / 1.12) | 34 |
| TGR1 t26gR | 22782 | 60.7 (44.2) | 691 / 5386 us / 70 ms | 2114 (22028) | 47061 (754) | 856 / 266 (3.8 / 1.17) | 91 |
| TGR2 t26gR | 40004 | 70.2 (55.2) | 597 / 5176 us / 51 ms | 1788 (39013) | 34570 (991) | 1207 / 425 (3.0 / 1.06) | 307 |

CRC by class on T (EVMGRANT, ok/n): MCS 0-4 at SNR>=10: TF 8269/8291, 1634/1646 (99.7, 99.3 %); TG 3489/3532, 2342/2368 (98.8, 98.9 %); TGR 1151/1175, 1890/1912 (98.0, 98.9 %). All-SNR MCS 0-4: TF 88.3 / 71.4 %, TG 82.3 / 76.2 %, TGR 81.4 / 75.2 % (the t26f run-to-run spread, 17 pt, exceeds the arm differences). MCS 10-16 at SNR>=10: TF 14.9 / 36.8 %, TG 18.4 / 17.0 %, TGR 16.3 / 5.4 % (n varies 299-7204, not comparable).
Context common to all T arms including t26f: the DMRS_ID line reports PDSCH MISMATCH (estimated n_id 253/455/305/444 vs assumed 923, margin 10-13 dB) in every T run, so the T CRC ceiling is arm-independent.
Retry bodies (PDTIMCLS retry n): TF 2480 / 2789, TG 2471 / 2605, TGR 1491 / 1314.
The PDTIMCLS body p50 collapses to 5 us in the g arms (GPU pass bypasses the stage timers), so the body-level p99 is not comparable between f and g; only the whole-call dec_us above is.

## 2. GPUPASS census (DL)
Final GPUPASS line per run. fb = fallback (host module / tbparam); tb_fail = TB CRC failed in the pass (continues to CPU: cont_fail / old_first); shadow / shadow_bad; nomem = host gate counter.
| run | launches (graph_hits) | fb | tb_fail | commit | shadow (bad) | shadow_fail (bad) | old_first | nomem | MISMATCH/DISABLED |
|---|---|---|---|---|---|---|---|---|---|
| G1 V | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | none |
| G2 V | 64 (64) | 0 | 13 | 63 | 0 | 0 | 0 | 0 | none |
| GR1 V | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | none |
| GR2 V | 93 (93) | 2 (module 2) | 57 | 93 | 0 | 0 | 2 | 153 | none |
| TG1 T | 35694 (35694) | 146 (module 146, tbparam 51) | 14706 | 35137 | 327 (0) | 229 (0) | 753 | 0 | none |
| TG2 T | 41825 (41825) | 159 (module 159, tbparam 118) | 15055 | 41171 | 418 (0) | 235 (0) | 930 | 0 | none |
| TGR1 T | 21477 (21477) | 27 | 8280 | 21136 | 206 (0) | 129 (0) | 430 | 2793 | none |
| TGR2 T | 38884 (38884) | 28 | 11255 | 38277 | 431 (0) | 175 (0) | 923 | 2896 | none |
- Fallbacks by reason: only host module/tbparam (0.4 % of launches in t26g, 0.07-0.13 % in t26gR); unseen/busy/unsup/err/ldpc_key = 0, need_cpu = 0 everywhere. tb_fail is ~41 % of launches = the CRC-fail rate; those grants continue on the CPU path (cont_fail), which doubles work for the failing ones.
- V: GPUPASS barely engaged (0-93 launches), because the V grants in G/GR were broadcast-like/small; the V census says nothing about V decode quality.
- t26gR gates GPUPASS with nomem 2793 / 2896 (T) and 153 (V GR2) grants (snapshot holders pin memory), absent in t26g.
- Warm-up: the log has no timestamps, so the wall time receiver-start to "GPUPASS enabled" is NOT measured. Order in every g/gR log: DL_GPU warm-up begin (before RX) -> "GPUPASS enabled" (log line ~64) -> "GPUPASS_UL enabled" -> HW/USRP configure (line ~101) -> USRP_RING -> USRP_RX_FIRST_BUFFER (line ~296). The radio is not streaming during warm-up, so the ring cannot overflow in it; rfstall=0 and zero overflow/drop lines in the first 30 s and afterwards. PDCCH_NID_GPU kernel_overrun lines (180-271 per run) are discovery GPU kernels, present equally in t26f runs, unrelated to the radio.

## 3. UL
Columns: ULDCI = distinct UL grants seen; CRC lines = UL_PUSCH_CRC lines (all attempts incl. probe-layout trials) ok/total; cpu = UL_PUSCH_CRC cpu_us p50/p99; PUSCHQ = queued / decoded / crc_ok / stale-dropped, max_lag slots (limit 5) (no UL_E2E trace in these runs: the queue-wait proxy is the stale drops and lag); bwp = last ULLAYOUT state.
| run | ULDCI | CRC-OK / attempts | cpu_us p50 / p99 | PUSCHQ q / dec / ok / stale (lag) | bwp |
|---|---|---|---|---|---|
| F1 V f | 1414 | 0 / 112 | 2115 / 15951 | 1208 / 82 / 0 / 24 (29) | searching |
| F2 V f | 13102 | 0 / 941 | 1930 / 27923 | 12045 / 648 / 0 / 290 (62) | searching |
| G1 V g | 97 | 0 / 0 | - | 0 | searching |
| G2 V g | 2545 | 0 / 179 | 1998 / 5577 | 2396 / 164 / 0 / 142 (25) | searching |
| GR1 V gR | 0 | 0 / 0 | - | 0 | searching |
| GR2 V gR | 1000 | 0 / 80 | 2583 / 9379 | 166 / 77 / 0 / 15 (19) | searching |
| TF1 T f | 37444 | 1 / 23052 | 1890 / 23755 | 48753 / 22733 / 1 / 5995 (62) | searching |
| TF2 T f | 54204 | 0 / 9596 | 21800 / 32807 | 114661 / 9037 / 0 / 15300 (68) | searching |
| TG1 T g | 30327 | 3 / 23450 | 1255 / 4075 | 26353 / 18057 / 2 / 123 (29) | LOCKED 217@0 |
| TG2 T g | 52597 | 2 / 10008 | 13051 / 32161 | 94502 / 9234 / 2 / 15139 (67) | searching |
| TGR1 T gR | 20421 | 1 / 3264 | 2773 / 25987 | 7404 / 3246 / 1 / 945 (67) | searching |
| TGR2 T gR | 48110 | 0 / 9856 | 12493 / 29624 | 91115 / 9330 / 0 / 14805 (57) | searching |

GPUPASS_UL census (last line):
| run | runs / launches | cache hit / miss / stale / put | ok / fail / refine | shadow (bad) | GPUPASS_UL_TIME |
|---|---|---|---|---|---|
| V G1, GR1 | 0 | 0 / 0 / 0 / 0 | 0 | 0 | - |
| V G2 / GR2 | 0 | 0 / 340 / 0 / 0 and 0 / 211 / 0 / 0 | 0 | 0 | old grant_us p50 990 / 871, p99 2478 / 7901 |
| TG1 | 0 | 0 / 30171 / 0 / 0 | 0 | 0 | old p50 923, p99 3772, max 207018 us |
| TG2 | 7 / 7 | 7 / 15941 / 0 / 2 | ok 0, fail 7 | 0 | old p50 3311, p99 33205; pass_call p50 232 p99 489 us (n=7); pass_fail grant p50 304 p99 599 |
| TGR1 | 8 / 8 | 8 / 8677 / 3 / 1 | ok 0, fail 3, refine 5 | 0 | old p50 802, p99 20238; pass_call p50 221 p99 233 us (n=8); pass_fail grant p50 290 p99 300 |
| TGR2 | 0 | 0 / 15625 / 0 / 0 | 0 | 0 | old p50 3270, p99 30810 |
All UL pass launches (15 over the window) failed CRC (tb_fail = runs); shadow_bad = 0 only because the shadow check (every 16 passes) never ran (runs <= 8): the UL GPU pass is NOT validated on air.

### Why the UL cache is almost never filled
1. The cache is filled only on a real CRC-OK decode (ul_hyp_store at nr_pusch_passive_decode.c:2918), and not for solver probes, cfr_only, gapped, fdra_unresolved, transform precoding or a stale cfg epoch. The validation-only seed (ISAC_UL_GPU_PASS_SEED=1, stores after a CRC fail too) is not set in t26g/t26gR.
2. UL CRC-OK is rare in every arm, t26f included: T t26f 1 of 32648 attempts (TF1 1/23052, TF2 0/9596); T t26g 5 / 33458; T t26gR 1 / 13120; V 0 in all 6 runs (0 / 1312 attempts in total). Rate ~1e-4. The successes are tiny low-MCS TBs (tbs 26-301 bits) at DMRS SNR -5.8 to -11.2 dB, so the UE uplink arrives far below the decode threshold at the passive antennas.
3. The UL layout (BWP) is not locked in 11 of 12 runs (ULLAYOUT bwp=searching 0@0), so the queue holds layout-probe trials (PUSCHQ2 real queued = 0 in these runs, except TG1 real 931), most of which fail by construction. Only TG1 reached LOCKED 217@0 (3 CRC-OK lines, still put = 0: those OKs did not meet the put conditions, probably probe/solver-path; not verified).
4. A cache hit needs the same RNTI and the same start-symbol/num-symbols shape; C-RNTIs rotate. Where it did fill (TGR1 put 1, TG2 put 2) the hypothesis was used 8 / 7 times and every pass failed CRC (the cached TA and DMRS identity from one low-SNR grant do not predict the next).
5. Stale drops of 11-16 % of UL jobs in the heavy T runs (lag 57-68 of 5 allowed) remove many attempts.
Comparison with RESULTS-20261015-night.md (/home/nicola/NICOLA/wt/rr-orchestration/ota/): there UL CRC-OK was also ~0 in all envs except a single V t26eG run with BWP LOCKED 217@0 (25 CRC-OK, n=1), and UL DMRS SNR medians were -4.6 to -13.7 dB. This window agrees: <= 3 CRC-OK per run, none on V, and V traffic was far lower than the V runs of that report. t26f UL CRC-OK here: 1 total (T), 0 (V); the g arms are not worse (T g 5, gR 1 total, n=2 each, all within a count of a few).

## 4. RECOV (t26gR)
| run | submitted | rescued | stale | expired | dropped_full | fb_ok / fb_fail | nosnap | late ms p50 / p99 | RECOVLATE lines |
|---|---|---|---|---|---|---|---|---|---|
| GR1 V | 0 | 0 | 0 | 0 | 0 | 0 / 0 | 0 | 0 / 0 | 0 |
| GR2 V | 174 | 55 | 67 | 0 | 1 | 55 / 148 | 29 | 4 / 24 | 55 |
| TGR1 T | 8905 | 8 | 100 | 0 | 16 | 8 / 8939 | 42 | 1 / 3 | 8 |
| TGR2 T | 11879 | 5 | 148 | 0 | 17 | 5 / 11887 | 13 | 1 / 3 | 5 |
On T the async recovery rescues 0.05-0.06 % of submissions (13 of 20 784) and never expires; on V GR2 it rescued 55 of 174 (32 %) but with 4 / 24 ms lateness (V is a low-traffic, different-traffic run; no t26f/t26g reference with matching traffic).

## 5. Verdict per cell (t26g and t26gR vs t26f; sub-slot target p99 < 500 us)
Criteria: (a) no CRC loss within run spread; (b) no new fat tails; (c) check_bad / shadow_bad = 0; (d) sub-slot p99 DL and UL.

### V (n=2, traffic not comparable)
- (a) CANNOT BE JUDGED: g/gR runs carry 0.5k-2.1k PDSCH grants vs 3.5k-17k for f, with a different grant mix; observed CRC (g 92.5 / 96.9, gR 99.1 / 62.7 vs f 22.7 / 83.0) tracks the mix.
- (b) CANNOT BE JUDGED for the same reason; no tail above 154 ms in any V run.
- (c) PASS: all bad counters 0; GPUPASS barely engaged (0-93 launches), so the pass was exercised on V only marginally.
- (d) NOT MET: DL whole-call p99 1.3-13.9 ms, UL cpu p99 5.6-27.9 ms.
- Overall: no V evidence for or against; repeat when V carries daytime traffic.

### T (n=2, comparable load)
- t26g: (a) PASS within spread (EVM CRC 61.9 / 67.6 vs 68.1 / 70.2; 98.8-98.9 vs 99.3-99.7 % at SNR>=10, MCS 0-4, a 0.4-0.9 pt dip that is within the 1 pt granularity of these counts); (b) FAIL: >5 ms calls 1.57 / 1.12 % vs 0.66 / 0.12 % (t26f), >2 ms 7.0 / 5.3 % vs 4.8 / 1.2 %, whole-call p99 5.8 / 5.2 ms vs 4.3 / 2.2 ms, <=96 PRB p99 3.0 / 2.3 ms vs 2.3 / 1.6 ms (the max 202 ms is below the t26f TF1 max 1270 ms); (c) PASS (shadow 745, shadow_fail 464, all bad 0); (d) NOT MET.
- t26gR: (a) PASS within spread (60.7 / 70.2); (b) PARTIAL: <=96 PRB p99 2.1 / 1.8 ms is at t26f level, but >5 ms calls 1.17 / 1.06 % and whole-call p99 5.4 / 5.2 ms are above t26f, and the >96 PRB tail is long (p99 35-47 ms, n 754-991); RECOV rescues almost nothing on T and nomem blocks ~2.8k GPUPASS grants per run; (c) PASS (shadow 637, all bad 0); (d) NOT MET.
- Sub-slot (d), both arms and t26f: DL whole-call p50 0.57-0.79 ms, p99 2.2-5.8 ms (not < 500 us); UL cpu p50 1.1-21.8 ms, p99 4.1-32.8 ms; the only sub-500 us UL numbers are the 15 GPU pass calls (p50 221-232 us), all of which failed CRC.
- Merge-relevance: GPUPASS is correct (bit-exact checks 0 bad, shadows clean) and engages on ~100 % of T grants, but on air at night it does not yet lower the T whole-call tail; it raises the >5 ms count. Candidate causes (hypotheses, unverified): ~41 % of passes end in tb_fail and then repeat on the CPU path (double work); GPU contention with discovery kernels. UL pass has no on-air validation (shadow never ran).

## 6. Not done / caveats
- n=2 per arm per cell, one night, no daytime load; V arms confounded by the traffic collapse. Per the no-cell-tuning rule this is indicative only, not an OTA validation of tails.
- First-pass p99 per PRB bucket uses whole-call dec_us (no E2E trace in these runs).
- Warm-up wall time and UL queue-wait in microseconds are not available from these logs.
- Run-to-run spread inside t26f is large (TF1 vs TF2: >5 ms count 0.66 vs 0.12 %, p99 4.3 vs 2.2 ms), so a 2x change in the >5 ms rate is borderline at n=2; the g arms exceed the larger t26f value in all four g/gR runs.
