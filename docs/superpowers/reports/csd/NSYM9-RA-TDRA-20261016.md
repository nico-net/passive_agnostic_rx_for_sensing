# DL nsym-9 0 % class: RA-RNTI RAR decoded with the wrong default TDRA row (2026-10-16)

Sonnet agent, commit 9db7f4b113 (nr_pdsch_passive_decode.c only); saved by the orchestrator.

## Cause (Vodafone)
- EVMGRANT `nsym` = number of DATA symbols (dec.evm_nsym), not L [CODE-READ nr_pdsch_passive_queue.c:2220-2232]: nsym 9 + ndmrs 3 = L 12, S 2, DM-RS 2/7/11. (Health-grid nsym classes are data symbols.)
- The V class (tbl 0, mcs 0, 25-51 PRB, Nl 1, nsym 9) is one RNTI: RA-RNTI 0x007f, DCI 1_0 RAR in Type1-CSS, rb 0+48, type A S=2 L=12 [MEASURED, DGX OTA 2026-10-16, V aVX1: 23 grants 0 OK; aVX2 7, xVaXE1 16, all 0 OK]. DCIs genuine (7 of 32 DCIQUAL lines mismatched_bits=0). All other nsym <= 10 EVMGRANT lines are solver probes (probe=1).
- V SIB1 has no servingCellConfigCommon (NSA-style) [MEASURED run.log], so no pdsch-ConfigCommon TDRA list: the blind extractor falls back to default table A (tda_common_count=0, tda_count=0 -> get_dl_tda_info) [CODE-READ nr_pdcch_blind_monitor.c:4503-4546], row 0 = S=2 L=12 (TS 38.214 5.1.2.1.1, Table 5.1.2.1.1-1). The cell's actual row 0 is S=1 L=13. DM-RS table for ld=14 is correct [CODE-READ nr_pdcch_blind_monitor.c:4066-4079, 4098-4140].
- No recovery arm covered it: all retry arms are gated on dci11 && rnti_class==0 (passive_decode_retry ~:6560), and the S toggle only covers S<=1 ending at 14.

## Fix (default off: ISAC_RA_TDRA_SWEEP=1)
For a failed non-SI, non-C-RNTI DCI 1_0 grant ending at symbol 14, re-decode with type-A S=0..3, DM-RS recomputed by nr_pdcch_blind_dmrs_mask (pos2, maxLength 1); inline or on the async recovery worker (credited via RECOVLATE). Spec rules only. No unit test (S/L/DM-RS computation unchanged); ctest not run.

## Replay A/B [MEASURED, DGX replay raw_pci415_12s_b, t26aXE, n=2 per arm]
| | A (off) | B (ISAC_RA_TDRA_SWEEP=1) |
|---|---|---|
| RA 0x007f decoded | 0/24, 0/21 | 23/23, 19/19 (all S=1 L=13) |
| class (0,0,25-51,1,9) | n_q 16, 0.0 % | n_q 31, 51.6 % (rest = floor / first passes) |
Other classes with n_q >= 40 unchanged within noise. CAPSET=tim n=1: no RA grants, no change.

## Remaining
- TIM nsym 9 (190 grants) is NOT this: C-RNTI DCI 1_1, ndmrs 2, all in slot%10 == 3 [MEASURED, T aTX2 OTA, probe=0]: slot 3 nsym 11 2089 grants 0 OK, nsym 9 222 grants 0 OK — the whole slot 3/13 is dead regardless of length. The TIM MAC log reports a TDD special slot with 6 DL symbols [MEASURED, not verified against slot 3]; lead [INFERRED]: grants there are decoded as full-slot (S=0/1 to 14) while the real PDSCH in the special slot is shorter; check nr_tdd_pdsch_last_symbol clipping in the decode. This is the "TIM dead slots 3/13" of DLCRC-ROOTCAUSE section 3.
- V nsym 10 (51 grants, 11.8 %) not analysed.
- Orchestrator note: the retry costs a failed first pass per RAR; the general fix is to learn the cell's RA/common TDRA row once (first S that decodes) and use it first.
