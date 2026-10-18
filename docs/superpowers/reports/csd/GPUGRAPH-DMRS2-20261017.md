# GPUGRAPH: Task 4 review fixes, DM-RS type 2 in the pass, more than 32 segments (2026-10-17)

Branch csd/int16-gpupath. Not merged. Every new path is default off.

Commits:
- e6dd9c4693: Step A, the Task 4 review fixes.
- 270a62f1b1: Step B, DM-RS type 2 (`ISAC_PDSCH_GPU_GRAPH_DMRS2`).
- 784367428b: Step C part 1, the 64-segment class (`_SEG64`).
- 51463e6b9c: Step C part 2, the 128-segment class with C in chunks. `_SEG128` replaces `_SEG64`.

Arms:
- OFF = t26gE.
- PREV = t26gE + `ISAC_PDSCH_GPU_GRAPH=1 ISAC_GPU_PASS_RETRY_OLD=1 ISAC_GPU_PASS_POLL_SLEEP_US=600 _COVER=1 _SPLIT=1 _RETRY=1`.
- ON = PREV + `_DMRS2=1`.

Latencies are goal_metrics tot, in us.

## Summary

- **DM-RS type 2 now runs on the pass, and it is bit-exact.** Chest refusals for reason 4 fall to 0 (they were 1051-1990 per run on b and 425-837 on tim).
- **p99 did not move.**
  - tim: ON 2528 against PREV 2557.
  - b: ON 3495 against PREV 3301, +5.9 %. This fails gate (e) by a small margin. In the high-n_real mode b is +10 % (3356 against 3050).
- **The stage-path share of the tail did not fall on b** (36 % to 42 %). On tim it fell only a little (58 % to 53 %). Type-2 grants were not the tail grants.
- **The stage-path tail is now mostly segmented grants of 80-217 PRB**, with stage chests of 400-1300 us.
- **Moving those grants onto the pass makes p99 worse.**
  - `_SEG128` empties the segment gate (gate seg = 0) and cuts the stage-path tail share to 8-34 %.
  - But p99 rises by 39 % on b and 32 % on tim, in every interleaved run.
- **Coverage is no longer the lever.** The pass bodies themselves are now the tail.
  - The first body of pass-path tail grants takes 1.0-3.5 ms, against a launch-to-done p50 of at most 512 us.
  - Per-grant GPU work and contention are the next targets (Task 5: the slot FEP shared per slot instead of per grant). So is a2e, at 0.5-1.3 ms of the tail mean.

## Step A: Task 4 review fixes (e6dd9c4693)
- **S1:** `nr_gpu_graph_call_dirty()` now runs right after the SSB FEP block, which writes the caller's rxdataF [CODE-READ nr_pdsch_passive_decode.c:2862].
- **S2:** `g_sub_on[RETRY] &= g_sub_on[SPLIT]` after the flags are read. The S5 cap path clears SPLIT, RETRY, DMRS2 and SEG128.
- **S3:** `nr_gpu_pass_call_dirty` now releases the held instance (busy bit cleared, `t_held = -1`).
  - The call stays open, so its next pass acquires an instance again and holds it.
  - `dirty=` now counts every dirty mark on a held instance.
- **Nits:**
  - A thread_local guard releases a hold when its thread exits.
  - `copy_mask` is now part of `c_sig`.
- **Test changes, GpuGraphRetry.EntryFollowsChangedInput:**
  - It now covers 5 grant shapes, including a segmented grant with 8 segments, and 9 bodies per call.
  - A new `ring_offset` change (a C input that is not a pilot) enters at C.
  - The held count is +2 per call, because the instance is acquired again after the dirty release.
  - Mutation check: with `ring_offset` dropped from `c_sig`, the test fails (the body enters at D and the LLRs differ) [OFFLINE VERIFIED, DGX GB10, DIRECT].
- **Discrete ctest filter: not changed.** Editing CMakeLists makes ninja re-run cmake. The exact change, for the orchestrator, is in `openair1/PHY/CODING/CMakeLists.txt:165`:
  - `--gtest_filter=GpuPassDl.*:GpuPassUl.*:GpuPassFence.NoAllocAfterFence:GpuPassFence.UlNoAllocAfterFence`
  - becomes `--gtest_filter=GpuPassDl.*:GpuPassUl.*:GpuGraph*:GpuPassFence.NoAllocAfterFence:GpuPassFence.UlNoAllocAfterFence`
  - The whole binary already passes under `NR_GPU_PASS_DISCRETE=1`: 47/47.

