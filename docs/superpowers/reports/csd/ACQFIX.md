# ACQFIX: late banking of dedicated CORESETs

Host: DGX aarch64 / GB10. Branch `csd/acq-fix` (base `csd/otatest` 80cae1d872). Date 2026-10-10.
Offline only: no radio, no hardware tools, no sudo. Every build, test and replay ran through `heavy.sh`.

## Root cause

[MEASURED, OTA LIVE 2026-10-10 logs; MEASURED, replays] Verification does not fail because of a wrong nID, the
DCI length, the dwell budget or the DM-RS ID. It fails because the CORESET geometry that GPU discovery publishes
(start RB, span, CCE-to-REG mapping) is an estimate, and that estimate is often wrong. The discovered-CORESET dwell
then verifies only exactly the published geometry. When that geometry is shifted, every CCE is misaligned, no DCI
passes CRC, and the dwell is rejected after 4000 occasions ("no fresh CRC evidence"). The same wrong geometry is
published again and rejected again. The bank add happens only when a pass happens to publish the true geometry
(grid RB 0, 36 groups, non-interleaved).

[CODE-READ] Why the estimate is ambiguous. PDCCH DM-RS is indexed from CRB 0, so its correlation is the same for a
CORESET and for the same CORESET shifted by whole RBs. GPU stage 2 (`nr_pdcch_nid_gpu.cu` `hull`/`generate`/`finish`)
takes the start and span from the 2 %/98 % quantiles of the lit RBs in a handful of snapshots, plus a phase score.
With sparse traffic, these give shifted or partial spans.

### Live evidence (6 runs, logs under /home/nicola/NICOLA/ota_runs/)

| run | first published geometry (nid/grid_rb/groups) | bank add (s after sync) | rejections | Technique A commit | SOLVED |
|---|---|---:|---:|---|---:|
| ab_C1_1_022115 | 62922/2/35; 62922/5/35 | 35.7 | 26 | yes (line 3819) | 2 |
| ab_C2_1_022635 | 62922/179/2 L=2 R=2 | 56.6 | 19 | yes (3476) | 0 |
| ab_H2_1_014243 | 62922/11/34 | 94.7 | 11 | yes (3369) | 0 |
| ab_I2_1_022915 | 62922/11/34 | 27.7 | 4 | yes (3835) | 7 |
| ab_H1_1_013722 | 62922/2/35; 62922/0/36 | 7.8 | 5 | no | 20 |
| ab_I1_1_022354 | 10163/0/36 | 3.4 | 0 | no | 15 |

[MEASURED, OTA LIVE] Dwell outcome by geometry class, over all six runs:
- Canonical `grid_rb=0 groups=36 L=0` dwells of nID 62922 verified **every time** (one per run). They verified
  19–317 log lines after the dwell started, with len=49.
- **All 56** dwells on any other geometry were rejected (C1 21, C2 16, H2 10, I2 4, H1 5).
- The other canonical rejections were nID 9020 (8×), which never verifies in any geometry, and one 10163 window.

## Answers

**Q1: why no fresh CRC evidence in 4000 occasions.** Wrong geometry: shifted start (phase 2/5), partial span, or
interleaved (L=2 R=2). The other candidate causes were checked against the logs:
- **Not the nID.** 62922 verifies as soon as its 0/36 geometry gets a dwell.
- **Not the DCI length.** Every bank add, early or late, uses len=49.
- **Not budget or dwell starvation.** The dwells ran (SCANPASS `dwell[runs=3803..13735 acc=0]`); they ran with zero accepts.
- **Not the async anonymous worker.** The dwell's evidence path (`nr_pdcch_discovered_observe`) does not use it.
- **Not the DM-RS ID.** Refuted by the table in the task.
- **GPU nID discovery timing does matter, but indirectly.** It decides *when* a correct publication first appears,
  and so when a late bank add happens.

**Q2: why no bank use after the late bank add.** That is a counter artifact, not a scan defect.
[MEASURED, OTA LIVE] After the late add in ab_C2 (line 6707), the bank decodes 6776 DCIs of 0x087a plus 9 other RNTIs.

[CODE-READ, nr_pdcch_blind_monitor_rt.c] Why the counter stays at zero:
1. SCANPASS and the `bank[runs]` counters (`pass_note`) are only updated on the dedicated-only path, and printed
   every 4000 dedicated-only occasions.
2. In every bad run, Technique A's fallback committed a bogus 6-RB footprint after 8 dwells without any bank
   ("recurrent oracle committed", "Phase 3 autodiscover -- CORESET footprint rb_offset=21/51/177 span_rb=6").
3. That commit sets the root `ss_monitoring_slot_periodicity=1` and calls `gate_publish()`. From then on,
   `nr_pdcch_ss0_occasion()` is true for every slot, and the dedicated-only path is never taken again.
