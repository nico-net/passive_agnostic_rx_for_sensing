# Task 9 report: segmented channel estimation and RE extraction in the passive PDSCH decode

**Status: DONE_WITH_CONCERNS**. Commit `65b651780b` on `sdd/agn-prb` (lane `prb`, `/home/sens/NICOLA/agn-wt/prb`).

## Summary
- `freq_alloc_bitmap_t` (defined in `common/utils/bits.h`, per ruling 1) gains `uint16_t n_prb_list; uint16_t prb_list[275]; uint8_t prg;`. A `_Static_assert` in the decoder ties 275 to `NR_PRB_SET_MAX`. Every existing constructor (`set_bitmap_from_start_size`, `set_start_end_from_bitmap`, `= {0}`) zeroes the new fields, so every grant produced today is legacy.
- New pure helper `nr_prb_gather_index()` in `nr_pdsch_prb_set.{h,c}`, TDD-tested.
- `nr_pdsch_passive_decode()`: `seg_path = !(nseg == 1 && prg == 0)`, the brief's guard taken literally. Every new branch is guarded by `seg_path`, so the contiguous case runs the old code path by construction.
- Data-aided CFR tap (`nr_pdsch_data_aided.c`): data RE `j` maps to PRB `prb_list[j/12]` (absolute subcarrier) when a list is present.
- **UL (Step 5) not implemented.** The brief's premise is false in this tree (see Concerns).
- Nothing produces a PRB list yet (Tasks 10-12 will). The new decoder path runs only through the pure gather test. I built no synthetic end-to-end harness (ruling 3).

## Step 1: allocation assumptions (also in the commit message)
| # | Where | Assumption | On seg_path now |
|---|---|---|---|
| A | `nr_pdsch_passive_gpu_job` | contiguous `start_rb`/`nb_rb` for GPU FEP+chest+MMSE+LLR | returns false (CPU only) for list/PRG grants |
| B | PT-RS (`ptrs_unav(num_rbs)`, PT-RS positions in `nr_rx_pdsch`) | PT-RS REs numbered over PRBs in increasing order (38.211 7.4.1.2.2) | PT-RS refused (UNSUPPORTED, `UNSUP@seg-ptrs`); PT-RS sweep not armed |
| C | CSI-RS RM (`nr_dlsch_extract_rbs` picks the even/odd pattern by CRB parity) | each RB stays at its real CRB | refused only if a segment's `data_index` and `prb_start` differ in parity (`UNSUP@seg-csirm-parity`) |
| D | TBS, G, G_max, probe horizon | `num_rbs` = PRB count | correct once the list is normalised (`num_rbs = n`) |
| E | slot-share chest cache | estimate index 0 = `first_rb` | bypassed (`chest_hit` false), cache invalidated after a segmented estimate |
| F | `nr_pdsch_channel_estimation` | memsets the whole row, writes from index 0 = first RB of the alloc it is given; pilots from `first_rb` | called once per segment with a 1-segment alloc; the output is reassembled in data order |
| G | `ISAC_DC_FIX` | treats the allocation-relative row as FFT bins (looks like a pre-existing mismatch) | not applied |
| H | DMRSFO SFO slope, `ISAC_SFO_CORRECT` ramp | frequency-ordered subcarrier axis | skipped / eps = 0 |
| I | BRANCHFO, CHESTDIAG, branch gate, TINTERP, `nr_chest_time_domain_avg` | index 0..num_rbs*12 | still valid in the data-ordered virtual layout |
| J | `nr_dlsch_extract_rbs` (inside `nr_rx_pdsch`) | walks bitmap blocks in increasing PRB order and **resets the estimate pointer to index 0 for every block** | replaced by a virtual contiguous allocation |
| K | `nr_rx_pdsch` sizing / CSI count loop | contiguous | virtual allocation |
| L | `dl_valid_re`, `rxdataF_comp` → EQDIAG / EQDIAG2 / LLRFILL | data order | come out in data order; EQDIAG `start_re` printed as -1 (no FFT origin) |
| M | rbmap census | `first_rb..first_rb+num_rbs` | counts only PRBs set in the bitmap |
| N | data-aided tap | `j` contiguous from `first_rb`, `isac_k = base_sc + j` | maps via `prb_list` |
| O | queue file (not touched, out of scope) | slot-share union, rank/CDM probes, DM-RS-ID accumulator, oracle gate, GPU self-check read `first_rb`/`num_rbs` as contiguous | **Tasks 10-12 must guard these** |
| P | TBPARM / TBRESULT / EQDIAG2 | print `first_rb+num_rbs` | labels only |

