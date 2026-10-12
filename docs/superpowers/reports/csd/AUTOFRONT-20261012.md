# AUTOFRONT 2026-10-12: automatic blind front end (no hand -C / --ssb / --initial-fo)

Host: DGX aarch64, X410 34B2AF1 captures of 2026-10-08..11 (Polimi DEIB, Milan). Offline analysis + receiver replay (`build_t26`, ENVFILE `ota/t26w.env`, ISAC_SSS_REL_RATIO=2.5, 4 RX, SPEED 0.5). No OTA. Cells/captures are not merged across centres.

## Method [IMPLEMENTED, tests/passive_rx/autofront/autofront.py]
1. Discovery (`discover`): every n77/n78 sync-raster GSCN inside the capture band (30 kHz SCS), 20 ms chunks, band cut-out per GSCN, PSS correlation |cc|^2 summed non-coherently over all 4 RX and folded modulo k*20 ms (k=1,2,4,8) = multi-burst accumulation (ssb_search2 `pass1`). SSS -> PCI with the scale-invariant ratio (best/second SSS score), PBCH-DMRS -> i_ssb (ratio best/second). CFO hypothesis grid from SCS and oscillator tolerance only (step SCS/4 = 7.5 kHz, span +-2 ppm of the capture centre; `cfo_grid`), each hypothesis re-cuts the band; final CFO = hypothesis + PSS->SSS phase slope (symbols 0 and 2, unambiguous +-7 kHz). Detection kept when SSS ratio >= 3 and DMRS ratio >= 3. No cell-specific constant. Output `cells.json`.
2. Tuning (`tune`): pass 1 `--ssb 1171`, `-C = GSCN + (1182-ssb)*30 kHz`, `--initial-fo = round(CFO)`. Pass 2 (`refine_from_log` on run.log): `--ssb = 1164 + k_SSB/2`, `-C` recomputed; log also yields SIB1 PRB/offsetToPointA/PLMN. Driver `auto_cells.sh` stops when pass 2 equals pass 1.
3. Unit tests (`test_autofront.py`, 7 tests incl. gscn arithmetic, both triples of the hand recipes, JSON, log parsing, pick rule): all pass [MEASURED].

## Offline discovery [MEASURED, DGX, 1 s x 4 ch per capture, 6 captures, ~25 min under heavy.lock]
| capture (centre) | GSCN / MHz | PCI | i_ssb | SSS ratio | DMRS ratio | CFO Hz | auto pass-1 triple (-C, ssb, fo) |
|---|---|---|---|---|---|---|---|
| survey c3430 (3430) | 7795 / 3426.24 | 675 | 0 | 3.36 | 3.12 | -4881 | 3426570000, 1171, -4881 |
| survey c3550 (3550) | 7895 / 3570.24 | 508 | 1 | 4.00 | 4.25 | -4816 | 3570570000, 1171, -4816 |
| raw_pci415_12s_b (3680.01) | 7971 / 3679.68 | 415 | 4 | 4.51 | 3.73 | -4846 | 3680010000, 1171, -4846 |
| raw_pci923_tim_12s_tile3 (3731.91) | 8007 / 3731.52 | 923 | 0 | 4.55 | 5.18 | -4890 | 3731850000, 1171, -4890 |
| survey c3610 (3610), run alone, --top 25 | 7933 / 3624.96 | 403 | 3 | 3.88 | 4.72 | +2177 | 3625290000, 1171, +2177 |

- Found: 415, 923, 675, 508, 403 (all expected cells). c3330 gave none (nothing expected). 403 was missed in the 6-capture run with `--top 12` (rank > 12 among raster candidates); default raised to 25 and re-found when c3610 was run alone [MEASURED]. The 6-capture run was not repeated with top 25 (cost) [NOT RE-RUN].
- CFO: the X410 free-running clock shows -4.8 kHz (+-0.04) on 4 of 5 cells (same oscillator, 3.4-3.7 GHz). 403 returns +2177 = -4863 + 7040: PSS/SSS relative sign flip (pi phase), the estimator's alias. Hand seeds differ (415 +6900 on 8 Oct clock; -2400 for 403/508/675 on 11 Oct) [MEASURED vs hand values in CARRIERS*/NEWCELLS]. The receiver tolerates all of them (below), so the seed need not be unique.
- PCI 508 vs 4 at GSCN 7895: NEWCELLS found PBCH CRC/PDCCH giving PCI 4; with the auto triple + t26w the receiver synced PCI 508 on c3550g4. Two co-channel cells with the same N_ID_2 remain possible [INFERRED]; not resolved here.

