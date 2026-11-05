# CSIRS-CLASS: can downlink precoder estimation start on the DEIB cells? (2026-11-05)

Branch `twomodes/csiclass`. Host: DGX GB10 (aarch64), 4 RX. Cells are never merged: every number names its capture. Evidence labels follow PROJECT_MEMORY section 0.1;
`[OFFLINE raw-IQ]` = Python on the stored 4-RX capture (no receiver in the loop, no gNB log or config used); `[REPLAY]` = grant list from a receiver replay of the same capture
(`tools/csiclass/replay_sense.sh`, binary `rr-twomodes` build 64abf23085, FROZEN_20261018 env minus radio flags, sensing export on, CRC from the receiver).
gNB logs and configs were not read. No receiver code was changed.

## Verdict (one line per cell)

| cell | (b) Type I codebook match | (c) per-PRG LS | what is missing |
|---|---|---|---|
| 923 (TIM) | **no clear winner**: layer 0 best 0.955 vs random-best 0.934 (ceiling, rank-1 H), layer 1 0.860 vs 0.757, but on-grid beats a half-step-shifted grid in only 1/30 and 5/30 grants | **neither**: H is rank 1, 3-6 % of an isotropic W is identifiable | no Type I quantisation signature; a channel with rank > 1 (more RX antennas or a richer scene) |
| 415 (Vodafone) | **no evidence**: best 0.38 vs random-best 0.41, 4/21 grants above null (chance); the test does recover a planted codeword | **neither**: PRG is 16 RB, usable rank 6-8 of 16 ports, residual equals the shuffled control | the 16-RB precoder change says subband or reciprocity precoding; no Type I signature |
| 508 (WindTre) | **neither** | **neither** | no multi-port CSI-RS found (all 1024 IDs x 20 slots tried), almost no traffic |

## 1. Periodic CSI-RS resources found

Method [OFFLINE raw-IQ, `tools/csiclass/map_coh.py`]: slot grid from the cyclic prefix, residual CFO and drift; per (slot mod 160, symbol, subcarrier class k mod 12) the quantity
`T = mean_p |sum_{16 RB} z| / sqrt(sum |z|^2)` with `z = sum_ant Y_p conj(Y_{p-1})` (consecutive 160-slot periods). Noise gives 0.9, a repeated deterministic RE up to 4.
(Plain antenna-summed coherence is useless: the 4-RX vector of any QPSK RE is coherent with itself, median 0.58 everywhere.) The whole grid is slipped by one symbol (my l = true l - 1; proved by the
PDCCH DM-RS comb sc 1,5,9 appearing in my l13 and by the 923 CSI-RS matching at true symbols 8,9,12,13). Every resource below was then confirmed by its TS 38.211 7.4.1.5.2 sequence
(`seqscan*.py`, adjacent-RB coherence of the despread RE pairs: 0.9 present vs 0.12 absent). The sequence index used is the OAI/spec one (`m' = 2n + k'`, n counted from CRB 0, density 1, alpha = 2).
Port numbering `p = s + 2j` (OAI `nr_phy_common_csirs.c`).

### 923 (TIM), capture `raw_pci923_4s_1102_0951`, 217 PRB, centre 3759.99 MHz = carrier centre

| # | role | ports / CDM / row | density | symbols / subcarriers | period / slot | extent | evidence |
|---|---|---|---|---|---|---|---|
| 1 | **CSI acquisition** (NZP) | **32 ports**, fd-CDM2 (row 16 in OAI numbering, 16 groups) or cdm4 (row 17): the two give the same RE set and the same port-structure statistics, not separable blind | 1 (RE every RB) | true symbols 8,9,12,13; sc 4..11 (fd pairs k = 4,6,8,10) | period 40 slots, n_s = 8 (abs % 40 == 8 as in S10); 200 occasions in 4 s | 217 RB (13 of 13 blocks) | ID 923 n_s 8: stat 0.87 (l8), 0.83 (l9), 0.72 (l12), 0.68 (l13) vs 0.10 noise |
| 2 | unresolved, probably CSI (NZP, other ID) | 16 ports if fd-CDM2 | 1 | symbols 5,6; sc 0..7 | same occasion, n_s 8 | grows with RB index | ID 576 n_s 8: stat 0.2-0.47 (noise 0.12), weak and not confirmed |
| 3 | **TRS** (tracking) | 1 port, noCDM, row 1 | 3 (comb 4) | true symbols 4,5,8,9 | two consecutive slots (n_s 16,17), period 80 | full carrier (13/13 blocks) | three IDs: 997 (k0 1, stat 0.86), 576 (k0 0, 0.79), 921 (k0 3, 0.50) |
| - | not CSI-RS | - | - | PDCCH DM-RS (sc 1,5,9 of symbol 0 of almost every slot); periodic PDSCH DM-RS comb (symbols 2,11) | - | - | listed so nobody mistakes them for CSI-RS |