4. So the last SCANPASS line predates the late add. In ab_C2, SCANTIME also switches to "full" with p50 ≈ 1 ms.

The full path is expensive: it runs the bogus Technique A walk every slot. Measured scan-queue `drop_stale`:
- bad runs: C2 3248, H2 16065;
- good runs: ≤ 5.

This is a consequence of late banking. With a bank in place, Technique A runs at 1/20 duty and never committed
in any good run or fixed replay.

**Q3: is the first divergent decision deterministic given the input?** No.
- [MEASURED, replays] The first divergent decision is GPU stage 2's published geometry. On the identical old
  capture (20261008), different replays published nID 10163 first as 1/36, 0/36, 156/2 L=2, 10/34 or 11/34.
  The outcome is bimodal: 1 main RNTI solved when 10163 is rejected, 4 when a 0/36 publication arrives. This
  holds in 20 earlier gpu6/csi-full old-capture replays and in this campaign's baselines.
- [CODE-READ] Why it varies. GPU arithmetic is exact (GPU6), but the snapshot set is timing dependent:
  - `nr_pdcch_nid_gpu_push()` drops symbols on `pthread_mutex_trylock` failure;
  - it also drops them when all 3 staging batches are busy;
  - the duty cycle is wall-clock based.
- Live, the input itself differs between runs.

**Capture b does not trigger it** [MEASURED]. Unseeded capture-b replays published 0/36 first in 2/2 baseline runs
here (0.25× and 1.0× speed) and in ~60 earlier GPU replays. Note that `ISAC_PDCCH_DISCOVERED_FILE` is ignored
whenever `ISAC_PDCCH_NID_GPU=1`, so those GPU replays were effectively unseeded. The old capture is the offline
reproduction.

## Fix (flags, default off)

**`ISAC_PDCCH_DISC_COVER=1`**, in `nr_pdcch_blind_monitor.c`:
- **Cover-first queue.** `discovered_cover_queue()` rebuilds each published queue: first one *cover* entry per
  distinct (nID, symbol, duration), then the published lines.
- **The cover geometry.** Non-interleaved, grid RB 0, floor(carrier/6) groups, resolved in
  `nr_pdcch_discovered_snapshot()`. A non-interleaved CCE j occupies RBs [start+6j, start+6j+6). So the cover
  decodes every non-interleaved CORESET on the RB-0 six-RB grid, whatever span was published.
- **Skipping redundant published lines.** A published line is skipped when either:
  - it equals the cover, or
  - `nr_pdcch_discovered_cover_banked()` finds a VERIFIED bank entry with the same nID/symbol/duration that
    overlaps it.
- **Lines the cover cannot handle stay queued.** rb-Offset (phase ≠ 0) and interleaved lines remain in the queue
  after the cover.
- **GPU feedback.** `nid_budget_feedback()` counts a record as verified when its cover is banked, so the GPU duty
  cycle can pause as before.
- With the flag unset, behaviour is unchanged.

**`ISAC_PDCCH_RBOFFSET_FIX=1`**, in `dci_nr.c` (the GPU6 suspicion) [CODE-READ, confirmed]:
- **The defect.** `nr_pdcch_extract_rbs_single()` took both the bitmap bit and the RB position from
  `(cset_start + coreset.rb_offset)/6`. The blind monitor builds the bitmap from bit 0 and puts the absolute start
  in `coreset.rb_offset`, which gives two failures:
  - **rb_offset ≥ 6:** the loop tests bits `rb_offset/6..` and drops the last `rb_offset/6` groups. For a 26-group
    CORESET at RB 36 that is CORESET RBs 120..155 — exactly GPU6's nID 7684 observation.
  - **rb_offset % 6 ≠ 0:** data REs are read at unshifted RBs, while `nr_pdcch_channel_estimation()` estimates
    from `rb_offset`.
- **The spec.** Per TS 38.213 §10.1 and TS 38.211 §7.3.2.2, bit i is the i-th 6-RB group from the CORESET
  reference, shifted by rb-Offset.
- **The fix.** `nr_pdcch_coreset_group_rbs()` places set bit g at `rb_offset_total + 6*(g - first set bit)`. These
  are the RBs the channel estimate covers. The result is identical to legacy when rb_offset = 0.
- **Effect here.** It does not affect the cover (rb 0). It matters for genuine rb-Offset CORESETs and for
  published lines with offset ≥ 6.

**Unit tests** (`test_nr_pdcch_blind_monitor`), all pass [MEASURED]:
- `DiscoveredCover.FlagOffDwellsOnShiftedPublicationAndCannotDecode`: the live 62922 publication at 2/35 cannot
  decode the synthetic RB-0 CORESET.
- `DiscoveredCover.CoverDwellFirstVerifiesAndSkipsSubsumedPublications`: the cover decodes it, banks it, and skips
  the shifted lines, before and after a refresh.
