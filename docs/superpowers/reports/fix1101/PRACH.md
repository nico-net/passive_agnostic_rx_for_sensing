# fix1101/prach: PRACH sensing exports nothing

Base cand1101 (ac5c4ec2af). Branch fix1101/prach. All numbers `[OFFLINE VERIFIED, DGX aarch64 unit]` unless stated.

## What was checked (chain vs TS 38.211 6.3.3 / 38.104)
pci 415 (SIB1 in rx.log): DDDDDDDSUU, prach-ConfigurationIndex 3 (format 0, x=2 y=0, subframe 9 = slots 18-19, UL), msg1-FDM 1, msg1-FrequencyStart 8,
217 PRB @30 kHz, 122.88 Ms/s, 4 RX. (pci 923: idx 9, format 0, subframe 7, bwp 0+217, fstart 3.) Root, zeroCorrelationZoneConfig, restricted set are NOT logged
(new log line below). Checked: occasion table / slot / symbol, UL-symbol gate, window start (slot - N_TA_offset + N_CP + 16Ts ext), CP removal,
98304-pt DFT, tone map (K=24, kbar=1, k1 = 12*n_start - 6*N_RB_UL, N_RA_RB=3 PRB), ZC roots/cyclic shifts/NCS, margin units (dB/10 of the strongest
preamble power, per-antenna normalised, floor = p25 of the last 32 peaks).
Result: the chain is correct for a preamble with the cell's parameters (test below). No scale bug pins the margin: idle peaks are 9.0-11.1 dB, spread ~1.5 dB.

## Findings
1. `[OFFLINE VERIFIED]` PRACH ignored the receiver's carrier offset. The capture is not CFO-corrected (capture_info.json receiver_corrections_applied=false; cell
   shows -4827 Hz = 3.9 preamble tones). Every other consumer removes ue->dl_Doppler_shift+freq_offset (+ branch FO) per consumer; the PRACH snapshot did not.
   With -4827 Hz injected the peak still exists but lands on the wrong index (17 -> 18) and delay (288 -> 3648 samples): RAPID/TA never matches the RAR.
   Fixed: the snapshot is de-rotated (own double-anchored phasor; nr_fo_compensation drifts ~1 kHz-equivalent over 98304 samples) before the DFT.
2. `[NOT REPRODUCED]` the on-air symptom "margin_max = 1" (= margin 0, the 1 is the sentinel) means the live peaks were CONSTANT (almost surely 0). The same
   capture through the production hook offline gives varying peaks, so the live cause is not in the correlator/geometry as exercised here. Remaining suspects
   that only a live run can separate: live window content / ring alignment, N_RB_UL / k1 differing from the SIB1 view, gain-reset of the floor. The new counters decide it.
3. 'No Msg1' cannot be excluded either: RA-RNTI VERIFIED shows Msg1 happened on air, but a 4 s / 100 s capture at one site holds few.

## Changes
- nr_prach_passive.c: `derotate()` + per-job `fo_hz[]` (common + branch, same total as the OFDM front end); per-occasion counters; one-time `PRACH CONFIG` log line
  (idx, format, L, root, n_roots, zczc, restricted, fdm, k1, start symbol, N_RB_UL, fs); test_reset clears the new counters.
- nr_prach_passive.h: `nr_prach_passive_peaks_t`, `nr_prach_passive_diag_peaks()`.
- nr_passive_metrics.{h,c}, nr_passive_metrics_json.c: ISAC_METRICS gets prach_peak_last/min/max/mean, prach_floor_last, prach_peak_zero, prach_seeded.
- tests: nr_prach_passive_test_bridge.{c,h} `test_prach_real_iq` (spec-derived 4-RX format-0 preamble generated independently of the receiver, added to real capture
  IQ at a measured SNR, run through nr_prach_passive_process + worker); nr_prach_passive_test.cc: RealIqDetectsInjectedPreamble (36 cases: SNR -10/-5/0 dB per
  antenna sample = +20.7 dB in-band, preambles 0/17/40/63 across roots, delays 0/300/1500: right index, delay within 1 bin = 96 samples, margin >= 8 dB),
  RealIqDetectsPreambleUnderCarrierOffset (-4827/700/2500 Hz), RealIqIdleCellStatistics (no detection, no zero/constant peaks).
  The test skips if the capture is absent. The CFO test FAILS on cand1101 (wrong index/delay, shown before the fix); the no-CFO tests pass on cand1101.

## Occasion gating (coordinator item 1)
Already the case on cand1101 and verified by the tests: `nr_prach_passive_occasion()` derives (SFN mod x = y, slot, start symbol, N_t_slot, duration, format) from
prach-ConfigurationIndex (TS 38.211 6.3.3.2-2/-3), the UL-symbol gate uses the SIB1 TDD pattern, msg1-FDM x msg1-FrequencyStart give the frequency occasions, and the
correlator only tests the 64 preambles of (root, NCS, restricted set). 50 occasions/s for idx 3. Counted: `prach_occasions`, `prach_skipped`, `prach_drops`.

