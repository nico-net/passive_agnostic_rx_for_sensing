# DECODE-RES (csd/decode-res, 2026-10-10) -- PARTIAL: item 1 only

Host DGX GB10. Replays: capture b (raw_pci415_12s_b), env ota/t26j.env minus radio flags (tests/passive_rx/decres_replay.sh), heavy.sh. Capture 8 not used. Items 2-5 NOT done (session limit).

## Item 1 (class B)
Flags (all default off): ISAC_PDSCH_TINT_AB (paired TINTAB diagnostic), ISAC_PDSCH_TINTERP_RETRY (flipped time interpolation arm), ISAC_PDSCH_EVM_RB (DECODEEVMRB per-RB map), ISAC_PDSCH_OWN_PDCCH_RM (+ ISAC_PDSCH_OWN_PDCCH_DB, default 4).

1. TINTERP [MEASURED, paired, same samples, 1 run, 285 MCS0-4 >=10 dB grants]: flipping ISAC_CHEST_TINTERP changes SNR on 1667/5220 decodes but CRC on 0 grants (primary-only 0, flipped-only 0). It is NOT the class-B lever. The within-symbol SNR drift (sym3 26 dB -> sym10 17 dB) is real but the failures are not decided by it.
2. OTA ab_J1 [MEASURED]: class B fail concentrates in S=1/L=13 (nsym 11) grants in slots 1-5/11-15, RB0 start: 54-65 % ok vs 95-98 % for nsym 12 (S=0) and 85-99 % outside those slots. In replay, the DCIRAW TDRA field (payload bits 16-19) is 0000 for 120/123 pass and 1000 for 93/108 final pass: failures are the TDRA=1000 population (true S=0, parsed S=1/L=13; the DECODETDRA arm recovers most).
3. Root-cause hypothesis [MEASURED + INFERRED]: a true S=0 grant loses the PRBs of its own PDCCH on symbol 0 (TS 38.214 5.1.4.1). Per-RB DD-SNR maps (DECODEEVMRB) show symbol 0 dirty on a contiguous PRB run in the remaining failures (e.g. 0x37f3 192.5 RB 48-59, 478.5 RB 48-59, 202.14 RB 24-35; runs are multiples of 6 PRB = CCE size).
4. Fix: ISAC_PDSCH_OWN_PDCCH_RM=1 runs last in the retry chain: decode S=0, detect the dirty run, retry with those PRBs rate-matched on symbol 0 (SSB-plan mechanism), +-1 PRB edge hypotheses, logs OWNPDCCH. Gains over all other arms (identical samples): 12 / 11 / 3 grants in three runs (previously 0 in every baseline run). Pooled whole-run class-B MCS0-4 >=10 dB: base 85.4 / 91.7 %, with flag 88.0 (arm ran first, buggy order) / 91.6 / 71.4 % (low-sample run). Run-to-run sample sets differ (bimodal acquisition), so these totals are NOT evidence; the per-grant gains are.
5. Gate (class B >= 95 %) NOT met: the arm recovers ~10 grants per run of ~10-15 remaining failures at best; the rest (e.g. clean 0x007f S=2/L=12, 0x37f3 860.12 at 21 dB) are unexplained.

## Items 2-5: not done
Open: non-TRS full-band CSI-RS (abs%160 24/63/105/135), RV!=0 self-decodable share, 2-layer QAM margin list, DCI-miss table. Bimodality seen: replays of opr3 reached only ~1900 EVMGRANT versus ~5600 in others (acquisition); not investigated.

## OTA env
t26j.env + ISAC_PDSCH_OWN_PDCCH_RM=1 (optional diagnostics: ISAC_PDSCH_DECODE_DIAG=1 ISAC_PDSCH_EVM_RB=1). Do not set ISAC_PDSCH_TINTERP_RETRY / ISAC_CHEST_TINTERP.
Tools: tools/decres_final.py, decres_rbmap.py, decres_tintab.py.
