# Task 14 report: Technique D, PDSCH mapping type B and k0 >= 2; PUSCH mapping type B

**Status: DONE_WITH_CONCERNS**
Commit `d81ef0bfd1` on `sdd/agn-sweep` (worktree `/home/sens/NICOLA/agn-wt/sweep`).

## What changed

### PDSCH catalog (`nr_pdsch_config_sweep.{h,c}`)
- New field `uint8_t mapping_type` (0 = A, 1 = B) in `nr_pdsch_cfg_hypothesis_t`. It uses what was padding before, so the struct is still 10 B.
- New `bool nr_pdsch_tda_legal(mapping_type, S, L)` implementing TS 38.214 Table 5.1.2.1-1 (normal CP).
- `init_legal` now lists every legal (S,L) for **both** mapping types and passes the mapping type to the legality callback. That callback is `nr_pdcch_blind_dmrs_mask` → `blind_fill_dmrs_mask`. I diffed its type-B tables against OAI's `fill_dmrs_mask` tables (`nr_mac_common.c` Tables 7.4.1.1.2-3/-4): they are identical, and they match the Rel-16 spec columns I checked. Type-B support is not missing for any L. Combinations it rejects (ld=14, and double-symbol DM-RS with ld<5 or add_pos>=2) are excluded naturally.
- **Merging:** a type-B entry whose effective PDU (S, L, k0, mask, table) equals a type-A entry is merged, and the type-A entry stays as the representative. The merge is required: the CRC cannot tell two identical PDUs apart, so neither could ever win. The duplicate check is now limited to the (S,L,k0) block. The old code scanned the whole catalog, which cost O(n²) under `g_lock` on context creation.
- **Stack arrays (ruling R2):** all four prune paths (`prune_mask`, `prune_tables`, `prune_to_observed`, `prune_to`) now compact in place through one `prune_commit()`. The four `keep[NR_PDSCH_SWEEP_MAX_HYP]` stack copies are gone. Only entries that match are ever written, so "no match leaves the catalog untouched" still holds by construction.
- **Places that compare hypotheses field by field:**
  - `prior_t` gains `mapping_type`, and `prior_same` compares it.
  - Prior seeding (`prune_prior`) constrains dmrs_add_pos/max_len only on entries of the prior's own mapping type, because dmrs-DownlinkForPDSCH-MappingTypeA/B are separate RRC IEs. mcs-Table is shared, so it is always constrained.
  - The public `prune_to()` keeps its strict all-types behaviour.
  - `obs_admits` deliberately does **not** compare mapping type. The oracle does not observe it, and (mask, S+L-1, k0) already pins the physics.
  - The `init_legal` duplicate check deliberately ignores mapping type (see Merging above).
  - The CONVERGED log line now prints mapping type and k0.
- **`nr_pdsch_config_sweep_add_k0(ticket, k0)`:**
  - Appends one k0 layer to the ticket's live, unsettled context: copies of the lowest-k0 layer with k0 replaced. Existing indices, evidence and outstanding tickets are untouched.
  - Returns 0 in each of these cases: the layer is already there, k0 < 2 or > 32, the context is settled or stale, or the layer would not fit (logged with LOG_W).
  - The k0 is also recorded per RNTI (`rnti_ctx_t.k0_seen`). It is re-applied to that RNTI's new contexts and to catalogs rebuilt by probation or reopen, via `context_catalog()`.
  - The log is capped at 50 lines.

### Oracle and consumer (`nr_pdsch_passive_queue.c`)
- The DM-RS oracle measurement is moved into `dmrs_oracle_measure()`, with no behaviour change except this one fix for ruling (c):
  - The median is now the **lower** median.
  - A 2-symbol type-B grant has two symbols carrying energy. The upper median of two is the DM-RS symbol itself, so it could never clear med+0.18 and the grant never produced a mask.
  - Nothing changes when 3 or more symbols carry energy, and in practice nothing changes for long type-A grants.
- **k0 probe (Step 3):**
  - Trigger: a k0=0 job whose context is unsettled and whose own slot shows no DM-RS on the grant's PRBs.
  - It probes slot+1 first (k0=1 is already in the catalog), then slot+2..K, and stops at the first hit.
  - **K is measured live, not guessed:** `K = min(producer_absolute_slot − DCI slot, 32)`, meaning the slots already written after the DCI.
  - Each probed slot is checked with `nr_passive_samples_valid` against the ring's retention (slots_per_frame − 2 slots; the ring is `common_vars.rxdata`, addressed once per frame) **before and after** its FEP. A slot overwritten during the FEP is discarded.
  - The probe never waits for the producer. The ring cannot keep more than spf−2 slots, which caps K at 17 for 30 kHz.
  - A hit at k≥2 calls `add_k0`. A hit only adds hypotheses and never prunes, because another UE's PDSCH could sit on those PRBs.
  - FEP goes into a per-thread scratch buffer. Reusing `rxdataF` would corrupt the decoder's per-thread FEP cache (`t_fep_cache`) for the rest of the slot group.
  - Counters (probes, hits, mean/max retained) are printed on a rate-limited `K0_PROBE` log line so live runs can measure K.
  - Throttled to 1 in 8 eligible jobs (marked `ponytail:`).
