# fix1031/ulcov: UL coverage (U3, U4, U6, UE-ID eviction, UL timing)

Branch `fix1031/ulcov` off `mpci-int-1031`. Context: the offline power study (csd/int16-ulpow, UL-POWER-20261018) found the scheduled UL PRBs at
the DEIB site carry no power above empty PRBs, so none of this can be validated on the DEIB captures; the effects below are to be validated on
rfsim / OCUDU beds. No flags, no env vars, no per-cell constants were added.

## What changed

### 1. U3: UCI on PUSCH for CRC16 TBs (also DFT-s-OFDM)
- `nr_pusch_uci_blind.c` `search_small()`, `small_try()`, `small_confirm()`, `ack_beta_mask()`; `nr_pusch_uci_blind_search()` early-returned for
  `tbs <= 3824`, now branches to the small path. `nr_pusch_uci_blind.h`: `nr_uci_search_out_t.provisional`.
- Hypotheses (bounded, <= 1 + 6 O values x 16 betas, each distinct RE count once; stops on `budget_us`): HARQ-ACK of <= 2 bits (reserved REs),
  rate-matched HARQ-ACK with O from the DAI prior / the O values the RNTI used / ascending, over the 16 TS 38.213 Table 9.3-1 betas; plus the CSI
  parts learned from the RNTI's CRC24 wins (`lrn_t.csi`).
- Confirmation rule: a CRC16 pass is NOT delivered. It is held per RNTI (`lrn_pend_t`, 4 entries): class (<= 2 bits / rate-matched), CSI key, and the
  set of HARQ-ACK betas that reproduce its RE count (the same beta gives different counts on different widths, so the test is on beta sets). A later
  grant of the RNTI passing with the same class/CSI key and a common beta delivers it and calls `learn()`. After a confirmed no-CSI <= 2-bit pass
  (`punct_conf`) that class is delivered at once; learned state (`wins >= 1`: only CRC24 wins or confirmed passes create it) is delivered on one pass. The default set runs only when the caller's
  rate limiter allows (`explore`), the learned part always.
- `nr_pusch_passive_decode.c` (~2831): the `tbs > 3824` and `!transform_precoding` conditions of the call are removed. A failed/provisional search
  restores the codeword and the plain verdict (existing path, `tb_tests > 0`).
- Cost of the confirmation: the first pass of a hypothesis per RNTI is lost (held), by design.

### 2. U4: UL TDRA rows as CRC-arbitrated hypotheses
- `nr_ul_trial_variant.h`: `NR_UL_TDA_ROWS = 8`, `nr_ul_tda_row()` {S0 L14 A, S0 L13 B (pre-existing), S0 L13 A, S0 L12 A, S0 L10 A, S2 L10 B, S4 L10 B,
  S4 L8 B}, `nr_ul_tda_legal()`, `nr_ul_variant_count_rows/get_rows`, `nr_ul_variant_tda_round()`.
- `nr_pdcch_ul_layout_lock.c`: variant bits 4..6 now carry the row (was bit 4 = S0 L13 B); `apply_variant()` applies it; trial tag mask 0x7ffff -> 0xfffff
  (20 bits, still below the TRIAL bit 60), `var & 63` -> `& 127` (3 sites). Live trials (`nr_ul_layout_crc_trials`): while a length has no decisive
  CRC pass (`ev_dec == 0`) one variant cycle in four tries rows 2..7 instead of the two-row core; stage B (`nr_ul_layout_crc_hyps`) enumerates all 8 rows
  while `ev_dec == 0`, the two-row core after. The existing evidence rule (decisive CRC, or CRC16 in >= 2 slots with the Bonferroni bound over the tracked
  hypotheses) decides; nothing new latches.
- Not covered: Table 6.1.2.1.1-2 rows S4 L6 B and S8 L6 B, rows with K2 > k2+1, a different row per TDRA index (all rows of a locked layout share one
  (S,L,mapping)), special-slot UL allocations such as S10 L4.

