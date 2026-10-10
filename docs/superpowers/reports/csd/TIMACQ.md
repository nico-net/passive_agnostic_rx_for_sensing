# TIMACQ: dedicated CORESET acquisition on PCI 923 (TIM)

Host: DGX aarch64 / GB10 (GPU host). Branch `csd/tim-acq` (base `csd/otatest` 59642975a7). Date 2026-10-10.
Offline only: no radio, no hardware tools, no sudo. Every build, test and replay ran through `heavy.sh`.
Evidence labels: [MEASURED, replay] = offline replay of the named capture through this branch's receiver; [MEASURED, OTA LIVE] =
the orchestrator's 2026-10-10 18:47 / 18:49 logs; [CODE-READ]; [INFERRED]. Cells are never merged: PCI 923 and PCI 415 are
reported separately.

## Root cause

[MEASURED, OTA LIVE + replay; CODE-READ] **The receiver was not tuned to the TIM carrier, and the dedicated-CORESET decode
assumed that receive-grid RB 0 is CRB 0 (Point A).**

- **The tuning.** TIM's SIB1 gives an 80 MHz, 217-PRB carrier with offsetToPointA = 38 (15 kHz units), MIB k_SSB = 10. The
  receiver was started at -C 3731.91 MHz with --ssb 1169. That rule (CARRIERS.md) only holds when offsetToPointA = 194, as on
  Vodafone.
- **Where Point A lands.** Point A sits at grid subcarrier 936 = **grid RB 78**. The live log says so itself: `ACQ carrier
  MISMATCH ... Point A at grid subcarrier 936 ... the cell's carrier centre is 3759.990000 MHz` (T1 run.log line 423). So CRB 0..138
  of TIM fall on grid RB 78..216, and CRB 139..216 are outside the receive grid.
- **GPU nID discovery is right.** It indexes the PDCCH DM-RS from Point A (`nr_pdcch_nid_algorithm.h`: `pilots[...]`,
  `re[point_a_sc + ...]`) and publishes grid RBs (`grid_rb = phase + 6*g0 + point_a_sc/12`). That is why it finds nID 1931/1929,
  and why it publishes `grid_rb=78 groups=23 L=0`: TIM's dedicated CORESET at CRB 0, 23 groups visible.
- **The dwell/bank decode is wrong here.** It builds a PDCCH-Config CORESET with BWPStart 0 and rb_offset = grid RB. `dci_nr.c`
  then uses `dmrs_ref = BWPStart` (0), so the DM-RS index is the grid RB, not the CRB. Every DM-RS is 78 RBs (234 sequence
  elements) off, the channel estimate is garbage, and no DCI passes CRC → "no fresh CRC evidence in 4000 occasions".
- **The cover misses it too.** The ACQ-FIX cover (grid RB 0, 36 groups) uses the same wrong DM-RS reference. Even with the right
  reference it would be misaligned: the CORESET's CCE j is cover CCE j+13, and 13 is odd, so no AL >= 2 candidate lines up.
- **Vodafone never showed it.** Its recipe puts Point A at grid RB 0.

### Candidates from the task, checked [MEASURED, replay, unless noted]

| candidate | verdict |
|---|---|
| interleaved / rb-Offset / grid the cover cannot catch | the CORESET is non-interleaved, at CRB 0; the cover was on the wrong grid origin (above) |
| duration 2/3 | no: verified at dur 1 |
| DCI 1_1/0_1 sizes | they differ (TIM len **47**, Vodafone 49), but the length sweep finds 47 by itself once DM-RS is right |
| SS / AL set | not the blocker: verified with the default ladder (USS inferred AL={1} dominant) |
| DM-RS scrambling 1931 vs 1929 vs PCI | **1931** verifies; 1929 is published but never verified in 12 s (limits) |
| precoder granularity | not the blocker (verified with the existing estimator) |
| TCI/beam | not observable/needed: acquisition works with 4-RX MRC |
| CFO seed sign | not the cause: both live syncs lock, residual 58 Hz (T1, -4800) and 203 Hz (T2, +6900) [MEASURED, OTA LIVE] |
| CORESET#0 BWP vs active BWP | related: the active BWP is 0+217 *CRB*, i.e. grid RB 78.. here — that is the root cause above |

