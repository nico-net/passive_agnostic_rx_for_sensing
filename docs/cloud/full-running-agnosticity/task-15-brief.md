### Task 15: AL16 lookahead lanes and AL1 union decoding

Two AL gaps remain after Tasks 4–5: lanes cannot scan AL16 (per-lane RE budget sized for AL8, `nr_pdcch_blind_monitor_rt.c:1030`; the lane vector comment at ~1770 still says AL2-only), and an AL1 verification that leaves more than one AL1 family (Task 5's `AL1_VERIFY … distinct_al1_families>1`) decodes only the banked family's AL1 candidates.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`, `nr_pdcch_blind_monitor.c` (bank), `nr_pdcch_al1_map.{h,c}`, `tests/nr_pdcch_al1_map_test.cc`
- GPU path: whichever file holds the lane ladder/device buffers (memory `joint-solver-gpu-vs-cpu-benchmark`, "fifth ladder slot") — find with `grep -n "ladder" *.c *.cu`.

**Interfaces:**
- Produces: `int nr_pdcch_al1_union(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n, uint16_t (*regsets)[6], int max);` (deduplicated union of the AL1 REG sets of `cand`); bank entry gains `nr_pdcch_al1_map_t al1_fam[NR_PDCCH_AL1_MAX_COVER]; uint8_t n_al1_fam;`.

- [ ] **Step 1: Failing tests** (`nr_pdcch_al1_map_test.cc`): `UnionOfOneFamilyIsItsCces` (non-interleaved 270×1 → 45 sets); `UnionContainsTheTruthAfterNarrowing` (truth {2,3,101} at 270×2, narrow by one observation, union contains all 90 of truth's sets and is ≤ 90 × surviving families). And an AL16 lane test in `tests/nr_pdcch_blind_monitor_test.cc`: `ISAC_LANE_ALS=16` is accepted and the per-lane RE budget covers 16 CCEs × 6 REG × 9 RE (864).
- [ ] **Step 2: AL16.** Size the per-lane buffers for AL16 (E = 1728 = `NR_PDCCH_JOINT_MAX_E`, already supported by the joint solver), accept 16 in the lane-AL parser, extend the GPU ladder by the AL16 slot, update the stale comments.
- [ ] **Step 3: AL1 union.** At Task 5's verification point, store the surviving families in the bank entry; when scanning AL1 on a banked CORESET with `n_al1_fam > 1`, generate AL1 candidates from `nr_pdcch_al1_union(...)`; each later AL1 decode narrows the stored families (`nr_pdcch_al1_narrow`), each AL2+ decode under the banked mapping collapses them to 1. Log `AL1_UNION fam=… sets=…` on every change.
- [ ] **Step 4:** Build + ctest; commit `Blind PDCCH: AL16 lanes; AL1 decoding over the union of still-consistent mapping families`.

---

