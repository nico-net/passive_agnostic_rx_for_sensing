# UL-REALDEC-20261018: why real-capture UL never decodes, and the UL latency

Branch csd/int16-ulrfsim (from dc8bc5efd8), not merged. Labels: [MEASURED, DGX replay, capture, n] unless stated; cells never pooled.
Replays: p99_replay.sh, t26gE.env + sd_t26ul.cfg, TMO=90, SPEED=1, build_t26. On-air evidence: the coordinator's night runs
(ab_nuT1_1_023857 = TIM, ab_nuV1_1_024431 = Vodafone, 300 s each, binary dc8bc5efd8, t26uE env) [MEASURED, DGX OTA, night low load, n=1/cell].

## Outcome
- UL CRC goal: **not met**. 0 genuine UL CRC passes on b and tim, before and after this work.
- The "contrary evidence" (0x5186 at TA ~0, 10-17 dB, PCI DM-RS) is an **artifact**: it is the gNB's own PDSCH DM-RS in DL slots.
  So the earlier "SNR-starved" conclusion **stands** for the UL slots of both 12 s captures.
- One real timing defect found and fixed behind a flag (`ISAC_UL_TA_TRACK`): on air (TIM) UEs arrive about 480 samples (> CP) off the
  nominal advance, but about 90 % of their grants were decoded at the nominal 1600.
- UL latency: `ISAC_UL_BGDISC=1` (existing flag) cuts UL tot p50/p99 from ~13-49 / 80-133 ms to ~2.2-2.8 / 5.6-8.5 ms. The < 3 ms p99
  goal is still missed: the floor is ~1 ms of CPU per decode call, at ~1.3 calls per grant.

## 1. Hypothesis (1): per-UE timing (TA). Confirmed as a defect, but not the b blocker
- **Real-time decode.** The decode does search ±2048 samples per grant (nr_pusch_passive_decode.c, `timing_disc`; on because
  ISAC_UL_PUSCH_CHAIN=1). It applies the result only when *that grant's* search passes score >= 0.35. Otherwise it decodes at
  N_TA_offset = 1600.
  - The per-RNTI tracker existed but was never used as the fallback.
  - It was also wiped by any decode of an equal or older slot (2 consumers, CRC-first trials of the same slot) and by 1 s of silence.
  - [b, dc8bc5efd8 run nidr_b_on12, n=1]: 601/663 0x5186 real grants decoded at ta=1600.
- **U4 stage B.** It passes `ta_nom` (1600). Its decodes run the same per-grant search with the same fallback, so they mostly decoded
  at 1600 too.
- **Where the "TA ~0" came from.**

  | slot (mod 20) | slot type | UL_TIMING_DISC acceptances | residual |
  |---|---|---|---|
  | 0, 10 | DL | 246 | ~-1600 |
  | 9 | UL | 14 | ~0 (UE at the nominal advance) |

  [b, all 22 earlier nidr/nid replay logs, 260 acceptances].
  - Slot 10 is DL: SIB1 (0xffff, 1_0) is sent there.
  - UL DCIs are in slots 6/7/16/17, so UL slots are 8/9/18/19 (DDDDDDDSUU).
  - The 10-17 dB hits come from CRC-first k2+1 trials and look-alike BWP trials that land in DL slots 0/10.
  - PDSCH and PUSCH DM-RS share the same PCI-seeded gold sequence (38.211 7.4.1.1.1 / 6.4.1.1.1). The found mask 0x804 = symbols 2+11
    is the cell's PDSCH DM-RS pattern.
  - So "advance 0" is the DL frame timing, not a UE.
- **On air TIM [ab_nuT1].** Every acceptance is in UL slots 4/5/14/15, with residual +416..+576 samples (0x46fe, 0x4c1e; 20
  acceptances). Yet 4325/4943 and 2587/3067 of the top RNTIs' grants were decoded at 1600. 480 samples > CP (288), so those
  decodes are outside the cyclic prefix. This is the genuine defect `ISAC_UL_TA_TRACK` fixes.
- **12 s tim capture.** 0 timing acceptances in 90 s; forcing nominal 2080 changed nothing (0x4e32 SNR median -17.1 vs -17.7 dB)
  [tim, n=1 each]. The capture's UEs are below the detection floor at any timing.

## 2. Hypothesis (2): UL BWP 217@0 vs 214/215. Not resolvable by CRC, and not the blocker
Arms on b (diagnostics, never defaults):

