# FRONTLIVE 2026-10-13: fast automatic blind front end for the live chain (n78/n77, 30 kHz SCS)

Host: DGX aarch64 (GB10), numpy 1.26 / scipy 1.11, CPU only (no CuPy/torch installed), run through heavy.sh (nice 19, 10 cores shared with other agents). Offline analysis of existing X410 34B2AF1 captures; no OTA, no capture, no receiver run in this task. Cells/captures are not merged across centres.

## Deliverables
| item | file |
|---|---|
| fast scanner (cells.json, same schema as autofront.py discover + `n_cells`, `dt_samples_7p68`) | `tests/passive_rx/autofront/fastscan.py` |
| live chain driver (capture / discover / all / pass2) | `tests/passive_rx/autofront/live_chain.sh` |
| pass-2 hook (`autofront.py refine`, `apply_refine`) | `tests/passive_rx/autofront/autofront.py` |
| unit tests (CFO arithmetic, SSS group, timing clusters, fold period, keep/dedupe, cells.json/pass-2 JSON, spec lines) | `tests/passive_rx/autofront/test_fastscan.py` (6 ok), `test_autofront.py` (5 ok) |
| `SAMPLES=` env in survey_capture.sh (default unchanged, 0.3 s) | `tests/passive_rx/survey/survey_capture.sh` |
| data | `docs/superpowers/reports/csd/frontlive_data/` |

## Method [IMPLEMENTED]
Stage 1 (detect): M=12 chunks of 20 ms x 4 ch; one 2.46 M-point FFT per chunk/channel; every GSCN band cut in frequency at 3.84 MS/s; PSS correlation by FFT for the 3 N_ID_2, |cc|^2 summed over channels, folded modulo k*20 ms; threads over GSCNs (scipy.fft releases the GIL). One CFO hypothesis (0): the PSS symbol loses <= 1 dB at +-7 kHz, so the 3-point CFO grid of autofront.py (3x the cost) is dropped. Burst period k is the smallest period whose phase class holds the energy (the old max over k is biased to k=8).
Stage 2 (verify, top 40 GSCNs by excess): short raw windows (32768 samples, mixed to the GSCN, decimated to 7.68 MS/s) around the folded PSS time of EVERY k-th chunk of the whole capture (up to 40), per-occurrence PSS timing, timing clusters (co-located cells), SSS -> PCI with group test, PBCH-DMRS -> i_ssb, CFO from the phase between the DMRS channel estimates of PBCH symbols 1 and 3 (lag 548 samples, unambiguous +-7007 Hz = the +-2 ppm tolerance at 3.6 GHz).
New vs autofront.py: (a) ~35x faster; (b) co-located cells with the same N_ID_2 are separated (PCI 508 + 4: best/second SSS is only 1.3 when both are present, so the rule is "smallest group of top scores with amplitude gap >= 2 to the rest", DMRS bar 2.0 for groups, 2.5 for single, SSS gap >= 2 instead of 3); (c) CFO from DMRS, not PSS->SSS.

## Runtime [MEASURED, DGX, heavy.sh, 6 captures in one process]
- 42.5 s for 6 captures of 1 s x 4 ch = 7.1 s per capture (stage 1 ~6.3 s for the +-50 MHz / 70 GSCN scan, stage 2 0.5-1.0 s) including numpy import; < 10 s target met. autofront.py needed ~4 min per capture. A 0.3 s capture (15 chunks, the survey_capture.sh default) is as fast.
- Under heavy.lock with other agents running the same scan took up to ~15 s (loaded host, 10 cores shared) [MEASURED, first version with 24 chunks; M=12 halves it].

