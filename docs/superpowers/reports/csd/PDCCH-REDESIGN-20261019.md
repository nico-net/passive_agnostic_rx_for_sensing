# PDCCH redesign: slot arrival -> DL grant enqueued from 0.5-0.74 ms to <= ~0.15 ms (investigation and design, 2026-10-19)

Branch csd/int16-pdcchinv (from csd/int16-merge19 c79c2e94d0). Design only: no production code changed. Prototypes and micro-benchmarks are under
`tests/pdcch_redesign/` (standalone g++/gcc/nvcc, `build.sh`, no cmake; raw outputs in `tests/pdcch_redesign/results/`).

Evidence labels used on every claim:
- [MEASURED on air 2026-10-19] the two daytime logs `ab_jjsT1_1_090021` (TIM) and `ab_jjsV1_1_090338` (Vodafone, "V").
- [MEASURED on air 2026-10-18 evening] A/B arms that were built with `ISAC_A2E_TRACE` (the 10-19 daytime logs carry only `e2e=`, no `a2s=/a2p=/a2x=`).
  Used only for stage SHARES; arms differ in env flags, so they are never merged with the 10-19 totals.
- [MEASURED bench, DGX] micro-benchmark in this directory, DGX Grace aarch64, taskset 0-4 (Cortex-A725), nice 10, inside heavy.sh, no receiver running.
- [READ FROM CODE] file:line reading of this tree. [INFERRED] derived by arithmetic from the above, not measured.
Cells are never pooled: TIM (36 CCE CORESET, DCI 1_1 len 47, AL1 dominant) and V (CORESET 33-36 CCE, DCI 1_1 len 49, AL2/AL4) are reported side by side.

## 0. Answer in ten lines