ZP-CSI-RS / CSI-IM cannot be inferred from IQ alone [INFERRED: nothing in this capture distinguishes them].

### 415 (Vodafone), capture `raw_pci415_4s_1102_0951`, 217 PRB, centre 3680.01 MHz = carrier centre

Slot numbers below are the receiver's `abs % 160` (mapping j = abs - 14242 verified: TRS phase 13 <-> abs 15).

| # | role | ports / CDM | density | symbols / subcarriers | period / slot | evidence |
|---|---|---|---|---|---|---|
| 1 | **TRS** (own) | 1 port, noCDM, row 1 | 3 (comb 4) | symbols 4,5,8,9, k0 0 (l4,8) / 1 (l5,9), RB 0-51 | period 160, abs 15,16 (and 20,21) | ID 415, stat 0.98 (k0 0), 0.07 for k0 1..3 |
| 2 | **CSI acquisition**, serving ID | 16 REs/RB, read as 16 ports fd-CDM2 (2 fd pairs x 4 symbols); row (cdm2/cdm4) unresolved | 1 | symbols 10-13, sc 4..7 | abs % 160 == 105 (n_s 5), 50 occasions | ID 415: stat 0.68 / 0.67 |
| 3 | CSI acquisition, co-slot neighbour | same shape | 1 | symbols 10-13, sc 8..11 | abs 105 | ID 418: 0.90 |
| 4 | CSI acquisition, neighbour sectors | same shape | 1 | symbols 10-13, sc 0..3 (ID 249) and sc 8..11 (ID 252) | abs 63 (n_s 3) | 0.69-0.80 and 0.49-0.56 |
| 5 | CSI acquisition, neighbour | same shape | 1 | symbols 10-13, sc 8..11 | abs 24 (n_s 4) | ID 380: 0.58-0.72 |
| 6 | other TRS | 1 port, row 1 | 3 | symbols 4,5,8,9 | abs 15,16,20,21 | IDs 748, 249, 582, 380, 713 as in `docs/superpowers/reports/csd/CSIFULL.md` (not re-measured) |

The same slot hosts up to three FDM'd 16-port resources of different sector IDs; the serving ID (415) is on sc 4..7 at abs 105 [INFERRED: scrambling ID = PCI, as for the TRS].
The receiver's own blind discovery logs show the same slots and IDs 418/380 (ab_Apci415_1_095532), but its row/CDM winners are ambiguous (rows 13-18 for the same REs).

### 508 (WindTre), capture `raw_pci508_4s_1102_0952` (grid not CRB aligned: Point A at grid sc 324/330, carrier 162 PRB)

No CSI-RS sequence recovered. The periodic structure exists (period 10 slots at true symbol 13, sc {0,1,6..11}, T up to 3.0) but no ID/slot matches:
1-port density-1 and fd-CDM2 hypotheses, all 1024 IDs x 20 n_s, RB-index offsets 26/27/28, subcarrier pairs 0/6/8, 8 occasions: best stat 0.17 (noise 0.12-0.35, no peak).
The live receiver (ab_Apci508_1_095211) found no WINNER either. [OFFLINE raw-IQ] Not found does not mean absent.

## 2. Class A (non-precoded) or class B (beamformed)?

Test [OFFLINE raw-IQ, `tools/csiclass/class_test.py`, `stability.py`]: despread every group to a port channel `H[occ, rb, port, rx]`; the port covariance R (ports x ports, over RB, RX, occasions) is fitted
by dual-polarised UPA DFT steering vectors `[a(n1,n2); e^{j phi} a(n1,n2)]` (oversampling 8) over every (N1,N2) and port ordering (20 models); the same maximisation is repeated on randomly permuted ports (null).
A class A resource (one port per antenna element/polarisation) gives a structured R; a beamformed one (one port per beam) gives non-uniform port power and no element-DFT structure in port order.