### 3. TP by trial CRC per RNTI: NOT implemented
The length-level choice is already CRC-arbitrated: the trial cycle covers both tails (CP-OFDM / TP) and `crc_lock_locked()` latches the passing one;
`lock_length_locked()` picks by cost only until a trial passes, and the merged N_RB legality rule refutes TP tails. A per-RNTI override needs per-RNTI
failure accounting and per-RNTI trial booking on a locked layout (`crc_trials` returns 0 for a decisively locked length, and `crc_fail_run` is reset by
any RNTI's pass), which I could not validate offline. A mixed TP/CP cell remains unsupported.

### 4. U6: UL sweep on the second-pass (dedicated-BWP) CORESET
- `nr_pdcch_blind_monitor_rt.c`, end of the "Passive BWP: second monitoring pass" block: the existing hashed UL length sweep (`nr_pdcch_dci_length_sweep_feed_budget`,
  `ul_length_score`, GPU prefill, CFAR empty-CCE screen) runs on the pass-2 candidates, keyed by a pass-2 geometry key (CORESET start/width/duration/ref/nID/PCI,
  distinct seed from `length_coreset_key`), per RNTI via `nr_pdcch_dci_length_context(b2, g2, rnti)`, for a DL-resolved UE with no UL length on that
  geometry, only on the mapping hypothesis that has CRC votes (`g_p2_votes >= 3`). On a lock the same notifications as the main pass run
  (`nr_ue_ctx_on_param`, `nr_pdcch_ul_bank_note`, `ul_cell_len_note`, bank converge). Then every RNTI with a resolved UL length on that geometry gets a
  0_1 `ul_scan` task per candidate (`ul_auto = 1`, scrambling as the DL pass-2 tasks / the C-RNTI when the CORESET carries a configured DM-RS id).
- Simplifications versus the main pass: no scout/second length, no DM-RS candidate ranking, no USS-hash gate.

### 5. UE-ID eviction
- `nr_pusch_passive_ul_ids.c`: `ul_ids_ue_t.last_seen`, `nr_pusch_passive_ul_ids_victim_locked()`: when the 64 entries are full, RNTIs without a CRC pass go
  first (least recently seen first), then the least recently seen CRC-proven one. New counters `nr_pusch_passive_ul_ids_evicted()` (next to `_dropped()`),
  test helper `nr_pusch_passive_ul_ids_has()`. The per-RNTI data-ID lock (separate table, 64, round-robin) is unchanged.

### 6. UL timing (no new search)
- (a) `nr_ul_ta_rules.h`: `nr_ul_nta_offset_tc()` / `nr_ul_nta_tc_to_samples()`; `nr_pusch_passive_monitor_rt.c` `n_ta_offset_samples(ue)` uses the SIB1
  n-TimingAdvanceOffset (`ue->nrUE_config.cell_config.N_TA_offset`: 0 / 25600 / 39936, 0xFFFFFFFF = absent -> 25600) once the MAC applied the cell
  configuration (`phy_cell_id == (uint8_t)Nid_cell`; a zeroed config would read as n0). The operator override `ul_ta_offset_samples` still wins.
- (b) `nr_pusch_ul_disc.c` `nr_pusch_ul_disc_timing_evidence()`: per RNTI, 3 arrival-window candidates (a1 = window advance - closed-form delay, +-4 samples),
  non-coherent sum of dof*ratio with e-folding decay of 600 slots; the first observation of a candidate pays the union over its delay trials, later
  ones the positions inside the window; accepted at the same 1e-6 bound as the other evidence rules (x3 candidates). Used by the worker (`ul_bg_run`,
  `undec` branch, via the new `nr_pusch_ul_disc_timing_out_t.undec_a0`) and inline in the decode (where the worker drops the job: `bg_expired`), both
  train the tracker (`timing_update_ev`) so the next grants seed near the arrival. The inline path moves the window and re-FEPs on acceptance
  (as the existing single-grant rule), the worker does not store a hypothesis from it (identity may be unconfirmed).
  New counters in the `UL_TIMING_CF` line: `tm_evidence=<worker>/<inline>`.

## Tests `[OFFLINE VERIFIED, DGX aarch64 unit]`
Results (DGX aarch64, built with heavy.sh -j6; full `nr-uesoftmodem pdsch_gpu ldpc_cuda test_nr_pdcch_blind_monitor nr_pusch_ucisim` build clean):
`test_nr_ul_ids_evict` 2/2, `test_nr_uci_small` 6/6, `test_nr_ul_rules` 7/7, `test_nr_pusch_ul_disc` 27/28 (the one failure also fails on the base build),
`test_nr_ul_layout_lookalike` ok, `test_nr_pdcch_blind_monitor` pass, `test_nr_pusch_uci_{mapcheck,ack_rm,csi,ack_csi,csi2_oai,narrow,small}` pass,
`test_mpci_*` (a3..a6, cellfeat, audit, percell_scorers) pass. `static_audit_check` / `test_static_audit_scanner` fail on the base too (3 rows of other work
not in `tools/mpci/mpci_statics.tsv`: g_tp_spec_refuted, g_phs, g_ul_pin_seen_epoch); the 4 rows this branch adds are in the table.
- `test_nr_pusch_uci_small` (new ctest, `nr_pusch_ucisim`, real OAI UE TX with 4-bit rate-matched HARQ-ACK at beta 9 on 6 PRB QPSK, real LDPC): trial 1 held (found the
  true footprint, RE counts equal the UE's: provisional), trial 2 delivered with the right payload, trials 3-4 delivered by the learned state in 1 TB test.
- `test_nr_ul_ids_evict` (new): full table recycles the oldest unproven RNTI and keeps proven ones; all-proven evicts the least recently seen.
- `test_nr_uci_small` (new, stubs for LDPC; the TB callback is the oracle): first pass held, second grant of the RNTI delivers; a lone chance pass is never
  delivered; confirmation is per RNTI; the <= 2-bit class is delivered at once after its confirmation; no exploration -> no default set; search bounded (<= 97 TB tests).
- `test_nr_ul_rules` (extended): SIB1 N_TA_offset rules; TDRA row table legal/distinct/contains the TS 38.214 rows; variant cycle keeps the former variants first
  and reaches every row; live trials spend 3 of 4 cycles on the core.
- `test_nr_pusch_ul_disc` (extended): timing evidence converges in 3-6 grants at a fixed window, never on noise at random windows, decays, counts a slot once, per RNTI.
- `test_nr_ul_layout_lookalike` still passes (variant functions).

## NOT verified
- U3 on real LDPC/OFDM: the new path is exercised with a stub oracle only; `nr_pusch_ucisim` (OAI UE tx with UCI on a small TB) was built but not extended
  with a small-TB case. No TP (DFT-s-OFDM) UCI case at all: the mapping is assumed to be the CP-OFDM one on the post-despreading symbol stream (the `G`
  consistency check in the search is the only guard, plus the CRC rule).
- U4 / U6 / timing: nothing replayed (not allowed here). U6 is in the RT monitor (no unit seam); needs the sa_bed dedicated-BWP rfsim bed (PROJECT_MEMORY 29.1:
  expect `UL automatic DCI length locked ... dedicated-BWP CORESET`, `dci01 accepts > 0`, `ulscan[sched>0]`).
- Pre-existing failure, also on the base build: `UlAlias.TrackedSeedOnAliasIsCorrected` in `test_nr_pusch_ul_disc`.

## Expected on-air effect `[HYPOTHESIS]`
- Small grants that carried HARQ-ACK (DDDSU) start to decode after the second confirmed grant of an RNTI; `UL_UCI recovered` lines for TBS <= 3824; `UL ids table full: evicting`.
- Dedicated-BWP UEs get a `UL automatic DCI length locked` for the pass-2 geometry and PUSCH tries.
- Cells with S/L other than S0 L14 A / S0 L13 B lock a layout (`ULLAYOUT_CRC_LOCK ... var=` with bits 4..6 > 1), slower on cells that need the core only
  (<= 4/3 longer variant cycle while no decisive pass exists).
- Weak UEs: `tm_evidence` counters rise, `UL_PUSCH_CRC ta=` converges to the tracker value; N_TA_offset from SIB1 (39936 cells) shifts the nominal advance by up to 896 samples at 122.88 Msps.

## Risks for other fixes touching the same files
- `nr_pusch_passive_decode.c`: three hunks (UCI gate ~2831; inline timing evidence ~2294; worker evidence ~1655; counters ~1547); a `phase`/`ptrs` fix editing the UL decode will conflict textually only.
- `nr_pdcch_ul_layout_lock.c`/`.h`: tag mask widened to 20 bits; anything else that decodes `hyp_generation >> 40` with a narrower mask would misread the variant (grep found none outside this file).
- `nr_pusch_ul_disc.h`: `nr_pusch_ul_disc_timing_out_t` gained `undec_a0`; `nr_pusch_uci_blind.h`: `nr_uci_search_out_t.provisional`.
- `CMakeLists.txt`: two test targets added before `test_nr_pusch_solver`.
