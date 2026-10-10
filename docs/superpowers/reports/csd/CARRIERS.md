# n78 carrier survey, 2026-10-10 (DGX Spark + X410 34B2AF1, DPDK, antenna RX1, gain 38)

Evidence labels: [M] measured on the air this session, [R] offline replay of those captures through the receiver
(nr-uesoftmodem, build_ota of rr-csd-a, raw_iq digital retune), [I] inferred.

## Method
- 7 captures of 0.3 s x 4 ch at 122.88 MS/s, centres 3350 3430 3510 3590 3670 3750 3790 MHz, one heavy.lock hold (~1 min),
  `tests/passive_rx/survey/survey_capture.sh` (guardian enter/exit-ota, claimed:False wait, hugepage clean, SIGINT via timeout).
- `survey_psd.py` / `carrier_map.py`: stitched PSD (2 MHz bins, dB over floor). `ssb_search.py`: PSS correlation over all
  sync-raster GSCNs (3000 MHz + N*1.44 MHz, GSCN 7499+N) in each capture, then SSS -> PCI (ratio best/second SSS > ~2 = reliable).
- Control: the known cell is found at GSCN 7971 (3679.68 MHz) with PCI 415 (SSS ratio 3.8), so the detector is calibrated.
- `replay_survey.sh` + `tile.sh`: a 0.3 s capture is too short for the receiver start-up, so it is tiled 10x (0.3 s = 15 SSB periods
  = 30 frames, so SSB timing and SFN stay continuous) and replayed with the receiver digitally retuned to -C.

## Spectrum [M] (dB over noise floor, antenna 0)
3312-3332 +6 | 3395-3413 +6 | 3470-3486 +9 (three ~18-20 MHz blobs, no sync raster SSB found: not NR on the 30 kHz raster, unclassified)
3550-3620 +12..17 | 3620-3640 +18..26 | 3640-3720 +30..35 (our PCI 415 / Vodafone, sharp edges 3640 and 3720)
3720-3770 +12..18 (PCI 923) | 3770-3800 +21..23 (band edge at 3800).

## Carrier table [M] (PSS/SSS) and [R]
| carrier (est.) | GSCN | SSB MHz | PCI | SSB PSS-sym over median window | operator | receiver sync |
|---|---|---|---|---|---|---|
| 3640-3720 (80 MHz) | 7971 | 3679.68 | 415 | 28.9 dB | Vodafone (MCC/MNC 222-10, SIB1) = the current cell | [R] sync + MIB + SIB1 (control) |
| 3720-3770 (~50 MHz) | 8007 | 3731.52 | 923 | 24.9 dB | TIM (MCC/MNC 222-01, SIB1, cellId 0x5b941823d), SA-style: barred=0 | [R] sync + MIB + SIB1 + PDCCH_SCRAMBLING_ID CONFIRMED |
| ~3550-3640 (two blobs?) | 7933 | 3624.96 | 403 | 14.8 dB | unknown (not synced) | [R] no sync (FO 0, +-2k, +-7k, +-9k, 6k tried) |
| 3720-3770 | 8027 | 3760.32 | 518 | 23.9 dB, SSS ratio 2.7 | unknown | [R] no sync (FO -4800) |
| weak | 7795 / 7857 | 3426.24 / 3515.52 | 675 / 347 | 8.9 / 4.5 dB | unknown | [R] no sync |
Other peaks with SSS ratio < 1.6 (3740.16 PCI154, 3754.56 PCI70) are not reliable.

## Ready receiver arguments
Geometry rule (derived from the 415 recipe, which is the same rule): the receiver grid starts at -C - 108.5 PRB. SSB first
subcarrier --ssb = 12*97 + k_ssb/2 = 1164 + k_ssb/2 (k_ssb from the MIB; 415: k_ssb 14 -> 1171), and
-C = GSCN_MHz + (1182 - ssb)*0.03 MHz (415: 3679.68 + 11*0.03 = 3680.01). k_ssb comes from the first sync, so a first pass with
--ssb 1171 syncs and prints k_ssb, then restart with the derived pair (it was needed for 923: k_ssb 10).

TIM / PCI 923 (validated offline) [R]:
  -C 3731910000 -r 217 --numerology 1 --band 78 --ssb 1169 --initial-fo -4800 (rest of the 415 recipe unchanged)
  Sync needs the CFO seed: with --initial-fo 0 it did not sync; CFO measured -4.8 kHz (PSS/SSS phase slope, same value on 415, so it is
  the X410 LO offset: 415 measured -4.9 kHz; this sign agrees with the working replay value -4800). Live, use the seed of your OTA calibration;
  the 415 recipe seed 6900 is the opposite sign, so first try -4800 and, if it does not sync, +-the current X410 offset.
  k_ssb=10, coreset0=12, ss0=4, scs_common=1, SFN seen, CSS0 autoconf OK, SIB1 decoded.
Vodafone / PCI 415 (control, unchanged): -C 3680010000 -r 217 --numerology 1 --band 78 --ssb 1171 [R] sync + MIB (k_ssb 14) + SIB1.

Replay sub-cases with SIB1 evidence: 415 retuned from capture 3670; 923 from capture 3750 (SSB at -18.5 MHz of the capture centre).
Limits: one site, one time, antenna 0 for detection, 4 antennas in the receiver; 403/518/675/347 are detections only.