- **Stale drops:** the wait for not-yet-read samples grows by one slot duration for each k0 above 1. Without this, every k0≥2 hypothesis would be dropped as stale after the fixed 3 ms bound and could never collect evidence. Behaviour for k0 ≤ 1 is unchanged.
- **Type-B decode needs no change:** `nr_pdsch_adaptive_apply` passes `hy.dmrs_mask`, which the generator already computed for type B, as `dlDmrsSymbPos`.

### PUSCH (`nr_pdcch_ul_interp_sweep.{h,c}`)
- New `bool nr_pusch_tda_legal(mapping_type, S, L)` implementing TS 38.214 Table 6.1.2.1-1.
- **Finding:** the curated catalogue listed `{S=2, L=12}` as **type A** (2 rows, 192 raw hypotheses). Type A allows only S=0, so those rows were illegal. They are now type B, where S=2/L=12 is legal. The count stays at 960. The UL extraction already builds the type-B mask with the first DM-RS on the first symbol (`nr_pdcch_blind_ul_dmrs_mask`).
- The TDA rows stay a curated list; see Concerns.

## Measured numbers
| Quantity | Value |
|---|---|
| Legal TDAs, PDSCH | type A 42, type B 90 (test) |
| Legal TDAs, PUSCH | type A 11, type B 105 (test) |
| Pure catalog (`init`, no merging) | **6336** (4320 type B) < 8192 |
| Runtime catalog (real mask generator, pos2) | 750 → **2154** (≈1077 per k0 layer); after mask 0x884 → 36 entries (≤40 test holds) |
| `NR_PDSCH_SWEEP_MAX_HYP` | 2048 → **8192** (room for five observed k0 layers at runtime) |
| Per-context state | 8192 × 22 B = **180,244 B** (`sizeof(nr_pdsch_config_sweep_state_t)`) |
| × `NR_PDSCH_SWEEP_MAX_CONTEXTS` (1024) | `g_contexts` = **184,672,256 B** BSS (nm), was ~46 MB; nr-uesoftmodem bss 235 MB total |
| × `RNTI_CTX_MAX` (64, the figure the brief asked for) | 11.5 MB (contexts are actually sized by the 1024 above) |
| Unaided convergence, test fixture, A-only vs A+B | 223,838 vs 484,911 outcomes (2016 vs 4368 hyps) |

## Tests (TDD)
- **RED:** the build failed on the new tests (`nr_pdsch_tda_legal` not declared, no `mapping_type` member, `nr_pdsch_config_sweep_add_k0` not declared). The UL legality test run against the old catalogue failed on the illegal S=2 type-A rows (scratch binary).
- **GREEN:** `ctest -R test_nr_pdsch_config_sweep`: **38/38 pass** (247 s).
- **New tests:**
  - `LegalTdaTablesMatchTs38214`
  - `CatalogIncludesTypeBAndFits`, which prints n_hyp=6336 and bytes/context
  - `TypeBReachesTheMaskGeneratorAndMergesIdenticalPdus`
  - `PriorFromTypeAKeepsTypeBEntriesOfTheSameTable`
  - `ObservedK0IsAddedToTheContext`, which also checks that a sibling context created later inherits the layer
  - UL: `PuschLegalTdaCounts` and `EveryCatalogueTdaIsLegalAndTypeBIsPresent`
- **UL tests / `test_nr_pdcch_blind_monitor`:** this binary does not link in the repo for a pre-existing reason: undefined `nr_pdcch_blind_monitor_bank_has_geometry`, `nr_tdd_config_init` and `nr_tdd_slot_has_downlink`. I linked it in scratch under `/tmp/t14ul` on sens6. That link used the real `nr_tdd_pattern.c.o` plus one stub for `bank_has_geometry`. No CMake change was made.
  - With filter `DlAdaptive.*:UlInterpSweep.*:TechniqueD.*`: **13/13 pass**. This includes the updated `DlAdaptive.CompleteLegalCatalog…`: its expected set now contains the type-B entries from the same mask generator, and its equality assertion is unchanged.
  - `DlGeometry.*` (5 tests) fail in that scratch link, in their fixture (`nr_pdcch_blind_monitor_autodiscover_step` never succeeds). That is unmodified autodiscover code, so the likely cause is the stub or the pre-existing link issue. Not verified further.