**The oracle.** The same capture and the same build were decoded with each DM-RS reference (the flag selects it):
- With CRB 0 = grid RB 0: 0 DCI 1_1 CRC passes.
- With CRB 0 = Point A from SIB1: 4218 CRC-OK DCI 1_1 over 12 distinct C-RNTIs, all at len 47 on nID 1931 (RNTI-consistent: every
  RNTI recurs, and each locks the same length on the same CORESET).

## Fix (default off): `ISAC_PDCCH_POINTA_REF=1`

General mechanism, no TIM constants. Point A comes from the existing SIB1/MIB carrier check (`nr_passive_acq_verify_carrier()`).

- **`nr_pdcch_blind_monitor.c`:**
  - `nr_pdcch_pointa_ref_enabled()` reads the flag once.
  - `nr_pdcch_crb0_grid_rb()` returns the grid RB of CRB 0. It is 0 when the flag is off, SIB1 is not checked yet, or Point A falls
    between RBs. The value is cached and refreshed every 256 calls (the acquisition snapshot takes a mutex).
  - `nr_pdcch_cover_start_rb()` (pure) anchors the cover at CRB 0. If Point A is below the grid, it uses the first CRB multiple of
    6 inside the grid.
  - `nr_pdcch_discovered_snapshot()` puts the cover at that start with `(carrier - start)/6` groups, and its "cover already tried"
    skip follows it. With the flag off the start is 0, which is the old behaviour.
- **`dci_nr.c` `nr_rx_pdcch_symbol()`.** For a PDCCH-Config CORESET, `dmrs_ref = BWPStart - crb0`, where `crb0` is a per-thread
  value set only by the blind monitor (`nr_pdcch_set_crb0_grid_rb()`).
  - It is applied only while the index stays inside the generated sequence (`0 <= dmrs_ref + rb_offset` and
    `+ n_rb <= N_RB_DL`); otherwise the old value is kept.
  - CORESET#0 (MIB/SIB1 type) and the MAC-driven path are unaffected: the value is 0 outside the blind monitor.
- **`nr_pdcch_blind_monitor_rt.c` `run_occasion_one()`:**
  - Sets the per-thread CRB 0 at entry and restores the previous value on every return (a `cleanup` guard), so every bank, USS,
    lane and dwell decode of the occasion uses it.
  - Skips the byte-compared GPU PDCCH FEP for PDCCH-Config CORESETs when the offset is non-zero, because its job struct carries the
    old DM-RS reference and would only mismatch. The CPU LLRs are used; the `.cu` file is not touched.
- **Unit test:** `PdcchPointA.CoverStartFollowsCrb0` covers 0, 78, -5, -12 and 214.

## Results

### PCI 923 (TIM), capture raw_pci923_tim_4s_1010_1853 tiled 3x (12 s, `raw_pci923_tim_12s_tile3`), t26m env minus radio flags, 0.25x [MEASURED, replay]

| arm | tuning | flag | dedicated dwells / rejected | first bank add (s after sync) | CRC-OK DCI 1_1 (distinct C-RNTIs) | PDSCH TB CRC ok / decoded |
|---|---|---|---|---|---|---|
| tim_off_1 (live reproduction) | -C 3731.91 --ssb 1169 | off | 4 / 3, cover 0/36 and 78/23 rejected | none | 0 (0) | 0 / 0 |
| tim_pa_1 | -C 3731.91 --ssb 1169 | **on** | nID 1931 78/23 verified first | **0.63** | 4218 (12), len 47 | 0 / 8070 |
| tim_rt_off_1 | **-C 3759.99 --ssb 233** | off | 1931 0/36 cover verified | **0.57** | 1364 (11) | 0 / 2001 (crashed at ~5 s, below) |
| tim_rt_pa_1 | -C 3759.99 --ssb 233 | on | 1931 cover verified | 0.63 | 428 (7) | 0 (crashed, below) |
| tim_rtnc_pa_1 | -C 3759.99 --ssb 233, CSI-RS disc off | on | 1931 verified | 0.65 | 2053 (12) | 17 / 5531 |
| tim_rtnc_pa_2 | same | on | 1931 verified | 0.55 | 1486 (11) | 16 / 4690 |

