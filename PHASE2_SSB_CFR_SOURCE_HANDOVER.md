# Phase 2 — SSB as a standalone CFR source — HANDOVER

**Status: WORKS, live-validated on the X410 — but only after a real bug found and fixed during this
task's own live validation (see §2). All four checks (CPI closure, cadence, vel_max, cross-source
axis agreement) PASS after the fix. One pre-existing, unrelated harness condition (`VOID_DL_ZERO`,
the already-documented Phase 1 SI-RNTI autoconf gap) is present in every capture below and does NOT
affect the SSB sensing pipeline — see §4.**
**Branch:** total-passive-rx-UL-DL-graphics
**Host:** sens6 · **Repo:** /home/sens/NICOLA/openairinterface5g-total-passive-ue
**Written:** 2026-09-04

## What this phase adds

NR_ISAC_SRC_SSB: a CFR source requiring no grant, no RNTI, no decode -- PBCH DM-RS's own known
gold sequence over the SSB's 240 subcarriers, tapped from the live PBCH tracking path in
phy_procedures_nr_ue.c (NOT the initial-sync-only paths), submitted through the existing
nr_isac_submit_cfr_multi() API alongside every other source.

## 1. Measured

Capture: `/home/sens/NICOLA/captures/ssb_only_143150/` (DUR=150s, NANT=1, `sources = "ssb"` alone,
carrier 3450.0 MHz / 273 PRB / numerology 1).