1. The PDCCH critical path of a known RNTI today is: wake of the scan consumer (~65 us) + one full scan pass (FEP/LLR 76, then sweeps, pre-pass, UL scan, ~95 us of polar decodes: ~190-250 us total) + accept->enqueue (125-178 us, of which the DCI 1_1 solver reading pass is 94-142 us). Polar decode is only ~15-20 % of it. [MEASURED on air 2026-10-18]
2. Faster polar alone cannot reach 0.15 ms. A prototype NEON fast-SSC is 2.1-3.5x faster than `polar_decoder_int16` for one candidate and 5-12x with 8 candidates per call (0.24-0.83 us per decode vs 2.8-5.2 us), with equal decode success. [MEASURED bench, DGX]
3. A CPU-only path cannot reach 0.15 ms either: even with a fast lane, a perfectly timed wake and a parse cache, the floor is FEP+LLR (76) + accept bookkeeping (~25) + solver readings (~70) + decode (~10) = ~180-200 us (TIM) / ~245 us (V). [INFERRED]
4. What does reach it, at zero extra cores: do not wait for the slot to end. The CORESET symbols are in the radio ring ~430 us before the read that is stamped "arrival" returns. Start the fast lane when the CORESET symbols are in the ring (inside the RX thread's read wait) and release the PDSCH job at arrival. arr2enq becomes <= 0 (the grant is ready before the slot ends) for the known-RNTI lane. [INFERRED from READ FROM CODE; not prototyped, needs the RX-loop change]
5. GPU polar does not help: a GPU fast-SSC is bit-exact with the CPU prototype but costs 31-59 us per batch and 33-63 us per persistent-kernel round trip; the CPU does the same 5-30 decodes in 2-20 us. [MEASURED bench, GB10]
6. Wake/hand-off: cross-core cond/futex/eventfd wake costs ~100 us p50 on cores 1->2 whatever the primitive; same-core 2.6 us; a timer-then-spin hybrid (wake 15-40 us before the next slot's known time) costs 0.2 us at 3-8 % of one core. [MEASURED bench, DGX]
7. FEP is not shared: the PDCCH symbol is FFT'd by the CPU scan (OAI dft4096 25.8 us per antenna on A725, 103 us for 4), the PDSCH uses its own GPU exact-FEP call on its own symbols. A shared GPU slot-FEP would not help latency (GPU FEP call p50 <= 64 us wall).
8. Side finding (robustness, not latency): `polar_decoder_int16` mis-decodes clean AL4/AL8 DCIs when the soft-bit amplitude is >= ~100-200 (int16 -32768 hazard in the SIMD min-sum). Production caps the LLR at +-31 (autoscale + clip, `dci_nr.c:1008`), so it is latent today, but any LLR-scale change (GPU LLR, wider clip) would hit it. [MEASURED bench, section 4.4]
9. Recommended design: "PDCCH-FL" = fast lane A (screen, known-RNTI + CSS candidates, batched fast-SSC, DL accept first) inline on the RX thread inside the slot read wait, discovery/UL lane B unchanged on the scan consumers behind it. Expected arr2enq p50: <= 0 (grant ready before arrival), cores: -0.1 to -0.3 (net), no per-cell parameters.
10. Staged delivery with a measured gain at each stage: S1 fast-SSC (low risk, ~-40..-80 us), S2 DL-first ordering (~-90..-140 us), S3 hybrid wake (~-55 us), S4 early start (~-430 us).

## 1. Breakdown of arr2enq (question 1)

### 1.1 Totals, daytime 2026-10-19 [MEASURED on air 2026-10-19]
`EVMGRANT e2e` field 1 (arr2enq), us. "real" = probe=0; probe grants (layout probes) are listed separately because they leave the same pipeline later.

| cell | grants | real p50 / p90 / p99 | probe p50 / p90 / p99 | all |
|---|---|---|---|---|
| TIM | 116198 (56483 real) | 503 / 784 / 1337 | 695 / 1025 / 1583 | 591 / 951 / 1476 |
| V | 56335 (38344 real) | 743 / 1868 / 3722 | 1025 / 1812 / 3521 | 834 / 1835 / 3666 |

The real-grant p50 reproduces the 503 / 741 us in the brief.

### 1.2 Per-stage, p50 / p90 / p99 us [MEASURED on air 2026-10-18 evening, `a2s/a2p/a2x`, real grants only]
Two runs per cell (largest real-grant counts); stages from `a2s=` (stamps in `nr_pdcch_blind_monitor_rt.c`, FIELDS.md section 1), pass split from `a2p=`, solver split from `a2x=`.

| stage | TIM eTC2 (n=39987) | TIM kTZ2 (n=38216) | V gVY1 (n=19807) | V eVC1 (n=31484) |
|---|---|---|---|---|
| arr2enq (total) | 518 / 804 / 1264 | 451 / 688 / 1055 | 525 / 1044 / 2258 | 533 / 927 / 1735 |
| a slot arrival -> scan occasion start | 65 / 171 / 475 | 64 / 163 / 459 | 61 / 298 / 1376 | 64 / 289 / 1003 |
| b occasion -> this pass start | 1 / 52 / 340 | 2 / 13 / 239 | 2 / 8 / 447 | 2 / 99 / 368 |
| c pass: FEP/LLR, sweeps, pre-pass, decode | 249 / 374 / 599 | 189 / 324 / 484 | 234 / 425 / 910 | 222 / 436 / 861 |
| d wait for Phase-2 lock | 0 / 0 / 236 | 0 / 0 / 121 | 0 / 0 / 338 | 0 / 0 / 304 |
| e Phase-2 start -> candidate | 0 / 135 / 299 | 0 / 51 / 238 | 0 / 166 / 480 | 0 / 13 / 261 |
| f candidate -> accept | 0 / 1 / 2 | 1 / 2 / 3 | 1 / 2 / 3 | 1 / 2 / 3 |
| g accept -> enqueue | 125 / 156 / 196 | 131 / 175 / 208 | 160 / 199 / 271 | 178 / 234 / 311 |
| inside g: accept -> solver start | 21 / 94 (readings p50=94) | 25 / 96 | 30 / 126 | 33 / 142 |
| inside c: FEP + LLR (a2p) | 76 / 87 / 101 | 78 / 86 / 101 | 75 / 87 / 104 | 74 / 83 / 93 |
| inside c: demap | 3 / 5 / 6 | 3 / 4 / 5 | 3 / 5 / 6 | 3 / 5 / 6 |
| inside c: DL length sweep | 15 / 22 / 42 | 14 / 19 / 94 | 19 / 38 / 177 | 18 / 36 / 170 |
| inside c: UL sweep | 15 / 36 / 280 | 11 / 25 / 228 | 21 / 113 / 585 | 18 / 128 / 492 |
| inside c: pre-pass | 9 / 17 / 33 | 8 / 15 / 33 | 12 / 23 / 39 | 11 / 27 / 53 |
| inside c: polar decode of all pass candidates | 95 / 213 / 260 | 49 / 172 / 246 | 76 / 186 / 282 | 69 / 216 / 294 |

Reading (p50 sum a+c+g: TIM 440 / 451 us, V 457 / 460 us):
- Four serial blocks: wake 61-65 | FEP+LLR 74-78 | decode of every candidate of the pass 49-95 (+ sweeps/pre-pass 35-50) | accept -> enqueue 125-178. The solver reading pass alone (94-142 us) is as large as the whole polar stage.
- The first grant of a slot already waits for the decode of EVERY candidate of the pass (Phase 1 decodes all tasks, then Phase 2 walks them, `nr_pdcch_blind_monitor_rt.c:8688-8760`). [READ FROM CODE]
- Phase 2 is serial: the n-th grant of a slot pays another accept->enqueue. By rank in slot (10-19, ALL grants of consecutive EVMGRANT lines of the same slot, probes included, arr2enq medians): TIM 524, 635, 700, 706, 723 us for ranks 0..4; V 755, 991, 1088, 1098, 1103 us. [MEASURED on air 2026-10-19]

### 1.3 What grows in daytime [MEASURED on air 2026-10-19 unless stated]
- V p50 743 us (10-19) vs 525-533 us (10-18 evening): the difference is queueing in front of the scan, not the work of the pass. V SCAN_E2E wait p90 <= 700 us, p99 <= 2350 us (TIM p90 <= 200, p99 <= 500); V SCANTIME_FINE p50 452 us with 41 % of occasions over the 500 us slot (95609 / 232000), TIM 33 % (64735 / 196000).
- V runs 3.2 passes per occasion (770804 BTIM passes / ~244000 occasions) vs TIM 1.6 (323518 / 204000): per-occasion scan work is TIM 254 us x 1.6 = 404 us, V 170 x 3.2 = 545 us mean (BTIM TOTAL), i.e. the two V consumers run near saturation. [INFERRED from BTIM TOTAL means]
- Grant rate (5 s windows, 10-19): TIM low-third 480 grants/s p50 458 / p90 656; high-third 1274 grants/s p50 504 / p90 831. V low-third 196 grants/s p50 641 / p90 1149; high-third 805 grants/s p50 888 / p90 1980 (corr(grants/s, p50) 0.54 TIM, 0.45 V; 27 / 24 windows). p90 grows 2-3x faster than p50: it is the queue.
- Polar decode per pass grows with time and traffic on TIM: incremental BTIM mean per pass 37 -> 48 -> 74 -> 101 -> 108 -> 120 us over the run (FEP 41 -> 68, demap/pre-pass flat). V: 37, 53, 42, 37, 36, 26 us (flat). The dedicated-RNTI set is "seen twice within 10000 << mu slots = 10 s" (`nr_pdcch_blind_rnti_bootstrap.c:50-55`), TIM keeps 12 (p50) to 22 (max) distinct RNTIs in a 10 s window, V 21 (p50) to 49 (max). The decode count is therefore proportional to the live RNTI set and to the UL scan, not to the grants. [READ FROM CODE + MEASURED]
- Decodes per pass, from the summary counters (cumulative): TIM 6.2 DL candidates + 27.9 UL (0_1) decodes per pass; V 3.4 DL + 8.0 UL (`candidates=`, `ulscan[sched=]`, `dci01[rejects=]`; 323518 / ~771000 passes). So 70-82 % of the polar decodes in a pass are the anonymous UL DCI 0_1 scan and sit in front of the DL grant. [INFERRED from counters]
- Bank effect (V eVC1, 10-18): grants from CORESET bank 1 p50 550 vs bank 0 528, p90 1013 vs 888 (they wait for bank 0's pass).

## 2. Two lanes (question 2)

### 2.1 What is not needed on the critical path of a known RNTI's DCI
| work in front of the grant today | needed before enqueue? | evidence |
|---|---|---|
| UL DCI 0_1 anonymous scan (28 decodes/pass TIM, 8 V) | no (DL grant does not read it) | counters above; UL after DL per goals-20261017 |
| DL length sweep (15-19 us), UL sweep (11-21), pre-pass (8-12) | no for a RNTI whose length is locked | a2p |
| open-RNTI decodes (unknown RNTI per distinct length), joint/anon sweep (`janon`, async already) | no | READ FROM CODE 7984-7992; janon async worker |
| other banks' passes (bank 1 waits for bank 0) | no | section 1.3 |
| post-accept bookkeeping | already deferred by FIX-B' | FIX-B report |
| solver reading pass (94-142 us) | yes today (it chooses which jobs exist) | DL-A2E 2026-10-18 item 3 |

### 2.2 Fast lane content and size
Fast lane A = per CORESET: FEP/LLR (once, `ISAC_PDCCH_FEP_CACHE` already shares it across banks of a slot) -> occupancy screen of all positions -> decode of the lit positions for (a) ready RNTIs whose USS hash allows the position, (b) the common search space candidates (CORESET#0 / SI / RA / P, a handful per SSB-associated slot), nothing else.
- Candidate geometry [READ FROM CODE + MEASURED on air 2026-10-19]: TIM N_CCE 36, USS hash fitted `M=[5 1 3 3 0]` (AL1/2/4/8/16) = 12 hash positions per RNTI per slot; V N_CCE 33, `M=[0 4 1 0 0]` = 5 per RNTI. The ladder has 67 positions (36 AL1 + 18 AL2 + 9 AL4 + 4 AL8).
- Ready set R: distinct real-grant RNTIs per window, p50 / p90 / max: TIM 100 ms 3 / 7 / 12, 1 s 6 / 10 / 16, 10 s 12 / 19 / 22; V 100 ms 3 / 8 / 16, 1 s 6 / 18 / 29, 10 s 21 / 41 / 49. Busy slots: 18.0 % (TIM) / 13.6 % (V) of slots carry a real DL grant, 1.13 / 1.12 distinct RNTIs per busy slot.
- Without a screen the hash pairs are R x M = TIM 72-228, V 30-245 per slot: too many (x 0.24-2.3 us). With the screen the decodes are (lit positions) x (RNTIs the hash allows at that position, ~R x M / positions = 2-4): per busy slot ~8-50, per average slot ~2-10 (idle slots ~0.2 at the 3.09 sigma false-alarm point). [INFERRED]
- Cost of the fast lane polar part with the prototype: 2-10 decodes at 0.24-0.83 us (batched) = 1-8 us per average slot, <= ~40 us for the worst busy slot; with the shipped decoder 6-50 us typical, 150+ us worst. The screen over all 67 positions is 0.80 us (section 4.3).
- Therefore the fast lane compute is dominated by FEP+LLR (76 us) and the accept path, not by candidates: it can hold <= 0.15 ms only if those two move (sections 6, 7).
- Correctness constraints: the discovery lane must still find new/returning RNTIs (open-RNTI decode, joint solver); the fast lane must obey the same Phase-2 serial state (dci_thres EMA, persistence, `accept_dup`). A bounded LRU of ready RNTIs ranked by recency keeps the lane cost bounded without a cell-specific threshold.
- Prior art in this tree: `ISAC_PDCCH_KNOWN_FAST` (csd/int16-kfast 1dc617a351, report KNOWN-FAST-20261017) is a known-RNTI pass before the scan, replay only: arrival-to-enqueue p99 -100..-350 us but tot p99 flat, TIM n_real -10 % / crc_q -3 points unexplained, parked. It did not touch the wake, the FEP or the solver, and its TIM side effect is the solver feedback timing (section 7 risk). [MEASURED, DGX replay, from that report]

## 3. Hand-offs (question 4)

### 3.1 How many today [READ FROM CODE]
arrival stamp = `nrue_ru_read` returns (`nr-ue.c:1747-1760`); then
H1 RX thread -> DL actor (NotifiedFIFO `dl_actors`, `nr-ue.c:2436`), the actor runs `UE_dl_processing` (MAC slot indication, PBCH, OAI pdcch_processing, `nr_pdcch_blind_monitor_process`);
H2 DL actor -> scan consumer (`nr_pdcch_passive_queue_enqueue`, mutex + condvar, `nr_pdcch_blind_monitor_rt.c:5012`);
H3 consumer -> bank-1 helper thread (`ISAC_PDCCH_BANK_HANDOFF=1`, banks >= 1 only);
H4 scan consumer -> PDSCH consumer (after arr2enq, in qwait: p50 14-16 us).
Two (three with banks) hand-offs sit inside arr2enq. The arrival stamp itself includes the RX thread's wake from the ring condvar (`usrp_lib.cpp:2230-2245`).

### 3.2 Cost per hand-off [MEASURED bench, DGX]
`handoff_bench` producer cpu 1, consumer cpu 2 (both Cortex-A725, non-isolated), 500 us cadence, 12000 iterations, SCHED_OTHER (production consumers run at priority 50, so absolute numbers are a worst case):

| variant | p50 | p90 | p99 | max | consumer CPU |
|---|---|---|---|---|---|
| cond (mutex+condvar, signal after unlock = queue.c) | 100.7 | 104.2 | 170.0 | 762.8 | 0 |
| futex | 98.3 | 101.6 | 133.9 | 412.4 | 0 |
| eventfd | 99.5 | 103.1 | 169.2 | 458.5 | 0 |
| yield-spin up to 300 us, then cond | 78.8 | 103.7 | 158.7 | 439.4 | 60 % of a core |
| busy spin (arm `yield`) | 0.2 | 0.2 | 0.2 | 11.0 | 100 % |
| hybrid: timer 40 us before the known next slot, then spin | 0.2 | 0.2 | 0.2 | 220.0 | 8 % |
| hybrid: timer 15 us before, then spin | 0.2 | 0.2 | 0.2 | 302.4 | 3 % |
| cond, producer and consumer on the same cpu | 2.6 | 2.7 | 3.2 | 2582 | 0 |
| timer wake lateness, default 50 us timer slack | 52.8 | 53.1 | 58.3 | 1718 | - |
| timer wake lateness, `PR_SET_TIMERSLACK` 1 ns | 2.8 | 3.1 | 7.8 | 2446 | - |

- The ~100 us is the cost of waking an idle core, independent of the primitive (FIX-B' measured 98-104 us; Grace LPI exit latencies 0 / 42 / 231 / 433 us). Production sees ~60 us for H1+H2 together because cores are busier than in the bench. A wake onto the core of the (still running) producer costs 2.6 us. A thread that sleeps on a timer with the default 50 us slack adds 53 us by itself: any "sleep until just before the slot" scheme must set `PR_SET_TIMERSLACK` to 1 ns (3 us p50, 8 us p99).
- Slot cadence is exact (500 us at 30 kHz, 1 ms / 2^mu in general), so a hybrid wait costs 3-8 % of one core and no idle exit. Busy polling costs a whole core per consumer: rejected ("fewest cores").

### 3.3 Can the fast lane run inline on the RX thread?
- As today (after the read returns): no. BTIM `rt` already costs the RX thread ~20 us per slot (mean, TIM), and the code comment at `nr_pdcch_blind_monitor_rt.c:4966-4973` records PBCH lock loss above ~18 % RX duty; FEP+LLR alone (76 us) is 15 % of a 500 us slot.
- Inside the read wait (before the end of the slot): yes. The RX thread blocks on the radio ring (`ring_cv.wait_for`, `usrp_lib.cpp:2230`) for ~430 us per slot after the CORESET symbols are already there. Work done there does not delay the next read as long as it finishes before the slot's last samples are in the ring; a deadline check between stages (skip the lane when late) keeps the existing "ring overrun" fatal out of reach. [INFERRED; not prototyped]

## 4. Polar (question 3), all [MEASURED bench, DGX], cores 0-4 (A725), `results/polar_*.txt`

Setup: standalone link of the real `openair1/PHY/CODING/nrPolar_tools` objects (g++/gcc -O3 -mcpu=native), shapes seen on air: DCI 1_1 length 44 (UL 0_1 on air), 47 (TIM), 49 (V) x AL 1/2/4/8, 256 random payloads with RNTI 0x591c, QPSK soft bits amplitude 20 (the production range, +-31 clip), 40 repetitions, input memcpy of the reference subtracted.

### 4.1 Per-decode time and decode success
| len | AL | N / E | ref us | fast-SSC us | 8-lane batch us per decode | ok of 2000 at sigma/amp 0.6 / 0.9 / 1.2: ref / fast-SSC / batch |
|---|---|---|---|---|---|---|
| 47 | 1 | 128 / 108 | 2.83 | 0.89 | 0.25 | 1900/1900/1900, 138/141/141, 2/2/2 |
| 47 | 2 | 256 / 216 | 3.53 | 1.26 | 0.41 | 2000/2000/2000, 1957/1957/1957, 787/782/782 |
| 47 | 4 | 512 / 432 | 4.59 | 2.03 | 0.76 | 2000/2000/2000, 2000/2000/2000, 1994/1994/1994 |
| 47 | 8 | 512 / 864 | 4.98 | 2.35 | 0.82 | 2000/2000/2000 (all three) |
| 49 | 1 | 128 / 108 | 2.88 | 0.87 | 0.25 | 1865/1863/1863, 102/100/100, 0/0/0 |
| 49 | 2 | 256 / 216 | 3.54 | 1.30 | 0.43 | 2000/2000/2000, 1940/1941/1941, 686/684/684 |
| 49 | 4 | 512 / 432 | 4.83 | 2.04 | 0.77 | 2000/2000/2000, 2000/2000/2000, 1989/1989/1989 |
| 44 | 1 | 128 / 108 | 2.78 | 0.82 | 0.24 | 1945/1944/1944, 269/276/276, 1/1/1 |
| 44 | 4 | 512 / 432 | 4.48 | 2.02 | 0.75 | 2000/2000/2000, 2000/2000/2000, 1998/1998/1998 |

(12 shapes in `results/polar_bench_amp20.txt`; the second run at amplitude 200 reproduces the timings within 1 %.) Speed-up single 2.1-3.5x, batch 5-12x. Decode success is equal within +-0.3 %: the fast-SSC nodes (rate-0, rate-1, repetition, SPC) are ML on their subtrees, so decisions are not bit-identical to min-sum SC, only equal in performance; no list decoder is needed.
Prototype breakdown for one candidate (AL4 N=512): dematch 0.22 | tree 0.85 | polar transform 0.89 | info extract + CRC 0.18 us. The byte-wise inverse transform is half of it and would shrink to ~0.1 us with 64-bit packed butterflies (not implemented): single candidate ~1.3 us at AL4. Soft bits are bounded +-31, so an int8 saturating tree (16 lanes) is possible (not measured; est. another 1.5-2x).
The existing `nr_polar_params()` lookup (mutex + free list) costs 0.019 us per call: not a factor.

### 4.2 Early termination / pruning
- CRC-aided pruning or an RNTI-mask pre-check before the decode: not available. The 24 CRC bits are interleaved over the information set (`interleaving_pattern`), so the CRC is only known after the last SC leaf; there is no earlier hook. The useful early exit is BEFORE the decode: skip candidates whose soft bits are noise-like. For a known RNTI the CRC test is already exact (residual must equal the RNTI).
- List decoding: not needed (SC at equal success above); would cost ~L x.
- Screen cost (section 4.3).

### 4.3 Candidate screen over the whole ladder (67 positions, 15552 soft bits), `results/screen_bench.txt`
| method | us |
|---|---|
| scalar per candidate (what runs in the frozen config; `ISAC_PDCCH_NEON_SCORE` unset) | 5.85 |
| shipped NEON per candidate | 6.87 (slower: horizontal adds per step) |
| prototype: per-CCE (sum|x|, sum x^2) once, every AL by prefix sums | 0.80, same score to the bit |
The score (sum|x|)^2 / (n sum x^2) is additive over CCEs, so one pass over 36 CCEs gives all AL candidates.

### 4.4 Reference decoder hazard (found while validating)
`results/polar_ref_saturation.txt`: clean (high-SNR) vectors, shipped `polar_decoder_int16`, RNTI and payload exact: AL4 12/12 at soft-bit amplitude 4, 20, 60; 11/12 at 200; 0/12 at 2000. AL8 12/12 up to 60; 0/12 from 200. Fast-SSC decodes all of them. Cause [INFERRED]: the saturating G in int16 can reach -32768, and `abs_epi16(-32768)` stays negative in the min-sum F, flipping signs; N=512 trees reach it first. Production is safe today because `nr_pdcch_blind_llr_autoscale=1` puts the 90th percentile at half the +-31 rail and `nr_pdcch_llr()` clips [READ FROM CODE `dci_nr.c:191,1008-1057`]. The prototype saturates symmetrically (+-32767) and does not have the hazard.

### 4.5 GPU persistent kernel and batched GPU fast-SSC, GB10 sm_121, `results/gpu_bench.txt`
Same node program on a warp per candidate (shared memory, device recursion, no per-cell code); u bytes bit-exact with the CPU prototype for all 128 test vectors per shape.
| batch / mode | AL1 (N128) | AL2 (N256) | AL4 (N512) | AL8 (N512) |
|---|---|---|---|---|
| kernel (event), 1 / 8 / 32 candidates | 30.8 / 31.9 / 31.9 | 34.2 / 36.0 / 36.1 | 48.1 / 48.4 / 48.3 | 48.3 / 48.4 / 48.4 |
| kernel, 128 candidates | 38.1 | 44.2 | 58.6 | 58.6 |
| launch + exec + sync wall p50, 1 candidate | 35.6 | 39.0 | 52.9 | 53.1 |
| persistent kernel round trip (LLR written -> all u back) p50, 1 / 8 / 32 cand | 32.6 / 39.0 / 61.5 | 40.2 / 46.0 / 68.7 | 59.3 / 64.4 / 87.6 | 62.7 / 67.4 / 94.3 |
- The persistent kernel removes the launch (35 -> 33 us at AL1) but the serial tree depth stays: ~30-48 us per decode wave for every batch size up to 128. The earlier GPU SC (SCAN2 s5: 250-330 us) was ~7x worse because its chain ran in global memory; a fast-SSC in shared memory gets to 31-48 us, still 15-130x the CPU time of the same few decodes.
- Break-even: GPU wins only above ~80-150 decodes per call; the screened scan has 5-30 (section 2.2).
- Still open for any GPU PDCCH: the LLR sign parity of the GPU chest (`nr_pdcch_gpu_fep.h` STATUS, GPU4.md) and PDSCH contention. Persistent spin-polling kernels also occupy SMs next to the RT green context.

## 5. FEP (question 5)

- PDCCH FEP: CPU `nr_slot_fep` per CORESET symbol on the scan thread, cached per (slot, symbol, FO) on that thread (`ISAC_PDCCH_FEP_CACHE`, `nr_pdcch_blind_monitor_rt.c:6336-6377`). The GPU alternative `ISAC_PDCCH_GPU_EXACT_FEP` exists but is off in the frozen env (`pdcch_gpu=0 pdcch_cpu=0` in both logs). [READ FROM CODE + MEASURED on air 2026-10-19]
- PDSCH FEP: one GPU exact-FEP call per grant on its own symbol ranges (start symbol .. +nsym plus the DM-RS symbols; or the whole slot when the slot share is on), `nr_pdsch_passive_decode.c:3355-3410`; calls=62000 / 38000 in the two logs, wall mean 71 / 100 us, p50 <= 64 us, p99 <= 576 / 896 us. The CORESET is 1 symbol (symbol 0) on both cells, outside those ranges: the work is NOT shared and NOT duplicated either (different symbols).
- A single GPU slot-FEP feeding both cannot lower PDCCH latency: its p50 (<= 64 us wall, which includes launch and sync wake) is no better than the CPU FEP+LLR (76 us for 4 antennas incl. LLR; dft4096 alone is 25.8 us per antenna on A725, 103 us for 4, `results/handoff_bench_cpu1_cpu2.txt`), the PDSCH data symbols are not available before the slot ends, and the GPU LLR parity problem remains. Its only benefit would be CPU time (~13 % of a core on TIM: BTIM fep_llr 55.7 us mean x ~2350 passes/s), traded for GPU round-trip waits.
- Measured production fep_llr (74-78 us) is lower than 4 x dft4096 on A725 (103 us), so the scan consumers presumably run on the Cortex-X925 cores [INFERRED]; on A725 the 4 FFTs alone would exceed the 76 us.

## 6. Options ranked by expected arr2enq p50

Baselines: TIM 503, V 743 us (10-19 real grants). "Estimate" columns are [INFERRED] from the measured stage medians of section 1.2 and the benches; only the fast-SSC and hand-off rows rest on direct measurements of the replaced part.

| # | option | est. arr2enq p50 TIM / V (us) | cores (net) | risk | effort |
|---|---|---|---|---|---|
| 1 | early start of the fast lane on the RX thread inside the read wait (S1+S2+S3+S4 below), PDSCH job released at arrival | <= 0 / <= 0 (grant ready 0.1-0.3 ms before the slot ends); effective DL latency -0.4..-0.5 ms | -0.1..-0.3 | high: RX loop and ring read split, deadline guard, TDD/numerology generality | L, 2-3 weeks |
| 2 | early start of the existing scan consumer (no lane split, hybrid wake) | ~70 / ~310 | +0.03..0.08 | medium | M, 3-5 days |
| 3 | two lanes (DL known + CSS first, discovery and UL after), fast-SSC, hybrid wake, parse cache; no early start | ~200 / ~250 | -0.1..-0.3 | medium: Phase-2 ordering and solver feedback timing (TIM n_real shift seen with KNOWN_FAST) | M, 1-2 weeks |
| 4 | DL-first ordering only (S2) | ~410 / ~600 | 0 | low-medium | S, 2-4 days |
| 5 | fast-SSC polar only, batched (S1) | ~440 / ~700 | -0.15..-0.3 | low (bit-exactness not required; success equal) | S, 2-3 days |
| 6 | hybrid timer+spin wake only (S3) | ~450 / ~690 | +0.03..0.08 | low (timer slack fix is mandatory) | S, 1-2 days |
| 7 | CPU-only to <= 0.15 ms: option 3 + 4-way antenna FEP + provisional enqueue before the solver readings | ~105 / ~150 | +2..3 | high: solver feedback timing changes n_real/crc_q on TIM | L |
| 8 | GPU persistent / batched polar | worse: +30..60 us on the path | + GPU | high; parity unsolved | L |
| - | GPU PDCCH scan (csd/int16-ppdcch 9354a94692, SCAN2 s5) | slower than CPU, not merged | + GPU | - | - |

Derivations (TIM / V, us):
- Option 3: wake 65 -> 8; pass 249 / 234 -> FEP 76 + demap 3 + screen 1 + decode <= 10 = 90 (sweeps, UL scan, pre-pass moved behind the enqueue); accept -> solver 21-30; readings 94 / 126 -> 70 / 100 with the parse cache (bit-exact, -20..35 us, DL-A2E); -> 8 + 90 + 25 + 70 + 5 = ~200 and 8 + 90 + 30 + 100 + 5 = ~235 (+ headroom for V's bank-1 wait). The daytime V queue term (wait p90 700 us) is removed because the lane runs ahead of the discovery backlog.
- Option 1/2: the trigger is the moment the CORESET symbols (max over active banks of first symbol + duration, plus half a CP) are in the ring: ~36-110 us after slot start, i.e. ~390-460 us before the read that is stamped arrival. Option 2 keeps today's stage time (503 / 743 us at 10-19) and subtracts the ~430 us lead: ~70 / ~310. Option 1 also shrinks the stage time itself (option 3: ~200 / ~250), so the grant is ready ~230 / ~180 us before arrival.
- Option 7: FEP 76 -> ~25 on four threads, readings 70 -> 25 by enqueueing with the RNTI's last confirmed layout and correcting afterwards: 8 + 45 + 25 + 25 + 5 = ~105 (TIM).

What cannot be removed on a CPU-only path: FEP+LLR (>= 65-76 us unless parallelised) + the DCI 1_1 solver readings (70-140 us). That is why <= 0.15 ms at p50 is only reachable by moving the work earlier in time (options 1-2) or by spending cores on FEP and re-timing the solver (option 7).

## 7. Recommended design: PDCCH-FL (fast lane, early start, inline on the RX thread)

Stages, each independently shippable, each gated by its own on-air A/B (rules of goals-20261017: daytime, n=3 per cell on V PCI 415 and TIM PCI 923, CRC_q >= 80 %, drops < 1 %, p99 < 3 ms):

S1 Fast-SSC batched polar, drop-in for `blind_polar_decode`/`polar_decoder_int16` on DCI shapes (A <= 64 first, then all A <= 140): node program built from `information_bit_pattern` at `nr_polar_params` time; single and 8-lane entries; same return contract (CRC residual = RNTI, payload). Gain ~-40..-80 us p50 of the pass and 25-60 % of the scan CPU. Files: `nr_polar_decoder.c` (new entry), `nr_pdcch_blind_monitor.c:4198` (call site), prototype in `tests/pdcch_redesign/polar_fssc.h`.
S2 DL-first ordering inside `run_occasion_one`: tasks are already built in order (`cand_task[]`); split Phase 1/Phase 2 into DL known + CSS, then everything else. The occupancy screen (per-CCE prefix sums, 0.8 us) replaces the per-candidate scalar score. Enqueue the DL grants before the UL scan, sweeps and open-RNTI decodes. Expected -90..-140 us. Risk: Phase-2 state order (dci_thres EMA, persistence, `accept_dup`) must be preserved per lane; golden tests like `Dci11ObserveGolden` of FIX-B'.
S3 Hybrid wake for the lane's thread: `PR_SET_TIMERSLACK` 1 ns, timer to (known next trigger - 15..40 us), bounded spin to the trigger; cadence from numerology. +3..8 % of one core, -55 us.
S4 Early start: split the slot read in `nr-ue.c:1745` into `[0, trigger)` and the rest (the loop already supports multi-read slots: short-read retries and the end-of-frame `first_symbols` read, accumulated in `rx_samples_consumed`); after the first part is converted into `rxdata`, run lane A inline (RX thread is otherwise blocked on `ring_cv` for ~430 us). Trigger sample = end of the last CORESET symbol of any active bank + half a CP, derived per slot from the CORESET config the monitor already holds (`cfg->ss_first_symbol`, `duration`), not from a constant. Hold the PDSCH job in a per-slot slot-ready list released by the RX thread right after the second read returns (extends `nr_passive_samples_valid` semantics; PDSCH FEP must not start earlier). Deadline guard: lane A aborts and defers to the scan consumer when `now > slot_end - margin`. Report `pdcch_ready_ns - arrival_ns` (negative is the goal) plus the old arr2enq for the A/B.

Cores: S1 saves CPU; S3 adds 3-8 % of a core; S4 moves ~100 us per occupied slot from the scan consumers to the RX thread's idle wait. Net -0.1..-0.3 core. Discovery (UL scan, sweeps, joint/anon, probes) is unchanged and runs behind lane A; UL latency unaffected (UL after DL by plan).

## 8. Test plan

1. Polar unit (offline, bit-level): fast-SSC vs `polar_decoder_int16` on 10^7 vectors per (A in {12..140 sampled, 44, 47, 49}, AL in {1,2,4,8,16}): (a) payload+RNTI agreement on every vector where the reference is correct at amplitude <= 31; (b) success-rate difference within +-0.5 % at three SNR points (prototype: +-0.3 %); (c) amplitude sweep 1..32767 (the reference fails above ~100 at AL4/8, section 4.4); (d) noise-only false-alarm rate equal (2^-16 CRC). Extend `tests/pdcch_redesign/polar_bench.cc`.
2. Screen unit: per-CCE prefix-sum score equals the scalar score on all AL, random and captured LLRs (bit-exact already in `screen_bench`).
3. Golden replay on EVERY capture (tim, b and the rest of tests/passive_rx/captures, read-only; replays via heavy.sh only): decoded DCI set (rnti, slot, CCE, AL) superset of the baseline with <= 0.1 % loss; n_real, crc_q, drops within run-to-run spread (n >= 3 per arm, the KNOWN_FAST TIM n_real shift must be absent). Per-grant `a2s/a2p` before/after.
4. Lane-A/B ordering: unit test of Phase-2 with permuted lane order proving identical state hashes (the FIX-B' `StateHash` pattern), no duplicate (rnti, slot) enqueue, CSS grants never lost.
5. Hand-off regression: `handoff_bench` on the host (same table), on air `SCAN_E2E` `pre_us/q_us`, with the timer slack fix verified by a unit test of the wake lateness.
6. Early start: (a) synthetic ring writer test with a controllable write pointer (gtest) for the split read, timestamps and `rx_samples_consumed`; (b) replay harness that feeds the slot in two chunks; (c) negative tests: lane runs late (deadline abort), TDD special slots, CORESET at symbols 2-3, 15 kHz and 120 kHz SCS, ring recovery (`usrp_rx_recover`) during a split read; (d) on air: `pdcch_ready - arrival` p50/p99, PBCH lock stability for >= 30 min daytime, no "ring overrun".
7. Generality matrix (rfsim and recorded cells): CORESET duration 1/2/3, 6..216+ RB, interleaved/non-interleaved, AL 1..16, M_L = 0..8, N_CCE 6..~150, scrambling id configured vs cell id, n_RNTI = 0 CSS, two operators/PCIs at once.
8. Gate: the FROZEN guard `git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30`; builds only when `pgrep -x nr-uesoftmodem` is empty.

## 9. Generality (question 6): no per-cell tuning

| proposal | derived from | cell-specific input? |
|---|---|---|
| fast-SSC | N, E, K, info set, rate-matching pattern from `nr_polar_params` (TS 38.212 tables, any A <= 140 / AL 1..16) | none; node types are computed from the spec reliability sequence |
| 8-lane batch | groups candidates of equal (A, AL) | none |
| screen | additive per-CCE sums; threshold = 3.09 sigma of the noise null (existing `ISAC_PDCCH_KNOWN_PRESCREEN` rule) | none; null mean 2/pi is a property of QPSK noise |
| USS hash positions | TS 38.213 hash with N_CCE and M_L fitted from the cell's own accepted DCIs (`ussh_fit`); no fit -> all CCE candidates (`ussh_allows` true) | none; fitted per CORESET geometry at run time (TIM N=36 M=[5 1 3 3], V N=33 M=[0 4 1], both fitted, not set) |
| CSS candidates | CORESET#0 / SearchSpace parameters from MIB/SIB1 | from the cell's broadcast, as today |
| ready-RNTI LRU | recency of accepted DCIs, size bounded by a per-slot decode budget | budget in decodes, not in RNTIs or ms of a cell |
| hybrid wake | slot period 1 ms / 2^mu | numerology only |
| early start | last CORESET symbol of the active banks (config the monitor already has) + half CP | none; TDD slot types from `proc->rx_slot_type` |
| nID | CORESET scrambling id / cell id as today; open-RNTI path for CSS | unchanged |

Not generic and therefore not proposed: any threshold tuned on TIM or V (e.g. a fixed RNTI age, a fixed AL list), the RNTI layout cache (parked, cost TIM grants).

## 10. Not measured / risks

- Nothing was run on a radio or replayed (forbidden). All "on air" rows are existing logs; every estimate in section 6 is arithmetic.
- LLR generation cost (the second half of `fep_llr`) was not reproduced standalone; the lane's FEP+LLR (76 us) comes from the logs. The 10-19 logs have no a2s/a2p, so the per-stage table is from the 10-18 evening arms (different env arms).
- The hand-off bench uses SCHED_OTHER consumers on cores 1-2 only (rules: non-isolated cores); production consumers are priority 50 on other cores. X925 timing of fast-SSC and dft4096 was not measured (cores 5-9 are out of the bench set); the fast-SSC speed-ups are ratios on the same core.
- The decode counts in section 2.2 are estimates from hash geometry, RNTI-window statistics and counters; no per-pass count of lit candidates exists in the logs.
- Early start depends on how `rxdata` is filled in `ring_direct` mode (conversion at read time, `usrp_lib.cpp:2474-2497`) and on `nr_slot_fep` windowing near the split; not prototyped. The RX thread is the most sensitive thread in the receiver (PBCH lock at ~18 % duty): the deadline guard and an A/B on lock stability are mandatory.
- Risk in S2/S3: the solver feedback timing (KNOWN_FAST changed TIM n_real by -10 %); the lane order must keep the per-RNTI sequence of solver notes identical.
- `polar_decoder_int16` hazard in section 4.4 is a finding, not a fix; it should get a unit test whenever LLR scaling changes.

## 11. Reproduce

```
tests/pdcch_redesign/build.sh <outdir>          # polar_bench, handoff_bench (gcc/g++, no cmake)
nvcc -O3 -arch=sm_121 -o <outdir>/gpu_bench tests/pdcch_redesign/gpu_bench.cu
g++ -O3 -mcpu=native -Iopenair1 tests/pdcch_redesign/screen_bench.cc -o <outdir>/screen_bench
<outdir>/polar_bench dump <dir>                 # code tables + vectors for gpu_bench
# each run: CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps \
#   /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh taskset -c 0-4 nice -n 10 <bench> < /dev/null
# AMP=20 polar_bench (default) | polar_bench breakdown | polar_bench debug <AL> <amp> | handoff_bench 1 2 | gpu_bench <dir>
```
Log analysis scripts (not committed): EVMGRANT parsing of `e2e=`, `a2s=`, `a2p=`, `a2x=`; percentiles over `probe=0`.
