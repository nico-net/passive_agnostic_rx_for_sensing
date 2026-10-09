# QAM-FIX: multi-layer QAM soft output, plus the low-MCS residual classification (csd/qam-fix, 2026-10-09)

Branch `csd/qam-fix` on top of `csd/otatest` 55c9b39931. Host: DGX GB10 (aarch64). Every build, test and replay went through `heavy.sh`, one at a time. No radio, capture or hardware tools were used.
Replays use `tests/passive_rx/qamfix_replay.sh`, which takes `ota/t26f.env` and strips the radio-only flags and core pinning (`ISAC_USRP_*`, `ISAC_UHD_*`, `ISAC_RX_RECOVER*`, the GPU/USS core pins). It runs with `ISAC_PDSCH_CORES=8,9,12,13,14`, speed 0.25, `ISAC_MLOCK=0`, and adds `ISAC_CSIRS_DISC=1` and `ISAC_PDCCH_DISCOVERED_FILE=tests/passive_rx/dgx/scanfast_coresets.txt`. Without the CORESET file, the 12 s capture acquired no C-RNTI (`replay_ab1`). Captures: `raw_pci415_12s_b` (CAP_TICK 208029025) and `raw_pci415_12s_20261008` (CAP_TICK 210036277). The two captures are always reported separately.

## Root cause 1: the multi-layer MMSE shrinks QAM amplitudes [MEASURED, replay, paired]

[CODE] The multi-layer path is `nr_dlsch_mmse()`, which is stock OAI. It forms `z = adj(G + nvar/8 I) H^H y` and compares `z` against the thresholds `det(G + nvar/8 I) * n1/n2/n3`. With diagonal loading `s`, the coefficient on the wanted symbol is `det(G) + s G_ll`, not `det(G + sI)`. The output is therefore shrunk relative to its own thresholds by about `1 - s G_kk / det(G)`. This bias grows sharply when the two layers are correlated (small `det(G)`), which is the normal case for a passive receiver: the precoder was chosen for another UE's channel.
- QPSK only uses the sign, so it is immune.
- 16QAM loses its amplitude-bit margin.
- 64QAM and 256QAM outer points fall inside the next threshold, so the amplitude bits are wrong with high confidence.

A second problem is that every layer shares one threshold array, `dl_ch_mag[0]` = det, so there is no per-layer reliability. The 1-layer MRC path has neither problem (kappa 1.00-1.02 in the live EVMGRANT lines).

[MEASURED] Diagnostic: `kappa` = mean(|Re z|,|Im z|) / mean(threshold n1). An unbiased equaliser gives about 1. `nr_rx_pdsch` now computes it for Nl >= 2 too, and EVMGRANT and QAMAB log it. Stock values on 2-layer QAM: 0.66-0.88. With the fix: 1.02-1.15 (above 1 only because of noise at low SNR; about 1.0 at 15 dB or more).

**Fix** (`ISAC_PDSCH_MMSE_MODE`, default 0 = stock):
- `=2`: float zero-forcing per RE, `x_l = [G^-1 H^H y]_l`. This is unbiased.
- Per-layer soft scale `w_l = 1/[G^-1]_ll`, the post-ZF SNR of that layer. The output is `z_l = w_l x_l` with thresholds `w_l * n1/n2/n3` per layer, in `dl_ch_mag[l]`. The LLR kernels now read per-layer thresholds in this mode.
- The LLR is on the same scale as the 1-layer `|h|^2 x`. `G` and `H^H y` carry the same `2^-shift`, so nothing else changes and K38 normalisation still applies.
- At 3-4 layers QPSK keeps the stock MMSE: measured ZF noise enhancement cost 16 -> 9 of 246 paired grants below 10 dB. QAM at any rank uses mode 2.
- `=1` is fixed-point ZF (the stock adjugate without loading). It is unbiased but not better than stock in CRC terms, and its determinant underflows at 3-4 layers. Do not use it.