Finding J matters beyond this task: the upstream bitmap path (RA type 0 via `rb_bitmap`) reads block 0's channel estimate for every later block. RA type 0 was therefore never safe through a plain bitmap in this receiver.

## Design
1. **Normalise:** a list grant is copied into a local `fa_list`, and `first_rb`/`last_rb`/`num_rbs`/`bitmap` are re-derived from the list, so TBS, G, `nr_ue_csi_rm_unav_res` and the stats agree with it. A PRB outside the BWP or listed twice returns UNSUPPORTED (`UNSUP@seg-list`). `prg > 0` without a list segments the contiguous range.
2. **Segment:** `nr_prb_segments(prb, n, BWPStart, prg, seg, NR_PRB_SET_MAX)`.
3. **Channel estimation, per DM-RS symbol × layer × segment:**
   - Run the estimator on the segment's own contiguous allocation (real pilots and real subcarriers).
   - Park the `n_prb*12` outputs per antenna at `data_index*12` in a thread-local heap buffer.
   - Rewrite the row as `[data-ordered estimate | zeros]`.
   - nvar is weighted by segment width, so its per-(symbol × layer) sum means the same as before.
   - **1-PRB segments:** the estimator has no minimum size (the TYPE1/TYPE2 FIR loop does the same arithmetic at 12 LS points), so no merge and no `seg_skipped` stat was needed (ruling 2).
4. **Gather:**
   - `nr_prb_gather_index(seg, nseg, 12, …)` gives, for virtual RE i, its source BWP-relative RE.
   - Each allocation symbol × antenna of the real `rxdataF` is permuted into a thread-local virtual buffer at BWP PRBs 0..n-1.
   - `nr_rx_pdsch()` gets `set_bitmap_from_start_size(0, n)` plus the virtual buffer.
   - DM-RS RE positions are the same in every PRB (12 is divisible by both 4 and 6), so moving whole PRBs keeps them.
   - The LLR stream, `dl_valid_re` and `rxdataF_comp` come out in data order with no change to `nr_rx_pdsch()`.
5. **Caller's `rxdataF` is untouched.** The data-aided submit forms Y/X on the real subcarriers through `prb_list`.

## TDD evidence
- **Red:** the test was added verbatim from the brief. `lane-make.sh prb test_nr_pdsch_prb_set` failed with `undefined reference to 'nr_prb_gather_index'`.
- **Green:** after implementing, `ctest -R test_nr_pdsch_prb_set` → `Passed`, and the binary reports `[  PASSED  ] 10 tests.`

## Commands and outputs
- `pgrep -x nr-uesoftmodem` → `none` before every build.
- First `lane-make.sh prb nr-uesoftmodem` → `EXIT=0`.
  - It printed two maybe-uninitialized warnings in the decoder. The one on `nsc_seg` was mine: I removed the use after the goto.
  - The one on `dmrs_first` is **pre-existing**. I reproduced it on stashed HEAD, where it sits at line 2474; it comes from the existing `goto gpu_llr_ready`.
- Final build: `make nr-uesoftmodem` rc=0; `strings nr-uesoftmodem | grep -c 'PDSCH segmented decode\|UNSUP@seg-'` → 4 (the new code is in the binary).
- `ctest -R 'test_nr_pdsch|test_nr_dl_adaptive|test_nr_passive'` → 6/8 passed: xoverhead, config_sweep, mac_ta, bwp, prb_set, ptrs_unav.
  - `test_nr_dl_adaptive` (binary `test_nr_pdcch_blind_monitor`) and `test_nr_passive_acq_state` show **Not Run**: their executables do not link.
  - Link errors: `undefined reference to nr_tdd_config_init / nr_tdd_slot_has_downlink` from `nr_passive_acq_state.c`. `test_nr_pdcch_blind_monitor` also misses `nr_pdcch_blind_monitor_bank_has_geometry`.
  - **Pre-existing:** I reproduced the same undefined references for `test_nr_pdcch_blind_monitor` on stashed HEAD `2895cc06cd`. I did not re-link `test_nr_passive_acq_state` on HEAD, but it fails on the same symbols, from `nr_passive_acq_state.c`, which this task does not touch. These are missing test link dependencies, not my code.

