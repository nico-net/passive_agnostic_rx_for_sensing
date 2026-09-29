# Task 16 report: blind CSI-RS rows 6-18 (8-32 ports)

**Status:** DONE_WITH_CONCERNS
**Commit:** `e3b3bf13e1` on `sdd/agn-csi` (lane csi, `/home/sens/NICOLA/agn-wt/csi`). Not pushed.
**Tests:** `test_nr_csirs_blind_search` 33/33 pass (7 new footprint tests, 1 extended row-ports test). `test_nr_csirs_blind_synth` passes. `nr-uesoftmodem` builds clean with no new warnings. Nothing was run live (no rfsim or OTA).

## What changed

| File | Change |
|---|---|
| `nr_csirs_blind_search.{h,c}` | `nr_csirs_blind_row_ports()` and `nr_csirs_blind_row_needs_bits()` now read their own 18-entry tables (`kRowPorts`, `kRowNeedBits`). `kRows` stays as the round-robin set. The unused `kPorts` and `kNeedBits` are deleted. New pure functions: `nr_csirs_blind_footprint`, `nr_csirs_blind_symbol_on`, `nr_csirs_blind_fp_record`, `nr_csirs_blind_fp_match`, `nr_csirs_blind_append`, plus the `nr_csirs_blind_fp_t` accumulator. |
| `nr_csirs_blind_rt.c` | Per call: builds the 12-subcarrier profile of the symbol that was already FFT'd and records its hits. At most once per 1000 calls, and only when new evidence has arrived: runs the match, then appends each fit to `g_st` and `g_zp` so the two stay mirrored. The first new fit is pinned for `NR_CSIRS_BLIND_PIN_CONFIRM_CALLS` if nothing else holds the pin. Wide rows are now allowed through the port check. |
| `tests/nr_csirs_blind_search_test.cc` | `RowPortsMatchTheSpecTableAllRows` (row 6 becomes 8, as the brief says), `EveryRowsFootprintHasPortsRes`, and footprint, profile, matching and append tests. |
| `CMakeLists.txt` | **Not in the brief's file list, but required.** The gtest now calls `get_csi_mapping_parms()`, so it links the same OAI sources and libraries as `test_nr_csirs_blind_synth`. |

`get_csi_mapping_parms()` is the only source of RE positions. The footprint walks its output exactly as `csi_rs_resource_mapping()` does. CDM type is derived from it as `ports / size` (1/2/4/8 map to noCDM / fd-CDM2 / cdm4 / cdm8), and whether a row uses `l1` is asked of the table by comparing footprints at `l1 = 5` and `l1 = 6`. Nothing about RE positions is transcribed. Two small tables are hand-coded because the mapping function cannot supply them:
- **Port count:** it sizes the buffers the generator writes into.
- **Need-bits:** the generator's bitmap walk has no bound and spins forever with fewer bits, so this table is what makes calling it safe.

One spec fact is hard-coded: density 0.5 exists only for rows 11-18 (16 or more ports).

## Design, and the one place I departed from the brief

**The accumulated EPR mean cannot see wide rows.** This is the evidence behind the design:
- EPR compares power on a candidate's REs with the other REs of the same RBs and symbol, averaged over visits. For a 32-port row, 8 of the 12 subcarriers in the symbol are the resource itself. So one resource RE against the other 11 reads at most 11/7 = 1.57, even in a perfectly quiet slot.
- Only 1/period of visits land on a slot that carries the resource. The mean therefore reads about 1 + 0.57/P, which is about 1.01 at P = 40.
- For row 9 (all 12 subcarriers) the contrast is exactly 1.

So the per-candidate `g_epr_sum/g_epr_n` accumulator is structurally blind here. That is not the same as BLOCKED: the same measurement taken inside one slot works.

**What I built instead.** I evaluate the EPR statistic per slot, at subcarrier granularity, on the symbol the tap already FFTs:
1. Sum `|y|²` per subcarrier-in-RB over the carrier, read with the same `(i + first_carrier_offset) % N` mapping as every comparator, and split by RB parity so density 0.5 can be detected.
2. In a slot where the resource is sent into an otherwise quiet symbol, the 12 values split cleanly into on and off. The split is the largest ratio gap among the sorted values, with a threshold of 2.0 (`NR_CSIRS_BLIND_FP_GAP`). Noise or flat PDSCH gives neighbouring sorted means a few tens of percent apart.
3. Hits are recorded per (density, symbol, subcarrier) cell in a ring of the last 8 slots.
4. A cell whose hits pass `nr_csirs_blind_infer_period` counts as periodic energy. Cells whose hits are jointly periodic form one measured footprint.