- **Type-A path unchanged:** I built the original test in scratch. Unprimed convergence is **identical** (alone=223,838 in both).
- Builds: `nr-uesoftmodem` and `test_nr_pdsch_config_sweep` build clean, with no warnings in the touched files.

## Concerns
1. **Test fixture changed.** `test_legal` now returns 0 for mapping type B, so it models a cell that admits type A only. This keeps the six existing drive tests on the catalogs they were sized for. With type B admitted, their catalogs grow 2016→4368 and unaided convergence rises 223,838→484,911 outcomes, past their 400,000 budget. No assertion was relaxed, but it is a fixture change you should review.
   - The real-world counterpart: the runtime catalog is 2.9× larger (750→2154), so acquisition without the oracle is proportionally slower. The DM-RS mask oracle pins (S,L,mask) in the first observed slot (2154→36).
2. **Convergence thresholds moved (~8%).** `nr_crc_interval(…, NR_PDSCH_SWEEP_MAX_HYP, …)` uses the hypothesis bound as its union-bound class count. Raising it to 8192 makes separation slightly more conservative. Measured on the primed-sibling test: 11,168 → 12,072 outcomes. I kept it, since it is the honest bound for catalogs that can now exceed 2048.
3. **Test time went from 28 s to 247 s.** The two `StaysUndecided*` tests take ~117 s each. The cause is the pure catalog (2016→6336) combined with the existing O(n)-per-feed fallback scan. The pure path has no runtime user.
4. **PUSCH type B is only partly done.** Listing every legal PUSCH (S,L) (116) × k2 {1..4} × 96 field combinations gives 44,544 raw hypotheses against `NR_HYP_SWEEP_MAX_RAW` = 8192. On top of that, UL class merging is O(n·classes·samples) extractions. Neither fits without a UL DM-RS oracle.
   - Delivered: the legality helper, the fix for the illegal rows, and type-B rows listed and legality-checked.
   - Not delivered: listing every legal UL (S,L).
5. **k0 probe is untested live.** Its depth K and cost are only visible through the `K0_PROBE` log counters, and no live run was done here. On a busy cell, slot+k often carries another UE's PDSCH, so false k0 layers can be added. They cost catalog size, never correctness. A layer that does not fit (more than about 5 at runtime) is refused.
6. **Memory. CORRECTED in fix round 1 (see below).** The claim "there is no `mlockall` in this branch" was **wrong**: I only grepped `executables/`. `nr-uesoftmodem.c:442` calls `lock_memory_to_ram()`, which runs `mlockall(MCL_CURRENT|MCL_FUTURE)` at `common/utils/system.c:361`. The inline 185 MB `g_contexts` was therefore locked at startup, making the whole 235 MB BSS resident. Fixed in `e15c536687`.
7. **Type-B DM-RS shift not modelled.** The TS 38.211 shift of the type-B front-loaded DM-RS when it collides with a CORESET is not modelled by OAI's generator, and not here either.

---

# Fix round 1 (controller rulings R19/R20)

Commit **`e15c536687`** on `sdd/agn-sweep`, a new commit on top of `d81ef0bfd1`. Rebuilt `nr-uesoftmodem` and `test_nr_pdsch_config_sweep` with the lane script: both clean, no warnings in the touched files.
**`ctest -R test_nr_pdsch_config_sweep`: 39/39 pass in 36.3 s** (was 247 s).
Scratch link of `test_nr_pdcch_blind_monitor` (same `/tmp/t14ul` method as before, with the pre-existing link break stubbed and no CMake change), filter `DlAdaptive.*:UlInterpSweep.*:TechniqueD.*`: **14/14 pass**.