**Paired before/after** [MEASURED, replay `raw_pci415_12s_b`, runs ab2/ab4/ab5/abq6/abq7]. Each grant is decoded on the same samples twice: the stock arm, then mode 2. Both arms run the DECODE-FIX recovery arms, and only the stock result was fed back. `ISAC_PDSCH_MMSE_AB=2 ISAC_PDSCH_MMSE_AB_QPSK=1`, QAMAB lines; full dedicated decodes, probe=0.

| Nl | Qm | DD-EVM SNR | paired grants | stock CRC | mode 2 CRC | kappa stock | kappa mode 2 |
|---|---|---|---:|---:|---:|---:|---:|
| 2 | 2 | <10 | 2574 | 1309 (51 %) | 1351 (52 %) | – | – |
| 2 | 2 | 10-15 | 41 | 36 (88 %) | 37 (90 %) | – | – |
| 2 | 2 | >=15 | 4 | 4 | 4 | – | – |
| 2 | 4 | <10 | 972 | 0 | 0 | 0.71 | 1.15 |
| 2 | 4 | 10-15 | 215 | 39 (18 %) | **81 (38 %)** | 0.88 | 1.05 |
| 2 | 4 | >=15 | 8 | 4 | 5 | 1.00 | 1.02 |
| 2 | 6 | <10 | 67 | 0 | 0 | 0.66 | 1.26 |
| 2 | 6 | 10-15 | 348 | 0 | 0 | 0.77 | 1.14 |
| 2 | 6 | >=15 | 15 | 0 | **2** | 0.98 | 1.06 |
| 2 | 8 | 10-15 / >=15 | 6 / 3 | 0 / 0 | 0 / 0 | 0.79 / 0.89 | 1.19 / 1.10 |
| 3 | 2 | <10 | 246 | 16 | 9 → stock kept (gated) | – | – |
| 4 | 2 | <10 | 36 | 2 | 0 → stock kept (gated) | – | – |

These 3-4-layer QPSK rows were measured before the gating. On the final binary (`replay_final`), 3-layer QPSK reads 22/22 and 4-layer 2 vs 1 of 32 (run noise).

On the final binary [MEASURED, `replay_final_*`, same capture]: 2-layer 16QAM at 10-15 dB goes 10 → 19 of 51; 2-layer QPSK below 10 dB goes 740 → 769 of 1606. No QAM or QPSK grant flips from pass to fail in aggregate. The 1-layer path is unchanged by construction: `nr_dlsch_mmse` is not called at Nl=1.

Per MCS at 12 dB or more, 2-layer, mode 2 (runs ab2-ab5):
- 16QAM MCS 5-8: 21 of 23 pass.
- MCS 9 (R 0.60): 1 of 4.
- 64QAM MCS 11-19: 1 of 116. The per-layer DD-SNR is 11-15 dB (`snr_l` in `replay_final2`).

At the 2-layer MCS 10-16 operating points the replay captures reach, the receiver is SNR-limited. The decision-directed EVM over-states 64QAM SNR below about 15 dB, and GENIE.md bounds channel estimation at under 2 dB.

**Gate status: NOT demonstrated.** "2-layer MCS10-16 and MCS17+ at 15 dB or more decode at 80 % or better where the SNR exceeds the requirement by 2 dB" cannot be tested on these replays. They hold only 15 2-layer 64QAM grants at 15 dB or more (2 pass with mode 2, 0 stock), all within about 1-4 dB of the MCS requirement. The high-SNR 2-layer QAM population exists only in the OTA logs (live: 64QAM at 15.8 dB median, 23/328 CRC). The bias that mode 2 removes was present on every one of those grants: the live stock path has the same code, and the replay kappa is 0.77 at 10-15 dB. The orchestrator's OTA A/B is the gate test. Expected: 2-layer QAM moves toward the 1-layer rates at equal per-layer SNR (live 1-layer 64QAM MCS10-16 at 16.5 dB: 65 %).

**MCS17+, 1 layer** [MEASURED live + replay]: live 1-layer MCS17+ runs at 13-16 dB against a requirement of about 17-24 dB (64QAM R 0.75-0.85, 256QAM), so it is SNR-limited. In replay capture 20261008 at 21-23 dB, 64QAM MCS17-19 decodes 7/10. The 1-layer 256QAM MCS21-23 grants at 21-25 dB, wideband 217 PRB, decode 0/6. This is not explained here and is listed as open. Their sign agreement and RE budget are clean.