This stays inside the RT structure:
- Per call: one extra pass over N_RB × 12 REs, a few microseconds.
- The match costs about 60 µs (measured in the test at 273 PRB) and is rate-limited.
- No allocation.

**Matching rule: "subset and maximal" instead of the brief's literal "equals".** A candidate is kept if its footprint lies inside a group and is not strictly inside another fit's footprint. When a group holds one resource this reduces to equality. The reason for the change: the only real cell this project has measured (Swisscom) carries a TRS at `40:31` and a CQI resource at `160:31`. They share an offset, so their cells group together, and strict equality would find nothing. Maximality still drops sub-footprints: the test asserts that rows 13, 11 and 6 inside a row-16 pattern are rejected.

**Feeding the confirm path: append, not a parallel path.** This was the least-code option. Appended candidates go through the unchanged round-robin, `feed()`, periodicity check, ZP search, IDSWEEP and rate-matching export.

**Other decisions:**
- `l1` is restricted to after `l0`. The swapped order is the same RE set with the CDM groups relabelled, which the tests exposed as duplicate hypotheses.
- Ports 3 and up of wide rows write into a shared sink plane. Only plane 0 is ever scored, so a 32-port reference costs no extra 7 MB of thread-local buffers and no 7 MB memset per slot. Rows 1-5 get exactly the same buffers and pointers as before.

**Rows 1-5 are unchanged.** Rows 1-5 footprints contain no pair structure large enough for a row 6-18 fit. The test with a TRS pair plus a row-5 resource returns 0 fits, so nothing is appended and the search runs exactly as before. The same test shows periodicity is what rejects look-alike energy: DM-RS-like pairs on symbols 11-12 at traffic-driven slots are rejected, and the same energy made periodic is matched as row 7.

## Concerns

1. **Rows with the same footprint cannot be told apart by the existing oracle.** Examples: 16/17, 7/8, 11/12, and 16/17/18 when `l1 = l0 + 2`. The confirm path correlates only port 0 on symbol `l0`, and port 0's REs and sequence are identical across these rows. Whichever candidate confirms first wins. The RE set is always right, so rate matching is correct, but the reported row and CDM type may be wrong, and that matters for per-port channel estimation. Telling them apart needs a correlation across the CDM codes on `l0+1..l0+3`, which is not built.
2. **Row 9 (12 ports) is never matched.** It fills the whole symbol, so there is no contrast within the symbol.
3. **Evidence only comes from quiet symbols.** A CSI-RS symbol that also carries PDSCH shows no contrast. A heavily loaded cell may never yield a footprint. The EPR/TRS path has the same limitation.
4. **Poisoned cells.** If aperiodic structured energy (for example PDSCH DM-RS in other slots) lands on the same symbol index as the CSI-RS, affected cells fail the periodicity check and the footprint comes out incomplete. The last-8 ring recovers after 8 clean hits, but not while the collisions keep happening.
5. **Wide-row candidates will probably never be chosen by IDSWEEP.** IDSWEEP picks a candidate whose mean EPR is above 2, and a wide-row candidate's EPR is capped near 1.57 for the reason above. So a wide resource whose scramblingID is not the PCI would not get swept. Direct confirmation (scramblingID = PCI) does work through append plus pin.
6. **The search still stops at the first confirmation** (existing behaviour). If a rows 1-5 resource confirms first, rows 6-18 are never searched unless RANK mode is on.
7. **RT cost of wide references is not measured on target.** `nr_generate_csi_rs` for row 18 at 273 PRB writes about 70k REs per visit, compared with a few thousand for rows 1-5. While a wide candidate is pinned, that cost is paid on every call.
8. **Nothing has run live.** The 2.0 gap threshold is justified from noise statistics, not calibrated on a real cell. The first check on rfsim or OTA is to look for `CSIRS_BLIND FOOTPRINT` lines.