- `DiscoveredCover.QueueOrderCoversFirstThenPublishedLines`: queue order for ab_C2's interleaved 179/2 publication.
- `PdcchRbOffset.GroupPositionsFollowBitmapAndOffset`: legacy vs fixed positions, including the 7684-like case
  (legacy 20 groups, fixed 26), rb-Offset 2, and a non-contiguous MAC bitmap.

ctest `test_nr_pdcch_{blind_monitor,coreset_bank,coreset_map,nid_worker,scan_fast}`: 5/5 pass.

## Replay results (before/after)

Setup:
- Wrapper `tests/passive_rx/acqfix_replay.sh`: `ota/t26h.env` (the env of the failing OTA runs) minus
  radio-only/core-pin flags, speed 0.25.
- GPU nID discovery on, **no discovered-file seed**.
- "Fix" = `ISAC_PDCCH_DISC_COVER=1 ISAC_PDCCH_RBOFFSET_FIX=1`.
- Times are signal seconds from PBCH lock to the bank add.
- The two captures are reported separately.
- Logs: `replays/replay_acq_*`.

**Capture b** (raw_pci415_12s_b) [MEASURED]

| arm | runs | first bank add ≤ 2 s | first bank add (s) | SOLVED RNTIs (with later full-TB CRC) |
|---|---:|---:|---|---|
| baseline, 0.25× | 1 | 1/1 | 0.99 | 30 (25) |
| baseline, 1.0× | 1 | 1/1 | 0.96 | 35 (33) |
| **fix** | 10 | **10/10** | 0.99 (9 runs), 1.47 (1 run) | 19, 27, 24, 32, 24, 37, 30, 35, 15, 23; full-TB 14–31 |
| seed (GPU off + scanfast_coresets.txt), baseline | 1 | 1/1 | 0.1 | 42 (38) |
| seed, fix | 2 | 2/2 | 0.0 / 0.1 | 43 (38), 41 (39) |

Gate (≥ 9/10 early bank add and ≥ 5 RNTIs without seed): **met, 10/10**. Every run has ≥ 14 RNTIs with full-TB
support. Seeded arm: no regression.

**Capture 20261008** (old; 4 main RNTIs 8f5a/b876/e01e/13ae) [MEASURED]

| arm | runs | 10163 banked | main RNTIs solved |
|---|---:|---:|---|
| baseline | 3 | 2/3 (base3: 10163 published only as 1/36, rejected) | 4, 4, **1** |
| baseline, earlier gpu6/csi-full replays (same mechanism) | 20 | 5/20 | 4 ×5, **1 ×15** |
| **fix** | 11 | **11/11** (via cover; 10163 was never published at 0/36 in e.g. try1) | **4/4 in 11/11** |

- In the fix arm, serving 62922 banks at 1.3 s in every run.
- 10163 banks 2.4–7.3 s after sync. Its cover dwell starts within ~90 log lines of 10163's first GPU publication,
  so the remaining delay is GPU publication latency for the second CORESET, not verification.

## OTA flags

Append to the current OTA env (t26i.env; the replays used t26h.env, which differs only in GPU exact-path flags):

```
ISAC_PDCCH_DISC_COVER=1 ISAC_PDCCH_RBOFFSET_FIX=1
```

What to look for in the live logs:
- `discovered CORESET ... is the ACQ-FIX cover dwell (ISAC_PDCCH_DISC_COVER)`, followed by
  `VERIFIED -> bank add` a few seconds after sync.
- No "recurrent oracle committed" / Technique A footprint commit.
- SCANTIME should stay "ded-only".

Suggested acceptance over ≥ 6 live runs: the first bank add ≤ ~3 s after the first GPU publication of 62922, and
SOLVED counts like H1/I1.

## Limits and follow-ups

- **Phase-shifted or interleaved CORESETs.** The cover handles non-interleaved CORESETs on the RB-0 6-RB grid. A
  genuinely rb-Offset or interleaved CORESET still depends on the published line being right. Those lines are
  still dwelt after the cover, and `ISAC_PDCCH_RBOFFSET_FIX` makes offset lines decodable on the CPU path.
- **Cost of a never-verifying nID.** Such an nID (e.g. 9020) now costs one extra 4000-occasion cover dwell per
  queue cycle (before: its published lines only).
- **Separate defect, not fixed here.** Technique A's fallback commit overwrites the root SS gate with period 1,
  which disables the dedicated-only path. Bank pass counters are also missing on the full path (the Q2 artifact).
  The fix makes this unreachable in the observed runs, because the bank exists early. Separating
  `nr_pdcch_ss0_occasion()` from the root gate is a candidate follow-up; it is not validated here.
- **Not verified live.** The live acquisition rate with the fix is not yet measured; the orchestrator's ≥ 6 OTA
  runs are needed.