## Step B: DM-RS type 2 in the pass (270a62f1b1, `ISAC_PDSCH_GPU_GRAPH_DMRS2`, default off, needs SPLIT)
- **`nr_pdsch_gpu_chest_pass_inputs` accepts type 2 when allowed.** The stage path's own type-2 rules apply:
  - pilots are 4 c16 per RB;
  - `get_beta_dmrs(cdm, type2)`;
  - type-2 delta;
  - the 3280-pilot bound is computed per type.

  Source: [CODE-READ nr_dl_channel_estimation.c, nr_pdsch_gpu_chest_grant].
- **The DM-RS type is data, not part of the C key.**
  - The captured `k_ls` has the job's `Par.type` baked in by value.
  - So the chest row now carries its type (`nr_gpu_chest_row_t.type`; 0 means the job's type).
  - The stage path leaves it at 0, so its behaviour is unchanged.
  - No new sub-graph, no growth of the C key set, no new memory.
- **Job and module changes:**
  - The job gets `dmrs_type`.
  - `prepare()` copies `ppr * nrb` pilots and offsets segment pilots by `2 * ppr * pil_rb`.
  - `c_sig` includes the type.
- **Test GpuGraphDmrs2.Type2EqualsStagePath:**
  - Coverage: 28 grants. PRB counts 24, 48, 96 and 217. Layer counts 1-4: CDM group 0 only, or groups 0 and 1. 1 and 2 DM-RS symbols. Contiguous grants and an 8-segment grant. SNR 20 and -3 dB.
  - Result: 14 decoded, and every output equals the stage path: TB, CRC, need_cpu, seg_ok, nvar, nvar_ant, final LLRs, LLR statistics, rxdataF. [OFFLINE VERIFIED, DGX GB10, DIRECT + DISCRETE]
  - Mutation check: with the rows forced to type 1, there are 155 failures.
- **Fixture:** type-2 pilots are adjacent pairs 6 subcarriers apart; layers 2 and 3 are in CDM group 1 with delta 2. DM-RS symbols carry no data, so every CDM group is without data.

## Step C: more than 32 segments
- **784367428b, the 64-segment class:**
  - `NR_GPU_PASS_MAX_SEG` is now 64.
  - The class fits the existing 1024 chest rows for rank 1 (up to 3 DM-RS symbols) and rank 2 (up to 2).
  - Test: 40-64 segments [OFFLINE VERIFIED].
- **51463e6b9c, the 128-segment class.** The SEG64 replay showed that most stage-path tail grants have more than 64 segments (inferred below). So:
  - New `nr_pdsch_gpu_chest_dev_at(j, r0, n, ...)`.
  - The C sub-graph runs the chest rows in chunks of whole (DM-RS symbol, layer) groups, at most min(SEG_ROWS, LS rows) rows per chunk.
  - The scratch and LS buffers are reused in stream order, so no new device memory.
  - A job can have up to 2048 rows.
  - The segment class takes 4 bits of the key, at bit 44.
  - The fixture's stage path chunks the chest the way `gch_flush` does.
  - The test binary sets `ISAC_PDSCH_GPU_GRAPH(_SPLIT)=1` before the FEP init, which gives 1024 LS rows.
- **Test GpuGraphSplit.BeyondThirtyTwoSegmentsEqualStagePath:**
  - Coverage: 40-109 segments, rank 1-2, 1-4 chunks, up to 2048 rows, CRC ok and CRC fail.
  - Result: 12 grants, 6 decoded, every output equal to the stage path [OFFLINE VERIFIED, DIRECT + DISCRETE].
  - Mutation check: with compose run without the group offset, there are 20 failures.
- **All tests:** test_nr_gpu_pass 47/47 in DIRECT and in DISCRETE; ctest gpu|pdsch 32/32.

## Gate batch, Step B [MEASURED, DGX replay, binary 270a62f1b1, interleaved; index replays/gg_d2.idx]

| arm | n | p50 | p99 median (runs) | p99.9 | crc_q med (min) | n_real med (runs) | drops | stage-path tail share (median) | tail first body, stage / pass (us) | pass MB | chest_why[4] | unseen C |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| b OFF | 3 | 1272 | 3199 (3290, 3199, 2880) | 6263 | 94.8 (94.6) | 2590 (4878, 2590, 1718) | 0 | 97 % | 1779 / - | - | - | - |
| b PREV | 6 | 1312 | 3301 (3059, 3050, 3587, 3587, 3543, 3039) | 5691 | 97.2 (96.2) | 4175 (5112, 4470, 1970, 3879, 2778, 5146) | 0 | 36 % | 2418 / 1363 | 919-932 | 1051-1990 | 0 |
| b ON | 6 | 1380 | 3495 (3601, 3279, 3356, 4114, 3488, 3501) | 5878 | 97.2 (96.5) | 3045 (1857, 4574, 4233, 1815, 1789, 4472) | 0 | 42 % | 2546 / 1777 | 924-941 | 0 | 0 |
| tim OFF | 3 | 980 | 2222 (2222, 2774, 2034) | 4770 | 73.4 (67.4) | 4726 | 0 | 100 % | 1765 / - | - | - | - |
| tim PREV | 3 | 1084 | 2557 (2557, 2743, 2454) | 6135 | 74.5 (73.4) | 5841 | 0 | 58 % | 3146 / 2324 | 928-945 | 425-837 | 37-45 |
| tim ON | 3 | 1067 | 2528 (2754, 2528, 2401) | 5589 | 75.4 (74.8) | 5470 | 0 | 53 % | 2484 / 2057 | 933-952 | 0 | 29-41 |

**b like-mode** (n_real high >= 3900, low <= 3300) [MEASURED, same runs]:

| mode | PREV p99 | ON p99 |
|---|---|---|
| high | 3059, 3050, 3039 (median 3050) | 3279, 3356, 3501 (median 3356, +10 %) |
| low | 3587, 3543 (median 3565) | 3601, 4114, 3488 (median 3601, +1 %) |

PREV run 4 (n_real 3879) falls between the two modes: p99 3587.

**Gates:**
- (a) shadow_bad = 0 and no `GPUGRAPH MISMATCH` / `DISABLED` line: **pass**. The `DMRS_ID ... MISMATCH` lines are the existing scrambling-identity diagnostic and appear in OFF too.
- (b) crc_q: **pass**.
- (c) n_real ON/OFF: b 1.18, tim 1.16, **pass**. b is bimodal.
- (d) drops 0: **pass**.
- (e) p99 ON <= 1.05 x PREV:
  - b **FAIL**, 1.059. In the high mode it is +10 %.
  - tim pass, 0.99.
- (f) chest_why[4] = 0 in every ON run: **pass**.

**Other per-run numbers [MEASURED, ON runs]:**
- The module fallback rises (b 559-816 against 265-409). Nearly all of it is probe_busy: b 539-812 against 249-406. Type-2 grants now hold pass instances, so more probes are refused.
- Real-grant busy: 0-20 per run.
- Tail means (median of the runs):
  - b ON: a2e 1059, qw 279, pre 705, first body 2155 us;
  - b PREV: a2e 1278, qw 159, pre 612, first body 2052;
  - tim ON: a2e 565, qw 349, pre 230, first body 2326.

## Step C replay checks [MEASURED, DGX replay, interleaved, ON vs ON2 = ON + the class flag, n=3 per arm and capture]

| batch (binary) | arm | b p99 runs (median) | b n_real runs | tim p99 runs (median) | gate seg b / tim | stage-path tail share b / tim | pass-path tail first body b / tim | pass MB, capped |
|---|---|---|---|---|---|---|---|---|
| gg_s64 (784367428b) | ON | 3585, 4048, 3453 (3585) | 5059, 5336, 5132 | 2448, 2371, 2344 (2371) | 387-439 / 98-162 | 47-48 % / 44-54 % | 1759 / 1767 | 934-948, 0 |
| gg_s64 | ON2 (+SEG64) | 3580, 4151, 4047 (4047) | 1941, 2035, 4697 | 2534, 2391, **19259** (2534) | 24-99 / 75-104 | 10-25 % / 27-44 % | 2132 / 2229 | 951-958, 0 |
| gg_s128 (51463e6b9c) | ON | 3102, 3185, 3261 (3185) | 1753, 2430, 4417 | 2411, 2671, 2460 (2460) | 125-289 / 113-202 | 32-83 % / 54-61 % | 1011 / 2194 | 917-962, 0 |
| gg_s128 | ON2 (+SEG128) | 4814, 4282, 4427 (4427, **+39 %**) | 4925, 4259, 4824 | 3255, 3471, 3118 (3255, **+32 %**) | 0 / 0 | 12-14 % / 8-34 % | 3495 / 3282 | 993-1012, **6 and 50 in 2 of 6 runs** |

- **SEG64:**
  - The 64 class takes 107-254 grants per b run and 47-79 per tim run. The segment gate falls from 387-439 to 24-99 on b and from 98-162 to 75-104 on tim.
  - The p99 effect cannot be read on b: the ON2 runs are mostly in the low n_real mode. On tim there is no gain.
  - One tim ON2 run had a stall of 40-55 ms across the whole GPU. Pass bodies and stage bodies were both hit (frames 199-242), giving p99 19.3 ms and 0.9 % drops. The cause is not found; nothing else was on the GPU afterwards. It happened once in 36 runs.
- **SEG128 is worse in every run.**
  - It also brings the pass to the 1 GB cap. Two runs hit the cap, and in tim run 1 that left 209 C keys unseen.
  - The hold time p99 rises from <= 2048 to <= 4096 us. Real-grant busy rises from 0-9 to 22-34 on b, and prep_us p99 from <= 256-512 to <= 1024.
  - Heavy segmented grants on the pass hold an instance longer than the stage path takes for them. The other pass bodies wait or contend.
  - Keep `_SEG128` off.
- **Inference behind the 128 class [ESTIMATE].** The EVMGRANT line has no segment count. In the ON runs, the stage-path tail grants with a stage chest >= 400 us were 80-217 PRB (mostly 185-217). That is 7-17 of 7-19 stage-path tail grants per b run and 24-26 of 27-32 per tim run. The gate seg count matches them. With PRG 2 or interleaving, 217 PRB is about 109 segments.

## Where the tail time goes now
- **The tail grant is still one body** (bodies = 1.00). Its first body is the largest component:
  - b, ON with DMRS2: a2e about 1.1 ms + qw 0.3 + pre 0.7 + first body 2.2 ms;
  - tim: a2e 0.6 + qw 0.3 + pre 0.2 + first body 2.3 ms.

  [MEASURED, gg_d2 ON, n=6 / 3]
- **The stage-path bodies left in the tail are the segmented grants with more than 32 segments.** With SEG128 they leave the stage path, but the pass bodies become slower and p99 rises.
- **Pass-path tail first bodies are 1.0-3.5 ms**, while the pass's launch-to-done p50 is <= 512 us.
  - What fills them is per-grant GPU time under contention: whole-slot FEP of 14 symbols x 4 antennas per grant, the C chest, and LDPC. Instance holds and refused probes, which run on the stage path, add to it.
  - The next lever is per-grant GPU work: Task 5 (slot FEP once per slot), then the chest cost of grants with many segments.
  - Coverage is not the lever.
- **a2e (PDCCH, CPU) is 0.5-1.3 ms** of the tail mean and is untouched by this plan.

## Memory
- DMRS2 adds no device memory: pass 924-952 MB in ON against 919-945 MB in PREV [MEASURED, census estimate].
- SEG64: 951-958 MB, capped 0.
- SEG128: 993-1012 MB, at the cap in 2 of 6 runs [MEASURED].
- Host: the chest row grows from 32 to 40 B. Job, PassBlk and post parameters grow with 128 segments, by about 1.5 KB each.

## Deviations
1. **Step C went beyond the 64 class.** It went on to a 128 class with C in chunks, because the SEG64 replay showed the remaining tail grants have more than 64 segments. `_SEG64` was renamed `_SEG128`; the 64-only binary is 784367428b. Both are default off. SEG128 makes p99 worse, so keep it off.
2. **S3 changes the meaning of `dirty=`.** It now counts every dirty mark on a held instance, which releases it. Before, it counted only marks that invalidated an estimate.
3. **The test binary sets `ISAC_PDSCH_GPU_GRAPH=1` and `_SPLIT=1` in its environment** at static init. This gives the FEP module 1024 LS rows, the production split value. WarmSetFitsBudget now covers 4032 classes at 523 MB.
4. **The discrete ctest filter is not edited.** It needs a cmake re-run; the exact line is in the Step A section.
5. **Three link failures in `ninja tests`:** test_nr_csi_rb_extent, test_nr_ssb_rate_match_prod and nr_pusch_ra0sim. They fail on undefined `nr_srs_passive_enabled` and `nr_pusch_grant_book_*`. This diff does not touch those symbols. I did not check whether they also fail at 2bc890beab.
6. **OFF was not re-run** in the two Step C batches. Those batches compare ON against ON2 within one batch. Between batches the same ON arm moves by about ±400 us on b, for example b ON p99 median 3495, 3585 and 3185.
