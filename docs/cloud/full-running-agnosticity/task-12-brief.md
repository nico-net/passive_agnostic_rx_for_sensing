### Task 12: PRB-bundling (PRG) hypothesis for channel estimation

OAI's estimator interpolates across the whole allocation. If the gNB precodes per PRG (size 2 or 4), interpolation across a PRG boundary mixes two different precoders and costs CRC on commercial MIMO cells. The lab cell gives no signal on this, so it needs its own hypothesis.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c`

**Interfaces:**
- Consumes: Task 9's `prg` field and segmented chest; `nr_hyp_sweep`.
- Produces: per-RNTI 3-arm sweep `{wideband, 2, 4}` in `rnti_dec_t`; log `PRG rnti=… prg=… latched`.

- [ ] **Step 1:** Arm 0 (wideband) = today's behaviour and the default until the sweep has evidence. Each decode picks an arm, sets `prg`, and feeds the TB CRC back — identical pattern to the PTRS sweep.
- [ ] **Step 2:** Guard cost: the sweep only runs while its arms are unsettled; once latched it is one extra field read.
- [ ] **Step 3:** Build + regression ctest; commit `Passive PDSCH: PRG-aware channel estimation, bundle size swept per RNTI`.

---

