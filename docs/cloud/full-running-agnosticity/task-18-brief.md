### Task 18: DM-RS oracle reads the wrong PRBs when the BWP does not start at CRB 0

Found by two independent reviews (Task 9 review, Task 14 merge review). Pre-existing, not introduced by this plan.

**Defect.** `dmrs_oracle_measure()` in `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (~lines 451-496, called from the oracle gate ~740 and from the k0 probe ~800) indexes `coh[p]` and subcarrier `k` directly from the grant's BWP-relative `rb0`. But `nr_dmrs_prb_coherence()` fills `coh[]` indexed by ABSOLUTE carrier CRB (its own doc comment, `nr_dmrs_id_estimate.c:117-118`). The sibling probes in the same file do it right: the rank/CDM/DM-RS-ID probes compute `start_sc = fp->first_carrier_offset + (pdu->BWPStart + pr_rb0) * 12` (~1002, ~1056). So whenever `BWPStart != 0` (a dedicated BWP not starting at CRB 0 — common on commercial cells) the DM-RS mask oracle and the k0 probe measure the wrong PRBs and Technique D prunes on garbage.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c`
- Test: prefer a small pure helper (e.g. `static inline int dmrs_oracle_crb(int bwp_start, int rb)` or move the index computation into a function in an existing testable module) with a gtest; if the measurement cannot be unit-tested without the whole PHY, add the helper test and explain.

**Steps:**
- [ ] Read `dmrs_oracle_measure()`, `nr_dmrs_prb_coherence()` (confirm its indexing frame from the code, not just the comment), and every caller of `dmrs_oracle_measure()` (oracle gate, k0 probe). Also check the separate oracle-gate `coh[p]` use the Task 9 reviewer flagged (~queue.c:689-703 at that time) — same class; fix every site that mixes BWP-relative and carrier-absolute PRB indices.
- [ ] Write a failing test first for the index mapping (BWPStart = 0 → unchanged; BWPStart = 20, rb0 = 5 → CRB 25; bounds at the carrier edge).
- [ ] Fix by adding the BWP start consistently (take it from the job's PDU `BWPStart`, the same source the sibling probes use). BWPStart = 0 must be bit-identical to today (the lab cell).
- [ ] Build `nr-uesoftmodem` + the test with the lane script; run the related ctests (`test_nr_pdsch_config_sweep`, `test_nr_pdsch_prb_set`).
- [ ] Commit: "Passive PDSCH: DM-RS oracle indexes carrier CRBs (add BWPStart)" + trailer.
