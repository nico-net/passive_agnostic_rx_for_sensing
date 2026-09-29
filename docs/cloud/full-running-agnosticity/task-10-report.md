# Task 10 report: RA type 0 and dynamicSwitch in the DCI 1_1 / 0_1 layout search

**Status:** DONE_WITH_CONCERNS (the concerns are listed below: the cap is exceeded on narrow BWPs, and UL type-0 decode is a follow-up)
**Commit:** `e943533a5c` on `sdd/agn-prb` (lane prb). Not pushed.

> **Superseded in parts by fix round 1 (`0479e306d3`, section at the end).** Three claims below are no longer true:
> - The cap is 8192, not 2048.
> - DL modes are staged, not searched all at once.
> - The UL refusal no longer relies on stage-1 unanimity. The premise that stage 1 prunes type-1 layouts on a type-0 truth was **false** on the uplink.

## What changed

### Layout model (`nr_pdcch_dci11_layout_sweep.{h,c}`, `nr_pdcch_dci01_layout_sweep.{h,c}`)
- `nr_dci11_layout_t` and `nr_dci01_layout_t` each gain `uint8_t fdra_mode` and `uint8_t n_rbg`:
  - `fdra_mode`: 0 = type 1, 1/2 = type 0 config1/2, 3/4 = dynamicSwitch config1/2 (`NR_FDRA_*`).
  - `n_rbg`: 0 for type 1.
  - A zero-initialised layout is the old type-1 layout, so every existing caller and test behaves as before.
- `nr_dci11_offsets_t` (the offsets struct both formats share) gains `fdra_mode`, `n_rbg` and `riv_bits`. The FDRA field keeps its place (`riv`) and its width is still `tda - riv`, so everything that reads offsets needed no change.
- FDRA width: type 1 = `riv_bits`, type 0 = `N_RBG`, dynamicSwitch = `1 + max(N_RBG, riv_bits)`. The rule lives in `nr_fdra_bits()` and is shared by both formats' offsets and by the extractor.
- `nr_dci11_layout_enumerate_fdra()` and `nr_dci01_layout_enumerate_fdra()` run the existing switch enumeration once per mode, and the length constraint prunes the impossible combinations.
  - N_RBG is computed from the BWP CRB start and size.
  - rbg-Size config2 is skipped when its P equals config1's (above 144 PRB both are 16), because the reads would be identical duplicates.
  - The old `*_enumerate()` functions remain type-1-only.
- `nr_dci11_resolver_init_fdra()` is what the RT monitor now uses. It enumerates type 1 over every TDA width **first** and the other modes after, so truncation at the cap can never cost a layout the type-1-only search had. A test checks this.
- Plausibility now depends on the mode:
  - type 1: the RIV lies inside the BWP;
  - type 0: the bitmap is non-empty;
  - dynamicSwitch: the MSB is split off with `nr_fdra_dynamic_split` and the matching test applied.
- `nr_dci11_layout_to_field_bits` carries `fdra_mode`, and `apply_roundtrip` uses the mode's FDRA width. The round trip is checked for every resolved mode.

### PRB arithmetic (`nr_pdsch_prb_set.{h,c}`)
- New:
  - `nr_fdra_rbg_size(mode, bwp_size)`
  - `nr_fdra_bits(mode, n_rbg, riv_bits)`
  - `nr_fdra_prbs(field, mode, n_rbg, riv_bits, bwp_start, bwp_size, prb, max, *type0)`: FDRA field to data-ordered PRB list. It returns 0 when the allocation is impossible. The RIV decode is local and follows the same formula as NRRIV2BW / NRRIV2PRBOFFSET, because the test binaries do not link `nr_common`.
- New `nr_prb_list_normalise()` holds the pure core of Task 9's `nr_pdsch_passive_alloc_normalise()`. The Task 9 function in `nr_pdsch_passive_decode.c` is now a thin wrapper with the same semantics: on failure `fa` is untouched, and the list is refused if it is empty or too long, has an out-of-BWP or out-of-bitmap PRB, or lists a PRB twice. The move is what made it unit-testable.

### DL wiring
- **Extract options** (`nr_pdcch_blind_monitor.{h,c}`):
  - `nr_pdcch_blind_extract_opts_t` gains `fdra_mode` and `fdra_bwp_start`. A zero default means type 1, the old behaviour, and every constructor zero-initialises or copies the struct.
  - `blind_field_bits()` sizes the FDRA per mode, so `nr_pdcch_blind_dci_size_ex()` and extraction agree.
