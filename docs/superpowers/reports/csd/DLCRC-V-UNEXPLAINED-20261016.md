# Vodafone "unexplained" DL failures: accounting error + TDRA S ambiguity (2026-10-16)

Sonnet agent (tool fix bbc03bbc98) + orchestrator re-score. Labels: [CODE-READ], [MEASURED, DGX replay raw_pci415_12s_b, t26aXE, n=2], [MEASURED, DGX OTA 2026-10-16, <cell>, <arm>, n=1 per row].

## Cause
1. EVMGRANT logs only the FIRST pass [CODE-READ nr_pdsch_passive_queue.c:2185-2245]. With ISAC_PDSCH_RECOVERY_ASYNC=1 (t26aXE and the OTA env) a failed first pass goes to the recovery worker (TDRA S toggle, CSI-RS, CDM, wide MRC; nr_pdsch_passive_queue.c:2692-2723, nr_pdsch_passive_decode.c:6393-6640); a rescue logs only `RECOVLATE rnti fr.slot ... recovered_cfg=` and `DECODETDRA after=1`, no second EVMGRANT. goal_metrics.py / dl_crc_split.py now join RECOVLATE on (rnti, frame.slot) and credit the grant (`recovered=` count; test_recovlate_credits_first_pass_failure).
2. The V slot dependence is the TDRA start-symbol ambiguity (S=0 vs S=1), already fixed by the async retry: rescued share of QPSK grants with DD-SNR >= 11 dB by slot%10 (replay run 1 / run 2): 0: 0/22, 0/38; 1: 5/24, 9/30; 3: 9/33, 6/29; 5: 25/44, 22/47; 6: 0. With rescues credited, final CRC ~95 % in every slot class.

## Replay (qualified CRC, credited where noted)
| arm | n | qual CRC | slots 1-3/11-13 | slots 0/6/10/16 |
|---|---|---|---|---|
| retry=0, first-pass log | 2 | 84 / 75 % | 74 / 65 % | 90 / 92 % |
| retry=1 + CSIFULLAB, credited | 2 | 90 / 92 % | 85 / 88 % | 93 / 93 % |
| retry=1 base, credited | 2 | 93 / 95 % | 94 / 95 % | 90 / 93 % |
| retry=1 + OWN_PDCCH_RM, credited | 2 | 94 / 92 % | 92 / 90 % | 95 / 90 % |
(base arm had fewer grants, 1874/3258, host load.) Same capture replayed: n=2 reflects scheduling jitter only.

## Hypotheses refuted
SSB/PBCH (rate-matched per slot; overlap and non-overlap grants fail equally; slot 0 carries SSB and is good); own-PDCCH RM (no change); single bad symbol (DECODEEVM flat ~13 dB); LLR sign errors (~1e-4 both classes).

## OTA re-score with recoveries credited (goal_metrics bbc03bbc98, corrected SNR rule)
| run | tot p99 us | crc_all % | n_q | crc_q % | recovered |
|---|---|---|---|---|---|
| V aVX1 | 11248 | 51.0 | 17078 | 90.0 | 4386 |
| V aVX2 | 13345 | 49.8 | 16733 | 87.5 | 5451 |
| V qVaE1 | 12653 | 47.5 | 38979 | 84.5 | 6273 |
| V qVaE2 | 10991 | 61.1 | 22091 | 90.9 | 5947 |
| V qVaE3 | 15430 | 49.3 | 15814 | 87.2 | 8067 |
| T aTX1 | 8657 | 64.1 | 11981 | 94.8 | 6 |
| T aTX2 | 6679 | 55.6 | 12369 | 88.4 | 4 |
| T qTaE1 | 12824 | 62.5 | 8310 | 92.2 | 1 |
| T qTaE2 | 10320 | 58.4 | 2371 | 94.3 | 0 |
| T qTaE3 | 10457 | 59.4 | 7589 | 91.9 | 3 |
=> Both cells pass the G1 CRC criterion (V 84-91 %, T 88-95 %); the "V 70-75 % ceiling" was the first-pass accounting.

## Consequence for G1 latency (orchestrator)
- 10-15 % of V grants (4.4k-8.1k per run) decode only in the async recovery, i.e. well after the first pass; RECOVLATE carries no time, and `tot` keeps the first-pass latency, so the scorecard UNDERSTATES V latency for these grants. For the 500 us goal the S ambiguity must be resolved before the first pass (per-RNTI S/L learning from earlier rescues, ISAC_PDSCH_RETRY_PREDICT is on but does not prevent them), and RECOVLATE must log the recovery completion time.

## Remaining (~5-8 % of qualified V grants in the bad slots)
Wide grants (>= 48 PRB, RB 0) near the estimator floor; single-symbol dips on symbol 7 (abs%160 = 135, the undiscovered full-band non-TRS CSI-RS of CSIFULL section 1) and symbols 4/8; symbol-1 dips in S=1 grants (likely a 2-symbol CORESET with other users' PDCCH); rnti 0x007f always S=2 L=12, never decodes at 17-22 dB; wide grants over the SSB RBs in slots 1-3/11-13 ~70 % vs ~90 % (n 8-30, suggestive).
