# DL CRC root cause (Phase 0 Task 5), 2026-10-16

Sonnet agent; saved by the orchestrator. Tools: tests/passive_rx/dl_crc_split.py (745a25264c, 07090f27e4 adds `mcs`); scratch scripts in the session scratchpad (ext*.py, an*.py, sim.py).
Runs: V = ab_aVX1/2 + ab_xVaXE1/2/3; T = ab_aTX1/2 + ab_xTaXE1/2/3 (n=5 per cell, cells never pooled; aVX3 excluded). probe=0, rv=0, reserved MCS removed. "Qualified" = snr_l >= req + 2 dB (goal_metrics.py).
Labels: [MEASURED, DGX OTA 2026-10-16, V|T, arms X/Xold, n=5], [CODE-READ], [SIMULATED], [INFERRED].

## 0. Answer
Two different causes, one per cell; the "+8 dB lost in the DL chain" is mostly NOT a receiver loss.
1. `snr_l` is optimistic by construction (T: 90 % of qualified failures, V: 65 %). It is a decision-directed EVM, so pure noise reads 3.9 / 9.1 / 12.5 / 14.4 dB for QPSK / 16QAM / 64QAM / 256QAM, and low-MCS grants pass the +2 dB gate at any true SNR.
2. T: with the estimator corrected, the receiver is healthy (80 % at ~+3 dB corrected margin). The remaining high-SNR failures sit in deterministic (slot, frame-parity) bins: periodic REs the receiver does not rate-match around [INFERRED].
3. V: a real, margin-independent ceiling of 70-75 % CRC-OK. Passing and failing grants have identical SNR and equaliser metrics; failures depend strongly on the slot. Best-supported cause: PDSCH time-domain allocation ambiguity (S=0/L=14 vs S=1/L=13), as in CSIFULL.md section 6 "class B".

## 1. The estimator
[CODE-READ nr_dlsch_demodulation.c:2166-2300] separate zero-forcing re-equalisation of the grant REs; per-layer rescale to ideal constellation power including noise (`scale[l]`, :2230); error = distance to the nearest constellation point (:2240-2248); `layer_snr_db = 10 log10(ref/err)` (nr_pdsch_passive_decode.c:472). For Nl=1, snr = snr_l = -20 log10(evm/100), so "EVM-SNR vs snr_l" is the same number. LLRs do not use this equaliser. The solver is fed the same biased value (nr_pdsch_solver_note_snr, nr_pdsch_passive_queue.c:2220-2240).

[SIMULATED, AWGN, same estimator] estimate vs true SNR:
| true SNR | QPSK | 16QAM | 64QAM | 256QAM |
|---|---|---|---|---|
| <= -10 dB | 3.9 | 9.1 | 12.5 | 14.4 |
| 0 dB | 4.5 | 9.6 | 13.4 | 15.6 |
| 6 dB | 7.1 | 10.8 | 15.2 | 18.4 |
| 10 dB | 10.3 | 12.0 | 16.5 | 20.7 |
| >= 20 dB | ~equal | ~equal | ~equal | ~equal |
[MEASURED] failing T QPSK MCS0 grants: median snr_l 3.3 dB = the QPSK floor. They dominate the over-populated +8..+10 dB bin (24.8k grants, 92 % MCS0, 16 PRB, 64 % OK).

[MEASURED, corrected margin = inverted simulated curve - req]
| corrected margin | T CRC-OK | V CRC-OK |
|---|---|---|
| 0..2 dB | 56 % (non-dead bins) | 32 % |
| 2..4 dB | 72 % | 52 % |
| 4..6 dB | 90 % | 62 % |
| 6..8 dB | 98 % | 66 % |
Qualified failures not truly qualified (corrected margin < 2 dB or on the floor): T 31279 / 34899 (90 %), V 45870 / 71068 (65 %). Qualified grants on the noise floor: QPSK 39456/96187 (V), 43934/83021 (T); 16QAM 4605/28598 (V), 6366/17724 (T); 64QAM 1018/14274 (V), 486/7416 (T).
Caveat [INFERRED]: AWGN + ideal ZF inversion; multi-layer noise enhancement and chest error not modelled; ~+-1.5 dB near the floor.

## 2. Ranking of lost qualified grants
| cause | T lost | V lost |
|---|---|---|
| 1. estimator optimism (not truly qualified) | 31279 (90 %) | 45870 (65 %) |
| 2. V slot/TDRA-dependent ceiling (corrected margin >= 2, no dead bin) | 1194 (3 %, residual) | 24258 (34 %) |
| 3. T periodic dead bins | 2426 (7 %) | 940 (1 %) |

