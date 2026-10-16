# DMRSSOLVER-20261015: blind DM-RS identity solver (commit 571bd335d4) brought up to csd/otatest

Branch `csd/dmrssolver` (from csd/otatest 26d3028b5d). NOT merged anywhere. All new behaviour is behind `ISAC_DMRS_NID_SOLVE` (default off, flags-off = csd/otatest).
Evidence labels: `[CODE READ]`, `[OFFLINE VERIFIED, DGX aarch64]` (unit test), `[MEASURED, DGX replay <cap>]` = air-like replay, `p99_replay.sh`, SPEED=1, ENVFILE=t26f.env, 4 RX, under heavy.lock, one run at a time, arms interleaved, n = 3 per arm and cell. Cells never pooled: V = Vodafone PCI 415 (`raw_pci415_12s_b`), T = TIM PCI 923 (`raw_pci923_tim_12s_tile3`). Replay only, `[NOT MEASURED ON AIR]`; replay is not run-to-run deterministic (RNTI count, discovered banks and CRC counts vary between identical arms).

## 1. Classification of 571bd335d4 `[CODE READ]`
What it is: the SA-bed (rfsim) commit of 2026-10-07. A pure GF(2) solver for the PDCCH DM-RS identity N_ID (`nr_dmrs_nid_solve.c`: rotation from the 4th power, information-set decoding on the most reliable equations, rank-deficient enumeration) plus a wiring in `nr_pdcch_blind_monitor_rt.c` and a pile of tracker/discovery changes.

| piece | status vs csd/otatest |
|---|---|
| `nr_dmrs_nid_solve.{c,h}` (the solver) + its unit test | **still missing, ported** (no CPU/closed-form N_ID source exists; otatest finds N_ID by the GPU sweep `ISAC_PDCCH_NID_GPU` and the joint anonymous solver) |
| worker thread, per-window + whole-symbol voting in blind_monitor_rt | **ported in a reduced form**: own file `nr_dmrs_nid_solve_live.c`, fed from the same capture hook as the GPU sweep (`nr_pdcch_coreset_map_idsweep_push`), reference = Point A (the GPU sweep's pilot grid) instead of the commit's {CRB0, bwp_start} |
| leader supplies DM-RS identity to the PDCCH pilots (`nid_pilot_id`) | superseded (nID bank / GPU records / `ISAC_PDCCH_POINTA_REF` already supply it), not ported |
| leader supplies CORESET extent (tracker second pass, oracle extent hint, early unseeded commit, reseed, duration lap, half-window observer, pass-2 mapping rotation + vote lock, refuted-hypothesis rule after 2500 TB trials) | **conflicting / superseded**: tied to the passive-BWP tracker and `autodiscover_*` oracle that csd/otatest replaced by CORESET_DISC_WATCH + the discovered-CORESET bank; cherry-pick conflicts in `nr_pdcch_blind_monitor_rt.c`, `nr_pdsch_passive_queue.c`, `nr_dl_adaptive_test.cc`, CMake; SA-bed tuning (sensnuc3), not ported |
| `nr_passive_bwp.{c,h}` changes (lit-window decay) | superseded with the tracker, not ported |
| `nr_pdsch_passive_decode.c` data-id 2-pass latch and `g_ded_ever_ok` | **overlaps** the PDSCH identity work in csd/otatest (`ISAC_PDSCH_DMRS_ID_SCAN/_PER_CORESET/_REFRESH/_CONTRAST`, `DMRS_SCAN_ASYNC`, fattails `DMRS_ID_ASYNC`, solver `PRIOR_CRC/PRIOR_IDS`); not ported, not compared by a run |
| `nr_pdcch_coreset_map.c` `window_corr_n` | only used by the commit's tracker, not ported |
| `nr_pdcch_blind_monitor.c` CSS0 snapshot `rb_offset` reset, 3-RB sub-windows | not ported (SA-bed cell_restart fix; not a replay problem) |
| openair2 gNB (`nr_radio_config.c`: `ISAC_GNB_CORESET_IL`, `phy_ParametersFRX_Diff` null check) and UE `config_ue.c` interleaved CORESET alloc | not ported: gNB side / simulation only (user rule: gNB code is not receiver input) |

## 2. What was ported (all default off)
- `ISAC_DMRS_NID_SOLVE=1`: observe only. A worker thread (`dmrs_nid_solve`) receives the pilot REs of every `_EVERY`-th (default 32) symbol that passes the GPU sweep's occupancy gate, solves the whole symbol and every 6-RB window, votes per N_ID and logs `DMRS_NID_SOLVE leader / confirmed (8 votes) / summary` lines (monotonic clock `t=`, same clock as `PDCCH_NID_GPU pass`). Nothing consumes the result.
- `ISAC_DMRS_NID_SOLVE=2`: as 1, plus the leader (>= 8 votes) is offered once as a one-record hint (`nid`, symbols it solved on, extent from its window votes, non-interleaved L=R=0) through `nr_pdcch_nid_gpu_records()` while the GPU sweep has published nothing; the sweep's own records replace it and the bank still verifies by CRC-valid DCIs.
- `ISAC_DMRS_NID_SOLVE_OPT=0` turns off the cost cuts (default on when the solver is on). `_EVERY`, `_CORE` (pin the worker).
- Optimizations (`nr_dmrs_nid_solve_ex(..., fast)`; `nr_dmrs_nid_solve` is unchanged): one cos/sin for the four rotation classes; early stop at the first class with a confirmed solution; the information-set trials sort 128 instead of 384 rows (rank-deficient head falls back to 384); in the worker, windows below 2x the lower-quartile pilot power are skipped, and once the leader has >= 48 votes and >= 3x the runner-up only 1 job in 4 is processed.
- Unit test `test_nr_dmrs_nid_solve` (8 tests). Changes to the commit's test: split-bundle threshold 95 -> 90 (this host's RNG stream gives 94/100, a statistical property); new `FastModeGivesTheSameBestNidAndIsCheaper`; fast timing in the cost test.
- Files: `openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve.{c,h}`, `nr_dmrs_nid_solve_live.c`, hook in `nr_pdcch_coreset_map.c`, hint in `nr_pdcch_nid_gpu.{c,h}`, CMake (solver + live in library `nr_pdcch_nid_worker`), test.