| resource | port power spread (max-min / std) | R eigenvalue fractions | best model, fit of eigvec 1 / 2 | null (perm.) mean / p95 / max | verdict |
|---|---|---|---|---|---|
| 923 #1, 32 ports | 2.4 dB / 0.5 dB | 66 / 6 / 1.4 % | (N1,N2) = (8,2), first 16 ports = polarisation 0: **0.947 / 0.943** | 0.237 / 0.325 / 0.357 (100 perms) | **class A** [MEASURED, 923] |
| 415 ID 415, 16 ports | 6.3 dB / 2.1 dB | 48 / 15 / 7 % | (4,2) interleave: 0.47 / 0.72 | 0.287 / 0.348 / 0.382 (50) | weak: above null, port order unverified |
| 415 ID 418 | 5.4 dB / 1.6 dB | 43 / 26 / 8 % | (2,4) interleave: 0.75 / 0.56 | 0.367 / 0.474 / 0.578 (50) | structured |
| 415 ID 380 | 2.5 dB / 0.8 dB | 24 / 15 / 12 % | (4,2) block: 0.745 / 0.72 | 0.357 / 0.490 / 0.589 | structured, 4 dB SNR |
| 415 ID 249 | 2.9 dB / 1.0 dB | 16 / 12 / 11 % | 0.42 | 0.336 / 0.439 / 0.464 | not distinguishable (low SNR) |

923 supporting numbers: the fit is the same in the first and second half of the occasions (0.947 / 0.948) and in the lower/upper half of the band (0.944 / 0.947); the dominant beam is (l,m) = (27,2) of the 32x8 grid in 19 of 20
blocks of 0.4 s (one block (27,3)); relative port phases are identical in every block (circular concentration 1.000). The alternative port numbering `p = j + 16 s` fits only 0.715.
Row 16 vs row 17 (fd-CDM2 vs cdm4) cannot be separated: both give the same R spectrum and fits of 0.947 (8,2) and 0.949 (4,4). The sparse-atom (OMP) residual plateaus at 0.22 for both, the noise floor.
923 is dominated by one path (66 % of the energy in one eigenvector, the second pol is the second eigenvector), so the port vector looks like one steering vector, which is exactly what makes the angle domain trivial and the precoder hard (section 4).
Limits [INFERRED]: a beamformed resource whose beams are ordered like a DFT grid would also fit; its port power would then be peaked, which the 2.4 dB spread of 923 excludes. For 415 the 16-port element order (symbol-major, k ascending) is a guess.

## 3. Direct feasibility on decoded grants

Link between the two sides [REPLAY + OFFLINE raw-IQ]: the receiver's grant records (CRC-OK, abs_slot, rnti, start_rb, nb_rb, dmrs_symb_pos, dmrs_scrambling_id, Nl) give the slot of each grant
(`rx_ts` is the slot-start sample index, so j = abs_slot - c with c constant: 14242 for the 415 capture) and `tools/csiclass/grants_dmrs.py` re-estimates the DM-RS channel itself (LS at the type-1 comb of port 1000,
per RB mean over the DM-RS REs, n_SCID/comb/CRB-relative index tried). Check: the match statistic of the CRC-OK grants of RNTI 58971 is **0.95-0.99** (second hypothesis 0.1-0.35), so slot mapping, sequence and RB range are right.
H comes from the CSI-RS occasion nearest in time (|lag| up to 80 slots = 40 ms for 415).

### 415 (Vodafone) [REPLAY, 4 s capture, 4 RX]

- 366 grants, 304 CRC-OK; 288 UE-specific (15 RNTIs) with Nl = 1 / 2 / 3 / 4 = 88 / 111 / 46 / 43. **Every grant starts on a multiple of 16 RB and has a size multiple of 16** (RBG size 16).
- Precoder granularity from the DM-RS alone (`prg_profile.py`; no CSI-RS needed): adjacent-RB direction coherence of h_eff (4-dim chance 0.25) is 0.62 on average and **dips to 0.485 at the pair (15,16) mod 16** (0.57-0.66 elsewhere; 5558 RB pairs of 51 Nl = 1 grants).
  The precoder (or the DM-RS-visible channel) changes at 16-RB boundaries: PRG / subband of 16 RB, or frequency-selective reciprocity precoding [MEASURED, 415 only]. 16 RB is also the CSI subband size for a 145-275 PRB BWP (TS 38.214 Table 5.2.1.4-2) [INFERRED link].