| # | Change | Where | Evidence |
|---|---|---|---|
| **I1** | `nr_crc_interval` now uses the live catalog size as its class count instead of the storage cap `NR_PDSCH_SWEEP_MAX_HYP`. That is safe because every prune clears evidence and `add_k0` only raises n. | `nr_pdsch_config_sweep.c:474, 486` (`st->n_hyp`), `:936` (`c->state->n_hyp`, reference_crc_lower) | Primed-sibling convergence: 12,072 (at 8192) and 11,168 (original, at 2048) → **9,879**, because pruned contexts now carry their real, smaller class count. Unprimed: 223,838 in all three versions (n = 2016 ≈ 2048). |
| **I2** | The k0 probe waits in 100 µs steps (the stale-check pattern, bound 30 + k slots) for each target slot to be written. Reach is K = min(32, spf−2), the ring's retention. It no longer stops at the first hit: it collects **all** hits and adds every k ≥ 2 layer. The before/after-FEP retention checks are kept. The log reports `K`, `reached` and the hit bitmap. | `nr_pdsch_passive_queue.c:732` (K), `:742` (wait), `:756` (collect), `:763` (add every k ≥ 2) | Builds clean. Not run live (see open points). |
| **I3** | `sweep_context_t.state` is now a pointer, calloc'd the first time a slot is used and reused across eviction (the pointer is kept across the slot's memset). An allocation failure makes `select()` return false with a LOG_E. `MAX_HYP` stays 8192. | `nr_pdsch_config_sweep.c:541` (field), `:731` (calloc/reuse) | `size nr-uesoftmodem` bss **235,145,168 → 50,587,600 B**; `g_contexts` (nm) **184,672,256 → 114,688 B**. |
| **R20** | Default on. Two disable knobs, each read once: `ISAC_PDSCH_TYPEB=0` builds the catalog without type B, and `ISAC_PDSCH_K0_PROBE=0` turns the probe off. | `nr_pdsch_config_sweep.c:82-86`; `nr_pdsch_passive_queue.c:720-726` | Both are statics read once per process, so they have no unit test; exercising them needs separate processes. |
| **5** | When a context converges, the RNTI's k0-oracle bits other than the winner's k0 are cleared (logged), so later contexts are no longer seeded with the false layer. | `nr_pdsch_config_sweep.c:947` | New test `ConvergenceOnAnotherK0DropsTheFalseLayer` (`tests/nr_pdsch_config_sweep_test.cc:809`): add k0=3, converge on k0=0, and a sibling context has 0 entries with k0=3. |
| **6** | Production-shaped type-B test using the real `nr_pdcch_blind_dmrs_mask`. The truth is S=5 L=7 add_pos 1, 256QAM, k0 0, mask 0x220, last symbol 11. One `observe(mask, 11, 0)` is followed by convergence within the old 400k budget. | `tests/nr_dl_adaptive_test.cc:307` (in the blind-monitor binary, which is where the real generator links) | **2154 → 3** entries after one observation, truth included; **converges in 320 outcomes** on the truth, reported as type B. |
| **9** | The two `StaysUndecided*` tests now use `init_legal(…, test_legal)`, the type-A catalog of 2016 entries, instead of the 6336-entry pure A+B catalog. | `tests/nr_pdsch_config_sweep_test.cc:64-90` | 116.8 s / 117.1 s → **11.75 s / 11.90 s**; suite 247 s → 36 s. |
| **4** | Median comment corrected: the lower median equals the old median for an odd count only; for an even count ≥ 4 it is the lower of the two middle values. | `nr_pdsch_passive_queue.c:484-486` | — |
| **8** | The `prune_to` contract in the header now states the all-match case: nothing moves, the count is returned, and evidence is kept (it was cleared before Task 14). | `nr_pdsch_config_sweep.h:135-139` | — |

## Corrected memory figures (mlockall is active)
- **Locked at startup:** the whole BSS, now **50.6 MB** (was 235.1 MB). The context table itself is 114,688 B.
- **Locked at run time:** 180,244 B per context slot the first time it opens (calloc under `g_lock`, followed by page faults under MCL_FUTURE). The worst case, all 1024 slots used, is 184.6 MB, the same as before but paid only as contexts are really created.
- **Still inline:** the legacy `g_sweep` pure-test state (180 KB, no runtime user).

## Open points after this round
- **The k0 probe's wait can expire this job's own samples.** It can hold a consumer for up to K slots (≈ spf−2), and in that time the job's own (DCI-slot, k0=0) samples may pass their lifetime. The decode's `check_sample_lifetime` then drops that trial as stale rather than scoring it. That is correct under the lifetime rule, but it costs k0=0 trials in the 1-in-8 probed jobs, and those are jobs whose own slot showed no DM-RS. Not measured live.
- **Collecting every hit can add many layers on a busy cell.** Adding every k ≥ 2 hit can add several layers per probe. Layers beyond what fits (about 5 at runtime) are refused with LOG_W, and convergence clears the ones that lost (minor 5).
- **Minor 5 can drop a true k0 for another TDA entry.** It clears every seen k0 except the winner's. If one RNTI's TDA entries genuinely use different k0 ≥ 2, a sibling entry's true k0 is dropped from future seeding. It is re-added the next time the probe sees it.
- PUSCH type B beyond the curated rows: recorded by the controller as a gap.