## 3. Does it work? `[MEASURED, DGX replay]`
Truth: N_IDs with CRC-valid DCIs in the same run (`JOINT_ANON_HIT`: V 62922 and 10163, T 1931) plus the GPU sweep's records.

| cell | solver leader | time of the first correct leader (after the first offered symbol) | confirmed (8 votes) vs first `PDCCH_NID_GPU` record | wrong leaders | per CORESET |
|---|---|---|---|---|---|
| T (n = 15 solver runs: 12 at EVERY=8, 3 at 32) | 1931 correct in 9/9 runs, "no decision" 0 | 0.03-0.13 s (EVERY=8), 0.3-0.6 s (EVERY=32) | 0.55-2.2 s earlier than the GPU record (E32: 0.55, 1.7, 1.9; E8: 1.5-2.2) | 0 | the DCI-bearing CORESET is a dedicated one; T's other ids (1929) 2-3 votes, never leader |
| V (n = 15 solver runs) | 62922 correct (CRC-valid) in 15/15 as first-or-second leader; 9020 is the other leader | 0.002-0.9 s (EVERY=8), 0.6-2.4 s (EVERY=32); 62922 confirmed 0.4-4.2 s before the GPU record | 0 ids outside (CRC-valid or GPU-record); but N_ID 9020 takes the lead at some point in 9 of 15 runs (alternating with 62922, 250 votes at times) | 10163 (CRC-valid, AL1, 6-8 votes in 12 s) reaches 8 votes only in 2 runs, 14 s after the sweep (late/never) |

- 9020 is a real DM-RS identity (hundreds of confirmed solves; the GPU sweep also reports it, `grid_rb=11 groups=34`) but no CRC-valid DCI of the receiver's formats ever carries it: the solver cannot tell "has decodable DCIs" from "exists". Hence 1 wrong-first-guess hint in the hint arms (below).
- Chance rate is negligible: a pure-noise unit test returns 1/3000 valid ids, 0 with <= 1 violated check `[OFFLINE VERIFIED]`.
- Unit test `[OFFLINE VERIFIED, DGX aarch64]`: 8/8 pass: noiseless recovery 100 % at any rotation; 18-pilot window: 100 % at 20 dB, 51 % at 6 dB, 1 % at 0 dB; 72 pilots: 100 % at 6 dB, 77 % at 0 dB; split bundles in noise 94/100. The commit's SA-bed (sensnuc3 rfsim) case was not re-run (needs the OAI gNB/UE rfsim bed and the gNB-side changes, out of scope).
- Not covered: CORESET#0 vs dedicated split was not separated (the solver does not know which CORESET an id belongs to; all CRC-valid hits here are dedicated-CORESET ids), interleaved CORESETs (the solver finds the id, the hint carries no L/R/shift), OTA.

## 4. Cost and optimization `[MEASURED, DGX replay]`
Solver thread CPU (`CLOCK_THREAD_CPUTIME_ID`, whole 12 s replay, 768 jobs at EVERY=8; per cell n = 3 per arm):

| | unoptimized (OPT=0) | optimized (OPT=1) |
|---|---|---|
| T worker CPU | 0.963-0.966 s | 0.142-0.146 s (6.7x less) |
| V worker CPU | 0.974-0.985 s | 0.242, 0.529, 0.528 s (1.9-4x less; flapping leader keeps the 1-in-4 skip off) |
| per job: whole symbol / windows | 600-620 us / 650-655 us | 305-335 us / 240-360 us |
| windows solved | 27648 of 27648 | T 3.5k of 8.5k, V 15k of 27.6k |

