# OCUDU ISAC test knobs (`isac-test-knobs` branch)

Test-only, env-gated gNB configurations so the passive OAI receiver's blind decoder can be exercised
against DCI/PDU shapes the stock OCUDU scheduler never emits. Never merged upstream.

- Repo: `/home/sens/NICOLA/repos/ocudu` (GitLab `ocudu/ocudu`, `dev` branch has local uncommitted
  changes -- left untouched).
- Worktree: `/home/sens/NICOLA/repos/ocudu-test` (`git worktree add -b isac-test-knobs ... dev`).
- Build dir: `/home/sens/NICOLA/repos/ocudu-test/build` (separate from the main tree's `build/`,
  configured with the same `ENABLE_*`/`CMAKE_BUILD_TYPE`/`ASSERT_LEVEL` options read off the main
  build's `CMakeCache.txt`).
- Commit: on `isac-test-knobs`, trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Summary

| Knob | Status |
|---|---|
| 1. DL PDSCH RA Type 0 / dynamicSwitch | **Implemented, end-to-end** (RRC config -> DCI size -> scheduler grant shaping -> DCI packing -> MAC/PHY FAPI -> PHY signal). Unit-tested. |
| 2. UL PUSCH RA Type 0 / dynamicSwitch | **Implemented, end-to-end**, same chain, mirrored for PUSCH. |
| 3. `vrb-ToPRB-Interleaver` n2/n4 | **Already implemented upstream** (`--interleaving_bundle_size`, wired through `dci_1_1`'s VRB-to-PRB mapping bit and PHY's interleaved mapper). Added an env-var convenience alias only; no new logic needed. |
| 4. Second dedicated BWP + DCI-indicated switching | **NOT implemented -- assessed and reported per the task's own stop condition.** OCUDU's scheduler has zero runtime BWP-switching machinery (see below); this is a multi-day architectural change, not a knob. |
| 5. PRG precoding (n2/n4 + multi-port) / >12-port CSI-RS | **Reported only**, as requested. Both are genuinely unimplemented; concrete evidence below. |
| 6. DM-RS type 2 (DL+UL) | **Implemented, end-to-end.** Already fully wired everywhere except the factory default; one-line-per-direction flip. |
| 7. DL/UL DM-RS + data scrambling IDs | **Implemented, end-to-end.** Found and fixed a real gap: `scrambling_id0/1` existed in config but was never read at the PDU-builder level (always fell back to PCI); `dataScramblingIdentityPDSCH/PUSCH` was already wired, only the factory default was missing. |
| 8. Custom DL/UL TDA lists incl. mapping type B | **Implemented, end-to-end**, including genuinely new mapping-type-B DM-RS symbol-mask generation (previously a hard `ocudu_terminate("Mapping type B not supported")` for any dedicated C-RNTI grant, DL and UL). Cross-verified against OAI's own independently-implemented version of the same TS 38.211 tables rather than transcribed from memory alone. |

Stock YAML/CLI keys already covering the rest of the requested catalogue: see the dedicated section
near the bottom.

---

## 1 & 2. PDSCH/PUSCH Resource Allocation Type 0 (and dynamicSwitch)

### Why this needed more than a scheduler flag

Before touching anything, traced the DCI-size, scheduler-grant, and FAPI/PHY-signal layers
separately, because RA Type 0 turned out to be genuinely unimplemented at three DIFFERENT points,
not one:

1. **DCI size/RRC config layer was already fully general.** `serving_cell_config`'s `pdsch_config`/
   `pusch_config` structs already have `resource_allocation` (`type_0`/`type_1`/`dynamic_switch`) and
   `rbg_sz` (DL only) fields, and `ue_configuration.cpp`'s `get_dci_size_config()` already computes
   the correct `pdsch_res_allocation_type`/`nof_dl_rb_groups` (and UL equivalents) for all three
   modes. Only `serving_cell_config_factory.cpp` hard-codes `resource_allocation_type_1` when
   building the default config -- a one-line-per-direction override.
2. **`ue_cell_grid_allocator.cpp`/`dci_builder.cpp`/`sch_pdu_builder.cpp` only ever produced/packed a
   Type-1 contiguous `vrb_interval`.** `build_dci_f1_1_c_rnti`/`build_dci_f0_1_c_rnti` took
   `vrb_interval` by value/const-ref and always called `ra_frequency_type1_get_riv(...)`;
   `build_pdsch_f1_1_c_rnti`/`build_pusch_f0_1_c_rnti` likewise. The scheduler's own PDU-level
   `vrb_alloc` type (`pdsch_information::rbs`/`pusch_information::rbs`) already natively supports a
   Type-0 `rbg_bitmap` (`vrb_alloc::is_type0()`/`type0()`) -- it just had no producer or consumer.
3. **The FAPI wire layer had NO Type-0 representation at all**, in either direction:
   - `fapi::dl_pdsch_pdu`/`fapi::ul_pusch_pdu` only had a `resource_allocation_type_1` field.
   - `lib/fapi_adaptor/mac/p7/pdu_translators/pusch.cpp` had a live
     `report_error_if_not(!rbs.is_type0(), "PUSCH resource type allocation type 0 is not supported")`
     -- a hard runtime error, not merely an oversight.
   - `lib/fapi_adaptor/phy/p7/pdu_translators/{pdsch,pusch}.cpp` only ever called
     `rb_allocation::make_type1(...)`, even though `rb_allocation::make_type0(vrb_bitmap, ...)`
     already existed in `openair`-style form on the PHY side (`lib/phy/support/rb_allocation.cpp`).

### What was implemented

New file `lib/scheduler/support/isac_test_knobs.h` (header-only, test-only):
- `ISAC_OCUDU_TEST_DL_RA_TYPE0` / `ISAC_OCUDU_TEST_UL_RA_TYPE0` = `1` (forced `resourceAllocationType0`)
  or `2` (`dynamicSwitch`), unset/other = off (stock `resourceAllocationType1`).
- `ISAC_OCUDU_TEST_RBG_CFG` = `1`|`2` -> `rbg-Size` `config1`/`config2` (DL only; PUSCH-Config has no
  `rbg-Size` IE per TS 38.214 6.1.2.2.1, confirmed by reading the existing UL branch of
  `get_dci_size_config()`, which always passes `is_config1=true`).
- `ISAC_OCUDU_TEST_VRB_IL` = `2`|`4` (see knob 3).
- `isac_test::make_alternating_rbg_override(bwp_crbs, P, vrbs)`: builds a genuinely non-contiguous
  ("every other RBG") Type-0 bitmap out of an already-chosen contiguous `vrb_interval`, selecting only
  whole RBGs that lie **fully inside** that range. This is the key design choice: it never grows
  beyond a range the resource-grid bookkeeping already reserved, so the existing collision-check/
  grid-fill logic (still driven by the original contiguous `crbs`) stays correct without being
  touched -- the override only ever *thins* an existing reservation. Degrades to a single (contiguous)
  RBG, rather than failing, when only one whole RBG fits -- see "known limitations" for why that
  matters.

Wiring:
- `serving_cell_config_factory.cpp`: `pdsch_cfg.res_alloc`/`rbg_sz`/`vrb_to_prb_interleaving` and
  `cfg.res_alloc` (PUSCH) overridden from the knobs, placed before the existing "prb-BundlingType must
  match VRB-to-PRB mapping type" switch so both stay consistent.
- `ue_configuration.cpp`: **fixed a real pre-existing bug** found while verifying this against TS
  38.212/38.214, in code this knob depends on but did not introduce: `nof_dl_rb_groups`/
  `nof_ul_rb_groups` (documented as N_RBG, the *number* of RBGs, values 1-18) were being set to
  `get_nominal_rbg_size(...)` (the nominal RBG *size* P, values 2/4/8/16) with no `get_nof_rbgs(...)`
  wrapping. Confirmed load-bearing: `lib/ran/pdcch/dci_packing.cpp`'s
  `freq_resource_assignment_size()` uses `nof_rb_groups.value()` directly as the DCI field's bit
  width for Type 0, so this bug would have produced a garbage-width (and usually far too narrow)
  frequency-domain field for **any** real Type-0/dynamicSwitch deployment, not just this test knob.
  Fixed both DL and UL, both `resource_allocation_type_0` and `dynamic_switch` branches (4 sites).
- `dci_builder.h`/`.cpp`: `build_dci_f1_1_c_rnti`/`build_dci_f0_1_c_rnti` now take `const vrb_alloc&`
  instead of `vrb_interval` (implicit, non-explicit conversion from `vrb_interval` keeps every
  existing call site compiling unchanged when the knob is off). Branch on `vrbs.is_type0()`: Type-0
  packs `vrbs.type0().to_uint64()` directly as `frequency_resource` (`rbg_bitmap`'s
  `LowestInfoBitIsMSB=true` template parameter already matches TS 38.212 7.3.1.2.2's "MSB = RBG 0"
  convention, verified by reading `bounded_bitset`'s own worked doc-comment example, not assumed).
  **`dynamicSwitch` needed one more fix**: TS 38.212 7.3.1.1.1/7.3.1.2.1 prepend a 1-bit Type-0/Type-1
  selector ahead of `max(N_RBG, RIV_bits)` bits. A Type-0 value is always `< 2^N_RBG <= 2^max_bits`,
  so packing it directly leaves that leading bit at 0 (= Type 0) for free -- but a Type-1 RIV value has
  no such bound, so the Type-1 branch, when `pdsch_res_allocation_type == dynamic_switch`, now
  explicitly ORs in the selector bit via `isac_test::dynamic_switch_type1_selector_bit(...)`. Without
  this, any Type-1-shaped grant under `=2` (retransmissions, or a grant narrow enough that the RBG
  override wasn't applied) would have been silently mis-tagged as Type 0 on the wire.
- `sch_pdu_builder.h`/`.cpp`: `build_pdsch_f1_1_c_rnti`/`build_pusch_f0_1_c_rnti` likewise take
  `const vrb_alloc&`; `pdsch.rbs = vrbs`/`pusch.rbs = vrbs` already worked unchanged (both are
  `vrb_alloc` already). Precoding-size lookup (`cs_mgr.get_precoding(nof_layers, nof_rbs)`) needed a
  `isac_test::nof_rb(vrb_alloc, bwp_crbs, P)` helper to expand a Type-0 bitmap to a PRB count the same
  way `vrb_interval::length()` would for Type 1.
- `ue_cell_grid_allocator.cpp` (the single production caller of all four functions above, confirmed by
  grep before touching them): after the existing grid-fill/TBS/MCS logic runs unchanged, if the DL/UL
  knob is on and this is a new-Tx DCI 1_1/0_1 grant, build the RBG override from the *already-chosen*
  contiguous `vrbs`, recompute TBS/MCS for the override's (possibly smaller) actual PRB count via the
  same `compute_dl_mcs_tbs`/`compute_ul_mcs_tbs` the stock path already calls, and pass the resulting
  `vrb_alloc` (Type-0 bitmap or, on failure, the original Type-1 interval) into the DCI/PDU builders
  instead of the plain interval.
  - **HARQ retransmissions are deliberately excluded** (`not is_retx`): a retx must keep the exact
    shape/TBS the first transmission used, or soft-combining semantics are undefined. This is a
    scope limitation, not a bug: it means `=1`/`=2` only affect new-data grants.
  - **Forced Type 0 (`=1`) cannot fall back to Type 1 on override failure** (grant narrower than even
    one whole RBG): the DCI field width is a static, RRC-driven property of the whole SS, not a
    per-grant choice, so falling back would corrupt the field width/selector semantics. Fixed by
    cancelling that grant (same `INVALID_RNTI`/`.reset()` pattern the function already uses for "RBs
    could not be allocated") rather than emitting a wrong DCI. `dynamicSwitch` (`=2`) *can* fall back
    to Type 1, and does, now correctly tagged by the selector-bit fix above.
- FAPI layer (`resource_allocation_types.h`, `dl_pdsch_pdu.h`/`ul_pusch_pdu.h`, both builders, both
  MAC->FAPI translators, both PHY->FAPI translators): new additive
  `fapi::resource_allocation_type_0 { rbg_bitmap rbgs; }`, carried as an `std::optional` alongside
  (not replacing) the existing Type-1 field, so every untouched call site is unaffected. PHY
  translators expand the bitmap back to PRBs via the scheduler's own `convert_rbgs_to_prbs()` (already
  existed, just never called from here) then tag-cast the result
  (`prb_bitmap.slice<MAX_NOF_PRBS, vrb_tag>(...)`) into the `vrb_bitmap` `rb_allocation::make_type0()`
  expects.
  - **The DL rbg-Size (config1/config2) ambiguity**: the FAPI PDU carries only the bitmap, not which
    `rbg-Size` produced it (there was no Type-0 FAPI message to extend, so no precedent for a second
    field either). Rather than widen the wire format further, `infer_dl_rbg_p()` recovers it from the
    bitmap's own size (`config1`'s N_RBG for this BWP size, else `config2`'s) -- unambiguous because
    the two configs give a different N_RBG almost everywhere in the TS 38.214 breakpoint table.
    Documented as a knowing simplification, not silently assumed. UL has no such ambiguity (single
    RBG-size table, confirmed above).

### Unit test

`tests/unittests/scheduler/support/isac_test_knobs_test.cpp` (added to that dir's `CMakeLists.txt`,
gtest, links `ocudu_sched`): covers `make_alternating_rbg_override` producing a genuinely
non-contiguous bitmap for a wide grant, degrading to a single RBG rather than failing when only one
fits, rejecting grants narrower than the smallest RBG, `nof_rb()` agreement between the Type-0 and
Type-1 paths, and that every env knob defaults to `off` when unset. This is the only part of the
change that's testable without a live capture (DCI packing itself, and the FAPI/PHY translators,
weren't touched at the bit-packing level -- only their branching -- and are otherwise exercised by
whatever regression suite already covers the plain Type-1 path).

### Known limitations (by design, not oversights)

- Narrow grants (fewer PRBs than one whole RBG) either degrade to a single-RBG Type-0 grant (not
  genuinely non-contiguous) or, under `=1`, get skipped for that slot. Pick a scene/MCS/traffic level
  that produces wide-enough grants if the test specifically needs to see gaps in the bitmap every
  time.
- `compute_dl_mcs_tbs`'s DC-subcarrier-avoidance nudge (the `contains_dc` bool) is not re-derived for
  the override's actual (shrunk) PRB positions -- it's passed a conservative fixed value instead. This
  can only make the recomputed MCS slightly more conservative than optimal, never wrong/unsafe.
- The FAPI PDU's `fmt::formatter` still prints the (now-unused, default-empty) Type-1 `vrbs` field
  when a PDU is actually Type-0-shaped -- a cosmetic logging gap, not a functional one; not fixed
  (out of scope for a test knob).
- rfsim/OTA validation not performed here -- another agent owns the capture harness per the task's own
  rules; this report covers implementation + unit tests only.

---

## 3. `vrb-ToPRB-Interleaver` n2/n4

**Already fully implemented upstream, verified by tracing the whole chain, not assumed:**
`--interleaving_bundle_size` (CLI/YAML, `vrb_to_prb::mapping_type` = `none`/`n2`/`n4`) ->
`du_pdsch_resource_manager.cpp` -> `cell_cfg_list[].ran.init_bwp.pdsch.interleaving_bundle_size` ->
`serving_cell_config_factory.cpp` sets `pdsch_cfg.vrb_to_prb_interleaving` (and the matching
`prb_bndlg` bundling size, per TS 38.214 5.1.2.3) -> `ue_configuration.cpp`'s
`get_dci_size_config()`/`search_space_info::update_pdsch_mappings()` -> the scheduler's own
`request.interleaving_enabled` flag -> `build_dci_f1_1_c_rnti(..., enable_interleaving, ...)` sets the
DCI 1_1 VRB-to-PRB-mapping bit -> `build_pdsch_f1_1_c_rnti` selects the interleaved `vrb_to_prb::mapping_type`
for the PHY PDU. PHY-side interleaved mapping (`lib/phy/support/rb_allocation.cpp`) was already
exercised by this path.

Added only a convenience env-var alias, `ISAC_OCUDU_TEST_VRB_IL=2|4`, in
`serving_cell_config_factory.cpp` (`isac_test::vrb_interleaver_override()`), for symmetry with knobs 1
and 2 -- no new interleaving *logic*. Unset means "use whatever `--interleaving_bundle_size`/YAML
already says," so this is fully backward compatible.

Note: setting `ISAC_OCUDU_TEST_VRB_IL` together with `ISAC_OCUDU_TEST_DL_RA_TYPE0=1` (forced Type 0)
is a nonsensical combination -- TS 38.214 5.1.2.2 doesn't support interleaved VRB-to-PRB mapping for
Type 0 at all, and `get_dci_size_config()`'s Type-0 branch already ignores
`vrb_to_prb_interleaving` for exactly this reason, so it's harmless, just meaningless. Use
`ISAC_OCUDU_TEST_DL_RA_TYPE0=2` (dynamicSwitch) if both need to be exercised together.

---

## 4. Second dedicated BWP + DCI-indicated switching -- ASSESSED, NOT IMPLEMENTED

Per the task's own instruction to stop and report if this is large: **it is large, confirmed by
direct evidence, not estimated:**

- `ue_cell::active_bwp_id() const { return to_bwp_id(0); }` (`lib/scheduler/ue_context/ue_cell.h`) --
  the "active BWP" is a **compile-time constant**, not runtime state. There is no BWP-switch trigger,
  timer, or state machine anywhere in the scheduler (grepped for `bwp_indicator`/`switch_active_bwp`/
  `active_bwp_id` across `lib/scheduler/`: the only writers of "active BWP" are this one hard-coded
  getter and its callers).
- `dci_builder.cpp` never populates `f1_1.bwp_indicator`/`f0_1.bwp_indicator`, and
  `dci_size_config::nof_dl_bwp_rrc`/`nof_ul_bwp_rrc` (which drive that field's bit width, TS 38.212
  7.3.1.1.2/7.3.1.2.2) are effectively always 0 in practice, since nothing ever populates a second
  BWP into the config that feeds them.
- **The RRC-facing data model is not the blocker**: `serving_cell_config::dl_bwps` is already a
  `static_vector<bwp_downlink, MAX_NOF_BWPS>` (plural, sized for more than one). The blocker is
  entirely on the scheduler runtime side.

What a real implementation needs (not attempted): a per-UE active-BWP-id as mutable state; a switch
trigger (RRC-reconfig, DCI-indicated, and/or `bwp-InactivityTimer`); per-BWP CORESET/search-space/
PDCCH-monitoring configuration and switching; `bwp_indicator` population in `dci_builder.cpp` sized
from real `nof_dl_bwp_rrc`/`nof_ul_bwp_rrc`; HARQ-process continuity across a switch (a BWP switch can
change numerology/PRB grid entirely); and CSI-RS/SRS resource management re-scoped per BWP. This is a
multi-day scheduler-architecture change, not a config knob, and was not attempted.

---

## 5. PRG precoding (n2/n4, multi-port) and >12-port CSI-RS -- reported only

Both are genuinely unimplemented, confirmed from source (not from documentation/comments alone):

- **PRG precoding**: `ue_channel_state_manager::get_precoding()`
  (`lib/scheduler/ue_context/ue_channel_state_manager.h`) always emits exactly **one**
  `pdsch_precoding_info::prg_infos` entry covering the *whole* allocation
  (`nof_rbs_per_prg = nof_rbs`) -- i.e. wideband precoding only, unconditionally. There is no
  per-subband/per-PRG loop anywhere. The type-level scaffolding for dynamic bundling exists
  (`pdsch_prb_bundling.h`'s `dynamic_bundling::bundling_size_set1` has `n2_wideband`/`n4_wideband`
  variants, and `dci_size_config::dynamic_prb_bundling` exists as a DCI-size input) but
  `serving_cell_config_factory.cpp` only ever constructs `static_bundling{wideband}` -- the
  dynamic-bundling variant is never selected, so the DCI's `prb_bundling_size_indicator` field is
  never actually driven by anything. To implement: per-PRG channel-state tracking (today only
  wideband CSI is stored per UE), a real per-PRG precoder-selection loop in `get_precoding()`, and
  wiring `dynamic_bundling` into the factory + `dci_builder.cpp`'s TODO'd
  `prb_bundling_size_indicator` (already flagged `// TODO` in the existing code, confirming this was
  a known gap before this task).
- **>12-port CSI-RS**: `lib/scheduler/config/csi_helper.cpp` hard-caps at 4 ports in multiple places
  (`report_error_if_not(params.nof_ports <= 4, ...)`, plus several `report_error("Unsupported number
  of antenna ports {}", ...)` for anything outside `{1,2,4}`). The underlying RAN types already allow
  more (`CSI_RS_MAX_NOF_PORTS = 32`, matching TS 38.211 Table 7.4.1.5.3-1's row range up to 32 ports),
  but the scheduler's own resource-mapping builder never constructs a resource for rows 5-18 (the
  >4-port rows). To implement: extend `csi_helper.cpp`'s resource-mapping construction for the
  higher-port rows, extend the CSI report/codebook config for wider PMI codebooks, and expose
  port-count selection through the YAML/CLI layer (today it's `csi_params`-internal, not directly
  operator-settable per resource beyond what the 1/2/4-port helpers assume).

Neither was implemented (out of scope per the task; both would need real DL antenna-port hardware/RF
chain support at the gNB to mean anything anyway, which this ZMQ SISO/2x2 rig doesn't have).

---

## 6. DM-RS type 2 (DL + UL)

`ISAC_OCUDU_TEST_DMRS_TYPE2=1` -- forces `is_dmrs_type2 = true` on every mapping-type DMRS config
(PDSCH type A and B, PUSCH type A and B). Unset/other = stock type 1.

**Already fully wired everywhere except the factory default**, verified by tracing every consumer
before touching anything:
- RRC ASN1 encoding (`lib/du/du_high/du_manager/converters/asn1_rrc_config_helpers.cpp`, 2 sites)
  already branches on `is_dmrs_type2`.
- `dmrs_helpers.h`'s `make_dmrs_info_dedicated()` (both DL and UL) already sets
  `dmrs.config_type` from it.
- `dci_builder.cpp` already selects the DCI antenna-ports table by it
  (`get_pdsch_antenna_port_mapping_row_index(..., dmrs_config_type::type2, ...)`).
- `ue_configuration.cpp`'s DCI-size computation already sizes the antenna-ports field by it.
- PHY: `lib/phy/upper/signal_processors/pdsch/dmrs_pdsch_processor_impl.h` explicitly branches on
  `dmrs_config_type::type2`; PUSCH's `dmrs_pusch_estimator_impl.cpp` reads `config.get_dmrs_type()`
  generically (not hardcoded to type 1), confirming the RE-mapping/sequence generation code is
  type-agnostic for both directions, not literally type-2-string-matched only on the DL side.

The one-line gap: `serving_cell_config_factory.cpp` never set `is_dmrs_type2 = true` for any
mapping-type config. Fixed for all four (DL/UL x type A/B).

---

## 7. DL/UL DM-RS + data scrambling IDs

- `ISAC_OCUDU_TEST_DL_DMRS_ID0` / `_ID1` (uint16): `DMRS-DownlinkConfig` `scramblingID0`/
  `scramblingID1` (TS 38.211 7.4.1.1.1), applied to both PDSCH mapping type A and B configs.
- `ISAC_OCUDU_TEST_UL_DMRS_ID0` / `_ID1` (uint16): same, for `DMRS-UplinkConfig`'s
  `transformPrecodingDisabled` sub-config (TS 38.211 6.4.1.1.1.1 -- the transform-precoding-enabled
  case uses a different ID, `n_PUSCH_ID`, out of scope here since this test cell doesn't enable
  transform precoding).
- `ISAC_OCUDU_TEST_DL_DATA_ID` / `ISAC_OCUDU_TEST_UL_DATA_ID` (uint16): `dataScramblingIdentityPDSCH`/
  `PUSCH` (TS 38.211 7.3.1.1/6.3.1.1).
- **New, not requested by name but required for ID1 to be reachable at all**:
  `ISAC_OCUDU_TEST_DL_DMRS_NSCID` / `ISAC_OCUDU_TEST_UL_DMRS_NSCID` = `0`|`1` (default `0`) --
  selects which of ID0/ID1 is actually used (the DCI's `dmrs_seq_initialization` bit / PDU's
  `n_scid`, TS 38.211 7.4.1.1.1).

**Two real gaps found and fixed, verified by reading every consumer before changing anything:**
1. **`dataScramblingIdentityPDSCH/PUSCH` was already fully wired** (`get_pdsch_n_id()` in
   `sch_pdu_builder.cpp` for DL; the equivalent PUSCH `n_id` assignment in `build_pusch_f0_1_c_rnti`
   for UL) -- only the factory default was missing. One line per direction.
2. **`scrambling_id0`/`scrambling_id1` existed on `dmrs_downlink_config`/
   `dmrs_uplink_config::transform_precoder_disabled` but were never read anywhere**:
   `make_dmrs_info_dedicated()` (DL and UL, `dmrs_helpers.h`) hard-coded
   `dmrs.dmrs_scrambling_id = pci; dmrs.n_scid = false;` unconditionally, ignoring the config
   entirely. Fixed by adding an `n_scid` parameter to both overloads (default `0`, so every existing
   call site not yet updated -- there are none, both call sites were updated -- stays
   bit-identical), reading `scrambling_id0`/`scrambling_id1` and falling back to PCI exactly as TS
   38.211 specifies when unset.
   - **Why the NSCID selector knob exists**: `dci_builder.cpp` also hard-coded
     `dmrs_seq_initialization = 0` for both DCI 1_1 and 0_1 -- there was an explicit `// TODO: ...
     set dmrs_seq_initialization based on whether scramblingID0 or [ID1]` comment already in the code
     marking this as a known, deliberately-deferred gap. Without also fixing this, configuring ID1
     would have had no effect: the DCI would keep announcing ID0 and the PDU would keep scrambling
     with whatever `n_scid` its precomputed config carried (see below), so nothing would actually
     use ID1. `dci_builder.cpp` now reads the same `isac_test::dl_dmrs_nscid()`/`ul_dmrs_nscid()`
     value, keeping the DCI announcement and the PDU's actual scrambling consistent.
   - **Why NSCID is a fixed cell-wide choice, not per-grant dynamic**: `pdsch_cfg_list`/PUSCH
     equivalent (in `ue_configuration.h`) precompute one `pdsch_config_params`/`pusch_config_params`
     per (TDA index, layer count) pair ONCE at config time, not per grant -- `n_scid` is baked into
     that precomputed object. Making it vary per-transmission (as a real gNB might, e.g. for MU-MIMO
     orthogonality) would mean restructuring that precomputed-list architecture, which is out of
     scope for a test knob. The methodology is: run the test twice, once per NSCID value, to exercise
     both IDs -- a real, if less elegant, way to validate a receiver's ID0 and ID1 decode paths
     independently.

---

## 8. Custom DL/UL TDA lists including mapping type B

`ISAC_OCUDU_TEST_DL_TDA="k0:mapping:S:L;k0:mapping:S:L;..."` and the `UL_TDA`/`k2` equivalent
override the dedicated `pdsch-TimeDomainAllocationList`/`pusch-TimeDomainAllocationList` (TS 38.331
PDSCH-Config/PUSCH-Config). `mapping` is `A`/`typeA`/`B`/`typeB` (case-insensitive). Example:
`ISAC_OCUDU_TEST_DL_TDA="0:typeB:0:8"` forces every C-RNTI DCI 1_1 grant onto a single mapping-type-B
entry (k0=0, S=0, L=8). Unset, empty, or **any single malformed token** => the whole override is
ignored (empty list), i.e. stock table, unmodified -- a half-applied list would be a confusing test
artifact, not a convenience.

### The custom-list mechanism already existed; only mapping type B needed new code

`get_c_rnti_pdsch_time_domain_list()`/`get_c_rnti_pusch_time_domain_list()`
(`lib/scheduler/support/{pdsch,pusch}/*_default_time_allocation.cpp`) already prefer the dedicated
`pdsch_td_alloc_list`/`pusch_td_alloc_list` over the default table whenever it's non-empty -- exactly
the 3GPP `pdsch-TimeDomainAllocationList` IE. So the "custom list" part of this knob is just: parse
the env var, and set that field in `serving_cell_config_factory.cpp` (same pattern as every other
knob here). For **mapping type A** entries this was already fully functional end to end.

**Mapping type B was not.** Confirmed genuinely unimplemented, not merely untested, by reading every
consumer before writing anything:
- `dmrs_helpers.h`'s `make_dmrs_info_dedicated()` (the DCI 1_1/0_1 dedicated-grant DM-RS builder, both
  DL and UL) had `else { ocudu_terminate("Mapping type B not supported"); }` -- a hard process abort,
  not a soft error, reachable the instant a dedicated grant's TDA entry had `map_type == typeB`.
- `sib1_scheduler.cpp` explicitly filters out any `map_type == typeB` TDA candidate
  (`if (pdsch_td_res.map_type == sch_mapping_type::typeB) { continue; }`) -- independent confirmation
  that mapping type B is a known, deliberately avoided gap elsewhere in the codebase too, not
  something specific to the dedicated path.
- `get_pdsch_config_f1_1_c_rnti()`/`get_pusch_config_f0_1_c_rnti()` (the TDA-entry-to-DMRS-config
  callers) unconditionally passed `pdsch_mapping_type_a_dmrs.value()`/`pusch_mapping_type_a_dmrs.value()`
  regardless of the TDA entry's own `map_type` -- so even fixing the `ocudu_terminate` alone would
  have derived DM-RS from the WRONG config object (type A's additional-position/DM-RS-type/max-length,
  applied to a type B allocation).
- `pdsch_mapping_type_b_dmrs`/`pusch_mapping_type_b_dmrs` (the config fields TS 38.331 defines for
  exactly this) were never populated by the factory at all -- always `nullopt`.

**All four fixed:**
1. New `pdsch_dmrs_symbol_mask_mapping_type_B_single_get()` /
   `pusch_dmrs_symbol_mask_mapping_type_B_single_get()` (mirroring the existing type-A functions'
   file/struct layout in `lib/scheduler/support/{pdsch,pusch}/*_dmrs_symbol_mask.{h,cpp}`),
   implementing TS 38.211 Table 7.4.1.1.2-3 / 6.4.1.1.3-3's mapping-type-B columns (single-symbol
   DM-RS only, matching the existing type-A functions' own single-symbol-only scope -- double-symbol
   DM-RS mask generation isn't implemented for type A either in this codebase, out of scope here).
   **The exact table values were cross-checked against a second, independently-verified
   implementation of the same 3GPP tables** (OpenAirInterface's `nr_mac_common.c`, which has both
   PDSCH and PUSCH, both mapping types, as literal bitmask tables with worked examples in its own
   comments) rather than transcribed from memory alone -- given the choice between spending the time
   to cross-verify or risking a silently-wrong DM-RS RE position (which corrupts channel estimation
   without any error message, exactly the failure mode `verify-before-asserting` exists to catch),
   this was worth doing properly. PDSCH and PUSCH type-B tables are NOT identical past duration 11
   (confirmed by comparing values, not assumed from type A's own DL/UL identity) -- kept as two
   separate functions/tables for exactly this reason.
2. `make_dmrs_info_dedicated()` (DL + UL) now branches on `map_type`: type A unchanged, type B calls
   the new function with `nof_symbols = td_cfg.symbols.length()` (the allocation's own duration,
   *not* type A's "S+L to slot start" convention) and `start_symbol = td_cfg.symbols.start()` (used
   to shift the type-B-relative mask, which is anchored at the allocation's own first symbol, into
   slot-absolute symbol indices).
3. `get_pdsch_config_f1_1_c_rnti()`/`get_pusch_config_f0_1_c_rnti()` now select
   `pdsch_mapping_type_a_dmrs`/`_b_dmrs` (UL equivalent) by the TDA entry's own `map_type`.
4. `serving_cell_config_factory.cpp` now populates `pdsch_mapping_type_b_dmrs`/
   `pusch_mapping_type_b_dmrs` (mirroring type A's settings, including knobs 6/7's overrides) --
   **gated on the custom-TDA-list knob being non-empty, not unconditional**: populating it always
   would have changed the RRC wire format for every stock (non-test) deployment too (the
   `dmrs-DownlinkForPDSCH-MappingTypeB`/UL-equivalent IE would start being signalled to every real UE
   that was never there before), which is exactly the kind of stock-behaviour change every other knob
   in this file avoids off its own env var.

### What was NOT changed
`make_dmrs_info_common()` (the DCI 1_0/f0_0 common/fallback DM-RS paths, used for SIB1/RAR/Msg3/Msg4,
not C-RNTI dedicated grants) still hard-terminates on mapping type B, unchanged -- out of scope: the
task asked about "C-RNTI grants in the USS", and the custom-TDA-list knob only ever feeds the
*dedicated* list, which `get_c_rnti_pdsch_time_domain_list()`/UL equivalent only consult for
non-fallback (i.e. C-RNTI dedicated) search spaces -- so this path is structurally unreachable from
this knob and was left alone.

### Unit tests
Added to the existing `tests/unittests/scheduler/support/isac_test_knobs_test.cpp`: `parse_tda_token`
accepting valid entries (both mapping spellings, case-insensitivity) and rejecting every malformed
form tried (unknown mapping, missing/extra fields, out-of-range k0/k2, S+L exceeding the slot, zero
length); `parse_tda_list_env` reading multiple entries and clearing the whole list on one malformed
token; and both new DM-RS type-B mask functions checked against hand-picked table entries (verified
against the same OAI cross-reference used to build them, not re-derived independently -- that would
just be testing my own transcription against itself).

---

## Stock YAML/CLI keys already covering the rest of the requested catalogue

Verified by reading `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp` and
cross-checking against `configs/gnb_zmq_n78_tdd_2x2.yaml`'s actual schema (same app/config layer):

| Item | Key(s) |
|---|---|
| AL1-AL16 candidate counts (dedicated SS) | `--ss2_n_candidates` (**never pass a negative `al_cqi_offset`** -- confirmed crashing elsewhere in this codebase, per project memory, not re-tested here). |
| MCS tables (qam256, qam64LowSe) | `--mcs_table` (separately for `pdsch`/`pusch` sections; accepts `qam64`/`qam256`/`qam64lowse`). |
| DM-RS additional positions | `--dmrs_additional_position` (separately for `pdsch`/`pusch`). |
| HARQ/retx forcing | `--max_nof_harq_retxs` (separately for `pdsch`/`pusch`). |
| CSI-RS / TRS | `--csi_rs_enabled`, `--csi_rs_period`, `--tracking_csi_rs_slot_offset` (TRS-style periodic
  tracking resource), `--meas_csi_rs_slot_offset`, `--zp_csi_rs_slot_offset`. |
| VRB-to-PRB interleaving | `--interleaving_bundle_size` (see knob 3 above). |

**Checked and NOT found in this app's YAML/CLI schema** (grepped `add_option(...)` calls for
`scrambl`/`dmrs.*type`/`time_domain`/`tda`, zero hits): DM-RS **type 2** selection, PDSCH/PUSCH data
**scrambling ID** override, and PDSCH/PUSCH **TDA lists including Type B**. These appear to be
hard-coded in `serving_cell_config_factory.cpp`'s defaults today with no operator-facing knob in this
particular app config layer. Not implemented here (outside the requested priority list, and the task
asked only to *list* what's covered, not to add what isn't).

---

## Build status

Measured 2026-09-27 after the capture campaign: the isolated checkout is branch
`isac-test-knobs` at `153246e00d60a9333fbb1b8073e1fae9bd9d695f` (parent `a5fabd21518b...`).
The gNB and corrected fixture were rebuilt in `/home/sens/NICOLA/repos/ocudu-test/build` after the
fixture source changed. The resulting binaries are:

- `build/apps/gnb/gnb` SHA256 `c0ab6cb71c44ccf1be1f72c536825b1a07fba427f5aba31a344136c74016c7b7`;
- `isac_test_knobs_test` SHA256 `3d65c7d9e5a574addff64fb2ffac8e05f73d193cb2f4235c3a2c49ab1086622d`.

The corrected fixture (106-RB RBG config 1 uses `P=8`, not `P=4`) passes three shuffled runs,
seeds 70929-70931: 9/9 each, **27/27 total**. Evidence:
`/tmp/codex_ocudu_knobs_20260927.log`, SHA256
`b15a0becf183d4a0b72cc0e87ea10614caf7e8bd13bdad9f61b6bbcec203b644`.
The Ninja database SHA256
`025dc1d7b9ba905e2542d124c2e393b22a5dbe3b89d089349be303c03bed9091`
records the touched scheduler compilation, gNB link, and later fixture rebuild. The retained console
log is only the earlier interrupted 115/1367 attempt, so it does **not** establish the successful
full build's warning count; no zero-warning claim is made. Current tracked dirt is only the corrected
fixture (`tests/unittests/scheduler/support/isac_test_knobs_test.cpp`); `build/` is untracked.

This establishes build and unit evidence, not G4. No live knob arm or independent full-diff review is
recorded yet. DM-RS type 2 and UL Type-0/dynamicSwitch still require the handover's n>=3 alternated
live arms before their receiver lanes can pass G4.
