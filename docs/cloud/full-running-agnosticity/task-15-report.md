# Task 15 report: AL16 lookahead lanes and AL1 union decoding

**Status: DONE_WITH_CONCERNS.** The concerns are listed below. Nothing was validated live; everything was checked offline.

- **Lane:** al1, `/home/sens/NICOLA/agn-wt/al1`, branch `sdd/agn-al1`
- **Commit:** `58b390430e` "Blind PDCCH: AL16 lanes; AL1 decoding over the union of still-consistent mapping families"

## Files changed
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.{h,c}`
  - New: `nr_pdcch_al1_union()`, `nr_pdcch_al1_family_reps()`, `nr_pdcch_al1_demap()`.
  - New constant: `NR_PDCCH_AL1_UNION_MAX = 1088`.
  - `family_count` now shares a fingerprint helper with the new code.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.{c,h}`: AL16 lanes, the AL1 union, and two small exports for the test.
- `openair1/PHY/NR_UE_TRANSPORT/nr_polar_gpu_mod.c`: comment only.
- `openair1/PHY/CODING/nrPolar_tools/cuda/nr_polar_sc_cuda.h`: `NPC_MAX_E` raised from 1024 to 2048. `nr_polar_sc_cuda_test.cc` gains AL16 parameter sets.
- Tests: `tests/nr_pdcch_al1_map_test.cc` (7 new cases) and `tests/nr_pdcch_blind_monitor_test.cc` (1 new case).
- `nr_pdcch_blind_monitor.c` was not touched. The brief lists it for "the bank", but the bank (`g_coreset_bank`) lives in `rt.c`.

## Step 2: AL16 lanes
- `LANE_BATCH_AL_MAX` is now 16, so the lane-AL parser accepts 16.
- The parser is split out as `nr_pdcch_blind_parse_lane_als()`. `lane_als()` still caches the value and still falls back to SIB1 as before.
- `LANE_RE_PER_LANE` is now written as 8 × AL16 (8 × 864 REs). That is the same 6912 REs as before, so the per-thread `s_lane_re` heap does not grow. It holds every AL16 position of the largest CORESET: 270 RB × 3 symbols = 135 CCEs, which is 8 AL16 positions.
- `LANE_BATCH_VSTRIDE` is now 1728 (= `NR_PDCCH_JOINT_MAX_E`). Without this, AL16 lanes would always drop to the CPU path.
- **GPU:** adding AL16 did not need any change to the kernel launch. The kernel is independent of E apart from the `NPC_MAX_E` bound, and AL16 uses repetition rate matching, which the kernel already handles. So `NPC_MAX_E` goes from 1024 to 2048.
  - Evidence from `nr_polar_sc_cuda_test quick` on the RTX 4060 Ti: `len=47 AL=16 E=1728` and `len=58 AL=16 E=1728` gave 950 vectors each with 0 mismatches. Overall result: `BIT-EXACTNESS: PASS`, `VEC-PATH: PASS`.
  - The baseline before the change also passed.
  - The params table is still 256 entries, which is enough for 34 lengths × 5 ALs = 170.
- The out-of-date "AL2-only" comments at about lines 1771 and 4456 are updated, and so is the `s_lane_re` sizing comment.

## Step 3: AL1 union

### Library (`nr_pdcch_al1_map`)
- **`nr_pdcch_al1_union`**
  - Deduplicates through the existing key hash.
  - Output is family-major: `cand[0]`'s REG sets come first, in CCE order.
  - Returns the total count the way `snprintf` does, so a result above `max` means the output was truncated.
- **`nr_pdcch_al1_family_reps`**: keeps the first mapping of each family, in place.
- **`nr_pdcch_al1_demap`**: demaps an AL1 candidate from its REG set alone, giving the same `e_rx` as `nr_pdcch_demapping_deinterleaving()` for any (mapping, CCE) with that REG set. It is tested against a transcription of the `dci_nr.c` demapper on every (mapping, CCE) pair of 5 shapes.

### Bank entry
- New fields: `al1_fam[NR_PDCCH_AL1_MAX_COVER]` and `n_al1_fam` (as the interface specifies), plus a cached `al1_union[NR_PDCCH_AL1_UNION_MAX][6]` and `n_al1_union`.
- These fields change after the entry is published, so they are guarded by a new `g_al1_mu`. The `cfg` part stays immutable.
- Static cost: 13 KB per entry, about 104 KB for all 8 entries.

### Verification point
- `al1_verify_report()` now returns one mapping per family.
- `coreset_bank_add()` now returns the index of the entry.
- `al1_union_store()` stores the families when there is more than one.
  - It caps them at `NR_PDCCH_AL1_MAX_COVER` and logs the cap once.
  - It first checks that the bank entry's span and duration match the verified lane.