- At the default EVERY=32 a job comes 4x less often: about 16 jobs/s x 1.26 ms = 2 % of one core unoptimized, 0.3 % (T) to 0.7 % (V) optimized. Drops: 0 in all runs.
- Profile of the whole-symbol solve (perf, unit test) `[OFFLINE VERIFIED]`: 76 % memcpy + qsort of 384-row system copies (8 trials x 4 classes), not the GF(2) elimination (already one 31-bit word per row, so "bit-packing/NEON" has nothing to give). 819-pilot noise symbol 441 us -> 141 us per call (3.1x); the 2000-window SNR sweep: fast vs full both right 1473-1483, full-only 0-10, fast-only 0-15, i.e. equal within noise, 101 us -> 75-78 us per call.
- Same leader: opt0 and opt1 name the same leader id in all 6 cell-runs (62922 / 1931); the V flapping to 9020 occurs in both.

## 5. Benefit: hint (`=2`) `[MEASURED, DGX replay]`, n = 3 per arm
Time of the first `JOINT_ANON_HIT #1` (capture time, s; granular because the bank dwells are slot-scheduled) and decode-side figures (median of 3 runs):

| cell | arm | hit #1 (s) | dec p50 / p99 (us) | CRC-OK % (MCS 0-4 / 5-9) |
|---|---|---|---|---|
| V | off | 9.14, 8.12, 9.15 | 1156 / 5575 | 54.3 / 22.5 |
| V | solver, OPT=0 (observe) | 9.15, 9.14, 9.14 | 1099 / 5598 | 56.2 / 28.9 |
| V | solver, OPT=1 (observe) | 8.12, 9.15, 8.63 | 1106 / 5394 | 52.1 / 20.7 |
| V | hint, EVERY=8 | 7.10, 12.12, 7.10 | 1068 / 5702 | 58.2 / 26.9 |
| V | hint, EVERY=32 | 8.12, 8.63, 12.12 | 1076 / 5688 | 60.0 / 29.0 |
| T | off | 8.36, 8.36, 8.36 | 572 / 2849 | 69.3 / 35.9 |
| T | OPT=0 / OPT=1 | 8.36 x3 / 8.36 x3 | 572 / 2559; 573 / 3014 | 67.8 / 35.3; 69.1 / 34.8 |
| T | hint, EVERY=8 | 7.72 x3 | 568 / 3245 | 68.2 / 35.5 |
| T | hint, EVERY=32 | 8.36, 7.72, 7.72 | 575 / 3060 | 67.4 / 35.7 |

- Hint effect: T 0.64 s earlier first bank DCI in 5 of 6 hint runs (consistent), V 2 s earlier in 2 of 3 (EVERY=8) but one run +3 s later in each V hint arm: the leader was the real-but-DCI-less 9020, the bank spent its dwell on it first. So: mean gain small, variance up on V.
- Decode time: no resolvable change in any arm. Whole-call p50 within +-3 % of off in both cells; p99 differences (V -3..+2 %, T -10..+14 %) are inside the run-to-run spread (off arm p99 spread V 4.7-5.5 ms, T 2.8-3.1 ms with n = 3); max is dominated by single outliers (V off up to 194 ms, T 20-66 ms). Cannot exclude a few-percent p99 cost on T with the hint (+14 %, n = 3); the cause would be the extra worker thread (unpinned here; use `_CORE`).
- CRC-OK per MCS bucket: differences are within the variance of identical off runs (counts depend on how many grants were decoded).
- Dedicated CORESETs found / RNTIs: T 1-3 banks, 5-10 RNTIs in every arm; V 2-6 banks, 10-24 RNTIs in every arm; no arm better (the solver adds no CORESET not already found by the sweep; extra banks like 9020/7684 appear in off and on runs alike).

## 6. Verdict
- **Works?** Yes for what it solves: on both operators the leader is a real DM-RS id (T: always the DCI-bearing id; V: the DCI-bearing id is always among the top two, but a DCI-less real id, 9020, often outvotes it). 0 ids unexplained by the sweep or CRC.
- **Cost:** 1.3 ms CPU per offered symbol before, 0.2-0.4 ms after optimization (3-7x); at default rate 0.3-0.7 % of one core. No decode-time increase resolvable (n = 3).
- **Benefit:** it names the id 0.5-4 s before the GPU sweep's first record, but the receiver's time to the first CRC-valid DCI improves by at most ~0.6-2 s and only through the hint, which can also mislead. It finds nothing the sweep does not find, and the CORESET extent/mapping (the hard part of the commit) is not ported.
- **Recommendation: do not adopt into csd/otatest now (drop the hint, keep the branch).** Keep `=1` available as an observer if a CPU cross-check of the sweep is wanted. If revisited: (a) make the hint list the top-2 ids with >= 8 votes so a DCI-less id cannot block the real one, (b) needs OTA (night excluded by the no-cell-tuning rule) because replay cannot show cold-start timing, (c) the commit's extent/mapping logic would need a rewrite against the discovered-CORESET bank.

## 7. Reproduce
`ENVFILE=.../ota/t26f.env CAPSET=b|tim tests/passive_rx/p99_replay.sh <name> ISAC_DMRS_NID_SOLVE=1|2 ISAC_DMRS_NID_SOLVE_EVERY=8 [ISAC_DMRS_NID_SOLVE_OPT=0]` under `heavy.lock`. Flags-off: unchanged code path (one cached getenv per capture push).
Run directories: `replays/replay_{b,tim}{Off,On,O0_,O1_,H8_,H32_}*` (not committed).