| arm | result |
|---|---|
| k2+1 | 0x5186 SNR median -12.1 -> -4.8 dB, 37 grants >= 5 dB |
| configured BWP 214@0 | no better (-6.3 dB median) |
| forced ids 415:415 | 0 CRC |
| `ISAC_UL_LAYOUT_BRUTE` on 7 grants at 10-14 dB (BWP twins N-4..N+1 x table 0/1/2 x CDM 1/2 x TDA S0L14/S0L13(/S1L13/S0L12) x map A/B x dmrs-AddPos 0/1/2 x ids {415, 249}; 756-1728 decodes/grant) | **0 CRC, 0 segments** |
| NID_BG stage A (1024 ids, CB0) on a 13.1 dB QPSK grant | 0 |
| NID_BG stage B (MCS<=28, SNR>=5) | no hypothesis >= 5 dB |

[b, n=1 per arm]. The high-SNR grants are all in DL slots (above), so CRC could never pass there. The BWP question (217 vs
214/215) stays open. No grant in a real UL slot reaches 5 dB.

## 3. Hypothesis (3): DM-RS config, PTRS, TP, 2-layer, nSCID, CFO
- Not tested in detail.
- **2-layer:** grants with a non-zero precoding code point are refused at extraction (nr_pdcch_blind_monitor.c:5785), so 2-layer
  grants are not decoded at all.
- **CFO:** the replay retunes digitally (RAW_IQ_DIGITAL_TUNE +181 Hz), and fo_hz=0 is passed to UL. Not a suspect: the DL decodes
  at 96 % with the same IQ.

## Code (commit 77b25dc08c + follow-up)
`ISAC_UL_TA_TRACK=1` (default off), in nr_pusch_ul_disc.c and nr_pusch_passive_decode.c:
- A grant whose own search fails decodes at the RNTI's tracked advance. A never-timed RNTI uses the cell's modal residual
  (±64, last 16 acceptances).
- An old or equal slot no longer wipes the track. A stale track (> 2000 slots) is still the seed, searched ±2048.
- CRC-first trials do not train the tracker, inline or in the BGDISC worker.
- With BGDISC, a grant without a cached hypothesis decodes at the learned advance.
- Snapshot FEP clamps an advance in (-CP, 0) to 0 instead of "timing outside bounded UL snapshot" (323 such refusals [b, k2+1 arm]).

`ISAC_UL_LAYOUT_BRUTE=<snr>` (+ `ISAC_UL_LAYOUT_BRUTE_IDS=a,b`): a diagnostic CRC oracle on <= 3 failed grants. It is the tool to
run on air once a UL-slot grant shows >= 10 dB.

Tests:
- New test `PuschUlDisc.TaTrackKeepsAdvanceAcrossOldSlotsSilenceAndSeedsNewRntis`; test_nr_pusch_ul_disc 12/12 [UNIT, DGX, build_t26].
- Full ctest not run.

Removed again: a K2_DELTA diagnostic (its "gain" was the DL-slot artifact).

## UL latency: where UL_E2E time goes
| setting | where the time goes | UL tot p50/p99 (us) |
|---|---|---|
| without BGDISC (UTIM, b, n=1) | per decode call, the FEP stage (inline timing search + 64-id blind DM-RS scan) is 2455 us mean / 28 ms max; rx_pusch 669 us; LDPC 215 us. Wide grants (>= 64 PRB) cost 22-23 ms CPU each, and the queue backs up (arr2deq p99 ~100 ms) | 12686-38081 / 80721-118231 [b, n=3]; 49231 / 132736 [tim, n=1] |
| on air TIM, no BGDISC | wall p50 2.7 ms (< 16 PRB) to 17 ms (>= 64 PRB); arr2deq p99 39-53 ms; probe ring 33231 queued / 11094 full; NID_BG 115.7 s CPU in 300 s, all of it in stage-B hypothesis decodes (each with a ±2048 inline search), sweeps 0 | 3803 / 52423 |
| `ISAC_UL_BGDISC=1` (search moves to a worker on a snapshot copy) | FEP stage 160-176 us; per call rx_pusch ~600-700 us + LDPC ~220 us ≈ 1 ms; ~1.3 calls/grant; queue wait p99 ~2.2 ms | see next table |

`ISAC_UL_BGDISC=1` [b, n=5]:

| UL tot p50 (us) | UL tot p99 (us) |
|---|---|
| 2272 | 5318 |
| 2238 | 5847 (with GPU_PASS) |
| 2239 | 5685 |
| 2306 | 5360 |
| 2348 | 5817 (MMSE0) |

Levers that did not move p99 [b, n=1 each]:
- ISAC_UL_DISC_EVERY=8: UL 3784/30379, and DL p99 13 ms.
- 3 consumers: 2242/5458.
- PUSCH_SOLVER=0: 2328/6235.
- MMSE_MODE=0: 2348/5817.
- GPU_PASS=1: 2238/5847. Cache misses, since no hypothesis is ever confirmed.