## Receiver replay with the AUTO triple [MEASURED, DGX replay]
| cell / capture | pass 1 | pass 2 (from MIB) |
|---|---|---|
| 415, raw_pci415_12s_b (12 s) | sync PCI 415, MIB k_ssb=14, barred=1, SIB1 CELL mcc=222 mnc=10 cell_identity 0x007ed8847 | triple identical (k_ssb 14 -> ssb 1171) = hand triple except FO |
| 923, raw_pci923_tim_12s_tile3 (12 s) | sync PCI 923, MIB k_ssb=10 | -C 3731910000 --ssb 1169 --initial-fo -4890 = hand triple; sync, MIB, SIB1 (mcc=222 mnc=01, 217 PRB, offsetToPointA=38) |
| 675, newcells_tiled/c3430g32 (gain x32) | sync PCI 675, MIB k_ssb=22 | -C 3426450000 --ssb 1175: sync, MIB; no SIB1 |
| 508, newcells_tiled/c3550g4 (gain x4) | sync PCI 508, MIB k_ssb=2 coreset0=11 | -C 3570750000 --ssb 1165: sync, MIB; no SIB1 |
| 403, newcells_tiled/c3610g8 (gain x8), FO +2177 | sync PCI 403, MIB k_ssb=12 barred=1 | -C 3625320000 --ssb 1170: sync, MIB; 3 SIB1 CELL mcc=222 mnc=50 lines (barred cell, not expected, not verified) |
- Without digital gain (c3550, c3430) the receiver fails with "synch Failed" (absolute SSS metric floor, known from NEWCELLS), independent of the front end [MEASURED].
- SIB1 for 675/508 not decoded (as in NEWCELLS; capture too short/weak) [MEASURED].

## Live use
1. Wideband survey capture per centre (survey_capture.sh style, 1 s x 4 ch; >= 2 centres span the NR band; captures 100 MHz apart as in survey_1s_1011), digital or RF gain high enough to pass the SSS floor (live gain 38 + REL_RATIO env).
2. `tests/passive_rx/autofront/auto_cells.sh discover OUT CAPDIR...` -> `OUT/cells.json`, `args.txt` (receiver args), `specs.txt` (OTA window lines `TAG:ENV:CFG:FREQ:SSB:FO`; ENVF/CFGF env select env/cfg).
3. Start the receiver with the pass-1 args; after the MIB log line run `autofront.py args cells.json --log run.log` (or `triple`), restart once with the pass-2 pair. With a re-centre only `-C`/`--ssb` change, FO is kept.
4. `auto_cells.sh replay CELLS PCI CAPDIR [NAME]` automates pass 1 + pass 2 on a capture.

## N simultaneous receivers [INFERRED, not tested]
One nr-uesoftmodem per cell (own -C, --ssb, --initial-fo, own log/output dir); all fit in one X410 stream only if the cells lie inside the same 122.88 MHz capture span (here 415 and 923 are 52 MHz apart, centre 3680/3732: they do). The raw IQ stream would have to be fanned out to N consumers (the raw_iq device reads a file/ring per process) and the radio claim held by a single owner (X410 claim hygiene); CPU/GPU budget scales per receiver (heavy.lock). A multi-cell receiver in one process is the clean target (csd/mcell-spec). Cells in different spans need N radios or time-slicing.

## Limits
Python/CPU, ~4 min per 1 s capture (the C/GPU port contract = the pure functions in autofront.py); 30 kHz SCS raster only; strongest SSB per GSCN only; CFO sign-flip alias (7040 Hz) uncorrected (could be resolved by trying cfo +- 7040 in the receiver, or using the median CFO of cells of one capture); first pass needs enough SNR for the receiver SSS floor; 508/4 ambiguity open. Data: `docs/superpowers/reports/csd/autofront_data/`.
