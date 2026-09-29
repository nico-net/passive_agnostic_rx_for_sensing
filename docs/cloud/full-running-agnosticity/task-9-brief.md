### Task 9: Segmented channel estimation and RE extraction in the passive PDSCH decode

Makes the decoder accept an arbitrary data-ordered PRB list. With a one-segment list it must behave exactly as today.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.{h,c}` (freq-alloc struct, channel estimation ~1656-1700, RE extraction, and every later consumer of the extracted-RE layout: `dl_valid_re`, EQDIAG, the data-aided CFR tap)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` (UL: fill the gNB-side `rb_bitmap`/`resource_alloc = 0`, which OAI's gNB PUSCH receiver already supports; no segmentation needed on UL — PUSCH has no VRB interleaving and precoding per PRG does not apply to the gNB-side receiver's estimator in the same way)

**Interfaces:**
- Consumes: `nr_prb_seg_t`, `nr_prb_segments` (Task 8).
- Produces: in the passive freq-alloc struct (the type behind `job.freq_alloc`, fields `first_rb`/`num_rbs`): `uint16_t n_prb_list; uint16_t prb_list[NR_PRB_SET_MAX]; uint8_t prg;` — `n_prb_list == 0` means the legacy contiguous `first_rb..first_rb+num_rbs-1`, `prg == 0` means wideband.

- [ ] **Step 1: Read before editing.** Read `nr_pdsch_passive_decode.c` from the freq-alloc struct through the end of `nr_pdsch_passive_decode()`. List every place that assumes the allocation starts at `first_rb` and is `num_rbs` long (chest output indexed from 0 = first RB — see the comments at ~1656 and ~2736; `nr_dlsch_extract_rbs`; `dl_valid_re`; the CFR tap's absolute-subcarrier mapping — memory `ul-ch-estimates-are-relative-indexed` records how silently wrong a relative/absolute mix-up is). Write the list into the commit message.

- [ ] **Step 2: Write the failing test.** Add to `tests/nr_pdsch_prb_set_test.cc` a test of the one pure piece this task adds — the RE-gather order:

```cpp
extern "C" int nr_prb_gather_index(const nr_prb_seg_t *seg, int nseg, int re_per_prb, int *out, int max);
TEST(PrbSet, GatherConcatenatesSegmentsInDataOrder) {
  // Two segments: PRBs 4-5 (data 0-1) then PRBs 0-1 (data 2-3); 2 REs per PRB for the test.
  const nr_prb_seg_t s[2] = {{4, 2, 0}, {0, 2, 2}};
  int idx[8];
  ASSERT_EQ(nr_prb_gather_index(s, 2, 2, idx, 8), 8);
  const int want[8] = {8, 9, 10, 11, 0, 1, 2, 3};  // RE index within the symbol, BWP-relative
  for (int i = 0; i < 8; i++) EXPECT_EQ(idx[i], want[i]);
}
```

Implement `nr_prb_gather_index()` in `nr_pdsch_prb_set.c` (declare it in the header): for each segment in order, emit `prb_start*re_per_prb .. (prb_start+n_prb)*re_per_prb - 1`.

- [ ] **Step 3: Implement segmentation in the decoder.** If `n_prb_list == 0`, build a one-element list from `first_rb/num_rbs` and take EXACTLY the current code path (no behavioural change: guard with `if (nseg == 1 && prg == 0)` → existing calls unchanged). Otherwise: `nr_prb_segments(prb_list, n_prb_list, BWPStart, prg, …)`; run `nr_pdsch_channel_estimation()` once per segment with a `chest_alloc` of that segment (check its minimum size — if it asserts on 1 PRB, merge 1-PRB segments with their data-order neighbour only when contiguous, else skip that segment's REs and count it in a new `seg_skipped` stat); extract REs per segment and concatenate in segment order so the LLR stream is in data order; size `G` from the total PRB count.

- [ ] **Step 4: Regression.** `make -j12 nr-uesoftmodem` and `ctest -R 'test_nr_pdsch|test_nr_dl_adaptive|test_nr_passive'`: all pass. The contiguous path is untouched by construction (guarded branch); state that in the commit.

- [ ] **Step 5: UL.** In `nr_pusch_passive_decode.c`, when the grant carries a PRB list, set `resource_alloc = 0` and `rb_bitmap` (bit i = BWP-relative PRB i) instead of `rb_start/rb_size`; else unchanged.

- [ ] **Step 6: Commit** — `Passive PDSCH/PUSCH: decode non-contiguous PRB sets (segmented chest + data-ordered RE gather)`.

---

