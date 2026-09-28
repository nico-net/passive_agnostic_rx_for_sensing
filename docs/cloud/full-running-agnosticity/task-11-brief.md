### Task 11: Apply interleaved VRB-to-PRB mapping

The DCI's VRB-to-PRB bit is parsed (`out.vrb_to_prb`) and only logged; OAI's UE has no implementation. With the bit set, the PDSCH is decoded on the wrong PRBs.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (grant → PRB list)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (per-RNTI bundle-size hypothesis, next to the PTRS sweep in `rnti_dec_t`, ~line 310)
- Test: `tests/nr_hyp_sweep_test.cc` only if the generic sweep needs a change (prefer reusing `nr_hyp_sweep` unchanged)

**Interfaces:**
- Consumes: `nr_vrb_to_prb_interleaved` (Task 8), Task 9 PRB list, the existing generic `nr_hyp_sweep` (`nr_hyp_sweep.h`).
- Produces: `vrbL` field in `rnti_dec_t` (a 2-arm `nr_hyp_sweep`: L=2, L=4); log `VRB_IL rnti=… L=… latched`.

- [ ] **Step 1:** Rules (TS 38.211 7.3.1.6): DCI 1_0 in a common search space → `bwp_start = 0`, size = initial DL BWP size, `L = 2`, PRBs relative to CORESET#0's first RB; DCI 1_0 elsewhere → the active BWP, `L = 2`; DCI 1_1 → the active BWP, `L ∈ {2,4}` from RRC (`vrb-ToPRB-Interleaver`) → per-RNTI 2-arm sweep decided by the TB CRC, exactly like the PTRS sweep (pick arm → decode → feed CRC).
- [ ] **Step 2:** Test first: in `tests/nr_pdsch_prb_set_test.cc`, `InterleavedCssUsesCoreset0Grid` — size 48, L=2, VRBs 0..47 → assert it is a permutation and bundle 1 (VRB 2,3) maps to PRB bundle `C = 12` (PRBs 24,25).
- [ ] **Step 3:** When `vrb_to_prb == 1` and the allocation is type 1, build the PRB list with `nr_vrb_to_prb_interleaved(...)` for the RIV's VRB range and hand it to Task 9's path. Non-interleaved grants are unchanged.
- [ ] **Step 4:** Build + `ctest -R 'test_nr_pdsch|test_nr_hyp_sweep'`; commit `Passive PDSCH: apply interleaved VRB-to-PRB mapping (1_0 fixed L=2, 1_1 L swept by TB CRC)`.

---

