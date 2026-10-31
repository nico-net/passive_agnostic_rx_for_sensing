# fix1031/rm -- D5: CSI-RS/TRS rate matching that works on any cell

Branch fix1031/rm (base mpci-int-1031). Evidence labels as PROJECT_MEMORY 0.1. All results below are `[OFFLINE VERIFIED, DGX aarch64 unit]` unless marked.

## What changed
- `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_rm_arb.{h,c}` (new; compiled into nr_csirs_blind_rt.c by `#include` so the fixtures that include that file link unchanged):
  - `nr_rm_consolidate`: fragments of one resource (same type/row/symbols/subcarriers/density/scramblingID, RB ranges overlapping or separated by less than the narrower one) become their RB hull (0:52 + 128:52 + 64:116 -> 0:180).
  - `nr_rm_build`: candidate family per grant: OWN (the former `scramb_id == DM-RS ID / co-block` rule, always first, byte-identical lists), ALL_CONF (every non-clashing resource of the slot rate-matched, nothing erased), ALL_FOOT (+ discovery footprints as RE patterns, DM-RS symbols stripped), ALL_WIDE (resources merged from >= 2 fragments widened to the BWP), NONE. Candidates with equal RE sets are dropped; a slot with one candidate (no CSI-RS, no footprint, or all equal) has `cls == 0` and is never arbitrated.
  - Vote cache (per cell, one mutex, 256 entries LRU) keyed (RNTI, slot class = order-independent hash of the slot's resource list, DCI rm_ind, DCI zp_trig); plus a cell entry (RNTI 0) as prior. Adopt: >= 2 passes (cell: 3) and strictly more than any other kind; an alternate's pass is a vote against the failed first candidate; 3 consecutive strong failures of the adopted kind reset it (re-test).
- `nr_csirs_blind_rt.c`: `full_select` now = `full_gather` + `nr_rm_select_own` (same result); new `nr_csirs_rm_candidates`, `nr_csirs_rm_apply` (rate-match list into cfg + erase list into the demodulator's thread state, so the CPU path and the GPU job, which copies `nr_dlsch_csi_erase` and `csiRsForRateMatching`, see the same RE list).
- `nr_pdsch_passive_decode.c`: `passive_decode_entry` builds the candidates, applies the adopted (else OWN) one; `rm_arb_step` (after the first body, before every other recovery arm) votes the first decode and, on a strong-DM-RS CRC failure (`nr_slotphase_strong`, Nl capped at 2 for the SNR test), decodes the other candidates in vote order with `t_quiet`, within RETRY_BUDGET_NS; a pass is the result (`config_recovered`), none: plain view and erase list restored. Not in RECOVERY_ASYNC mode 1 (the worker, mode 2, does it).
- DCI fields: `nr_pdcch_blind_result_t.rm_ind/zp_trig` (read where the widths were only consumed, nr_pdcch_blind_monitor.c extract_11_impl), `nr_pdsch_passive_grant_t.rm_ind/zp_trig`, set in nr_pdcch_blind_monitor_rt.c job builder. They key the vote cache (value -> RM set learnt by CRC).
- `tools/mpci/static_audit.py` + `mpci_statics.tsv`: rows for the new statics (g_rm, g_rm_lock PER_CELL; t_rm_ctx TLS).
- No new env var / CLI / -D.

## Tests
`test_nr_csirs_rm_arb` (11 tests, pass): fragment merge; different IDs / far ranges stay separate; no CSI-RS -> 1 candidate, cls 0; OWN == former selection; TRS with a scrambling ID different from the DM-RS ID: OWN erases it (fails), arbitration adopts ALL_CONF by CRC, then the RNTI and (after 3 passes) a new RNTI use it first; OWN stays adopted where it works; fragmented footprints -> one hull candidate and a widened candidate, only the widened one passes; footprint DM-RS symbols stripped / clashing resources dropped; rm_ind value keys separate decisions; retest after failure streak; identical candidates not repeated.
Also pass: test_nr_re_pattern, test_nr_pdsch_slotphase, all test_nr_csirs_* (blind_search, disc_*, cell_iso, blind_synth, ports), test_nr_csi_rb_extent, test_nr_ssb_rate_match. Build: nr-uesoftmodem, pdsch_gpu, ldpc_cuda OK. test_nr_ssb_rate_match_prod fails (known ARM). static_audit_check / test_static_audit_scanner fail on the base too (4 NEW rows from other merges: g_ul_dmrs_pin_ns, g_tp_spec_refuted, g_phs, g_ul_pin_seen_epoch); none from this change.

## NOT verified
No replay, no on-air run, no latency measurement, no GPU-pass run (GPU honours the lists by construction: same cfg + erase set copy, unit-checked only on the CPU list builder). Widening to the BWP, the gap rule (< narrower width) and the vote thresholds (2 / 3 / streak 3) are design choices, not fitted to a cell.

## Expected on-air effect [HYPOTHESIS]
Structural all-CB failures of QPSK/16QAM strong grants in TRS slots (415 slots %20 in {0,1,15,16}, 923 abs%40 {8,18}) get rescued by the alternate candidate and then decode first time; no change in slots without CSI-RS. Cost: extra bodies only on strong-DM-RS failures in slots with > 1 candidate (bounded by RETRY_BUDGET_NS).

## Counters / log fields
`nr_rm_arb_stats()` (tries, passes, adopts) is exposed but not yet printed; a grant rescued by an alternate has `config_recovered=1` and an unchanged `numCsiRsForRateMatching` change visible in EVMGRANT. A log line for adoption is the first thing to add if the controller wants it (no behaviour change).

## Risks for other fixes
- nr_pdsch_passive_decode.c: `passive_decode_entry` and a new hook right after the `out->skip` check in `passive_decode_retry_impl`; the retry arms (RATDRA, csi_variants) run unchanged after it.
- nr_pdcch_blind_monitor_rt.c grant initialiser (one line), nr_pdsch_passive_decode.h grant struct (one line), nr_pdcch_blind_monitor.{h,c} result struct / extract (3 lines).
- Observation: the SLOTPHASE variants applied by the monitor into `dl.csiRsForRateMatching` appear to be overwritten by the former `nr_csirs_full_apply` at decode entry (and now by the candidate apply); not changed here, worth checking by whoever owns slotphase.