- **CPI closure period on ssb-only: T_slot = 40.000 slots exactly, on every one of 56 closed CPIs**
  (predicted ~20 ms = 40 slots at this cell's 30 kHz numerology -- exact match).
- **vel_max on ssb-only: 2.2 m/s** (predicted +-2.17 m/s at this cell's lambda=86.9mm/20ms period --
  matches to the log's 2-decimal display; vel_res = 0.034 m/s).
- range_res = 3.05 m, range_max = 9993 m (same axis as every other 273 PRB capture on this cell).
- 56/56 CPIs closed over the 150 s run (`grep -ac 'SENSING: CPI #'` on the run.log), ~27-32
  detections/CPI, and the dominant returned feature in 1371 of ~1512 total raw detections across
  those 56 CPIs sits at range = 3.05-3.1 m (the LOS bin) -- SSB alone mostly resolves the direct
  path and not much else, consistent with its much sparser occupancy (240 REs) vs csi_rs's.

Capture: `/home/sens/NICOLA/captures/ssb_csirs_fused_143518/` (DUR=150s, NANT=1,
`sources = "ssb,csi_rs"`, same carrier).

- **ssb+csi_rs fused ref_type distribution: 175/175 CPIs (100%) carry
  `"ref_type":"fused(csi_rs+ssb)"`** (`illuminator.ref_type` field in `reports.jsonl`, filtered to
  this run's own wall-clock window [1788532500e9, 1788532690e9] ns to exclude other sessions'
  entries sharing the same fixed `report_path` -- see §5's caveat about that path). `src_occ` per
  CPI shows both sources populating rows: csi_rs ~169-170 rows/CPI, ssb ~126-128 rows/CPI.
- **Axis cross-check (ssb vs csi_rs LOS range-bin agreement): PASS.** SSB-only's dominant/near-only
  detected feature sits at range bin 1 (3.05-3.1 m, see above). The fused run's own LOS-residual
  sync tracker (`sync.los_residual.range_bin`) locks onto bin 1-2 (~3-6 m) in every sampled CPI, and
  the fused run's own detections list also shows a strong ~3.05 m return in essentially every CPI.
  No disagreement between the two sources about where the direct path sits.
  - Secondary, non-blocking observation: a ~820-826 m feature appears repeatedly in the fused run
    and in older csi_rs-heavy captures on this cell (`fixture_105805`, `fixture2_110836`,
    `selfconf_smoke_105014`), but NOT in the ssb-only run. Read as an SNR/sensitivity difference
    (SSB's 240 REs give it far less integration gain than csi_rs's much larger footprint to resolve
    a weaker secondary reflector), not an axis disagreement -- the two sources agree exactly on the
    feature they both have the sensitivity to see (the LOS), which is the check this step asked for.

## 2. A real bug found and fixed during this task's live validation

**Before any fix, the FIRST ssb-only capture (`/home/sens/NICOLA/captures/ssb_only_142450/`) closed
ZERO CPIs over the full 150 s run** -- `grep -ac 'SENSING: CPI #'` = 0, despite 21522 individual
CFR submissions reaching the engine thread (`SENSING: engine thread stopped (processed=21522
dropped=0)`) and `cpi_slots=128` being sized sensibly for a 20 ms/row source (128 rows x 20 ms =
2.56 s, trivially achievable in 150 s -- ruled out per the brief's own troubleshooting note before
looking further).

**Root cause**, found by comparing the SSB tap's call site against the already-working `csi_rs`
tap: `phy_procedures_nr_ue.c`'s SSB tap (Task 3) passed `nr_isac_submit_cfr_multi()`'s `slot_idx`
argument as `proc->nr_slot_rx` alone -- which is **slot-within-frame only**, range `[0,
slots_per_frame)` (confirmed against its other uses in the same file, e.g. `nr_slot_rx == 0`,
`nr_slot_rx == 9` boundary checks). `csi_rx.c`'s existing CSI-RS tap instead computes an
**absolute** slot index: `proc->frame_rx * frame_parms->slots_per_frame + proc->nr_slot_rx`.

This cell's single SSB beam recurs at the *same* within-frame slot every occurrence (period 40
slots = exactly 2 frames), so every SSB submission carried an *identical* `slot_idx`. In
`sensing_engine.cc`'s `accumulate_cpi()`, the slow-time position of a new submission is derived
from the *delta* between its `slot_idx` and the previous submission's (`cpi_prev_slot`, wrapped mod
`slots_per_frame * 1024`); an always-zero delta made every submission's `this_span` equal to the
previous row's, so the `merge` test (`std::fabs(this_span - cpi_slot_span) < 1e-6`) was true for
every submission after the first -- every single SSB row silently folded into row 0, `cpi_row`
never advanced past 1, and `cpi_slots` (128 distinct rows) was never reached.

**Fix** (`openair1/SCHED_NR_UE/phy_procedures_nr_ue.c`, ~line 1449): compute
`ssb_abs_slot = proc->frame_rx * fp->slots_per_frame + proc->nr_slot_rx` and pass that as the
`slot_idx` argument instead of `proc->nr_slot_rx` alone -- mirrors `csi_rx.c`'s existing,
already-correct pattern exactly, no new logic invented.

**Verified**: rebuilt (`make nr-uesoftmodem -j8`, clean link, only the pre-existing unrelated
`log2_maxh` warning), re-ran the identical ssb-only capture
(`/home/sens/NICOLA/captures/ssb_only_143150/`) -- **56/56 CPIs closed**, `T_slot=40.000` on every
one, `vel[max]=2.2` m/s. All 4 existing NR_UE_ISAC test suites (95 tests total: 9 + 56 + 21 + 9)
still pass, zero regressions.

This fix is the reason §1's numbers exist at all -- without it, Steps 2-4 of this task would have
had nothing to measure.

## 3. Known limitation, unchanged from the roadmap

Unambiguous velocity is capped at +-2.17 m/s -- this source alone finds walking-pace targets, not
vehicles or drones. It is a guaranteed floor, not a replacement for the slot-rate sources; use it
fused with csi_rs/pdsch_* (Phase 1) for the combined coverage. Confirmed live: measured vel_max was
2.2 m/s, matching this exactly.

## 4. VOID_DL_ZERO is pre-existing and unrelated to the SSB source

Every capture in this document reports `run_arm.sh`'s generic verdict as `VOID_DL_ZERO`
(`dl_ldpc_ok=0`, `dl_tb=0%`, `sib1=1`). This is the **already-documented Phase 1 CSS0-autoconf gap**
(`PHASE1_CSS0_AUTOCONF_HANDOVER.md`: "Genuine SI-RNTI recovery is still 0 across three independent
live captures... a further blocker remains, out of scope"), inherited from the base conf
(`nrue.passive_rx.selfconf.conf`, which sets `pdcch_blind_monitor_autoconf = 1`) that this task's
brief explicitly named as the copy source. It is **not** caused by, or related to, anything in this
task -- the SSB sensing tap draws its rows directly from the PBCH tracking path
(`nr_process_pbch_symbol`), never from decoded PDSCH/PDCCH, so the blind-PDCCH/SI-RNTI decode
failure and the SSB CFR source are functionally independent subsystems that happen to share one
binary and one run.log. Not investigated further here -- it is squarely Phase 1's open item, not
Phase 2's.

## 5. Still open

- **The `occ[...]` CPI-summary log line does not print an `ssb=` field** (`sensing_engine.cc:1048,
  1059, 1071`) -- it lists `csi=/dmrs=/data=/blind=/pusch=/uldata=` only, a diagnostic gap left over
  from before `NR_ISAC_SRC_SSB` existed. Not a functional bug (CPIs close and detections are
  correct regardless -- confirmed via `reports.jsonl`'s `src_occ` array, which DOES carry the ssb
  count as its 7th element), only a readability gap in the human-facing log line. Cheap follow-up:
  add `ssb=%lu` to those three printf format strings and their argument lists.
  - **`reports.jsonl`'s `report_path` is a single fixed path shared across every conf/run on this
    host** (`/home/sens/NICOLA/captures/sens/reports.jsonl`, append-only, 30 MB / 5263 lines at the
    time of this task). This is the same class of trap documented elsewhere in this project's
    history (CLAUDE.md's Section 10, the retracted "491 fused track updates" number) -- any future
    reader of that file MUST filter by `cpi_start_time_utc_ns` against the specific run's own
    wall-clock window (from that run's `nic.csv` timestamps) before trusting a `ref_type` count or
    detection list out of it, exactly as done in §1 above. Not fixed here (no per-arm report path
    plumbing exists in this conf/harness today); flagged for whoever next needs to script off this
    file.
- **The bug in §2 means every measurement of the SSB source predating this task's fix (there were
  none -- Task 3's own report explicitly flagged "no live capture was attempted" as out of scope)
  never existed and cannot be retracted; there is nothing upstream of this document to correct.**
  Recorded here only so a future reader does not assume the tap "always worked" -- it did not, from
  the moment it was written until this task's fix.
- **k_ssb=0 remains a deployment-specific hardcode** (Task 3's own flagged concern, unchanged by this
  task) -- will silently produce a wrong subcarrier axis on any future cell with nonzero kSSB.
- Only single-antenna (`NANT=1`) captures were run here; the SSB tap's AoA/multi-antenna path
  (`nof_ant_clamped`, per-antenna packing) is exercised by the surrounding conf's
  `aoa_enable=1`/`rx_array` settings whenever `NANT>1`, but was not specifically validated with the
  SSB source in this task -- flagged, not required by this task's brief.