- (b) `run_b.py`, ID 415 resource (the serving one), 21 Nl = 1 CRC-OK grants of RNTI 58971 (16 to 208 RB): score = mean over RBs of cos^2(h_eff, H w). Best Type I rank-1 codeword over all 20 (N1,N2,ordering) models
  (mode 1 and mode 2 span the same beam x co-phase grid for rank 1, 8704 candidates): **mean 0.383, second best 0.383 (ties), random isotropic w best-of-8704: 0.405, same H with another grant's h_eff: 0.437.**
  Best above the random-best in 4 of 21 grants (chance level), above the shuffled control in 5. ID 418 resource: 0.387 vs 0.420 (12 grants). **No Type I signature.**
- Test power (`power_check.py`, planted codeword w on the measured H, noise, same sweep): true codeword within 2 % of the best score in 12/12 trials at 20 and 10 dB and 11-12/12 at 0 dB (16 and 96 RB); at 20 dB also 12/12 when the matching H is taken one or two occasions (80-160 ms) later.
  So a Type I gNB would have been seen with this H; the negative is not a lack of sensitivity [SIM-style control on measured H].
- (c) per PRG, same grants: with 2 or 4 RB the 16-unknown system is under/just determined (residual 0.00, identical to the shuffled control); at 8 RB residual 0.30-0.55 vs 0.33-0.50 shuffled; at 16 RB 0.52-0.75 vs 0.52-0.80.
  `ls_feasibility.py` on the ID 415 H: usable rank (sigma^2 > 2x noise floor) 4 / 5 / 6 / 8 of 16 for PRG 2 / 4 / 8 / 16 RB; identifiable fraction of an isotropic W 0.24 / 0.31 / 0.40 / 0.48 (0.81 wideband).
  A 16-RB PRG gives 64 equations for 16 unknowns but only half of the unknown space is observable with 4 RX. **(c) cannot start.**
- Rank 2-4 grants (200 of 288) were not tested: the rank-2 codebook builder exists (`codebook_match.typeI_rank2`) but the per-layer DM-RS separation (ports 1000/1001 CDM) was not implemented. Not claimed.

### 923 (TIM) [REPLAY raw_pci923_4s_1102_0951, FO -4832 (the FO -4800 attempt did not sync), 100 s of replay, 4 RX]

- 182 grants, 148 CRC-OK UE-specific; **Nl = 2 for 135, Nl = 3 for 11, Nl = 1 for 2** (132 of them RNTI 22978); all start on a multiple of 16 RB (RBG 16), MCS 0-8. (The OTA run of the same minute, ab_Apci923_1_095716, had 244 of 245 CRC-OK grants at Nl = 1: other samples, other RNTIs, not merged.)
- Slot mapping j = abs_slot - 2341; DM-RS re-estimate match 0.91-0.99 (second hypothesis 0.1-0.3). Check against the CSI-RS: abs % 40 == 8 <-> j % 40 == 27, exact.
- For Nl = 2 the DM-RS comb gives both layers: port 1000 = mean over the RE pairs, port 1001 = alternating-sign mean (OCC [+1 +1] / [+1 -1], channel assumed flat over 4 subcarriers). Both are columns of H W; for a rank-2 Type I W each column is itself a rank-1-form vector
  ([v; phi v] and [v'; -phi v']), so both are tested against the rank-1 candidate set.
- (b), per-RB cos^2, 32 ports, 17408 candidates (20 models), 40 grants of 16-217 RB, H from the nearest occasion (|lag| <= 20 slots):
  layer 0: best **0.955**, random-w best 0.934, other-grant h_eff 0.523 (best > random in 40/40, gap +0.02); layer 1: best **0.860**, random-w best 0.757, other-grant 0.498 (40/40, gap +0.10); free least-squares w 0.979 / 0.935.
  The 0.52 of the shuffled control against 0.955 shows that the chain is consistent (same cell, same direction in RX space at 20 ms lag), but with a rank-1 H the layer-0 score sits at its ceiling for ANY w.
  The same winners come back (model (2,8) or (4,4) interleave, l 5-7, m 11-12): beams along the dominant channel direction, which is what any good w would be [INFERRED].