## Concerns
1. **Step 5 (UL) not done; the brief's premise is false here.** This tree's gNB PUSCH receiver never reads `rb_bitmap` or `resource_alloc`: grep of `openair1/PHY/NR_TRANSPORT/nr_ulsch*.c`, `NR_ESTIMATION/*.c` and `phy_procedures_nr_gNB.c` finds them only in a debug print. `nr_ulsch_demodulation.c` uses `rb_start`/`rb_size` only (lines 163-164, 246-250, 273, 303, 405, 582). Filling `rb_bitmap` with `resource_alloc = 0` would silently demodulate `rb_start = 0 / rb_size = 0`. Also, `nr_pdcch_blind_ul_result_t` has no PRB-list field. UL RA type 0 needs a real gNB-receiver change: a separate task.
2. **Queue-side contiguous consumers (row O) are not guarded** because the queue file is outside this task. Before Tasks 10-12 produce lists, the DM-RS-ID accumulator especially (it feeds a cell-wide decision) and the rank/CDM probes must skip or map list grants.
3. **Replay captures:** `nr_passive_replay_capture.c` stores raw `nr_pdsch_passive_job_t` and checks `job_bytes == sizeof(...)`. The job struct grew by about 556 B (the `freq_alloc` fields), so **existing replay capture files are rejected** by the new binary. Any change to the job struct would do this; ruling 1 put the fields in the common struct.
4. `bits.h` is an upstream common type also used by the gNB (`defs_gNB.h`, `nr_dlsch.c`). It is now about 600 B instead of 48; the new fields are zeroed at every construction site.
5. The segmented path is compiled but has never decoded a real grant, because no producer exists yet. What is untested end to end:
   - nvar weighting and the last-segment per-branch nvar approximation;
   - whether the sensing engine accepts non-monotonic `isac_k` from the tap (its occupancy grid looks order-independent, but I did not verify);
   - the PT-RS and CSI-parity refusals.
6. Static link: `nr_pdsch_passive_decode.o` (PHY_NR_UE) now calls `nr_prb_segments`/`nr_prb_gather_index`, which live in the `nr_pdcch_blind_monitor` lib. nr-uesoftmodem links both inside `--start-group`, but simulators that link PHY_NR_UE without that lib would fail if they pull this object. I did not build them.

## Files (lane `prb`)
- `common/utils/bits.h`: struct fields
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.{h,c}`: `nr_prb_gather_index()`
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_prb_set_test.cc`: `GatherConcatenatesSegmentsInDataOrder`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.{h,c}`: normalisation, segmentation, segmented chest, virtual gather, guards
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.c`: PRB-list RE mapping

---

## Fix round 1 (commit `15658710b9`, on top of `65b651780b`)

### 1. List grants are normalised once, at enqueue (IMPORTANT)
- **Change:**
  - New `bool nr_pdsch_passive_alloc_normalise(freq_alloc_bitmap_t *fa, int bwp_size)`:
    - declared at `nr_pdsch_passive_decode.h:154-160`, defined at `nr_pdsch_passive_decode.c:1242-1265`;
    - for a list grant it re-derives `first_rb`, `last_rb`, `num_rbs` (= the PRB count) and `bitmap` from `prb_list`;
    - a legacy (non-list) grant is left unchanged;
    - it returns false, leaving `fa` untouched, if a PRB is outside the BWP, listed twice, or the list exceeds `NR_PRB_SET_MAX`.
  - `nr_pdsch_passive_queue_enqueue()` (`nr_pdsch_passive_queue.c:1142-1153`) applies it to the queued copy. An invalid list is refused (`LOG_W`, enqueue returns false).
  - The decoder's own normalisation (`nr_pdsch_passive_decode.c:1305-1324`) now calls the same helper on its local copy, as a CHECK. It logs `LOG_W ... NOT normalised by its producer` (rate-limited) if the caller's fields differ. It does not assert, because this runs on the RT path.
- **Why:** the data-aided tap (`nr_pdsch_data_aided.c:122,131`) recomputes `nb_rb`/G from the caller's `job.freq_alloc->num_rbs`. Before this fix only my decoder-local copy was normalised. Normalising the job itself fixes the tap, the queue probes and the narrow-grant budget (`:1102`) all at once.
- **Remaining entry point:** the blind monitor's inline decode path (`nr_pdcch_blind_monitor_rt.c:6044/6074`) does not go through the queue. It produces contiguous grants only today. When Tasks 10-12 emit lists there, it must call the helper.
  - If it doesn't, the tap still fails closed: `num_sc` comes from `n_prb_list` while G comes from `num_rbs`, so the existing `mod_idx == G/(Qm*Nl)` invariant trips and nothing is submitted.