- **Extractor result** (`nr_pdcch_blind_extract_11`):
  - The FDRA is decoded per mode.
  - An RA type 0 grant (including dynamicSwitch with MSB 0) comes back as `ra_type0`, `rbg_bitmap`, `rbg_size` and `rbg_bwp_start`, with `start_rb` = lowest PRB and `num_rb` = PRB count.
  - The new fields are in `nr_pdcch_blind_result_t`, and every producer memsets it.
- **Stage 2** (`nr_pdcch_blind_monitor_rt.c`):
  - The resolver is armed with `nr_dci11_resolver_init_fdra(cfg->bwp_start, …)`. A LOG_W fires if the set hits the cap.
  - Stage-2 options set `o.fdra_mode` and `o.fdra_bwp_start = cfg->bwp_start`, the same grid the resolver used. The existing `dci_size_ex != len` guard catches any disagreement.
- **Grant site** (`nr_pdcch_blind_monitor_rt.c`, where `start_rb`/`number_rbs` are set):
  - For `out.ra_type0`, `freq_alloc.prb_list` is filled with `nr_ra_type0_prbs(...)` and `nr_pdsch_passive_alloc_normalise()` is called **once, before** the fast-enqueue, deferred and in-line (≈6044/6074) decode paths. This follows the Task 9 carry-over.
  - An invalid list becomes a grantdrop, `ra-type0-prb-list-invalid`.
  - `dlsch_pdu.start_rb` / `number_rbs` are set to the first PRB and the PRB count.
  - `resource_alloc` stays 1. This is required, not cosmetic: `nr_dl_channel_estimation.c:1425/1606` AssertFatal()s on 0 when it PRB-averages.
  - The in-thread DM-RS channel estimate (CFR tap and SNR gate) runs on the list's **first contiguous run** (`chest_alloc`). `nr_pdsch_channel_estimation` restarts its output at index 0 for each bitmap block (Task 9 note J), so a multi-block estimate would be wrong. The decode itself uses Task 9's segmented path over the whole list.
  - Type-1 grants are unchanged.
- **Family key and diagnostics:**
  - `dci11_family_key` also hashes `ra_type0` and `rbg_bitmap`, so two type-0 reads with the same first PRB and count do not merge.
  - The DCI11_STAGE2 per-layout dump shows `f<mode>`.

### UL (ruling R11)
- **(a) Done.** The 0_1 stage-1 resolver (`nr_pdcch_dci01_layout_observe`) enumerates every mode with the UL BWP start, and later 0_1 fields are offset per mode in the resolver.
- **Learned mode and refusal.** Once every live 0_1 layout agrees on a non-type-1 FDRA mode, `g_dci01_fdra_mode` is set and UL grants are refused rather than booked. The refusals are counted, and `UL_FDRA_REFUSED` is logged on the first and then every 10000th.
  - Why refuse: the UL extractor still reads the FDRA as a RIV of the configured width.
  - Why this is safe: stage 1 never drops the true layout. On a type-1 cell a type-1 layout always survives, together with its type-0 aliases, so unanimity on another mode is only reachable when no type-1 layout fits the air.
- **(b) Not done: UL type-0 PUSCH decode. This needs a follow-up.** Evidence that it requires changes to OAI's gNB-side demodulation, beyond the passive wrapper:
  - `nr_pusch_passive_decode.c` hands the whole grant to `nr_rx_pusch_group_tp()`. That function does channel estimation, RE extraction, equalisation and LLRs internally, keyed only on `rb_start`/`rb_size` (`nr_ulsch_demodulation.c:163-164, 582-594, 719-721`).
  - The DM-RS pilots are generated from the absolute PRB position inside that call. Task 9's DL trick (per-segment chest, then demodulating a virtual contiguous allocation) therefore cannot be applied from the wrapper: the virtual allocation would get the wrong pilots.
  - Doing it means splitting `nr_rx_pusch_group_tp` into chest and demod stages.
- **Refusal lives in rt.c, not the extractor.** I first added `fdra_mode` to `nr_pdcch_blind_ul_opts_t` with extractor-level refusal, then removed it. Nothing sets the UL widths from the resolver: the UL resolver is observe-only for every width (pre-existing). Changing only the FDRA width would also move `nr_pdcch_blind_dci01_size()`, which derives the UL scan length when there is no override. That makes it dead code at best and a scan-length shift at worst.