## RAR look-back (coordinator item 2)
- The worker keeps the last 2 processed occasions (de-rotated snapshot, `PRACH_RETAIN`, pre-allocated with the cell: +2 x 17.6 MB at 4 RX, added to
  `nr_prach_passive_cell_heap_bytes`; no allocation after start). A decoded RAR (`nr_prach_passive_rar_pdu`, now also carrying the TC-RNTI) is matched to the newest
  retained occasion by RA-RNTI (s_id, t_id, f_id) and tested for its RAPID only, in the worker, after the job (never on a decode thread).
- Single-hypothesis detector: IFFT of the occasion for the RAPID's root, peak over the NCS2 bins of its cyclic-shift zone vs the occasion's own trimmed mean bin
  energy, noise exponential-tail bound (valid for correlated RX branches; measured effective Gamma order 2-3, not 4): threshold = mean * ln(NCS2 / 1e-3).
  Blind needs 8 dB over the best-of-64x-delays floor. Unrestricted sets only; restricted-set cells count `prach_lookback_noring`.
- Duplicate guard: skipped when the blind path already exported or still holds that RAPID for the occasion.
- Record: normal PRACH record with rar_match true, rar_rapid, rar_ta and `rnti` = TC-RNTI (nr_sense_records.c no longer forces rnti null for PRACH when set).
- Counters in ISAC_METRICS: prach_lookback_tried / _found / _noring.
- Test RealIqLookbackFindsWhatBlindMisses (real capture + synthetic preamble, 4 RX): at -34 and -33 dB per-antenna sample SNR blind gives 0 records and
  look-back 1 (right RAPID, TA 20, TC-RNTI 0x4a5b, rar_match true); at -32 dB blind finds it (and look-back adds no duplicate); at -36 dB and below neither.
  RealIqLookbackAbsentPreambleStaysAbsent: 12 RARs naming preambles that are not on the air over real noise: 0 found, all 12 tried.
- Not done: the retain depth is fixed at 2 (not sized from ra-ResponseWindow: it is not in the SIB1 prior struct); a cell with several occasions per RAR window
  would lose older ones (`noring`). Measured false-alarm rate only 12 trials.

## What to read on air (ISAC_METRICS, per cell)
- prach_peak_zero ~ prach_occasions, or prach_peak_min == prach_peak_max: correlator/window broken (not "no Msg1").
- peaks spread 1-2 dB around prach_floor_last, prach_seeded > 0, prach_detections 0: idle, no Msg1 in range.
- prach_peak_max - prach_floor_last >= 80 with no record: export problem. Records with rar_match true: chain works end to end.
- `SENSING: PRACH CONFIG ...` line gives the exact parameters the receiver uses.

## Cost / threading (request from sensing-async)
- Thread: the producer is the UE slot thread (nr_prach_passive_process at UL slot, memcpy of the 98304-sample x 4 window, ~1.5 MB `[HYPOTHESIS]` < 0.3 ms, not
  timed); all correlation runs on the cell's own worker (nr_core_plan_create, core -1: SCHED_OTHER on the background CPU set / float lane, HOUSE pool lease),
  bounded queue of 4 jobs, overflow counted in prach_drops. It is NOT on a PDSCH/PDCCH/scan thread and not RT.
- Already minimal: only real occasions (50/s for idx 3), one DFT per antenna per occasion. Per-call cost, unit bench, flock -x /tmp/td_measure.lock + heavy.sh
  (nice 19 shared host): before 4.42 ms/occasion (98304-pt DFT x4 = 4.4 ms, correlator 0.09 ms), after 5.15 ms (de-rotation +0.65 ms). I did not find a safe
  further cut (cost now 5.15 ms incl. de-rotation; the retain copy ~1.5 MB per occasion is inside the 5.15): a decimating front end (boxcar/FIR) folds other UEs' PUSCH into the band; a pruned DFT is not cheaper. Cost does grow by 15 %; the price of correct
  RAPID/TA. Not changed: queue depth, priority. Remaining lever (not done): run the DFT only for occasions after a cheap gate, or 2 of 4 antennas (-3 dB).

## Addendum (replay cand1101c): zero peaks = fixed-point underflow
Replay snapshot power was 17.5 dB (prach_snap_db 171-182) vs 29.5 dB in the raw capture (21 LSB rms). The correlator energy is the integer (sum|ifft|^2 >> 10)/n_rx; at that
input level it is 0 for every bin, so peak == floor == 0 and nothing can ever be detected. Fix: lift the occasion's frequency bins by a power of two to rms >= 16 (never
lowered, int16 headroom kept) and refer the energy back to the input scale (-6.02 dB per bit); lookback does the same. Test RealIqWeakInputStillCorrelates (capture scaled
by 0.25 and 0.1): fails without the lift (41/41 occasions peak 0), passes with it. Also fixed earlier: root index truncated to uint8 (449de0bc61).