## Low-MCS residuals (orchestrator scope addition)

Class shares [MEASURED, OTA LIVE ab_B1/ab_B2, t26f.env, EVMGRANT probe=0, MCS0-4 QPSK, 10 dB or more]:

| Nl | grants | fail | A: TRS occasion (abs%160 in 15/16/20/21) and RB < 52 | B: RB < 52, other slots | C: RB >= 52 |
|---|---:|---:|---|---|---|
| 1 | 2194 | 332 (84.9 % ok) | 69 fail of 69 (**0 % ok**), 21 % of failures | 172/986 fail (82.6 % ok), 52 % | 91/1139 fail (92.0 % ok), 27 % |
| 2 | 1336 | 170 (87.3 % ok) | 14 of 14 (0 % ok), 8 % | 111/582 (80.9 % ok), 65 % | 45/740 (93.9 % ok), 26 % |

Class B is slot-dependent:
- Failure rate is under 3 % at slots 0, 10, 6 and 16 of the frame.
- It is 15-40 % at slots 1-5 and 11-15.

Its RB and slot structure points at further periodic CSI-RS / ZP-CSI-RS in RB 0-51 that discovery has not exported.

Per-grant classification [MEASURED, replay DECODEDIAG attempt 0, MCS0-4, 10 dB or more]:
- `final2` (capture b): 13 failures. 5 have CSI exclusions configured and fall in the class A slots, 5 plain, 3 RV != 0.
- `t3` (capture 20261008): 14 failures. 12 plain (0x8f5a, 16 PRB, `sign_errors=0`, same geometry as its passing grants), 2 RV != 0.
- SSB overlap: 0 in every run.
- Probe-only/HARQ-combined: 0.

The RV != 0 class (retransmission without its initial transmission) is about 10-20 % of failures. It is not fixable without HARQ history.

New diagnostic `DECODEEVM` (with `ISAC_PDSCH_DECODE_DIAG=1`) logs the per-OFDM-symbol DD-SNR and, for the worst symbol, the SNR per data-RE index mod 12. Results:
- 0x37f3 512.15 (0+176, slot 15): symbols 5 and 9 at 8-9 dB against 15-18 dB elsewhere. Every odd subcarrier is at 6 dB.
- 465.0/465.1 (slots 20/21): symbols 4, 5, 8 and 9 degraded, with every third data RE at 6 dB.

So these grants have row-1 TRS combs on (4,8) and (5,9) that the configured exclusion set does not match. DECODE-FIX's offline CRC trials pass 0x37f3 9.1 only with RB 0-51 {(4,8) k%4=0, (5,9) k%4=1}, which is discovery's PCI-415 TRS pair {fd 0x1 @4/8, fd 0x2 @5/9}. That pair is discovered at offsets 15/16 but is needed at 20/21 too.