**Dedicated CORESETs bank and C-RNTIs solve at the PDCCH level in every fixed arm** (0.57–0.65 s after sync).

PDSCH "SOLVED" RNTIs (two independent TB CRC passes in the PDSCH solver) are **0 on TIM in every arm**. This is a separate,
downstream problem outside this task's files:
- **Untuned grid.** The PDSCH DM-RS reference has the same CRB 0 = grid RB 0 assumption (`nr_pdsch_passive_decode.c`:
  `dmrs_ref_rb = refPoint ? BWPStart : 0`), and the FDRA is on a BWP that starts at grid RB 0.
- **Retuned grid.** Point A is at grid RB 0, so that issue is gone, yet the solver still reports `probe_ok=0`. The receiver flags
  `RANK ... pair-coherence low 77 % => CONTRADICTED` (single-port assumption contradicted), which points at a multi-layer / DM-RS
  configuration difference on TIM [INFERRED]. Handed to the PDSCH/DECODE-RES owner.

### PCI 415 (Vodafone) capture b `raw_pci415_12s_b`, t26m env minus radio flags, 0.25x [MEASURED, replay]

| arm | first bank add (s) | second bank add (s) | SOLVED RNTIs |
|---|---:|---:|---:|
| vf_pa_1 (flag on) | 1.11 (62922) | 2.87 (10163) | 26 |
| vf_pa_2 (flag on) | 1.07 (10163) | 1.31 (62922) | 30 |
| vf_off_1 (control) | 1.07 (10163) | 1.31 (62922) | 28 |

Gate (banks within ~2.5 s, >= 15 solved): **met, 2/2**. On this cell Point A = grid RB 0, so the flag is a no-op by construction:
`crb0 = 0`, the cover start is 0 and `dmrs_ref` is unchanged. The run-to-run spread is the known GPU-publication order (ACQFIX Q3).

Times are RF-sample seconds from the first receiver timestamp, close to sync (`ts=` of the TIMEMUT lines).

### Unit tests [MEASURED, DGX]

ctest `test_nr_pdcch_(blind_monitor|coreset_bank|coreset_map|nid_worker*|scan_fast)`: **10/10 pass**, including the new `PdcchPointA.CoverStartFollowsCrb0` and the ACQ-FIX `DiscoveredCover.*` tests (flag-off cover behaviour unchanged).

## New defect found (not in my files): CSI-RS sensing stack overflow on TIM

[MEASURED, replay, gdb] With the retuned TIM grid, the receiver died about 5 s in, in 2/2 runs. The gdb backtrace:
`SIGSEGV in nr_ue_csi_rs_sensing_capture_shifted (csi_rx.c:1344) <- pdsch_processing (phy_procedures_nr_ue.c:2417)` on thread
"DL__actor".

The faulting line is the VLA `c16_t csi_rs_received_signal[nb_rx][samples_per_slot_wCP]` (983 KB). Line 1341 adds two more:
`[nb_rx][ports][4096]` x 2 = 1 MB for an 8-port resource. TIM's discovered CSI-RS are multi-port, so the frame overflows the
actor thread's stack:
- `MONITOR_ADD row=7 fd=40 rb=0:203`, 4 ports;
- `row=6 ... rb=0:208`, 8 ports.

Vodafone's TRS is 1-port, which is why it never showed up there.

- **Fix suggestion** for the CSI-RS owner: per-thread static or heap buffers instead of the VLAs.
- **Workaround** until then: `ISAC_CSIRS_DISC=0 ISAC_CSIRS_FULL=0` on TIM. With it, the retuned runs reach EOF.
- **Live T1 did not crash.** In the untuned grid its logged metrics show csirs_confirmed 0, so no multi-port monitor was ever added.

## Exact OTA env + args

Base env file: `/home/nicola/NICOLA/wt/rr-orchestration/ota/t26m.env` (the 18:47 TIM env).