## Tests (TDD: the new tests failed to compile against the old code, then passed)
- `test_nr_pdcch_dci11_layout_sweep`: 33/33 pass (8 new, `Dci11Fdra.*`).
  - One test per mode at 106 PRB, where the five widths are all distinct (13/14/7/15/14). Each builds the payload with that mode's FDRA and runs stage 1 (800 payloads) and stage 2 (only the truth decodes, p = 0.4). It asserts:
    - (a) `fdra_mode` of the resolved layout;
    - (b) MCS/RV/antenna-port offsets;
    - (c) the decoded PRB list against `nr_ra_type0_prbs` of the built bitmap (type 0 and dynamic MSB-0) or the RIV range (type 1 and dynamic MSB-1);
    - the field-bits round trip.
  - Also: `PlausibilityFollowsTheMode`, `LayoutCountFitsTheCap` and `TruncationNeverCostsAType1Layout`.
- `test_nr_pdcch_dci01_layout_sweep`: 13/13 pass (6 new, `Dci01Fdra.*`): the same per-mode cases through `nr_dci_resolver_init_from_offsets`, plus `LayoutCountFitsTheCap`.
- `test_nr_pdsch_prb_set`: 12/12 pass (2 new):
  - `ListNormaliseRejectsDuplicateOutOfBwpAndOverLength`: the Task 9 reviewer's case. It covers duplicate, out-of-BWP, over-length (276) and empty lists, the exact-275 accept, and outputs untouched on failure.
  - `FdraPrbsDecodesBothRivCasesAndTheModes`: RIV case 2, dynamic both branches, widths.
- `nr-uesoftmodem` builds clean. The only warnings are pre-existing ones on lines this task did not touch.
- `test_nr_pdcch_blind_monitor` does **not link** in this lane (undefined `nr_pdcch_blind_monitor_bank_has_geometry`, `nr_tdd_config_init`, `nr_tdd_slot_has_downlink`). This is the known pre-existing issue from the other lane, not caused by this change. The two layout test binaries link fine; I added `nr_pdsch_prb_set.c` to both in CMakeLists.

## Layout counts and the cap (concern)
- At the worst case the brief names (len 49, 273 PRB, TDA 0..4), all modes give **1717 < 2048**, so the cap is unchanged and memory is unchanged (the `hist` array stays 2048×116×4 B = 0.95 MB).
- **That is not the real worst case.** A narrow BWP gives an FDRA that is much shorter than the RIV (type-0 config2), which lets more of the switch space fit the length. Measured, all modes / type 1 only:

  | BWP | Length | All modes | Type 1 only |
  |---|---|---|---|
  | 106 PRB | 45 (TDA 0..4) | ~4818 | 694 |
  | 24 PRB | 44 | 7124 | 1706 |
  | 273 PRB | 51 | 3388 | 1706 |

  Note that type 1 alone already exceeds 2048 at 273 PRB / 53 bits (2521): the cap was binding before this task on long DCIs.
- **What I did:** type-1-first ordering, so truncation only cuts type-0/dynamic hypotheses (checked by `TruncationNeverCostsAType1Layout`), plus a LOG_W at arm time.
- **What I did not do: raise the cap.** That is your decision, because of the RT cost:
  - Stage 1 observes every live hypothesis per accepted payload on the scan consumer.
  - Going to 8192 would cost about 4× that CPU at arm time.
  - Memory per resolver at 8192 is 8192×(116×4 + ~60) B ≈ 4.3 MB; there are two resolvers plus thread-local order/score arrays.
- Consequence today: on a 106-PRB cell with an unknown TDA width, an RA-type-0 truth may not be enumerated. Configuring `tda_count` shrinks the set a lot (at a fixed 2-bit TDA, the per-mode sets are 127–1515).

## Other concerns / notes
- **Stage 1 cannot separate type 1 from its type-0 aliases.** A type-0 layout that is 2 bits wider elsewhere lands at the same MCS offset, and a RIV read as a bitmap is almost never empty. Only the TB CRC (stage 2) splits them, and that costs trials: the alive set roughly doubles on type-1 cells. The reverse direction (type-0 truth) is pruned by stage 1.
- **dynamicSwitch UL grants are refused wholesale once the mode is learned,** even the MSB = 1 (type-1) grants. They are refused because the static-width UL extractor reads every field at the wrong offset.
- **Not exercised live (rfsim / OTA).** Nothing in the lab cell uses type 0, and there is no gNB config in this lane to produce it. The end-to-end type-0 decode relies on Task 9's segmented path plus this wiring, and is covered only by unit tests.