- Coherent variant (`run_b_coh.py`, one scalar per grant, frequency profile must match; 6 grants of 64-217 RB, layer 0): codebook 0.921 vs random 0.908, codebook above random in 6/6 by 0.01-0.02, the same codeword (7,12,1) wins in 4 of 6; the planted control is recovered 6/6. Margin too small to call.
- **Quantisation test** (`run_b_grid.py`, decisive for "gNB uses Type I"): best on-grid candidate vs best candidate of the same family shifted half a grid step (and pi/4 in co-phase): on-grid wins in **1 of 30 (layer 0) and 5 of 30 (layer 1)** grants.
  A Type I gNB would win on-grid systematically; the shifted grid wins instead, i.e. no preference for the Type I grid (the true beam sits between grid points or the port-to-element map differs). **No Type I signature.**
- (c): the LS residual at 16 RB (0.18-0.35, other-grant control 0.38-0.54) is small only because H is rank 1; `ls_feasibility.py` on the 32-port H: per-element SNR about 6 dB (upper bound on noise), per-RB singular values^2 relative to noise x P: +10.4 / +0.3 / -3.5 / -7.2 dB.
  **Usable rank is 1 for PRG 2, 4, 8 and 16 RB and 2 over the whole 217 RB; the identifiable fraction of an isotropic 32-dim W is 3-6 %.** One dominant path (66 % of the energy) and 4 closely spaced RX antennas make the direction of H(f) nearly constant.
  W cannot be recovered per PRG on 923 [MEASURED, 923].
- PRG granularity: the direction-coherence profile of section 415 is uninformative here (mean 0.956 for layer 0, 0.916 for layer 1, dip at rb mod 16 = 15 of only 0.02-0.03), because with a rank-1 H the direction of h_eff is the channel direction whatever the precoder is.

## 4. Verdict per cell and what is missing

- **923: (c) neither, (b) no clear winner.** (c) is excluded by the rank-1 channel. (b): layer-wise scores beat random vectors only along the dominant channel direction, and the quantisation test (on-grid vs half-step grid) shows no Type I preference. What would settle it: a rank > 1 channel (the same cell from a position with scatterers, or more/wider-spaced RX antennas), or the port-to-element map of the gNB.
- **415: neither.** Precoder granularity is 16 RB (measured), the CSI-RS is class-A-like but weakly structured and no Type I signature exists in 21 CRC-OK grants with a test that recovers a planted codeword; with 4 RX only about half of a 16-port W is observable per 16-RB PRG.
  The 16-RB change plus Nl up to 4 in a TDD cell point to subband or SRS-reciprocity precoding rather than Type I PMI [INFERRED]. Missing: rank 2-4 evidence (layer separation of the DM-RS), the cell's own 16-port element order, UL SRS to test reciprocity.
- **508: neither.** No multi-port CSI-RS found, almost no traffic.
- 403 (never syncs) skipped as instructed.
- General limit: a Type I codebook test needs the CSI-RS port-to-element map. Here it is inferred from the data (923: 8x2x2, first half polarisation 0) and for 415 assumed.

## 5. Files

`tools/csiclass/`: `map_coh.py` (blind map), `csilib.py` (slotter, despread), `extract_h.py` (port channels), `class_test.py`, `stability.py` (class A/B), `ls_feasibility.py` (c), `codebook_match.py` (Type I rank 1/2, self-test 10/10),
`grants_dmrs.py`, `run_b.py`, `run_b_coh.py`, `run_b_grid.py`, `power_check.py`, `prg_profile.py`, `dmrs_scan.py`, `replay_sense.sh`. Scratch (not committed): `.superpowers/tmp/` (H*.npz, coherence maps, replay outputs `rep_p415`).
Rerun order for a capture: `map_coh.py` -> look at the map -> `extract_h.py --id --ns --j0 --period --symbols --kbars` -> `class_test.py`, `ls_feasibility.py`; replay -> `grants_dmrs.py` -> `run_b.py`, `prg_profile.py`.

## 6. Concerns

- 415 resource row/CDM type (16-port cdm2 vs cdm4) and element order are not determined; the 415 class and (b) results depend on that guess (the planted-codeword control assumes the same).
- Channel lag between CSI-RS (period 160 slots) and grants up to 40 ms; the control suggests 415's H is stable over 160 ms, 923's is not (20 ms, noise-limited).
- 923: the replay needed FO -4832 to sync (the -4800 first attempt failed); the 923 channel is rank 1, so (b) there has little power by construction; the planted-codeword control with the 20 ms-old H recovered only 3/12.
- Evidence is one 4 s capture per cell, one time of day; no OTA low-load data.
