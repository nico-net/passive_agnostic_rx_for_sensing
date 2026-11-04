# ulNidBg g_mu hold fix (twomodes/collapse, base 121fdf53dc)
Labels: [CODE] reads/edits in this worktree; [MEASURED bench] microbenchmark on DGX GB10 CPU, rfsim-free unit test, idle-ish host (load ~1); not OTA.

## Change
- nr_pdcch_ul_layout_lock.c: nr_ul_layout_hyp_list now = (1) g_mu: copy into hyp_snap_t (~1.9 KiB, sizeof printed below), unlock; (2) enumerate on the copy, no lock; (3) g_mu: if g_epoch/g_gen/g_have_cell/len-slot unchanged apply tp_ill/tp_ok deltas and g_tp_spec_refuted, else discard (return 0, *total=0, *next=first so the caller's loop stops; counted in g_hyp_discard, nr_ul_layout_hyp_list_discarded()).
- g_epoch (new, per cell) is bumped in reset_locked(). NOTE [CODE]: g_gen is never incremented anywhere in the file (constant 1), so the gen check is vacuous today; the epoch is the effective guard. Only a lock reset invalidates; other state changes (votes, fit, lock) during the walk are tolerated, like any state change between hyp_list's return and the later verdict.
- trial_opts_build(): pure helper extracted from trial_opts_ap_locked (locked path unchanged in behaviour).
- nr_ul_layout_hyp_list_ref(): verbatim pre-fix function, test-only; nr_ul_layout_hyp_list_gap_hook: test-only hook between sections (NULL in receiver).
- nr_ul_nid_bg.c worker(): pthread_setname_np "ulNidBg". Scheduling untouched.
- tools/mpci/mpci_statics.tsv: 3 rows (g_epoch, g_hyp_discard PER_CELL; gap_hook PROCESS).
- Tests (test_nr_pdcch_blind_monitor): HypListUnlockedEnumerationEqualsTheAllUnderLockReference (ref vs new, memcmp of every grant + total/next, nhyp/ap_n in {1/1,8/1,8/3,64/3,0/1}, 40 DCIs, cap 1 and 3, tp counters evolving; asserts grants non-vacuous), HypListResultIsDiscardedWhenTheStateIsResetDuringTheEnumeration (hook resets lock; n=0, total=0, next=first, discard counter +1), DISABLED_HypListLockHoldBench.

## Audit of every read of the enumeration (all under the old g_mu), nr_pdcch_ul_layout_lock.c at 121fdf53dc
- g_nlen, g_len[l].len (li search): 1579-1581 -> only len/li copied
- g_have_cell, ls->fitted, g_nhyp: 1583 (checked in section 1)
- pre_for_hyp(ls,h) -> g_hyp[h].n (riv width), ls->npre, ls->pre[k].riv: 1406-1411, calls 1588, 1624 -> snapshot hyp[].n, npre, pre[]
- sort/lead: g_cell.sib1_ul_size/start, g_hyp[].n/.start/.votes, g_locked_hyp: 1590-1600
- g_cell.sib1_ul_cfg: 1602; nhyp cap
- ls->ntail, ls->ev_dec, ls->tail[t].tp: 1604-1607, 1615, 1622, 1626
- g_gen: 1610 (gen tag; snapshot copy, same mask)
- cur_addpos_locked() -> g_addpos[0..2]: 1398-1402, 1620 (called per iteration, value constant under lock -> read once)
- trial_opts_ap_locked: g_nhyp bound (1429), ls->npre, ls->ntail, ls->pre[k], ls->tail[t], g_hyp[h].start/.n, ls->k2row, g_cell.pci/.mu/.typeA_pos, ls->len (1427-1434) -> all in snapshot
- apply_variant, nr_pdcch_blind_dci01_size, nr_pdcch_blind_extract_01, nr_ul_tp_nrb_legal, nr_ul_tp_nrb_legal/tail_refuted, nr_ul_variant_*: pure (no state)
- g_cell.pci for data_scrambling_id: 1640
- Writes: ls->tp_ill++ / ls->tp_ok++ (1629-1631), g_tp_spec_refuted++ (1629) -> local copy during walk, deltas applied in section 3. No other writes.
(Line numbers are of the pre-fix file; the function is now ~1675-1800.)

## Copy size
sizeof(hyp_snap_t) estimated by hand (not printed): 192*8 (hyp) + 8*sizeof(prefix) + 4*sizeof(tail) + cell + scalars, about 1.9 KiB on stack; copy is one loop over g_nhyp<=192 plus 3 small memcpy.

## Hold time before/after [MEASURED bench] (DISABLED_HypListLockHoldBench; 720 hyp_list calls/job, nhyp=64, ap_n=3, cap=1; a 2nd thread takes g_mu in a tight loop via note_pdcch_slot and records its wait)
| case | old: waiter mean / max | new: waiter mean / max |
|---|---|---|
| walk ends at once (valid payload), 144000 calls | 3.0-4.5 us / 70-690 us (scheduler noise) | 0.27-0.32 us / 110-350 us (noise) |
| long walk (all-ones payload, 555 us per call, 1440 calls) | mean 800 ms: the waiter got g_mu ONCE in the whole run (blocked behind back-to-back holds, 555 us each) | 0.09 us / 108 us, 6.26 M takes |
Per-call hold seen by consumers: old = whole call (0.6 us ... 555 us here; 12-48 ms OTA because of preemption and slower walks), new = snapshot + apply sections (sub-microsecond to a few us); max outliers in the new column are host scheduler noise, same as in the old column. Total work per call is unchanged (555 us vs 536 us). [INFERRED] in OTA: p50 12 ms hold -> ~1-3 us, independent of ulNidBg being starved, because the lock is no longer held while the walk can be descheduled (a preemption inside the two short sections is still possible).

## Verification
Build (ninja -k 0 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests): rc 0, 24 .so, 0 FAILED. ctest -j4: 391 tests, 18 failed; all in the baseline list except test_nr_spin_handoff (timing under load) which passes alone; UL layout lock tests (UlLayoutLock.* 15/15, test_nr_pdcch_blind_monitor, test_mpci_a6_ul_cells, test_nr_lockhold, static_audit_check, test_static_audit_scanner) pass. static_audit.py --strict exit 0 (after adding the 3 rows). Frozen check exit 0.

## Fix round 1 (review ulnid-lock-fix-review.md)
- I1: commit_fit_locked() now does g_epoch++ (not g_gen); comment at g_epoch updated. A re-fit during the unlocked walk discards the result.
- M1: nr_ul_layout_hyp_list = hyp_list_once() with one retry on discard (bounded to 1). A second discard returns 0, *total=0, *next=first. Each discarded attempt is counted. A reset leaves the state unfitted, so the retry returns 0 in section 1 without a second discard.
- M4: new test accessor nr_ul_layout_test_tp_counters(); the bit-for-bit test now also compares tp_ill, tp_ok and g_tp_spec_refuted after each full walk (equal in all 5 configs). LIMIT: with this generator the sum is 0 (no transform-precoding tail is fitted), so the counters are compared but the TP delta path itself is NOT exercised by any test; stated in the test.
- New test HypListRetriesOnceAfterAResetAndAfterAnotherLengthIsFitted: hook runs a fit (commit_fit_locked of len 42) during the gap -> discard +1 and the retry returns a grant (n=1). Reset-during-gap -> discard +1, n=0. The existing reset test now expects: n=0, total=0, next=first, discard +1.
- Verification: UlLayoutLock.* 16/16; build rc 0, 0 FAILED, 24 .so; ctest -j4 391 tests, 18 failed, all in the baseline list (test_nr_spin_handoff passed this time); test_nr_lockhold, test_mpci_a6_ul_cells, static_audit_check pass; static audit --strict exit 0; frozen check exit 0.