### Scan
- This applies to the main ladder, on a bank pass whose `cfg` is a bank entry.
- The AL1 positions run over the cached union of {banked mapping} + the stored families.
- The first `num_cces` positions are the banked mapping's own CCEs. The remaining positions get a virtual CCE number (their index in the union).
- The ladder keeps a copy of those REG sets for the occasion. Just before the demap, the virtual candidates are moved behind the real ones and demapped from their REG set.
- Downstream code that reads the CCE fails safely for virtual CCEs: `nr_pdcch_candidate_rbs` bounds-checks the CCE and returns -INF, and the index-check diagnostic is bounds-guarded.
- The union is only rebuilt when the families change, never per occasion.
- The union REG sets are capped at `NR_PDCCH_AL1_UNION_MAX` and the cap is logged once. The cap never binds for a legal shape: a test runs the full catalogue of every legal shape and stays at or below it.

### Evidence handling (`al1_union_accept`)
- It is called from the admitted `dl_auto` path and from the general accept path.
- In the general accept path it only acts for **RNTI-confirmed** accepts, because narrowing cannot be undone.
- A confirmed AL1 decode narrows the families.
- An observation that no stored family explains is ignored rather than emptying the set.
- A confirmed AL2+ decode collapses the set to the banked mapping.
- Every change logs `SENSING: AL1_UNION bank=%d fam=%d sets=%d beyond_banked=%d (why)`.

### Default behaviour (ruling c)
- Without `ISAC_AL1_COVER`, no families are ever stored, so `n_al1_union` stays 0 and the ladder is unchanged. The only difference is one uncontended mutex lock per bank-pass occasion.
- Without 16 in `ISAC_LANE_ALS`, the ALs the lanes scan are unchanged.

## Tests
| Test | Result |
|---|---|
| `test_nr_pdcch_al1_map` | 100 % pass. Includes the brief's `UnionOfOneFamilyIsItsCces` (45 sets) and `UnionContainsTheTruthAfterNarrowing`, plus my additional cases: many-family union, snprintf-style truncation count, cap over every legal shape, family reps, demap equivalence. |
| `nr_polar_sc_cuda_test quick` | PASS, including AL16. |
| `nr-uesoftmodem` build | Clean, exit 0. The two warnings in `rt.c` (unused `n_verified`, zero-length format) were already there before this change. |
| `test_nr_pdcch_blind_monitor` | **Cannot link**, as expected. The target does not link `rt.c`. The undefined symbols that were already there are `nr_pdcch_blind_monitor_bank_has_geometry` (an `rt.c` symbol), `nr_tdd_config_init` and `nr_tdd_slot_has_downlink`. My two new `rt.c` exports are now in that list too. The AL16 test `LookaheadLanes.Al16IsAcceptedAndFitsTheLaneBudget` is committed. I checked the parser and budget logic by compiling the exact `rt.c` text standalone: `"16"` parses to 1 AL, `"1,2,4,8,16"` to 5, `"32,3,0"` and NULL to 0, and the budget is 6912 ≥ 864. **If the other lane fixes the link by stubbing `rt.c` symbols rather than linking `rt.c`, this test will need the same treatment.** |

## Concerns
1. **The brief's `UnionContainsTheTruthAfterNarrowing` fixture passes trivially.** For truth {2,3,101} at 270×2, every single-CCE observation (I checked all 90 CCEs) leaves exactly 1 family. The union is then 90 sets and 90 ≤ 90 × 1. I kept the test as specified and added `UnionCoversEverySurvivingFamilyWhenOneObservationIsAmbiguous`: truth 2/2/0 observed at CCE 0 leaves 45 families, and every family's sets must be in the union.
2. **The family cap binds on real shapes.** In an offline probe over every mapping and every 7th CCE, the worst number of families left after one observation was: 48×1 → 4, 48×2 → 8, 270×2 → 45, 216×2 → 36, 270×3 → 90.
   - `NR_PDCCH_AL1_MAX_COVER = 32` therefore truncates on wide 2- and 3-symbol CORESETs, and the true family can be among the dropped ones.
   - Per ruling (a), this is capped and logged once.
   - A fix would need a larger bound or storing fingerprints instead of mappings. I have not done this.