## Verification commands
```
ssh sens6 "/home/sens/NICOLA/agn-wt/lane-make.sh csi test_nr_csirs_blind_search test_nr_csirs_blind_synth nr-uesoftmodem"
ssh sens6 "cd /home/sens/NICOLA/agn-wt/csi/cmake_targets/ran_build/build && ctest -R test_nr_csirs_blind --output-on-failure"
```

---

## Fix round 1 (commit `1c8ca6b7dc`, on top of `e3b3bf13e1`, not amended)

Rebuilt `test_nr_csirs_blind_search`, `test_nr_csirs_blind_synth` and `nr-uesoftmodem` with the lane script; all built clean with no new warnings. Results: `test_nr_csirs_blind_search` passes 34/34; `nr_csirs_blind_synth_check: PASS`.

### 1. CRITICAL: the 64-fit cap dropped wide rows before the maximality filter ran
- **Change:** `match_group` now enumerates rows 18 down to 6. A fit is rejected on insertion if its footprint lies strictly inside a fit already kept. Fits with an equal footprint are still kept, since only the sequence stage can separate them. The old post-filter is gone. Port count only falls as the row index falls, so a later fit can never strictly contain an earlier one. Every kept fit is therefore maximal, and hitting the cap (`min(max, 64)`) can only drop further maximal fits.
- **Where:** `nr_csirs_blind_search.c:770-778` (ordering and cap), `:832` (rejection on insertion).
- **Test evidence:** new test `RowEighteenDensityOneIsNotCrowdedOutByItsSubFootprints` (`nr_csirs_blind_search_test.cc:360`). It uses a row-18 density-1 truth (fd 0x0F, l0=6, P=40, `run_visits` model) and checks for exactly 3 fits, each with a 32-RE footprint: row 18 (6,0), row 17 (6,8) and row 16 (6,8), which share one RE set.
  - **Before the fix it failed with n=12,** fits of 16 REs and none of rows 16-18. That reproduces the reviewer's measurement.
  - **After the fix it passes.** The earlier row-16 and density-0.5 tests are unchanged and still pass.

### 2. IMPORTANT: discovery order changed without an opt-in knob
- **Change:** new knob `ISAC_CSIRS_BLIND_WIDE`, read once via `getenv`, default off, following the pattern of the other ISAC knobs. It gates both:
  - the per-call `nr_csirs_blind_symbol_on` + `nr_csirs_blind_fp_record` pass;
  - the match/append/pin block.
- **Where:** `nr_csirs_blind_rt.c:53` (state), `:293-297` (read-once and per-call gate), `:530` (match gate).
- **Why rows 1-5 are exactly as before Task 16 with the knob unset:**
  - No footprint code runs, so nothing is appended or pinned. `g_st.n`, the round-robin cursor and the pin state evolve exactly as before.
  - The other Task 16 changes that remain active are behaviour-neutral for rows 1-5:
    - `nr_csirs_blind_row_ports` and `nr_csirs_blind_row_needs_bits` return the same values for rows 1-5 (ports 1/1/2/4/4, one needed bit each). `enumerate()` therefore produces the identical 683-candidate list.
    - `n_planes = min(n_ports, 4)` equals `n_ports` for rows 1-5, so the same planes are memset.
    - The local `refp[p]` equals `t_refbuf[p]` for every p below 4, the only ports rows 1-5 write.
    - The widened port check (up to 32) is only reached by wide rows, and without the knob no wide candidate ever exists.
- **The union mislabel stays a known limitation when the knob is ON.** Two narrow resources sharing a period and offset (e.g. two row-4 resources at k0=0 and k0=4) fit a row-6 footprint. This is documented in the knob comment at `nr_csirs_blind_rt.c:46-51`. It is not fixed: telling a union apart from a real wide row needs a sequence-stage discriminator.

### 3. Minor fixes
- **g_zp index check:** the `g_zp` append result is compared with the `g_st` index. On divergence it logs `LOG_E(PHY, …)` once (static latch). Where: `nr_csirs_blind_rt.c:538-546`. It is not exercised by a unit test, because `rt_slot` needs PHY state; divergence is impossible while both populations come from the same `init` and the same appends.
- **Test printf:** the unconditional timing `printf` and the `<chrono>` include are removed from the test file. The earlier measurement was 61 µs per match at 273 PRB.
