### Task 10: RA type 0 and dynamicSwitch in the DCI 1_1 / 0_1 layout search

The layout search assumes the frequency-domain field is a RIV (`nr_dci11_layout_offsets()`, `nr_pdcch_dci11_layout_sweep.c:34-43`; the enumerator takes `riv_bits` as a fixed width). A cell configured with `resourceAllocation = resourceAllocationType0` or `dynamicSwitch` has a different field width, so every later field is read at the wrong offset — the same failure class as the 3-bit TDA/BWP gap of §12 in CLAUDE.md.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.{h,c}`, `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci01_layout_sweep.{h,c}`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (where the resolved layout's RIV becomes `start_rb/num_rb`, ~5600-5700)
- Test: `tests/nr_pdcch_dci11_layout_sweep_test.cc`, `tests/nr_pdcch_dci01_layout_sweep_test.cc`

**Interfaces:**
- Consumes: `nr_rbg_size`, `nr_rbg_count`, `nr_ra_type0_prbs`, `nr_fdra_dynamic_split` (Task 8); the Task 9 PRB list.
- Produces: `uint8_t fdra_mode;` in `nr_dci11_layout_t` and the 0_1 layout type — `0` type 1 (today), `1` type 0 config1, `2` type 0 config2, `3` dynamicSwitch config1, `4` dynamicSwitch config2. FDRA width per mode: type 1 = `riv_bits`; type 0 = `N_RBG`; dynamic = `1 + max(N_RBG, riv_bits)`.

- [ ] **Step 1: Failing tests.** In each layout test file, add one test per mode: build a payload with the known field order and that mode's FDRA width, run the resolver the same way the existing tests do, and assert (a) the resolved layout's `fdra_mode` equals the constructed one, (b) the MCS/RV/antenna-port offsets match the constructed payload, (c) the decoded PRB list equals `nr_ra_type0_prbs(...)` of the constructed bitmap (type 0) or the RIV range (type 1). Also add `LayoutCountFitsTheCap`: enumerate all modes at the worst case the existing comment names (length 49, 273 PRB) and assert the count `< NR_DCI11_LAYOUT_MAX`; print it.

- [ ] **Step 2: Implement.** Enumerate the existing layouts once per mode with that mode's FDRA width (the length constraint in the enumerator prunes the impossible ones). Validity: type 0 requires a non-zero bitmap; dynamic splits with `nr_fdra_dynamic_split` and applies the matching test (RIV range or non-zero bitmap). If `LayoutCountFitsTheCap` fails, raise `NR_DCI11_LAYOUT_MAX` to the next power of two and note the memory cost (`hist` is `MAX × 116 × 4 B`) in the commit message.

- [ ] **Step 3: Wire.** Where rt.c turns the resolved allocation into `dlsch_pdu.start_rb/number_rbs`, produce a Task 9 PRB list for type 0 (and dynamic→type 0); keep `resource_alloc = 1` and the contiguous fields for type 1. Same for 0_1 → PUSCH `rb_bitmap`.

- [ ] **Step 4: Run** `ctest -R 'test_nr_pdcch_dci11_layout_sweep|test_nr_pdcch_dci01_layout_sweep|test_nr_pdcch_blind_monitor'` and build `nr-uesoftmodem`: all pass.

- [ ] **Step 5: Commit** — `DCI 1_1/0_1 layouts: search RA type 0 and dynamicSwitch FDRA widths`.

---

