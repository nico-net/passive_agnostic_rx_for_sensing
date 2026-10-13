# BLINDPIPE 2026-10-13: blind dedicated-CORESET pipeline -- dwell starvation fix, DL traffic probe, Point-A sweep, duration sweep

Branch `csd/coreset-fable` (from `csd/otatest` db826ff278), worktree `rr-csd-fco`, build dir `build_t26` (CUDA, DGX GB10 aarch64).
Labels (PROJECT_MEMORY 0.1): `[MEASURED, replay <capture>]` = DGX replay 0.25x, 4 RX, ENVFILE t26z.env + the arm's flags,
`n` runs per arm as stated (indicative, never pooled across captures); `[MEASURED, OTA log]` = ota_runs logs read; `[OFFLINE VERIFIED]` =
unit tests; `[CODE-READ]`; `[NOT RUN]`. Cells and captures are never merged. Everything new is default OFF; the proposed default-on set
is section 7.

## 1. Dwell starvation: root cause and fix (ISAC_PDCCH_DWELL_OWN_BUDGET=1)

Root cause `[MEASURED, replay raw_pci923_tim_12s_tile3, rr-csd-cal replay_e923_off1_232315 (t26z + CARRIER_FROM_SIB1, flag off)]`:
with a discovered-CORESET dwell active, `run_occasion()` split the per-occasion candidate budget (64) between the banked CORESETs and
the dwell (`nr_pdcch_discovered_candidate_share`: 32 + 32). The candidate ladder fills AL2, AL4, AL8 first and AL1 last, so the banked
36-CCE TIM CORESET went from the `ladder: num_cces=36 ncand=64/64 (AL1=33 AL2=18 AL4=9 AL8=4)` it ran before the dwell to
`ncand=32/32 (AL1=1 AL2=18 AL4=9 AL8=4)` for every occasion while a dwell ran (39 ladder lines): one AL1 position in 36 per occasion.
TIM's C-RNTI DCIs are AL1/AL2. C-RNTI DCI 1_1 accepts per 1000 slots: 489 / 389 / 357 / 351 / 332 before the second dwell started
(abs slot 20232), 19 / 18 / 39 / 139 / 33 / 13 / 17 / 14 / 19 / 15 / 42 / 132 / 38 / 18 / 21 after it (dwells back to back until the end of
the capture). `tests/passive_rx/blindpipe_stats.py` on that log: accepts after the first bank add inside dwell windows 38 / 1000 slots,
outside 383 / 1000. The scan queue was not the mechanism (`scanq_drop_full 0`, `drop_stale 3`), nor the LLR cache (off), nor the USS
tracker (keyed per nID).

Fix `[CODE-READ]` (`nr_pdcch_blind_monitor_rt.c`, `dwell_own_budget()`): with the flag, bank passes keep `t_candidate_share = 0` (the legacy
full budget, byte-identical to the no-dwell path) and the dwell gets `NR_PDCCH_BANK_CANDIDATE_BUDGET` of its own, at all four share sites
(serial banks, PAR_BANKS helper, DED_EVERY_SLOT path, legacy path). The time cost of the extra 32 dwell candidates stays bounded by the
existing `ISAC_PDCCH_DED_BUDGET_US` / `_SHARE` skip rule. The original `nr_pdcch_discovered_candidate_share()` is unchanged (flag off).

Result: section 5 table (C-RNTI accepts inside vs outside dwells, PDSCHQ crc_ok, CRC-OK RNTIs).

## 2. DL traffic probe (ISAC_DL_TRAFFIC_PROBE=N) -- "no UE-specific traffic" vs "CORESET not found"

`nr_dl_traffic_probe.[ch]` (pure, `[OFFLINE VERIFIED]` test_nr_dl_traffic_probe 4/4) + `nr_dl_traffic_probe_rt.[ch]` (RX-actor mailbox, one
DL slot in N, antenna 0, SCHED_OTHER worker, FEP as nr_csirs_disc_td.c: FO compensation, FFT, symbol rotation).
- Statistic: energy per (symbol, RB) cell; a cell is lit when it exceeds the slot's noise-floor mean by the chi-square(24 dof) 1e-4 tail
  (2.4422 x; 12 REs x I/Q of one antenna); the floor mean is the slot's 10th percentile / 0.6524 (chi-square p10 / mean). No absolute
  level, no cell constant.