**Spec defect found and fixed behind a flag:** `nr_dlsch_csi_overlap_bitmap()` (stock OAI) applies every rate-matching CSI-RS resource to every allocated RB and ignores its CSI-FrequencyOccupation (`start_rb`, `nr_of_rbs`). TS 38.211 7.4.1.5.3 places a CSI-RS only in those RBs, with density-0.5 parity, and TS 38.214 5.1.4.1 rate-matches around them only. With the measured 52-RB TRS, a 176-RB grant lost 12 REs/RB in RB 52-175 as well.
- `ISAC_PDSCH_CSI_RB_EXTENT=1` (default off) uses a per-RB mask `nr_dlsch_csi_rb_mask()`. It applies consistently to G (`nr_dlsch_csi_unav_res`), the extractor, `nr_rx_pdsch`'s RE count and the SSB-plan count.
- Unit test `test_nr_csi_rb_extent` covers the production functions: extent, parity, and unchanged legacy behaviour.
- Paired runtime A/B (`ISAC_PDSCH_CSI_EXTENT_AB=1`, CSIAB lines, `replay_x1/x2/x3`): net neutral to slightly negative. On x2, Nl1 MCS0-4 at 10 dB or more is extent 220 vs legacy 223 of 242.
- Cause: the discovered resources are incomplete (the 20/21 occasions lack the serving cell's pair). The legacy full-band exclusion of a neighbour union happens to be a better superset on some grants (0x37f3 9.0/9.1).
- One alternate returned a RE-budget mismatch on a segmented (virtual-RB) allocation. The extent mask uses the virtual RB index there, so the flag must stay off for segmented grants until a phys-RB map is passed.
- **Recommendation: keep off.** It is correct per spec, but it only helps once discovery exports the complete serving-cell set.

A runtime "serving-cell template" retry (all same-ID resources seen in the last 160 slots, own extent and full band) recovered 1 TB in 4 replays and was removed. [MEASURED t1-t4]

**Low-MCS gate (Nl=1/2 MCS0-4 at 10 dB or more → 98 % or better): NOT met.** Replay with mode 2 and DECODE-FIX arms: Nl1 89.7-94.2 %, Nl2 88-97.5 %. What remains, in order of share:
1. Class B slot-patterned RB 0-51 failures. This needs discovery of the missing CSI-RS/ZP occasions; the classification and DECODEEVM give the exact symbols/combs to look for.
2. Class A TRS occasions. Same cause, and needs the full serving-cell TRS set per occasion.
3. RV != 0 without an initial transmission.
4. Small unexplained same-geometry failures, e.g. 0x8f5a frames 492-501.

## Flags for OTA (orchestrator)

t26f.env plus **`ISAC_PDSCH_MMSE_MODE=2`**. Optional diagnostics: `ISAC_PDSCH_DECODE_DIAG=1` (adds DECODEEVM) and `ISAC_PDSCH_MMSE_AB=0` (QAMAB lines pair stock against the primary on 2+ layer QAM; this costs one extra decode per such grant, so diagnostic runs only). Leave `ISAC_PDSCH_MMSE_MODE=1`, `ISAC_PDSCH_CSI_RB_EXTENT` and the `*_AB` flags unset in production. Read the result from EVMGRANT: `kappa` on Nl >= 2 QAM grants should sit near 1.0 at 15 dB or more (stock 0.7-0.9).

## GPU3 (nothing to mirror for the QAM fix)

`nr_pdsch_gpu_fep.cu k_llr` already computes a noise-whitened MMSE with the unbiased output `x/rho` and a per-layer SNR `rho/(1-rho)` max-log LLR. It never had this bias. The CPU mode 2 now matches its structure: unbiased per-layer estimate with per-layer reliability, ZF instead of whitened MMSE. Its LLR scale is the 1-layer `|h|^2` scale, and K38 normalisation follows.

If GPU FEP ever handles CSI-RS rate matching, it must use the same RB-extent rule as `nr_dlsch_csi_rb_mask()` behind `ISAC_PDSCH_CSI_RB_EXTENT`.

## Files

- `nr_dlsch_demodulation.c`: MMSE mode 2, per-layer LLR thresholds, multi-layer kappa, per-symbol/per-layer DD diagnostics, CSI-RS per-RB extent mask.
- `nr_pdsch_passive_decode.c`: QAMAB and CSIAB paired diagnostics, DECODEEVM, GrantWork fingerprint of both new knobs.
- `nr_pdsch_evm_acc.h`: diagnostic fields.
- `nr_transport_proto_ue.h`, `nr_ssb_rate_match.c`: extent mask in the SSB plan count (3 lines).
- `tests/nr_csi_rb_extent_check.c` and `CMakeLists.txt`.
- `tests/passive_rx/qamfix_replay.sh`.

Tests [MEASURED, DGX]: test_dlsch_fixed_point, test_nr_pdsch_xoverhead, test_nr_pdsch_prb_set, test_nr_ssb_rate_match, test_nr_ssb_rate_match_prod, test_nr_llr_norm and test_nr_csi_rb_extent all pass (7/7). The sens6 frozen gate is clean.
