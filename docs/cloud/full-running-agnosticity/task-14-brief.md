### Task 14: Technique D — PDSCH mapping type B and k0 ≥ 2; PUSCH mapping type B

The catalog is type-A only with `k0 ∈ {0,1}` (`nr_pdsch_config_sweep.c:70`: k0 = 2 does not fit the per-context state). TS 38.214 allows mapping type B and k0 up to 32.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.{h,c}`, `tests/nr_pdsch_config_sweep_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (oracle probe ~633: k0 measurement), the decode's DM-RS mask for type B
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.{h,c}` (PUSCH TDRA) + its test

**Interfaces:**
- Produces: `uint8_t mapping_type;` (0 = A, 1 = B) in `nr_pdsch_cfg_hypothesis_t`; `int nr_pdsch_config_sweep_add_k0(const nr_pdsch_sweep_ticket_t *t, uint8_t k0);`.

- [ ] **Step 1: Failing tests.**
  - `LegalTdaTablesMatchTs38214`: a helper `nr_pdsch_tda_legal(mapping_type, S, L)` — count type A (normal CP: S 0..3, L 3..14, S+L ≤ 14) = **42** (matches the existing comment), type B (Rel-16: S 0..12, L 2..13, S+L ≤ 14) = **90**.
  - `CatalogIncludesTypeBAndFits`: `nr_pdsch_config_sweep_init(&st, 2)` contains at least one `mapping_type == 1` hypothesis and `n_hyp < NR_PDSCH_SWEEP_MAX_HYP` (print n_hyp).
  - `ObservedK0IsAddedToTheContext`: a live context has no k0=3 hypothesis; `nr_pdsch_config_sweep_add_k0(&t, 3)` adds them (count > 0), a second call adds 0.
  - PUSCH: `PuschLegalTdaCounts` — type A (S = 0, L 4..14) = 11, type B (S 0..13, L 1..14, S+L ≤ 14) = 105 — in the UL interp sweep's test.
- [ ] **Step 2: Implement type B.** Enumerate type-B (S,L) with its DM-RS masks (first DM-RS on the first PDSCH symbol; additional positions per TS 38.211 Table 7.4.1.1.2-3, type-B columns) using OAI's own mask generator with the mapping-type argument — find it (`fill_dmrs_mask`) and use it rather than re-deriving; merge by effective mask like type A. Raise `NR_PDSCH_SWEEP_MAX_HYP` to fit (measure; put per-context bytes × `RNTI_CTX_MAX` in the commit). The DM-RS mask oracle already pins (S,L,mask), so the larger catalog is pruned in the first observed slot.
- [ ] **Step 3: k0 oracle.** At the oracle probe (~633), when no DM-RS is found at slot+k0 for k0 ∈ {0,1}, probe slot+k for k = 2..K where K = the passive IQ ring's retained slots after the DCI slot (measure it; do not guess); on a hit, call `nr_pdsch_config_sweep_add_k0`. Rationale: enumerate only k0 values the air shows, not all 33.
- [ ] **Step 4: PUSCH type B** in the UL interp sweep the same way (it already finds k2).
- [ ] **Step 5:** Build + ctest for the touched suites; commit `Technique D: PDSCH/PUSCH mapping type B; k0 >= 2 added on observation`.

---