- Masks: RBs outside the SIB1 carrier (grid when unknown), the SSB band (20 RB), CORESET#0 in its symbols, and every PDSCH allocation the
  receiver enqueued (grid RBs, symbols) -- registered by `nr_pdsch_passive_queue_enqueue()` via `nr_dltp_note_grant()`.
- Reported per 256 probed slots (`TRAFFIC ...` line): control-region lit RBs/slots outside CSS0 per symbol 0/1/2 (ungranted), data cells
  lit / granted-lit / ungranted-lit, PDSCH-like RB-slots (>= 3 consecutive lit data symbols: a 1-symbol CSI-RS or a 2-symbol TRS pair
  cannot make one) and the ungranted part, slots with unknown TDD pattern, and a verdict: PRESENT when ungranted PDSCH-like RB-slots >= 8
  (null false-alarm bound ~1e-11 per RB-slot; 8 absorbs leakage next to granted RBs), ABSENT when >= 100 probed slots and none, else
  UNDECIDED. Known confounders are stated in the line: a co-channel cell on the same carrier (PCI 4 on 508's carrier) is traffic to this
  probe; with the TDD pattern unknown (no SIB1) UL slots are probed too.
- `pdcch_pci_windows` (needs ISAC_POINTA_SWEEP=1, carrier known): lit 6-RB windows of the PDCCH DM-RS with nID = PCI outside CSS0 on the
  known CRB grid -- direct dedicated-PDCCH evidence for the commonest configuration (pdcch-DMRS-ScramblingID absent).

## 3. Blind Point-A sweep for cells without a SIB1 carrier (ISAC_POINTA_SWEEP=1)

Cells without SIB1 (PCI 4) or with an NSA-style SIB1 (Iliad 403; also the lab PCI 421 capture: MIB barred) never opened the nID sweep:
`nr_pdcch_coreset_map_idsweep_want()` needs `carrier_verified` (CRB 0 for the PDCCH DM-RS sequence index, TS 38.211 7.4.1.3.1).
The sweep (worker of section 2) correlates symbols 0-2 of every probed slot against the nID = PCI PDCCH DM-RS for every hypothesis
h = CRB index of grid RB 0 in [0, 275), on the 6-RB group grid implied by h, CORESET#0 and SSB RBs masked; a window is lit above 4x the
pure-noise mean |corr| (0.209 for 18 pilots: the existing nr_pdcch_coreset_map bar). When one hypothesis has >= 6 lit windows and >= 4x the
runner-up, `nr_passive_acq_note_carrier_hypothesis(-12 h)` publishes Point A (carrier taken as the grid; the DL BWP tracker learns the
width), which opens the GPU nID sweep and the dwell path exactly as a SIB1 verdict does. Cost: ~0.5 M complex MACs per probed slot on the
worker. Only nID = PCI is swept (a UE-specific nID x 275 hypotheses is GPU work: next step, section 8). Needs the receiver grid aligned to
the CRB grid (`ISAC_KSSB_ALIGN=1`, in t26z). Result: section 5 (PCI 421 capture, expected h = 0 from the calibration memory).

## 4. GPU nID sweep over durations 1-3 (ISAC_COREMAP_IDSWEEP_DUR=3)

`[CODE-READ]` The GPU stage already implements multi-symbol capture and an explicit-duration shortlist (`nr_pdcch_nid_gpu.cu`: `calibrate`,
`screen`, `combine`, `list_dur`, stage-2 `explicit_duration`), default `dur = 3` when the GPU is on; t26z pins `ISAC_COREMAP_IDSWEEP_DUR=1`,
so every record and every cover dwell was duration 1 and ENUM had to enumerate durations blindly. The generalisation is the env change;
section 5 measures its cost (stage1/stage2 ms per pass) and whether the 415/923 records keep their nID at dur 1 (control).

## 5. Replays (fill from tests/passive_rx/blindpipe_replay.sh, blindpipe_stats.py)

TODO table per capture: arm, runs, first bank (slots after sync), bank adds, C-RNTI accepts in/out dwells, PDSCHQ crc_ok/decoded, CRC-OK
RNTIs, CRC by MCS bucket, NID passes (stage ms, records by dur), TRAFFIC verdict, POINTA result.

## 6. Audit (d): remaining grid assumptions downstream of a verified CORESET on the 508 geometry (162 PRB in a 217 grid) `[CODE-READ]`

With `ISAC_CARRIER_FROM_SIB1=1` (CARRIERGEOM) the dwell BWP, DCI 1_0/1_1 RIV sizing, CORESET start, cover width, PDSCH RB origin and DM-RS
CRB0 follow the SIB1 carrier. Still on the grid (`fp->N_RB_DL`) in `nr_pdcch_blind_monitor_rt.c`:
- UL layout lock cell `carrier_rb = N_RB_DL` (lines ~8162, ~8185): UL BWP hypotheses up to 217 on a 162-PRB cell; the true width is in
  the set (`ISAC_UL_LOCK_BWP_ALL`), so slower, not wrong. Owner rr-csd-upr.
- Passive BWP tracker `nr_pbwp_init(N_RB_DL, ...)` (~5574): enumerates RIV sizes over the grid; a superset, not an assumption.
- Sensing API carrier `nof_prb = N_RB_DL` (~9912): metadata only.
- Technique A CPU bootstrap fixes `disc_symbol = 0` (ponytail comment, ~4480): a cell with its dedicated CORESET at symbol 1-2 is only
  found by the GPU sweep (rotates 0-2) -- acceptable since the GPU path is the live one, noted.
- The LLR cache and GPU PDCCH FEP keys carry `carrier_rb = N_RB_DL`: cache keys, not geometry.
No remaining 508-specific constant found in the dwell -> DCI 1_1 layout -> solver -> PDSCH chain beyond those listed.

## 7. Proposed env `tests/passive_rx/t26b.env.proposed` (single line) and OTA spec lines

t26b = t26z + `ISAC_CARRIER_FROM_SIB1=1 ISAC_PDCCH_DWELL_OWN_BUDGET=1 ISAC_DL_TRAFFIC_PROBE=16 ISAC_POINTA_SWEEP=1` and
`ISAC_COREMAP_IDSWEEP_DUR=3` (replacing `=1`). TODO: confirm each from section 5 before installing.

OTA spec lines (TAG:ENV:CFG:FREQ:SSB:FO; env = t26b unless a delta is given; cfg = the cfg of the named earlier run):
```
B415a:t26b:<cfg of ab_A1>:3680010000:1171:6900          # 150 s control: time-to-bank and CRC-OK RNTIs as t26z; TRAFFIC PRESENT with ungranted << granted
B923a:t26b:<cfg of ab_Tz1>:3759990000:233:-4800         # 150 s: C-RNTI accept rate must NOT drop while "dwell start" lines are active (blindpipe_stats.py in/out)
B508a:t26b+ISAC_PDCCH_DISC_ENUM=1:<cfg of ab_U508a>:3590190000:517:-4842   # >= 300 s: TRAFFIC verdict (PRESENT => CORESET problem, ABSENT => no UE traffic), NID records dur 1-3, pdcch_pci_windows
B508b:t26b+ISAC_PDCCH_DISC_ENUM=1:<cfg of ab_U508a>:3590190000:517:-4816   # alternative FO seed
B4a:t26b_pci4+ISAC_PDCCH_DISC_ENUM=1:<cfg of ab_P4>:3590190000:517:-4842   # ISAC_TARGET_PCI=4, >= 300 s: expect "POINTA_SWEEP undecided" lines, then "ACQ carrier HYPOTHESIS" if nID = PCI PDCCH exists, then NID passes
B403a:t26b:<cfg of ab_X403>:3625290000:1171:-2400       # >= 300 s: NSA cell -- the same POINTA_SWEEP path; TRAFFIC PRESENT would mean EN-DC UE traffic without a found CORESET
B675a:t26b:<cfg of ab_W675a>:3426570000:1171:-4881      # 150 s weak TIM cell: sync + TRAFFIC line only; no change expected
```
Read with `python3 tests/passive_rx/blindpipe_stats.py <run dir>`: time-to-first-bank, bank adds, C-RNTI accepts in/out dwells, ladders
(no `ncand=32/32` line may appear on a banked CORESET with the flag), NID pass times and records by dur, TRAFFIC and POINTA lines.

## 8. Not done / next

- UE-specific nID x Point-A hypotheses (GPU): the sweep kernel would need a sliding CRB reference; bounded by ranking hypotheses from the
  nID = PCI sweep first.
- The traffic probe reads antenna 0 only (4-RX MRC would lower the floor by ~6 dB); PCI 4 vs 508 cannot be separated without decoding.