## Validation, 1 s x 4 ch survey captures + 12 s captures (1 s window from t=0) [MEASURED, DGX]
| capture (centre) | GSCN / MHz | PCI | i_ssb | SSS gap | DMRS ratio | CFO Hz | pass-1 -C / --ssb |
|---|---|---|---|---|---|---|---|
| survey c3330 | none | - | - | - | - | - | (nothing expected) |
| survey c3430 | 7795 / 3426.24 | 675 | 0 | 3.17 | 3.19 | -4849 | 3426570000 / 1171 |
| survey c3550 | 7895 / 3570.24 | 508 | 1 | 2.14 | 2.81 | -4551 | |
| survey c3550 | 7895 / 3570.24 | 4 | 0 | 2.15 | 2.24 | -5297 | co-located with 508 |
| survey c3610 | 7895 / 3570.24 | 508 | 1 | 3.89 | 4.18 | -4761 | 3570570000 / 1171 (best capture kept) |
| survey c3610 | 7933 / 3624.96 | 403 | 3 | 4.07 | 4.91 | +2225 | 3625290000 / 1171 |
| raw_pci415_12s_b (3680.01) | 7971 / 3679.68 | 415 | 4 | 4.47 | 3.74 | -4849 | 3680010000 / 1171 |
| raw_pci923_tim_12s_tile3 (3731.91) | 8007 / 3731.52 | 923 | 0 | 4.68 | 5.29 | -4850 | 3731850000 / 1171 |
- Found 415, 923, 403, 508, PCI 4, 675 with no hand input. No false cell in c3330 or elsewhere (all other candidates have SSS gap 1.0-1.4).
- 0.3 s captures (first 15 chunks only, `--verify-chunks 15`): the same 6 cells, CFO within 40 Hz of the 1 s values (403 +2240, 4 -5267) [MEASURED].
- Pass 2 from existing receiver logs (rr-csd-afe replays, `live_chain.sh pass2`): 923 k_ssb=10 -> `-C 3731910000 --ssb 1169 --initial-fo -4850` (= hand triple); 415 k_ssb=14 -> `-C 3680010000 --ssb 1171` (= hand) [MEASURED]. retune_args.py reports "no mismatch" on those logs (it needs a log with an `ACQ carrier MISMATCH`).
- CFO vs live values: 415 -4849 (hand +6900 was the 8 Oct clock; AUTOFRONT measured -4846), 923 -4850, 675 -4849, 508 -4761 (lag-2 estimate, noisy because 4 overlaps), all on the common X410 offset -4.8 kHz [MEASURED].

## 403: +2225 Hz is not an alias [MEASURED / INFERRED]
The old PSS->SSS estimate (+2177) was read as "-4863 + 7040 alias". The DMRS estimate (lag 1->3, and independently 1->2 and 2->3) gives +2205/+2253/+2225 for 403, all consistent, and the receiver's own DMRS CFO tracker after sync with `--initial-fo +2177` showed a residual of -22 Hz (rr-csd-afe replay_af_403_p2_123441 run.log, "DMRSFO cfo=-21.9 Hz") [MEASURED]. That supports the cell really being ~+2.2 kHz from the radio frame (2 ppm gNB offset, barred anchor cell), not -4.8 kHz [INFERRED; the receiver tracker is unambiguous only to +-1556 Hz, so +2.2 kHz +-3.1 kHz k is not excluded, -4.8 kHz is excluded: -4849 - 2177 = -7026 is not a multiple of 3112]. So the "common offset" assumption is not applied: CFO is derived per cell. The seed need not be unique (hand -2400 also locks 403).
Also found: lag-1 pairs that involve DMRS symbol 2 (and the PSS->SSS pair) show a pi flip on some cells (508, 923: +14015 Hz error), lag 1->3 does not; the estimator therefore uses only 1->3. Cause not investigated [MEASURED, 4 cells].

## Live use (orchestrator)
1. `SAMPLES=122880000 tests/passive_rx/autofront/live_chain.sh all OUT 3430 3550 3610 3680 3730` (radio, heavy.lock, X410 RX1 gain 38; >= 2 centres 100 MHz apart cover the band) or `capture` and `discover` separately.
2. `OUT/triples.txt` (receiver `-C ... --ssb 1171 --initial-fo ...` per PCI) and `OUT/specs.txt` (TAG:ENV:CFG:FREQ:SSB:FO, ENVF/CFGF select env/cfg) feed the OTA window script. Cells at one GSCN (508 + 4) share -C: one receiver start per frequency, the second PCI is a decode-side issue.
3. After the first receiver run of a cell: `live_chain.sh pass2 OUT PCI RUN.LOG` (k_SSB -> `--ssb 1164+k_SSB/2`, `-C` recomputed; SIB1 PRB/offsetToPointA/PLMN stored; retune_args.py line printed on a carrier mismatch).

## Limits [shortcuts]
30 kHz raster only; strongest PSS timing per GSCN (cells > 3 us apart at the same GSCN are not separated; 3 cells max per group); CFO beyond +-7 kHz wraps; one CFO hypothesis in stage 1 (cells > 10 kHz off are missed); CPU numpy (GPU port: stage 1 is batched FFTs); first pass still needs the receiver SSS floor (`ISAC_SSS_REL_RATIO`, gain) to sync, unrelated to the scan; the 7-8 s figure is on a shared host. PCI 4 has the weakest evidence (SSS gap 2.15, DMRS 2.24 on c3550 only; not seen in c3610); NEWCELLS decoded it independently from PBCH/PDCCH [MEASURED earlier]. No receiver replay of the new triples was run here (the CFO values differ by < 100 Hz from the AUTOFRONT triples that locked, except 403/508/4 below).