---

## Fix round 1 (review I1–I3 + ruling R18): commit `0479e306d3`

All paths below are under `openair1/PHY/NR_UE_TRANSPORT/`. Line numbers are at `0479e306d3`.

### I1 — the UL refusal was inert on type-0 cells
- **Premise correction.** The earlier claim was that "stage 1 prunes type-1 layouts on a type-0 truth, so unanimity can only be reached towards the truth". That is **false on the uplink**:
  - A type-1 window that starts on the constant-zero identifier / UL-SUL / BWP bits always reads a RIV inside the BWP.
  - Measured by the new test `Dci01Fdra.TypeZeroTruthWithConstantLeadingBitsKeepsType1AliveUntilTbCrcRefutesIt` (`tests/nr_pdcch_dci01_layout_sweep_test.cc:288`) at 106 PRB, pre_riv 3, contiguous RBG runs, 20000 payloads: **144/208 type-1 layouts survive** stage 1 with a type-0 cfg1 truth, and **104/240** with a dynamicSwitch cfg1 truth.
  - The reviewer's harness gave 72/144 in a slightly different setup; the conclusion is the same.
  - The unanimity refusal is removed, and the rt.c comment is rewritten (`nr_pdcch_blind_monitor_rt.c:699`).
- **Change: UL FDRA staging, driven by the TB CRC.**
  - The 0_1 resolver is armed type-1-only (`nr_pdcch_blind_monitor_rt.c:774` region).
  - The booked PUSCH TB CRC feeds `nr_pdcch_dci01_fdra_feedback()` (`nr_pdcch_blind_monitor_rt.c:719`). It is called from both UL decode sites: `nr_pusch_passive_queue.c:129` and `nr_pusch_passive_monitor_rt.c:96`.
  - The decision is a pure, tested function, `nr_dci01_fdra_verdict()` (`nr_pdcch_dci01_layout_sweep.c:186`). It returns BOOK, then ARM once there are 0 passes over ≥ 64 booked TBs, then REFUSE while armed, no type-1 TB has passed, and a non-type-1 layout is alive.
  - `nr_pdcch_dci01_fdra_stage()` (`nr_pdcch_blind_monitor_rt.c:727`) appends the type-0 / dynamicSwitch 0_1 layouts via `nr_dci_resolver_append_offsets()`. It then refuses booking at `nr_pdcch_blind_monitor_rt.c:5186` and logs `UL_FDRA_REFUSED` (first occurrence, then every 10000th; the counter is `_Atomic`).
  - One refused grant in 64 is still booked as a probe (`UL_FDRA_PROBE_EVERY`, `:710`). UL CRC also reads 0 for reasons unrelated to the layout (MCS, CFO, timing), so a single type-1 pass ends the refusal.
  - If no type-1 layout fits the 0_1 length at all, the other modes arm at init and the first verdict refuses.
- **Refusal firing, measured.** In that same test on the type-0 cell:
  - `verdict(63,0,unarmed)` = BOOK, then `verdict(64,0,unarmed)` = ARM.
  - After arming, **190** (cfg1) / **112** (dynamic) non-type-1 layouts are alive, the truth is among them after 20000 more payloads, and `verdict(64,0,armed)` = **REFUSE**.
  - `verdict(69,1,armed)` = BOOK.
- **(b) is unchanged:** UL type-0 PUSCH decode remains a follow-up, because `nr_rx_pusch_group_tp` takes `rb_start`/`rb_size` only.

### I2 — type-0 / dynamicSwitch aliases inflated the stage-1 survivors on type-1 cells
- **Change: DL FDRA mode staging.**
  - `nr_dci11_resolver_init_fdra()` (`nr_pdcch_dci11_layout_sweep.c:306`) now enumerates **type 1 only**.
  - `nr_dci11_resolver_all_refuted()` (`:353`) is true when every live layout has ≥ `NR_DCI11_FDRA_ARM_MIN_TRIALS` trials (feed or first-code-block probes) and no pass of its own, as a probe, or through its interpretation family.
  - When it is true, the observer (`nr_pdcch_blind_monitor_rt.c:931`, checked every 4000 payloads) calls `nr_dci11_resolver_arm_next_mode()` (`:398`). That call **appends** the next mode, in the order type 0 cfg1, type 0 cfg2, dynamicSwitch cfg1, dynamicSwitch cfg2. The new entries get fresh counters and are published with a release store (`append_offsets`, `:372`). Nothing live is ever dropped.
  - Stage 1 is never the trigger.
  - If no type-1 layout fits the length, the next modes arm at init; this is vacuously "every type-1 layout refuted".