### 2. Per-branch nvar is width-weighted across segments (IMPORTANT)
- **Change:** `nr_pdsch_passive_decode.c:1825` (`nv_ant[]`), `:1837-1838` (accumulate `nr_dl_chest_nvar_ant[a] * n_prb` after each segment), `:1845-1849` (publish `nv_ant[a] / num_rbs` after the segment loop).
- **Why:** the estimator overwrites `nr_dl_chest_nvar_ant[]` on every call. Previously the per-branch substitution (`ISAC_RX_NVAR_PERBRANCH`, default on) fed the equaliser the last segment's value, which can come from a single PRB.

### 3. Queue sites that assumed a contiguous allocation (IMPORTANT, ruling R12)
All in `nr_pdsch_passive_queue.c`. The chosen option per site:

| Site (old line → new) | Option chosen | Why it is the simplest correct one |
|---|---|---|
| Slot-share union `:483/:490` → `:520-540` | PRB-list and PRG grants are excluded from the union. If no member is contiguous, `rb_lo = rb_hi = 0`, i.e. `rb_n = 0`, which disables the widening. | The decoder never reads the shared estimate for these grants. Their `first_rb + num_rbs` is a count, not a span, so including them would wrongly widen the OTHER grants' estimates. For all-contiguous slots the init and loop are unchanged. |
| Oracle gate `:633-638` → `:680-687` | Measures the grant's largest segment (`probe_span`); the `nrb >= 4` gate applies to that segment. | Only needs real grant PRBs carrying one precoder. Segments already split at PRG edges. |
| Rank probe `:847-850` and CDM probe `:858-861` → `:893-912` | Largest segment. | Same reason. Both are census statistics, so fewer PRBs only makes them slightly noisier, never wrong. |
| DM-RS identity accumulator `:898-901` → `:943-952` | Largest segment. | Feeds a cell-wide decision, so it must measure real DM-RS at real PRBs. One contiguous segment satisfies that exactly. |
| Count-only sites `:503` (width sort), `:835` (xOverhead TBS inversion), `:869` (big/small census), `:1102` (narrow-grant budget) | No change. | All use `num_rbs` as a PRB count, which is correct after (1). `:1102` runs inside `enqueue_one` on the already-normalised `g_pending` copy. |
| GPU self-check `:752-753` | No change. | Runs only when a GPU job exists, and `nr_pdsch_passive_gpu_job()` refuses list and PRG grants. |

- `probe_span()` is at `:445-478`. A contiguous grant with `prg == 0` returns `first_rb`/`num_rbs` exactly, so the contiguous path is unchanged.
- The rank and DM-RS-ID blocks gained a `pr_nrb > 0` guard. It is always true for a contiguous grant with `num_rbs >= 1`.
- Added `#include <limits.h>` for `INT_MAX`.

### 4. Gather index moved off the stack (minor)
- **Change:** `nr_pdsch_passive_decode.c:2738-2741`. `gidx` is now a thread-local heap buffer (`static __thread int *`), allocated once, instead of a 13 kB array in `nr_pdsch_passive_decode`'s frame.
- **Why:** the array was growing the decode function's stack frame on the contiguous path too. A heap pointer rather than a large `__thread` array, for the same TLS-layout reason the file already documents.

### Verification (fix round)
- `pgrep -x nr-uesoftmodem` returned `none`.
- `lane-make.sh prb nr-uesoftmodem` → `EXIT=0`. The only warning in the touched files is the pre-existing `dmrs_first` maybe-uninitialized one.
- `strings nr-uesoftmodem | grep -c 'NOT normalised by its producer\|refused an invalid PRB-list'` → 2, so the new code is in the binary.
- Test targets `test_nr_pdsch_prb_set`, `xoverhead`, `config_sweep`, `passive_mac_ta`, `passive_bwp` and `ptrs_unav` all build (rc=0).
- `ctest -R 'test_nr_pdsch|test_nr_dl_adaptive|test_nr_passive'` → 6/8 passed.
  - The same two show **Not Run** as before: `test_nr_dl_adaptive` and `test_nr_passive_acq_state`.
  - They fail at link time on the missing `nr_tdd_*` / `nr_pdcch_blind_monitor_bank_has_geometry` symbols, the same failure recorded in the first round (reproduced on HEAD for the executable behind `test_nr_dl_adaptive`).