**TIM / PCI 923 (recommended: tuned to the SIB1 carrier centre)**
```
env: $(cat ota/t26m.env) ISAC_PDCCH_POINTA_REF=1 ISAC_CSIRS_DISC=0 ISAC_CSIRS_FULL=0
args: -C 3759990000 -r 217 --numerology 1 --band 78 --ssb 233 --initial-fo -4800   (rest of the 415 recipe unchanged)
```
- **General rule.** -C = `ISAC_ACQ_RETUNE centre_hz` from the first run's log. --ssb = (k_SSB + 12*offsetToPointA)/2 at mu = 1:
  (10 + 456)/2 = 233. For PCI 415 the same rule gives (14 + 12*194)/2 = 1171.
- **The FO seed** is the X410 LO seed of the current calibration. T1 synced with -4800 and T2 with +6900, both fine. Keep the one
  that syncs.
- **What to look for** in the live log:
  - no `carrier MISMATCH` line;
  - `discovered CORESET ... nid=1931 ... VERIFIED -> bank add ... len=47` in < 1 s;
  - `DCI 1_1 length locked ... len=47` for many C-RNTIs.

**TIM / PCI 923 at the old tuning** (e.g. to keep the grid shared with something else):
```
env: $(cat ota/t26m.env) ISAC_PDCCH_POINTA_REF=1
args: -C 3731910000 -r 217 --numerology 1 --band 78 --ssb 1169 --initial-fo -4800
```
Here PDCCH acquisition works (bank add at 0.63 s, 12 C-RNTIs), but PDSCH cannot decode (PDSCH DM-RS/FDRA origin), and CRB
139..216 are not received.

**Vodafone / PCI 415 (unchanged recipe, flag harmless)**
```
env: $(cat ota/t26m.env) ISAC_PDCCH_POINTA_REF=1
args: -C 3680010000 -r 217 --numerology 1 --band 78 --ssb 1171 --initial-fo 6900
```

Replay commands (wrapper `tests/passive_rx/timacq_replay.sh`, takes heavy.lock):
```
CAP=/home/nicola/NICOLA/ota_runs/raw_pci923_tim_12s_tile3 CAP_SAMPLES=1474560000 RXC=3759990000 SSB=233 \
  tests/passive_rx/timacq_replay.sh tim ISAC_PDCCH_POINTA_REF=1 ISAC_CSIRS_DISC=0 ISAC_CSIRS_FULL=0
CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 CAPC=3680010000 RXC=3680010000 \
  SSB=1171 FO=6900 tests/passive_rx/timacq_replay.sh vf ISAC_PDCCH_POINTA_REF=1
```
The tiled TIM capture is `tests/passive_rx/survey/tile.sh raw_pci923_tim_4s_1010_1853 raw_pci923_tim_12s_tile3 3`.

## Limits

- **Replay passband.** In the retuned replay the capture is centred 28 MHz below the carrier, so grid RBs above ~170 are at or past
  the capture passband edge. Live tuning has no such loss.
- **nID 1929** (published by GPU discovery, live and replay) is never verified in 12 s. It may be an idle second CORESET or the
  interleaved shadow published at grid 126/132. Not needed for acquisition.
- **PDSCH solving on TIM is open** (see Results). This task makes the PDCCH side work.
- **No integration unit test** of the Point-A DM-RS decode through `dci_nr.c`: the existing synthetic fixture decodes through the
  rank grid, not `nr_rx_pdcch_symbol()`. The replays are the evidence.
- **UL-LAYOUT merge.** My `nr_pdcch_blind_monitor_rt.c` edits are confined to the top of `run_occasion_one()` (guard) and the GPU
  PDCCH FEP condition.

## Self-check

- **Helpers called:**
  - `nr_pdcch_pointa_ref_enabled` (crb0);
  - `nr_pdcch_crb0_grid_rb` (snapshot, run_occasion_one);
  - `nr_pdcch_cover_start_rb` (snapshot, unit test);
  - `nr_pdcch_set_crb0_grid_rb` (run_occasion_one, cleanup);
  - `crb0_guard_release` (cleanup attribute).
- **Flag read:** `ISAC_PDCCH_POINTA_REF`.
- **Flag off is unchanged:**
  - crb0 = 0 → `dmrs_ref` keeps `BWPStart`;
  - the cover start is 0 with `carrier/6` groups and the same skip test;
  - the GPU FEP condition reduces to the old one.
- sens6 gate clean. No `.cu`, `_gpu*`, PDSCH or CSI-RS file edited.