- **N = 64** (`nr_pdcch_dci11_layout_sweep.h:205`). This equals the existing stage-2 decision floor, `DCI11_S2_MIN_TRIALS`. Justification:
  - The lowest true-layout rate measured on these rigs is 12 % (marginal CRC, rank-4 bed). The chance of the truth reading 0/64 is 0.88^64 = 2.8e-4.
  - A false refutation is non-destructive: it only adds hypotheses. Its cost is the pre-staging dilution, not the answer.
  - UL uses the same N, with the probe path to recover.
- **Staged survivor counts on a type-1 cell** (`Dci11Fdra.TypeOneCellStage1SurvivorsUnchangedByStaging`, the reviewer's I2 scene, 20000 payloads, TDA 0..4):

  | Cell | Staged survivors | Type-1-only resolver | Every mode armed up front |
  |---|---|---|---|
  | 51 PRB, len 44 | **359** | 359 | 2406 / 4932 |
  | 106 PRB, len 46 | **438** | 438 | 2919 / 6190 |
  | 273 PRB, len 49 | **284** | 284 | 753 / 1717 |

  - The staged count is asserted equal to the type-1-only count, and `StagingStartsType1OnlyAndArmsOnlyOnTbCrcRefutation` asserts the hypothesis set is byte-identical to the pre-FDRA one at 106 PRB / 45 bits.
  - So a type-1 cell behaves exactly as before Task 10, and the n_alive ≤ 4/8 stage-2 hand-over is no longer delayed.
- **End to end:** the per-mode tests (`resolve_fdra_mode`, `tests/nr_pdcch_dci11_layout_sweep_test.cc:744`) now start type-1-only. Stage 2, in which only the truth decodes, refutes each earlier stage and arms the next, until the truth's mode is in and wins. The tests assert:
  - a type-1 truth arms nothing;
  - no mode beyond the truth's is ever armed.

  Measured at 106 PRB:
  - type-0 cfg1: 1 mode armed (+127 layouts).
  - dynamicSwitch cfg1: 3 modes armed (+187, +756, +127).
  - dynamicSwitch cfg2: 4 modes armed.
  - type-0 cfg2 at 39 bits: no type-1 layout fits, so both type-0 modes arm at init.

### I3 — the cap test measured a case that is not the worst
- **Change:** `NR_DCI11_LAYOUT_MAX` raised from 2048 to **8192** (`nr_pdcch_dci11_layout_sweep.h:80`). The 0_1 resolver shares the same constant.
- **Memory cost:** `sizeof(nr_dci11_resolver_t)` = **4,300,836 B** per resolver, of which `hist` is 8192×116×4 = 3.8 MB. There are two resolvers, 8.6 MB in BSS, which counts against `RLIMIT_MEMLOCK` under `mlockall`. Add about 0.5 MB of static enumeration scratch (the 0_1 hyp/off arrays and the arm buffers).
- **`LayoutCountFitsTheCap` replaced by `EveryStageFitsTheCap`** in both test files. The sweep covers:
  - every BWP size 6..273, with the CRB start aligned and worst-misaligned (N_RBG + 1);
  - DCI lengths 28..62;
  - TDA widths 0..4;
  - counts memoised on the FDRA width.
- **Results (all asserted < 8192):**
  - DCI 1_1, one stage (one mode, summed over TDA 0..4): **max 5458**, reached by every mode at 6 PRB (48–52 bits).
  - DCI 0_1: type-1 stage max **462**; armed set (modes 1..4 together) max **1830** (10 PRB / 47 bits); type-1 plus armed stays below the cap everywhere.
  - The cumulative DL set, with every mode armed, reaches **26,992** (6 PRB, about 51 bits) and is truncated there, earlier stages first. `arm_next_mode` then reports a short or zero `added`, and rt.c logs "TRUNCATED".
  - This happens only when four stages in a row have been refuted on a very narrow BWP. Freeing the space would mean compacting refuted layouts, which renumbers every layout id that Technique-D contexts and queued jobs hold, so I did not do it.
- The reviewer's numbers reproduce: type 1 alone 2521 at 273 PRB / 53 bits; 106 PRB / 46 bits = 6190 and 51 PRB / 44 bits = 4932 with every mode.

### Minors
- **Static TLS removed.** The per-layout scratch `order`/`sc`/`rot` was `static __thread` arrays (128 kB at 8192). It is now heap buffers held per thread, allocated on first use (`nr_pdcch_blind_monitor_rt.c:584`). All resolvers in the tests are moved to heap/static (`std::make_unique`).
- **Faster enumerator.** The O(n²) dedup (`already_have` / `have`) is replaced: each group's distinct sums are collected in first-occurrence order and combined (`enumerate_mode`, `nr_pdcch_dci11_layout_sweep.c:109`, `nr_pdcch_dci01_layout_sweep.c:95`). The output is distinct by construction.
  - **The output is byte-identical to the old enumerator:** DL + UL, 150 (BWP, TDA, length) configs, every mode, 9.6 MB dump, `cmp` equal.
  - Speed: 42.5 s → 0.24 s on that dump. At the worst case (6 PRB / 51 bits) init takes 2.1 ms for 5458 layouts, and arming to the 8192 cap takes 2.9 ms (the reviewer measured 551 ms before).
  - I chose this over the suggested bitset because it also makes the per-width sweep cheap enough to run as a unit test.
- **Type-0 expansion on the wrong BWP.** It used `cfg->bwp_start` with `cand_task[ti].bwp_size`. A stage-2 type-0 grant is now expanded only when it was framed on the dedicated BWP (`bwp_entry == 0` and matching start/size); otherwise it is a grantdrop, `ra-type0-foreign-bwp` (`nr_pdcch_blind_monitor_rt.c:5900`).
- **UL staging state is `_Atomic`:** try/ok counters, refusal flag, refused counter. `g_dci01_fdra_mode` / `s_refused` are gone.
- **LOG_W on 0_1 truncation,** both the initial set and the armed set.

### Verification
- `test_nr_pdcch_dci11_layout_sweep`: **34/34** pass.
- `test_nr_pdcch_dci01_layout_sweep`: **14/14** pass.
- `test_nr_pdsch_prb_set`: **12/12** pass.
- `ctest` over those three: 100 %. The full dci11 binary runs in 3.1 s.
- `nr-uesoftmodem` builds with the lane script. The only warnings are pre-existing ones on lines this task did not touch.
- `test_nr_pdcch_blind_monitor` still fails to link, the same pre-existing issue as before.

### Remaining concerns
- DL staging advances only once **every** live layout has 64 trials. On a type-0 cell with about 700 type-1 layouts at 8 probes per grant, that is roughly 5600 grants per stage. Slow, but bounded.
- DL arming runs on the stage-1 observer thread. Stage-2 readers on other scan threads see `n_hyp` published with a release store, but they are not otherwise synchronised. This matches the existing unlocked `alive` / `n_alive` updates.
- Nothing has run live (rfsim or OTA).

---

## Fix round 2 (re-review + ruling R22): commit `f8b40ac281`

All paths below are under `openair1/PHY/NR_UE_TRANSPORT/`. Line numbers are at `f8b40ac281`. This round supersedes these round-1 statements:
- "cap 8192 / cumulative truncates";
- "arming never drops a live layout";
- the per-layout refutation floor;
- UL evidence from every booked PUSCH.

### (a) UL oracle contamination (re-review 1) and false refusal on type-1 cells (re-review 2)
- **Feedback now carries the grant.** The signature is `nr_pdcch_dci01_fdra_feedback(const nr_pdcch_blind_ul_result_t *, bool)` (`nr_pdcch_blind_monitor_rt.c:742`), called from `nr_pusch_passive_queue.c` and `nr_pusch_passive_monitor_rt.c`.
- **The evidence is split** (`nr_dci01_fdra_evidence_t`, `nr_dci01_fdra_note`, `nr_pdcch_dci01_layout_sweep.c:186`, under a mutex in rt.c):
  - **Oracle:** format 0_1 with `width_hyp_class < 0 && interp_hyp_class < 0`, i.e. converged, non-discovery widths. Only these count as FDRA evidence.
  - **Link health:** DCI 0_0 and UL-discovery hypothesis passes. They are stamped with the oracle trial count at which they arrived and are never counted as a type-1 pass.
- **The verdict** (`nr_dci01_fdra_verdict`, `:199`) arms or refuses only when:
  - the link is healthy, meaning a link pass arrived during the last 64 oracle trials (the link demonstrably worked while the type-1 reads failed);
  - AND the oracle is 0/64.

  One oracle pass means BOOK permanently. A 0_0 pass never flips the verdict.
- **Booking** (`nr_dci01_fdra_book`, `:215`; used at `nr_pdcch_blind_monitor_rt.c:5237`) refuses oracle-class 0_1 grants only. DCI 0_0 and discovery grants are always booked, so the UL DM-RS CFR from 0_0 continues and the discovery search keeps its feedback. One refused grant in 64 is still booked as a probe.
- **Tests** (`Dci01Fdra.TypeZeroTruthWithConstantLeadingBits…`, type-0 cfg1 and dynamicSwitch cfg1 truths, 106 PRB, pre_riv 3):
  - Passes interleaved every 8th trial are counted as link health: `t1_ok` stays 0.
  - BOOK at 63 oracle failures, ARM at 64.
  - After arming, REFUSE, with 190 / 112 non-type-1 layouts alive.
  - `book()`: 0_0 is booked, oracle-class 0_1 is refused, the 64th is a probe.
  - 100 more 0_0 passes do not end the refusal; one oracle pass does.
  - Dead link (256 oracle failures with no link pass): BOOK, never ARM or REFUSE.
  - Stale link health (the only link pass is from before the 0_1 failures began): BOOK.
- **Re-review 2, residual — a known limit, stated in the rt.c comment.** A type-1 cell whose oracle-class 0_1 PUSCH fails for a non-layout reason while 0_0 decodes (e.g. an MCS-limited 0_1 decode) still meets "link healthy AND 0_1 0/64" and is refused. What the refusal costs is smaller than before, and it is recoverable:
  - 0_0 and discovery grants, including their UL DM-RS CFR, stay booked;
  - 1 in 64 oracle grants is still probed;
  - one oracle pass clears it permanently.

  Removing it entirely needs a per-grant signal that separates "wrong PRBs" from "decodes too hard", for example the DM-RS correlation at the RIV-read PRBs. That would be a follow-up.

### (b) DL: dynamicSwitch cut by the cumulative cap, n_alive growth, disarm
- **Arming order** is now type 1, type 0 cfg1, **dynamicSwitch cfg1, dynamicSwitch cfg2**, type 0 cfg2 (`kArmOrder`, `nr_pdcch_dci11_layout_sweep.c:438`; `nr_dci11_fdra_stage()` maps a mode to its stage). `fdra_next` is now a stage index.
- **Cap:** `NR_DCI11_LAYOUT_MAX` raised to **32768**. `sizeof(nr_dci11_resolver_t)` is **17,104,932 B**; both the 1_1 and 0_1 resolvers share the type, so **34.2 MB of BSS**, counted against `RLIMIT_MEMLOCK` under `mlockall`. The 0_1 enumeration keeps `NR_DCI01_LAYOUT_MAX` = 8192.
- **No cut anywhere in the sweep:** the whole staged set peaks at **26,992**, and `EveryStageFitsTheCap` now asserts cumulative < cap.
- **dynamicSwitch at 106 PRB is complete** (`Dci11Fdra.DynamicSwitchFitsAt106PrbLength49`; each stage's added count is asserted equal to its full enumeration):

  | Length | type 1 | type 0 cfg1 | dyn cfg1 | dyn cfg2 | type 0 cfg2 | Total |
  |---|---|---|---|---|---|---|
  | L=48 | 1706 | 1329/1329 | **988/988** | **1329/1329** | 4174/4174 | 9526 |
  | L=49 | 2107 | 1706/1706 | **1329/1329** | **1706/1706** | 4550/4550 | 11398 |
  | L=50 | 2521 | 2107/2107 | 1706/1706 | 2107/2107 | 4885/4885 | 13326 |

  The re-review had measured dynamicSwitch cfg2 at 0/1329 for L=48, and both dynamic configs at 0 from L=49.
- **Retiring refuted stages:** `nr_dci11_resolver_arm_next_mode()` marks every live layout that has no pass as dead before appending, so n_alive stays one stage. Example, dynamicSwitch cfg2 truth at 106 PRB, stage by stage: 187 → 127 → 79 → 127 live, against 314 / 393 / 520 enumerated.
- **Stage-1 cost, measured** (6 PRB / 51 bits, the largest stage, 5458 live):
  - `prune_by_distribution` now ranks by sort plus binary search instead of the O(n_alive²) double loop.
  - Worst observe call (a prune pass): **0.83 ms**, from 18.75 ms on the round-1 code with the same harness.
  - Mean observe: 106 µs per payload (was 127).
- **Disarm:** `nr_dci11_resolver_disarm()` (`:387`) runs on any type-1 CRC pass. It kills every non-type-1 layout, revives the passing type-1 layout if its stage had been retired, and never arms again.
  - This includes late feedback for a probe that was issued before arming: the feedback flags it, `nr_pdcch_blind_monitor_rt.c:541-548`, and the observer applies it at `:961`.
  - Test `Dci11Fdra.AType1PassDisarms`: after type 0 cfg1 is armed, type-1 layout 3 is retired; `disarm(3)` kills exactly the armed layouts, revives 3 (n_alive = 1), and `arm_next_mode` then returns -1.

### (c) DL link-health gate
- A decode made outside the stage-2 layout search now reports layout 0xFFFF to the feedback: format 1_0 (SIB1/RA/paging/C-RNTI fallback) and the manual/fallback layouts. A CRC pass there stamps `g_dl_link_pass_at` with the current stage-2 trial count (`nr_pdcch_blind_monitor_rt.c:535-541`).
- Arming requires such a pass within the last 64 × (n_alive + 1) stage-2 trials, which is the span a refutation accumulates over (`:966-970`). A dead link therefore cannot cascade-arm every mode on its 0-pass reads (CFO mis-lock, or Technique D still searching).
- **Pre-existing bug found and fixed on the way.** The sweep ticket was zero-initialised, so `layout_index` was 0, not "none". Every 1_0 and manual-layout decode, SIB1 passes included, was credited to stage-2 layout 0's probe tallies. It now defaults to 0xFFFF (`:5891`).
- The in-line (non-deferred) decode path now makes the same layout-feedback call as the deferred consumer.
- **Consequence to know:** a receiver that decodes no 1_0 or manual-layout PDSCH never has DL link health, so it never arms type 0 / dynamicSwitch. This is the conservative failure: it keeps the type-1 behaviour. (No type-1 layout fitting the length at all still arms at init, as before.)

### (d) all_refuted liveness
- **Choice: the reviewer's second option,** an aggregate rule, rather than a K-offers escape (`nr_dci11_resolver_all_refuted`, `:360`). The set is refuted when no live layout has any pass (own, probe or family) and the live set has taken ≥ 64 × n_alive trials **in total**.
- **Why not a K-offers counter:** there is no clean K. A layout can be parsed and offered yet never selected by Thompson, and its reads can be lost before enqueue (the size check or extractor rejection, rt.c ~670-683), so any K would be a guess.
- **Why the aggregate rule is sound:** it keeps the same evidence budget as the per-layout floor, and a single un-trialable layout can no longer freeze staging. The true layout is not the one that starves, because its reads parse, and zero-pass arms share Thompson's trials evenly.
- **Test:** one layout at 0 trials with the budget met gives refuted; one trial short of the budget gives not refuted.

### (e) Memory ordering and the comment
- `n_hyp` is now read with `__ATOMIC_ACQUIRE` everywhere another thread reads it: stage-2 candidates (`nr_pdcch_blind_monitor_rt.c:609`), the feedback bound check, and all_refuted / disarm / arm. It is still published with a release store.
- The rt.c:719 comment now cites the test and its 144/208.

### Verification
- `test_nr_pdcch_dci11_layout_sweep`: **36/36** pass (2 new; per-mode staging, the sweep and the staging test updated for the new order and retirement).
- `test_nr_pdcch_dci01_layout_sweep`: **14/14** pass (verdict test extended as above).
- `test_nr_pdsch_prb_set`: **12/12** pass.
- `ctest` over those three: 100 %.
- `nr-uesoftmodem` builds with the lane script, with only pre-existing warnings. `test_nr_pdcch_blind_monitor` still fails to link (pre-existing).

### Remaining concerns
- The type-1-cell false refusal described under (a) remains possible, but it is now limited to oracle-class 0_1 grants and is self-clearing.
- DL arming now depends on a 1_0 or manual-layout decode stream for link health.
- **Retirement makes a false DL refutation destructive.** Only disarm revives a retired type-1 layout, and only if a late pass arrives for it. The gates against that are the link-health requirement and the aggregate evidence rule.
- Memory is 34.2 MB of BSS for the two resolvers.
- Nothing has run live (rfsim or OTA).