The remaining ~1 ms per call is spread across idft64/256/1024/4096 (nr_est_delay), memcpy and nr_pusch_antenna_processing (perf, b).
Getting under 3 ms p99 needs a cheaper per-call rx path (or the GPU pass once ids are confirmed), not queue tuning.

## Final A/B [DGX replay, n=3 each; off = t26gE only, on = t26gE + proposed UL env]
Run 1, before the BGDISC-path TA patch:

| capture | arm | DL p99 per run (us) | DL crc_q % | UL n_real | UL tot p50/p99 (us) | UL crc_q |
|---|---|---|---|---|---|---|
| b | off | 2992, 2867, 3114 | 95.5, 97.5, 97.1 | 0 | - | - |
| b | on | 2950, 2961, 3163 | 96.6, 95.9, 95.8 | 2819, 2390, 4694 | 2256/6141, 2238/5647, 2262/6447 | n_q=0 |
| tim | off | 2350, 2405, 2406 | 73.5, 74.5, 73.4 | 0 | - | - |
| tim | on | 2274, 2305, 2357 | 74.2, 75.1, 73.0 | 3163, 3157, 3195 | 2497/6686, 2434/5592, 2831/8517 | n_q=0 |

- DL is within run noise. UL drops 0.0 % in every on run (they were 5-8 % without BGDISC).
- NID_BG CPU at CPU_PCT=10: 0.4-1.1 s per 90 s run.

Run 2, after the follow-up commit (TA_TRACK also in the BGDISC grant path); on arm only:

| capture | DL p99 per run (us) | DL crc_q % | UL n_real | UL tot p50/p99 (us) | UL crc_q |
|---|---|---|---|---|---|
| b | 3304, 2881, 2785 | 96.3, 96.4, 95.7 | 4297, 4012, 3922 | 2235/6353, 2276/6034, 2239/6324 | n_q=0 |
| tim | 2030, 2076, 2455 | 73.9, 72.4, 73.8 | 3264, 3261, 3287 | 2489/7063, 2481/6829, 2447/6638 | n_q=0 (1 on run 3, not passed) |

- Applied advances: ~5000 b decodes per run at the 1600 bucket (1500-1700 after refinement).
  - 10-32 per advance bucket from 1700 to 2000: worker-found or cell-prior advances.
  - No pull toward the DL-slot "advance 0". The 0-bucket entries (173-184) are refused grants with no advance applied.
- tim: no shifted advances (no acceptances on this capture).

## Proposed on-air UL env
The file is /home/nicola/NICOLA/wt/rr-orchestration/ota/t26uR.env: t26gE + the flags below, on one line.
```
ISAC_UL_MCS_TABLE_SWEEP=1 ISAC_UL_BWP_SEED=1 ISAC_UL_K2_PAIR=1 ISAC_UL_TDA_PAIR=1 ISAC_UL_BWP_DL_PRIOR=1
ISAC_UL_TA_TRACK=1 ISAC_UL_BGDISC=1 ISAC_UL_NID_BG=1 ISAC_UL_NID_BG_IQ=1 ISAC_UL_NID_BG_CPU_PCT=10 ISAC_UL_NID_BG_IDLE=1
```
- Optional diagnostic for one run: `ISAC_UL_LAYOUT_BRUTE=10` (CRC oracle on up to 3 grants of >= 10 dB, each 2-10 s of a consumer).
- The binary must include this branch's HEAD; TA_TRACK in the BGDISC path is in the follow-up commit.

## What would decide UL next (on air)
1. TIM daytime with t26uR. Check whether TA_TRACK lifts the UL-slot DM-RS SNR of the +480-sample UEs (UL_PUSCH_CRC ta= near
   2080, snr_db). If grants reach >= 10 dB in UL slots, run LAYOUT_BRUTE once.
2. If UL-slot SNR stays < 5 dB on air too, the passive RX does not hear the UEs (geometry). No decoder change fixes that; antenna
   placement does.
3. Latency: profile and trim the per-call nr_rx_pusch path (delay-estimation IDFTs) for < 3 ms p99.

## Risks
- TA_TRACK cell prior: one far UE can seed others with a wrong advance. The fallback applies only when a grant's own search fails,
  and stays within ±2048 of nominal.
- The DL-slot artifact could still poison the per-RNTI tracker through real grants whose k2 is wrong. Trials are guarded; real
  grants rely on the inferred k2 (correct on b: k2=2 into UL slots 8/9).
- Replays are 12 s captures; n=3.
