# Lane report: gap-dmrs2 (DM-RS configuration type 2)

**Status: DONE_WITH_CONCERNS.** All four commits landed and tested green (offline only — no radio
process started on sensnuc3, per instruction). G1 gate (clean `nr-uesoftmodem` build) passes,
re-confirmed after every commit. Transform precoding (gap 3) is now **implemented**, not just
assessed — see its section for the exact OCUDU arm and expected receiver log lines to hand to
whoever runs the live capture. The only remaining concern is live validation itself, which I did
not run (queue is owned by another agent on sensnuc3).

## G1 build gate: PASSED (with one unrelated environment bug found+fixed along the way)
`nr-uesoftmodem` builds clean in `gap-dmrs2` via `lane-make.sh gap-dmrs2 nr-uesoftmodem`. Two
build attempts failed first with `ld.bfd: ... nr_pdsch_gpu_fep.cu.o: ELF section name out of
range` / same for `nr_pdcch_gpu_fep.cu.o` (the CUDA `pdsch_gpu`/`pdcch_gpu` MODULE targets,
`ENABLE_LDPC_CUDA=ON` in this build dir's inherited cache). Root cause: this build dir was
created by `cp -a`-copying `val`'s already-built tree, and those two `.cu.o` intermediates were
stale/corrupt relative to the current binutils 2.46 + nvcc 12.4 toolchain — `rm`-ing each `.cu.o`
and letting `make` recompile it fresh fixed both (`libpdsch_gpu.so`/`libpdcch_gpu.so` now build
and link cleanly). Not a code issue, nothing under source control changed for this; noting it
because the next lane to build from a `cp -a`-copied dir will likely hit it too.

## Commits
1. `8940602423` — core DM-RS type-2 fix (DL antenna-ports tables were already complete; UL
   antenna-ports decode + discovery gate fixed).
2. `34d29e5df9` — UL `dmrs_max_length=2` (double-symbol DM-RS) support in `nr_pdcch_ul_discovery.c`.
3. `ccc60caa6b` — UL/DL DM-RS scrambling-ID 2-stage probe (`nr_dmrs_id_estimate.c`) made
   DM-RS-type-2-aware; DL 2-stage probe caller updated too (forced by the shared function's
   signature change, see that section).
4. `cd6bd3f63d` — UL transform precoding (DFT-s-OFDM): the 4-step plan from the earlier assessment,
   implemented together (discovery + decode gates lifted, antenna-ports table unified, MCS-table
   generator fixed, low-PAPR DM-RS wired in). See its own section for the OCUDU arm + expected logs.

Files touched, commit 1:
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.c`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_discovery.c`
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc`
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci11_layout_sweep_test.cc`

Files touched, commit 2:
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_discovery.c` (dmrs_max_length gate removal)
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc` (dmrs_max_length tests)

Files touched, commit 3:
- `openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h` / `.c` (dmrs_type parameter)
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_dmrs_id_estimate_test.cc` (type-2 tests)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c` (UL caller: gate removed, dmrs_type
  threaded through)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (DL caller: same, forced by the shared
  function's signature — see below)

## Test results (all confirmed by actually running them, not inferred)
- `test_nr_pdcch_blind_monitor`: **163/163 pass** (161 + 2 pre-existing/unrelated skips —
  `PdcchReplay.OtaCss0DecoderContract`, `PdcchReplay.BudgetTimingEveryWidth`), final count after
  all 4 commits (160/160 after commits 1+2, +3 for commit 4's new tests).
- `test_nr_pdcch_dci11_layout_sweep`: pass (commit 1 only touches this one).
- `test_nr_dmrs_id_estimate`: **15/15 pass** (commit 3), including all 4 new type-2 tests.
- `UlFieldSweep`+`UlInterpSweep` (part of the blind_monitor binary): **13/13 pass** after commit 4,
  including the updated 800-hypothesis catalogue-size assertions and the new TP-only-generates-its-
  own-tables test.
- `nr-uesoftmodem`: builds clean, re-verified after every commit (G1 gate). One pre-existing,
  unrelated `-Wunused-function` warning on `passive_ul_unav_res()` first noticed after commit 4's
  build but confirmed present in an earlier commit-3-era full-build log too — not caused by this
  work, not investigated further.
- All builds/runs done via `lane-make.sh gap-dmrs2`, `pgrep -x nr-uesoftmodem` checked clean before
  every build; several builds queued for minutes behind other lanes' live captures on the shared
  sens6 host — waited them out rather than building concurrently, per the "never build during a
  capture" rule.

## Commit 1: core DM-RS type-2 fix — what was actually wrong (root cause, not the whole surface the brief listed)

Before touching anything, I read every DM-RS-type-adjacent code path in the passive DL/UL chain.
Most of the surface the brief worried about (RE mapping, channel estimation, data-RE exclusion,
blind detection from evidence) turned out to **already be correct for both types** — the real gap
was narrow and specific:

1. **UL DCI 0_1 antenna-ports decode was a closed form valid ONLY for Table 7.3.1.1.2-8**
   (`blind_ul_finish()` in `nr_pdcch_blind_monitor.c`): transform-precoding-disabled / DM-RS type 1
   / maxLength 1 / rank 1. It rejected `antenna_ports > 3` outright with the reason
   `"antenna-ports code point outside Table 7.3.1.1.2-8's four rows"`. A type-2-configured cell's
   antenna-ports codepoints (≥4 rows) never had a chance.
2. **`nr_pdcch_ul_discovery.c` independently pruned every `dmrs_config_type != 0` hypothesis**
   before it could even reach the extractor (`supported()` and the DCI 0_1 handler both had
   `... && opts.dmrs_config_type==0 && ...`), with a comment explicitly acknowledging "the existing
   receiver cannot resolve these antenna-port tables ... this is missing receiver support".

Both are now fixed by **reuse, not new DSP**: `decode_dci_antenna_ports_val()`
(`openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c`) is the exact reverse-table lookup
`mac_tables.c`'s `ul_ports_config()` already uses for an attached UE's own PUSCH config. It already
covers DM-RS type 1 AND type 2 (`lut_rev_t1_r{1..4}` / `lut_rev_t2_r{1..4}`), both maxLength values,
and ranks 1-4, with its own bounds check (`val >= size` → -1), so the exact AssertFatal-in-
`get_dmrs_port()` landmine the old range guard existed to prevent (measured 2026-09-09,
`antenna_ports=14` → port bitmap with no port below 12) is still prevented, for free. This is
already unit-tested for type 2 in `openair2/LAYER2/NR_MAC_COMMON/tests/test_lut_dmrs.c`.

## What was already done and needed no change

- **DL (DCI 1_1) antenna-ports decode**: `nr_pdcch_blind_extract_11()` already has all four
  TS 38.212 7.3.1.2.2 tables in-tree (`g_table_7_3_2_3_3_1..4`, 12/31/24/58 rows — tables -3/-4 ARE
  DM-RS type 2, single- and double-symbol), correctly threading `dmrs_config_type`/
  `n_dmrs_cdm_groups`/`dmrs_ports` through. It had a stale, self-contradicting comment above it
  claiming "Type 2 ... is not decoded: rejected as out of range" directly above code that plainly
  does decode it — removed (dead comment, not dead code).
- **Blind type-1-vs-type-2 detection from evidence, DL side**: `nr_pdcch_dci11_layout_sweep.c`
  already enumerates `dmrs_type` as a competing hypothesis dimension at the 5-bit width (Table -2
  type-1-maxLen2 vs Table -3 type-2-maxLen1, same width) and lets the existing CRC-driven Technique D
  resolver (the stateful layout search already in `nr_pdcch_blind_monitor.c`) discriminate them from
  observed grants — exactly the "detect from evidence, not config" mechanism the brief asked for.
  No new mechanism needed; only a completeness gap in it (below).
- **UL side detection from evidence**: `nr_pdcch_ul_interp_sweep.c` already generates
  `dmrs_config_type ∈ {0,1}` as part of its hypothesis space; it was `nr_pdcch_ul_discovery.c`'s
  `supported()` that threw type-2 hypotheses away before they could ever be scored (fixed above).
- **RE mapping / data-RE exclusion, DL data-aided**: `nr_pdsch_data_aided.c`'s `dmrs_re_bitmap`
  logic already special-cases type1×{1,2 CDM groups} and type2×{1,2 CDM groups}, and correctly
  falls back to "whole symbol is DM-RS" (0xfff) for type1×2-groups and type2×3-groups (both of which
  legitimately consume the entire 12-RE symbol per TS 38.211 §7.4.1.1.2) — verified this covers the
  full space by hand, not just by reading the comment.
- **UL passive decode chain**: `nr_pusch_passive_decode.c` threads `dmrs_config_type`/
  `n_dmrs_cdm_groups` through into OAI's own shared `nr_rx_pusch_group_tp()` /
  `nr_ulsch_decoding()` — the mainline gNB-side PUSCH RX chain, which already handles both DM-RS
  types generically. The passive path reuses it as a library call rather than reimplementing
  chest/LLR extraction, so it inherits type-2 support automatically. `get_num_dmrs_re_per_rb()`-
  equivalent arithmetic at line ~772 (`(dmrs_config_type==0?6:4)*n_dmrs_cdm_groups`) is correct for
  both types.
- **Shared DM-RS generation core**: `dmrs_nr.c`'s `allowed_xlsch_re_in_dmrs_symbol()` and
  `nr_dmrs_rx.c`'s `nr_pusch_dmrs_rx()`/`nr_pdsch_dmrs_rx()` already implement TS 38.211's type-2
  RE mapping (2 adjacent REs per CDM group in a 6-RE block, `wf2`/`wt2` OCC tables) generically.
  Only `nr_pusch_lowpaprtype1_dmrs_rx()` (transform-precoded low-PAPR sequences) is type-1-only —
  correct, since that DM-RS generation mode is spec-defined for type 1 only.

## Fix 3 (completeness, not correctness): DL layout-sweep pruning signal

`nr_dci11_layout_offsets()`'s `ap_valid_rows` (a "free" plausibility-pruning check during the DCI
1_1 blind layout search) was only populated for the 4-bit (type-1) width; widths 5/6 got 0
(disabled). Extended to all four tables using the exact row counts (12/31/24/58) the real
extraction already relies on, so a misplaced-field candidate with an out-of-range type-2
antenna-ports code point is now pruned during search instead of only being caught once the search
happens to try `nr_pdcch_blind_extract_11()` itself. Speed-only; does not change which layout the
search eventually converges on.

## Tests (TDD, per brief)

`test_nr_pdcch_blind_monitor` (156/156 pass, was 153 + 2 skipped before my new tests existed;
2 skips are pre-existing/unrelated — `PdcchReplay.OtaCss0DecoderContract`,
`PdcchReplay.BudgetTimingEveryWidth`):
- `UlAntennaPortsType1DecodesViaReverseTable` — rewritten from the pre-existing
  `UlAntennaPortsCodePointOutsideItsTableIsRejected`, whose `ap<=3` boundary was the OLD closed
  form's own self-imposed scope, not a real spec bound (the reverse table has 14 valid rank-1 rows,
  not 4). Now cross-checks every `antenna_ports` code point 0..31 directly against
  `decode_dci_antenna_ports_val()` rather than a hardcoded boundary.
- `UlAntennaPortsType2DecodesViaReverseTable` — same cross-check with `dmrs_config_type=1`.
- `Dci11Type2AntennaPortsDecodeViaTables3And4` — exercises the previously-untested DL type-2
  extraction at both maxLength values (Table -3 row 6, Table -4 row 24, values read verbatim off
  the in-tree tables). Found a real TS 38.211 fact while writing it: Table 6.4.1.1.3-4 (maxLength 2
  DM-RS symbol mask) only defines additional positions 0/1 for mapping type A — positions 2/3 are
  `-1` (reserved) in every row, so the maxLength-2 sub-case needed `dmrs_add_pos=1`, not the
  pos2 default. Documented in the test, not worked around silently.
- `ApValidRowsCoversAllFourAntennaPortsTables` / `PlausibilityRejectsAnOutOfRangeType2AntennaPorts`
  (in `test_nr_pdcch_dci11_layout_sweep`, also passes) cover fix 3.

Build: `ninja`/`make -j4 test_nr_pdcch_blind_monitor test_nr_pdcch_dci11_layout_sweep` via
`lane-make.sh gap-dmrs2`, clean, no new warnings. `ctest -R 'test_nr_pdcch_(blind_monitor|
dci11_layout_sweep|ul_field_sweep|ul_interp_sweep)|test_nr_(dci11_pin|dl_adaptive)'` all pass.
Did **not** build the full `nr-uesoftmodem` executable separately — the same production `.c`
files are compiled and linked into the passing test binaries, which gives strong confidence, but a
full-executable link was not separately re-verified to save time under repeated capture-contention
on shared sens6 (see below).

## Commit 2: UL `dmrs_max_length = 2` (double-symbol DM-RS)

**Root cause was the same shape as commit 1: an upstream discovery-side pruning gate, not a
missing DSP capability.** `nr_pdcch_ul_discovery.c`'s `supported()` and the DCI 0_1 handler both
had `... && opts.dmrs_max_length<=1` alongside the (now-removed) `dmrs_config_type==0` — pruning
every maxLength-2 hypothesis before it could reach the extractor, with the same "receiver cannot
resolve this" framing.

Checked before touching anything: `blind_ul_finish()` already threads `opts->dmrs_max_length`
straight into `nr_pdcch_blind_ul_dmrs_mask()` (no hardcoding), which already carries BOTH TS
38.211 tables — `g_table_6_4_1_1_3_3` (maxLength 1) and `g_table_6_4_1_1_3_4` (maxLength 2). And
`decode_dci_antenna_ports_val()` (already wired in commit 1) needs no maxLength input at all —
`front_load` (1 or 2) is one of its OUTPUTS, read off the matched table row. So there was no DSP
gap to close, only the pruning gate — removed both instances (`supported()` and the DCI 0_1
handler), same as commit 1's `dmrs_config_type` fix.

**One real spec fact found while writing the test, not assumed**: `g_table_6_4_1_1_3_4` only
defines `dmrs-AdditionalPosition` 0 and 1 for mapping type A — columns 2 and 3 are `-1` (reserved)
in every row. So a maxLength-2 hypothesis paired with `add_pos ∈ {2,3}` legitimately can never
produce a valid mask; this needs no special-casing, since `nr_pdcch_blind_ul_dmrs_mask()` already
returns -1 for it and the caller already treats -1 as "reject this hypothesis, not evidence
against the whole search" — the same class of finding as commit 1's `Dci11Type2...` test hitting
the DL twin of this table.

**Files**: `nr_pdcch_ul_discovery.c` (2-line removal, both call sites, updated comments explaining
why maxLength-2/add_pos-2-or-3 doesn't need separate handling). New tests in
`nr_pdcch_blind_monitor_test.cc`:
- `UlDmrsMaskLookupSupportsMaxLength2` — direct `nr_pdcch_blind_ul_dmrs_mask()` calls: valid mask
  at add_pos∈{0,1}, `-1` at add_pos∈{2,3} (values read off the in-tree table, not derived).
- `UlAntennaPortsAcceptsMaxLength2AndDoublesTheDmrsSymbolCount` — end-to-end through
  `nr_pdcch_blind_extract_01()`: maxLength 2 / add_pos 1 decodes and produces a symbol mask with
  strictly MORE bits set than the maxLength-1 case (proves the double-symbol table genuinely
  engaged, not just "didn't crash"); maxLength 2 / add_pos 2 is rejected end-to-end with the
  expected reason string.

**Passive PUSCH decode path**: checked, needed NO change. `nr_pusch_passive_decode.c` derives its
DM-RS symbol count via `__builtin_popcount(g->ul_dmrs_symb_pos)` (line ~770/1028) — generic over
however many bits the mask has, never assumes "exactly 2". Channel estimation/LLR extraction go
through OAI's own shared `nr_rx_pusch_group_tp()`, which (per `nr_ul_channel_estimation.c`) already
handles maxLength 2 for a real attached UE. This is why commit 2 touches ONLY the discovery gate.

## Commit 3: UL DM-RS scrambling-ID 2-stage probe — DM-RS-type-2-aware

**This one WAS a real DSP gap**, not just a pruning gate: `nr_dmrs_id_estimate.c`'s
`nr_dmrs_id_accumulate()` hardcoded `NFAPI_NR_DMRS_TYPE1` into BOTH the reference-sequence
generation (`nr_pdsch_dmrs_rx(..., NFAPI_NR_DMRS_TYPE1, ...)`) AND the RE-stepping (`re = (re + 2)
% size`, comb-2 only). A type-2 cell's blind scrambling-ID search would have silently correlated
against the wrong REs — not a crash, a receiver that either never converges or (worse) converges
to a wrong answer by chance.

**Fixed by reuse of the spec formula, not reinvention**: added an `int dmrs_type` parameter to
`nr_dmrs_id_accumulate()` and `nr_dmrs_id_2stage_accumulate()` (threaded through both of the
2-stage wrapper's internal calls). The RE-stepping now computes each pilot index's absolute offset
via the TS 38.211 6.4.1.1.3-1/-2 formula (`get_dmrs_freq_idx_ul()` in `dmrs_nr.h`, CDM group 0 —
`delta=0` — matching the existing port-1000 assumption): type 1 = `4n+2k'` (comb-2, 6 REs/RB,
unchanged numerically from before), type 2 = `6n+k'` (2-adjacent-per-6, 4 REs/RB). Rather than
link `dmrs_nr.c` into every caller, the two-line formula is inlined at the one call site that
needs it (`nr_dmrs_id_accumulate`'s loop) with a comment naming the function it mirrors — same
practice the file already uses for `gold_for()`.

**`nr_dmrs_port_pair_coherence()` and `nr_dmrs_prb_coherence()` were deliberately NOT touched.**
Checked their actual callers first: `nr_pusch_passive_decode.c` (the file named in this task) does
not call either — only `nr_dmrs_id_2stage_accumulate()`. `nr_pdsch_passive_queue.c` (DL) uses
`nr_dmrs_port_pair_coherence()` for a SEPARATE feature (the rank/layer-count probe, gated
`pdu->dmrsConfigType == 0` at its own call site, left alone), and `nr_dmrs_prb_coherence()` is used
by passive wideband BWP discovery (`nr_passive_bwp.h`), an even more separate blind-carrier-scan
feature. Generalizing those would be scope creep beyond "the UL scrambling-ID probe."

**DL call site updated too, but this was mechanical, not optional**: `nr_pdsch_passive_queue.c`'s
own `nr_dmrs_id_2stage_accumulate()` call (line ~1082) HAD to change once the shared function's
signature changed (13 args now, not 12) — that's a compile error otherwise. Since I was forced to
touch that line's argument list anyway, I also lifted its `pdu->dmrsConfigType == 0` gate and
passed `pdu->dmrsConfigType` through: leaving DL silently gated after making the underlying
estimator type-2-capable would just move this exact gap to a different file with a straight-faced
excuse ("only the UL one was in scope") that wouldn't survive contact with the shared code. The DL
rank-probe gate at line ~1018 (a genuinely different feature, `nr_dmrs_port_pair_coherence`) was
left untouched, per above.

**Files**: `nr_dmrs_id_estimate.h`/`.c` (signature + RE-stepping), `nr_pusch_passive_decode.c` (UL
caller: gate removed — `transform_precoding` exclusion kept, see commit 3's own note on why —
`dmrs_type` threaded through), `nr_pdsch_passive_queue.c` (DL caller, mechanical + gate lift as
above). New tests in `nr_dmrs_id_estimate_test.cc`:
- `RecoversTheTrueIdentityWithType2Dmrs` — same structure as the existing type-1 large-margin test,
  but through the genuinely sparser type-2 RE pattern (4 REs/RB vs 6).
- `Type1AndType2ReadingsOfATtype2SignalDiffer` — cross-feed guard: a type-2 signal read with
  `dmrs_type` forced to type 1 must score far worse than reading it correctly, proving the two
  code paths are actually different formulas, not a relabelling that happens to still work.
- `DmrsId2Stage.WorksEndToEndWithType2Dmrs` — confirms `dmrs_type` reaches through the 2-stage
  wrapper's internal `nr_dmrs_id_accumulate()` calls, not just the direct-call path the tests above
  exercise.
- `synth()`, the test file's own signal generator, gained an optional `dmrs_type` parameter
  (C++ default arg = type 1, so every pre-existing call site is untouched) plus a small
  `dmrs_re_offset()` helper mirroring the same spec formula used in production, so the test can
  synthesize a genuinely different (not just relabelled) type-2 signal.

## Commit 4 (gap 3): transform precoding (DFT-s-OFDM PUSCH) — IMPLEMENTED

**Superseded note**: an earlier version of this report assessed this gap and deliberately did NOT
implement it (reasoning: discovery and decode gates cannot move independently — see below, still
correct — combined with "zero live/rfsim validation available in this pass"). The coordinator
confirmed live validation IS now available (OCUDU ZMQ bed on sensnuc3, stock
`--enable_transform_precoding` option) with the receiver's passive PUSCH decode already at ~97% on
QPSK there, so the 4-step plan below was implemented. **I did not run that validation myself** — no
radio process was started on sensnuc3, per instruction; the exact arm and expected log lines to
hand to whoever runs it are below.

**Why discovery and decode had to move together (unchanged reasoning, now acted on rather than
used as a reason to stop)**: the discovery engine's Technique-D-style convergence needs real CRC
feedback per hypothesis class to ever resolve. Lifting only the discovery-side gates while the
decode side still rejected every transform-precoding grant would have stranded that hypothesis
class permanently unconverged — worse than the prior clean exclusion, not better. All four pieces
below landed in one commit for exactly this reason.

**What was already there (confirmed before writing any code, not assumed)**:
- `nr_ul_channel_estimation.c:494` already branches on `transform_precoding` and calls
  `nr_pusch_lowpaprtype1_dmrs_rx()` for the low-PAPR DM-RS path.
- `nr_ulsch_demodulation.c:360` already applies `nr_idft()` (removing the DFT precoding) when
  `transform_precoding == transformPrecoder_enabled`.
- `decode_dci_antenna_ports_val()` already had a transform-precoding branch (`lut_tp_rev`, TS
  38.212 Table 7.3.1.1.2-6/-7) — unified with the type-1/type-2 branches into one call.
- `nr_mac_common.c` already had TS 38.214 Table 6.1.4.1-1/-2 as `Table_61411`/`Table_61412`
  (`table_idx` 3/4), used by `nr_get_Qm_ul()`/`nr_get_code_rate_ul()` — the gap was that nothing
  in the passive receiver's hypothesis space ever produced `table_idx` 3 or 4.
- `fill_pusch_pdu()` already correctly mapped `g->transform_precoding` into the FAPI PDU's ASN.1
  convention, and already populated `p->pusch_identity` with a correctly PCI-derived-or-overridden
  value (the existing scrambling-ID fallback logic) — this turned out to be exactly the input the
  low-PAPR DM-RS sequence needs (see step 3).

**The four steps, as implemented**:
1. **Antenna-ports table** (`blind_ul_finish()`, `nr_pdcch_blind_monitor.c`): the separate
   hand-rolled TP closed form (`cdm_groups=2; ports=1u<<antenna_ports`) is gone, replaced by the
   same `decode_dci_antenna_ports_val()` call used for type 1/2, now also passing
   `NR_PUSCH_Config__transformPrecoder_enabled` when `opts->transform_precoding`. Found while doing
   this: the old TP closed form had the SAME latent bug as the pre-fix type-1-only closed form —
   correct only for `antenna_ports` 0..3 (front_load 1); `lut_tp_rev`'s front_load-2 rows (4..11)
   wrap the port index instead of shifting it further, so this also fixed a real, previously-dead
   (because discovery excluded TP entirely) bug.
2. **MCS tables** (TS 38.214 6.1.4.1 Table 6.1.4.1-1/-2, `nr_pdcch_ul_interp_sweep.c`): fixed at
   the hypothesis GENERATOR, not with a downstream resolution layer — `opts.mcs_table`'s own header
   comment already documented "3..5 = TP variants" as the field's contract, so generating it
   correctly there means every consumer (the MCS validity check, `fill_pusch_pdu()`) needed no
   change. `tp=0` hypotheses get `mcs_table ∈ {0,1,2}` as before; `tp=1` hypotheses get `{3,4}`
   (qam256 does not combine with transform precoding per spec, one fewer valid choice). Raw
   catalogue size changes 960 → 800 (narrower, not looser — the 160 removed combinations all
   described a spec-illegal qam256+TP configuration).
3. **Low-PAPR DM-RS sequence** (TS 38.211 6.4.1.1.1.2, `fill_pusch_pdu()`): sets
   `p->dfts_ofdm.low_papr_group_number = p->pusch_identity % 30` and
   `low_papr_sequence_number = 0`. **No group/sequence hopping is implemented anywhere in this
   codebase** — checked `gNB_scheduler_ulsch.c` and `nr_ue_scheduler.c` (the gNB's and an attached
   UE's OWN reference computation of these exact two FAPI fields): both hardcode `f_gh=0` and `v=0`
   unconditionally. So matching that is parity with the rest of the codebase, not a shortcut
   relative to it, and no new hypothesis dimension was needed (`p->pusch_identity` was already
   correct). Confirmed against OCUDU's source (read-only, sensnuc3) that this matches the actual
   test bed too — see the arm section below.
4. **IDFT path**: no code change needed — already implemented, and now actually reachable once
   (1)-(3) and the gates stop preventing it from running with real, correctly-populated parameters.

**Gates lifted**: `nr_pdcch_ul_discovery.c`'s `supported()` and the DCI 0_1 handler no longer
exclude `transform_precoding=1`; `nr_pusch_passive_decode.c`'s explicit "DFT-s-OFDM (transform
precoding) not wired" reject is removed.

**Tests (offline only, TDD)**: `UlAntennaPortsTransformPrecodingDecodesViaReverseTable` and
`UlTransformPrecodingUsesTheTpMcsTables` (`nr_pdcch_blind_monitor_test.cc`, end-to-end through
`nr_pdcch_blind_extract_01()`, cross-checked against `decode_dci_antenna_ports_val()`/
`nr_get_code_rate_ul()` directly rather than hardcoded expected values) and
`UlInterpSweep.TransformPrecodingOnlyGeneratesItsOwnMcsTables` (checks EVERY generated hypothesis,
not a sample). Updated the pre-existing catalogue-size assertions (960→800). **163/163
`test_nr_pdcch_blind_monitor` pass** (161 + 2 pre-existing/unrelated skips), including 13/13
`UlFieldSweep`+`UlInterpSweep`. `nr-uesoftmodem` builds clean (one pre-existing, unrelated
`-Wunused-function` warning on `passive_ul_unav_res()`, confirmed present in an earlier
full-build log from before this commit — not touched by this change, not investigated further).

**Not implemented, deliberately**: group/sequence hopping for low-PAPR DM-RS (genuinely separate
gap, no reference implementation anywhere in this codebase to match — confirmed OCUDU's own arm
below doesn't enable it either, so it doesn't block this validation); the UL DM-RS scrambling-ID
2-stage probe (commit 3) stays excluded for transform precoding (it uses `nr_pdsch_dmrs_rx()`, not
the low-PAPR generator — unrelated to this gap).

### Exact OCUDU arm for live validation (do NOT run this yourself — hand it to the OCUDU-bed queue)

Read `/home/sens/NICOLA/repos/ocudu` on sensnuc3 (read-only) to confirm the CLI surface and how it
maps onto RRC, rather than guessing:

- **CLI**: `cell_cfg pusch --enable_transform_precoding 1` (a `pusch` subcommand nested under
  `cell_cfg`, `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp:1134`,
  bool, default `false`). Matches the harness doc's existing `GNB_EXTRA` pattern, e.g.:
  `GNB_EXTRA='cell_cfg pusch --enable_transform_precoding 1'`.
- **YAML equivalent**: `cell_cfg.pusch.enable_transform_precoding: true`
  (`du_high_config_yaml_writer.cpp:354`).
- **Leave `--mcs_table` at its default** (`qam64`) — this maps directly onto this fix's
  `mcs_table=3` case (TS 38.214 Table 6.1.4.1-1, the "no explicit RRC mcs-Table configured" case).
  `du_high_config_validator.cpp:402` confirms OCUDU treats qam256+TP as invalid (caps
  `max_ue_mcs=27` when either qam256 or TP is set), consistent with this fix not generating that
  hypothesis combination at all.
- **Confirmed (read-only, not guessed) that OCUDU does not expose group/sequence hopping or
  nPUSCH-Identity for PUSCH DM-RS at all** — grepped `asn1_rrc_config_helpers.cpp` for
  `groupHopping`/`sequenceHopping`/`nPUSCH-Identity` under the PUSCH path and found nothing (PUCCH
  and SRS have their own hopping config; PUSCH transform-precoding DM-RS does not), so both RRC
  IEs are absent and default to "neither hopping" / physCellId per spec — exactly what this
  fix assumes. If a future OCUDU version adds these knobs, do not enable them without also
  implementing real hopping support here first.
- **Constraint to watch**: `du_high_config_validator.cpp:426` requires `min_rb_size` to be a valid
  DFT-precoding size (2^a·3^b·5^c) when TP is enabled — OCUDU validates this itself at startup, but
  if the arm fails to start, check `min_rb_size`/`max_rb_size` first.

### Expected receiver log lines (what "it worked" looks like)

On the passive receiver side (`nr-uesoftmodem --passive-rx`, same binary/log conventions as the
rest of `ocudu-harness.md`'s M1-M3 runs):
1. **Discovery convergence** (`nr_pdcch_ul_discovery.c`, `LOG_A(PHY, ...)`):
   `"UL width and baseline CRC-validated: class=%d rnti=0x%x lower=%.3f ..."` followed eventually by
   `"UL interpretation search converged: class=%d tda=%d"` for the TP UE's RNTI. Before this fix,
   convergence for a TP-enabled UE was structurally impossible (the true hypothesis was never in
   the candidate pool), so this line simply never appeared for that RNTI.
2. **Per-grant confirmation** (run with `ISAC_PUSCH_DIAG=1`, per `ocudu-harness.md`'s own note on
   this env var): `"SENSING: PUSCHDIAG ... mcs=%u/tbl%u ..."` — `tbl` should read **3** (or 4 if
   `--mcs_table qam64lowse` was also set) for this UE's grants, never 0/1/2.
3. **Headline summary counter**: `"SENSING: pusch_passive[try=%lu crc_ok=%lu (%.1f%%) ...]"` — for
   this specific RNTI's grants, `try` goes from effectively 0 (pre-fix: the grant never reached
   decode because discovery never converged) to a nonzero count, with `crc_ok` percentage
   comparable to the ~97% QPSK baseline the coordinator cited (a large gap from that would indicate
   a step 1-3 bug, most likely the MCS-table or low-PAPR-sequence step, per this whole task's
   pattern of front-loaded-field/table-index bugs).
4. If it does NOT converge: check the OCUDU gNB's own PUSCH log for the RNTI's actual `tbl`/`mcs`
   fields and cross-reference against `Table_61411`/`Table_61412` in
   `openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c` by hand — the same reconciliation method that
   found commit 1's DCI field-width bug.

## Live OCUDU validation: blocked, and why (read before assigning this)

Per the brief, I read `/home/sens/NICOLA/repos/ocudu` on sensnuc3 (read-only, no radio processes
started there). Findings:

- OCUDU's internal data model **fully supports DM-RS type 2 already** — `is_dmrs_type2` exists on
  both `dmrs_downlink_config` and `dmrs_uplink_config`
  (`include/ocudu/ran/dmrs/dmrs_{downlink,uplink}_config.h`), and the scheduler/DCI-size code
  (`dci_builder.cpp`, `dmrs_helpers.h`, `ue_configuration.cpp`) already branches on it correctly
  when it's true.
- **There is currently no operator-facing config key (CLI or YAML) to set it to `true`** in this
  OCUDU checkout. `is_dmrs_type2{false}` is the field's only initializer anywhere in the tree —
  grepped every `.is_dmrs_type2 =` assignment site and found none; the CLI11 schema
  (`apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp`) exposes
  `--dmrs_additional_position` for both PDSCH and PUSCH but no `--dmrs_type`/`--pdsch_dmrs_type`/
  `--pusch_dmrs_type` option, and the underlying `du_high_config.h` params structs have no such
  field either. So **DM-RS type 2 cannot currently be turned on from this OCUDU build's config
  surface at all**, regardless of what the receiver now supports.
- Given "read-only" and "this is an OCUDU-side lane's concern, not mine", I did not add a CLI
  option there. **Update: the coordinator confirms a separate `ocudu-knobs` lane is adding this
  exact config surface (`ISAC_OCUDU_TEST_DMRS_TYPE2`)** — do not duplicate that work here; once it
  lands, live validation of everything in this report (commits 1-3) can proceed.
- **Expected log line once it IS enabled**: `ue_cell_grid_allocator.cpp` already logs
  `is_dmrs_type2={}` (as `"yes"`/`"no"`) as part of its PUSCH-allocation-failure warning line — but
  only on an allocation *failure*, not on every grant, so it's a weak positive-confirmation signal.
  I did not find a per-grant/always-on log line naming DM-RS type in this OCUDU tree; the more
  reliable confirmation would be reading the RRC `PUSCH-Config`/`PDSCH-Config` IEs the CU sends
  (`asn1_rrc_config_helpers.cpp` lines ~1014/~1800 build the ASN.1 `dmrs-Type` IE from
  `is_dmrs_type2` directly) via `cuLogs/` once a real dedicated config is pushed, or, receiver-side,
  the fact that a UE only converges to a 5- or 6-bit antenna-ports layout at all (this receiver logs
  its own `dmrs_config_type`/`ap_table` choice once Technique D converges).

## Environment hazard found and worked around (read before trusting a full-suite green run here)

`nr_pdcch_blind_monitor.c` in this worktree **already had unrelated, uncommitted changes** sitting
in it before I made my first edit — retransmission-aware MCS handling (referencing a
`nr_harq_init_tx.h` that does not exist anywhere in this tree, and gating DCI-1_0/1_1/UL-MCS
reserved-range rejection on RNTI class / HARQ state). I did not write these and never intended to
touch MCS-reserved-range logic at all.

This cost real debugging time: after my fix, `test_nr_pdcch_blind_monitor` showed 3 failures
(`Dci10RejectsTheReservedMcsRange`, `Dci01RejectsTheReservedUlMcsRange`,
`UlMcsBoundaryUsesActualSelectedTable`) that looked at first like regressions from my change. They
were not — `git stash`-based bisection (reverting to the committed `c295fa18f1` baseline) showed
0 failures there, and a targeted `git diff` inspection found the actual cause: three hunks
completely unrelated to antenna-ports/DM-RS-type, already present in the file when I first pulled
it, that had been silently carried through every edit/round-trip since I never intended to touch
that code. I hand-reverted exactly those three hunks (verified via `git diff` that the remaining
diff contains only my DM-RS-type-2 work), rebuilt, and confirmed 156/156 pass.

**Consequence**: doing this necessarily discarded that pre-existing uncommitted content from the
working tree (it was interleaved with, not separable from, a file I had to overwrite via `scp`
round-trips to edit). It is not lost forever: it exists in the git object database as part of my
own intermediate `git stash` commits, reachable (until GC) at
`f3767c9c0eab35c32ba68852235d0a8bbf141060` (the more complete of the two — `git show
f3767c9c0eab35c32ba68852235d0a8bbf141060:openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c`
recovers the combined file, from which the harq-only hunks can be diffed back out against
`8940602423` or `c295fa18f1`). Whoever owns that HARQ-retransmission work (possibly `gap-harq`,
which I observed building `test_nr_harq_init_tx` concurrently on sens6 during this session — worth
checking whether its agent's edits landed in the wrong worktree) should recover it from there
rather than re-deriving it. I did not attempt to identify or contact that lane; flagging here per
the report instructions.

## Open items / not done (in scope order)

- **Live validation of commits 1-4 is the only thing not done.** I did not start any radio process
  on sensnuc3 (queue owned by another agent) or on sens6 beyond the offline gtest binaries.
  - Commits 1-3 (DM-RS type 2, maxLength 2, scrambling-ID probe): blocked on the OCUDU config
    surface for DM-RS type 2, being closed by the separate `ocudu-knobs` lane
    (`ISAC_OCUDU_TEST_DMRS_TYPE2`). Once that lands: confirm the receiver's `occ[...]`/
    `dmrs_config_type`/CRC-pass-rate log lines show a converged non-zero-type decode, and
    separately confirm a maxLength-2 capture shows more DM-RS symbols/CPI than a maxLength-1 one.
  - Commit 4 (transform precoding): the OCUDU config surface already exists
    (`--enable_transform_precoding`, a stock option, no new knob needed) — see its section above
    for the exact arm and expected log lines. This is ready to run whenever the OCUDU-bed queue
    picks it up.
- `nr_dmrs_port_pair_coherence()` (DL rank/layer probe) and `nr_dmrs_prb_coherence()` (wideband BWP
  discovery) remain type-1-only — separate features from the scrambling-ID probe commit 3 fixed,
  correctly out of scope for this task's brief.
- Group/sequence hopping for low-PAPR DM-RS (commit 4's low-PAPR sequence step only implements the
  "neither hopping" case, matching this codebase's own reference gNB/UE scheduler support level —
  a genuinely separate, larger gap if OCUDU or any other test bed ever configures hopping).