## 3. T: deterministic failures at high SNR
[MEASURED, T, corrected margin >= 8 dB, n=35521, CRC 96.5 %] 96 % of failures are in a few (slot, frame-parity) bins with CRC < 10 %: slots 8/18 of EVEN frames 0 % (n=896; same slots in odd frames 100 %, n=1256); slots 3/13 with nsym=11 0 %. Present in all five runs. [INFERRED] a 20 ms periodic transmission (CSI-RS/TRS burst) not in the confirmed rate-matching set; same mechanism as CSIFULL.md section 1 for PCI 415; not checked against T's CSI-RS bank. Without the dead bins T CRC is 97.3 % over properly qualified grants (n=44949) vs 92.4 % with them. T TDRA retries recover 18 of 75407: TDRA is not T's issue.

## 4. V: the margin-independent ceiling
[MEASURED, V, corrected margin >= 8 dB, n=52903, CRC 70.8 %] Flat across lag, tot, qwait, Nl, ports, cdm, PRB start, MCS, pid, core and RNTI (per-RNTI rate unimodal 50-70 %).
Pass vs fail (MCS0, 16 PRB, QPSK, corrected margin >= 10 dB):
| | n | snr_l | evm % | cpe-snr | rho | alpha | nvar |
|---|---|---|---|---|---|---|---|
| pass | 23995 | 10.4 | 27.7 | 0.20 | 0.24 | 0.96 | 27.8 |
| fail | 9371 | 10.0 | 29.2 | 0.20 | 0.24 | 0.95 | 27.4 |
=> invisible to equalisation; the error is after demod or in the RE map (TDRA, rate-matching RE set, scrambling, layer mapping).
Slot dependence (MCS0, 16 PRB, nsym=11): CRC-OK slot 0: 96 %, 1: 42 %, 2: 48 %, 3: 47 %, 4: 77 %, 5: 63 %, 6: 95 %, 10: 93 %, 11-13: 46-48 %, 14: 80 %, 15: 61 %, 16: 91 %; nsym=12 in the same slots 87-92 %. One symbol of difference -> start symbol / length.
Existing evidence: CSIFULL.md section 6 (PCI 415 = V) class B in slots 1-5/11-15, 23/31 and 24/35 recovered by the TDRA arm (S=1/L=13 <-> S=0/L=14). In these runs DECODETDRA recovers V 18320 / 62655 (S=1/L=13 attempts) and 8005 / 27073 (S=0/L=14), ~29-30 % each way; T 18 / 75407. V's TDRA is genuinely ambiguous.
Open: whether EVMGRANT logs first, final or both attempts (12 % of V (rnti, frame, slot) keys have > 1 EVMGRANT line), so some failures may be retry intermediates later recovered. Side dips: V frame%8==1 slots 4/1 (45 % vs 80 %), slot 11 frame%16==2 0 %: periodic unmodelled REs.

## 5. Code read [CODE-READ]
nr_dlsch_demodulation.c:2166-2300 (EVM/snr_l); nr_pdsch_passive_decode.c:456-472 (evm_fill); nr_pdsch_passive_queue.c:2220-2240 (EVMGRANT print, solver SNR feed); nr_pdsch_passive_decode.c:2858-2880 (CSI-RS rate-matching G count, confirmed resources only; segmented odd/even parity refused without ISAC_CSIRS_FULL); :6393-6530 (TDRA retry, ISAC_PDSCH_TDRA_RETRY); :6017-6040 (static_csi_resources, PCI 415 only); :1690-1742 (TDRA S/L used for TBS and G: a wrong S/L changes G -> CRC fails with good EVM).

## 6. Actions (not run)
1. Metric: noise-floor-aware qualification (e.g. snr_l >= floor(Qm) + 3 dB with the inverted margin, or a DM-RS-based SNR from nvar_db/alpha); re-baseline the scorecard. G1(b) on today's qualification is not meaningful for QPSK/16QAM.
2. V confirming test: replay raw_pci415_12s_b with ISAC_PDSCH_TDRA_RETRY=0 vs 1 and ISAC_PDSCH_DECODE_DIAG=1; compare per-slot CRC. If the failing set in slots 1-5/11-15 equals the retry-recovered set -> TDRA ambiguity; fix = learn the time-domain table per RNTI (resolve S/L on first success).
3. T dead bins: replay raw_pci923_tim_12s_tile3 with ISAC_CSIRS_FULL=1 ISAC_CSIRS_FULL_AB=1; compare CRC in even-frame slots 8/18 and slots 3/13.
4. Default-off per-symbol EVM in EVMGRANT (err_sym/ref_sym accumulators exist, nr_dlsch_demodulation.c:2253) to see whether failing V grants have one bad edge symbol.
Unchecked: AWGN-only inversion; duplicate EVMGRANT keys; T dead-bin cause (hypothesis).