3. **AL1 budget.** A large union (up to about 1000 sets) is covered by the existing AL1 rotation within `al_cap[0]` per occasion, so a full lap takes many occasions. The adaptive split will only give AL1 more budget once AL1 accepts start arriving.
4. **Memory.** The lane-batch vector `g_lb_vec` doubles from 3.5 MB to 7 MB per scan thread that runs lanes. It is allocated whenever lanes run, even without AL16 or a GPU. This matters under `mlockall` / `RLIMIT_MEMLOCK`. `s_lane_re` was deliberately kept the same size.
5. **The opt-in `ISAC_PDCCH_DMRS_GATE` scores virtual candidates as -INF**, so while that gate is on it may drop them. The gate is off by default.
6. **Nothing is validated live.** No rfsim or OTA run was made. The union path only activates after an `ISAC_AL1_COVER=1` lane verification that leaves more than one family.

---

## Fix round 1

**Commit:** `b3a3e63c68`, a new commit (not an amend). Line numbers below refer to `nr_pdcch_blind_monitor_rt.c` at that commit.

**Build and test:**
- `test_nr_pdcch_al1_map`: 100 % pass (0.43 s).
- `nr-uesoftmodem`: builds, exit 0. The only warnings in `rt.c` are the two that were already there (unused `n_verified`, zero-length format).

### 1. Only this CORESET's own candidates count as AL1 evidence (IMPORTANT)
- **Change:** a new `al1_from_ladder(e_rx, pdcch_e_rx)` (:412) returns true only when the task's `e_rx` lies inside the main ladder's `pdcch_e_rx[NR_MAX_PDCCH_SIZE]`.
  - Both evidence hooks now require it: the admitted `dl_auto` path (:5486) and the general accept path (:5621).
  - The passive-BWP second-pass tasks and their probe tasks demap a different CORESET into `s_pdcch_e_rx2`. They are therefore excluded and can no longer narrow or collapse the banked families.
  - Main-ladder tasks that carry `bwp_entry > 0` or `bwp_probe` still count. They decode the same candidates of the same CORESET, only at a different length.
- **Evidence:** by construction. Every task-build site sets `e_rx` from its own buffer: main ladder `pdcch_e_rx`, second pass `s_pdcch_e_rx2`, lanes `s_pdcch_e_rx_lane`. Lane tasks also `continue` before reaching the accept path.

### 2. Bound on stored families (IMPORTANT, replaces ruling (a))
- **Change:** new `#define NR_PDCCH_AL1_MAX_FAM 96` in `nr_pdcch_al1_map.h:18`.
  - It is used for the bank's `al1_fam[]` (:200), the refresh list (:358), the narrowing copy in accept (:463), `al1_verify_report`'s output (:2604/:2617) and the verification-site buffer (:5274).
  - The cap that is logged once is kept, as a guard only (:387).
  - `NR_PDCCH_AL1_MAX_COVER` goes back to meaning the greedy-cover bound only.
- **Evidence:** new test `SurvivingFamiliesFitTheBankBoundForEveryLegalShape` (`nr_pdcch_al1_map_test.cc:186`).
  - It covers every legal shape (6..270 RB × D = 1..3) and every distinct AL1 REG set.
  - For each observation it counts the distinct families containing it. The argmax of each shape is cross-checked through the production `narrow()` + `family_count()`.
  - Measured output: `max AL1 families surviving one observation: 90 (first at 270x3), bound 96`. The test takes 188 ms.

### 3. Lane-batch buffer back to its old size by default (IMPORTANT)
- **Change:** the stride is now chosen per thread at run time.
  - A thread-local `g_lb_vstride` is set from `lane_batch_want_stride()` (:1942-1943), which is the larger of `lane_als()` and AL8, times 108.
  - `lane_batch_vec_ready` allocates `LANE_BATCH_MAX_VEC × want`.
  - If a SIB1 prior later adds AL16, the buffer is widened (vector buffer only) at the next occasion that has no vectors yet. Within an occasion, anything wider than the current stride keeps falling back to the CPU.
  - The width check (:2045), the slot indexing (:2049) and `decode_vec` (:2088) all use `g_lb_vstride`. The compile-time `LANE_BATCH_VSTRIDE` is removed.
- **Evidence:** without AL16 the stride is 8 × 108 = 864, which is the size before this task: 2048 × 864 × 2 B = 3.5 MB per thread. With AL16 it is 1728 (7 MB).

### 4. Minor findings
- **(6) Re-verifying an entry no longer widens its families again.** `al1_union_store` returns early if the entry already holds families (:397), so families that were narrowed or collapsed are kept.
- **(7) AL2+ decode under a mapping that AL1 evidence ruled out.** New `al1_bank_out` flag (:202). It is set when an AL1 narrowing excludes the banked mapping's own family (:471).
  - While the flag is set, an AL2+ decode does not collapse the families. It logs `LOG_W` once and keeps the union, which still contains the banked family (:448).
- **(8) Discovery replay.** `blind_discovery_replay` now returns early if any candidate's `CCE + L` exceeds the CORESET's CCE count (:1890). Virtual CCE numbers are therefore never recorded.
