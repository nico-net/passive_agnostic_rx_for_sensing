# fix3 report — test-target link failures (lane fix2, branch sdd/agn-fix2)

## Summary
Fixed 2 real CMake source-list gaps; the other 2 named targets were already fine and just
never got reached. Found and reported (not fixed, out of scope) one pre-existing failure in
`test_nr_ue_ra_procedures` and confirmed `test_vrtsim_cirdb` is pre-existing/unrelated.

## What was actually broken

### 1. `test_nr_pdcch_coreset_map` — real gap, PRE-EXISTING (not this plan's tasks)
`nr_pdcch_coreset_map.c`'s idsweep path (`nr_pdcch_coreset_map_idsweep_want`/`_push`, lines
~415-437) calls `nr_passive_acq_snapshot()` / `nr_passive_acq_tdd_slot_has_downlink()`
(`nr_passive_acq_state.c`). The test target compiles `nr_pdcch_coreset_map.c` directly rather
than linking the `nr_pdcch_blind_monitor` library, so those symbols were undefined.
- Evidence: `git show 222f98d072:openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c` already
  contains 3 calls to `nr_passive_acq_snapshot`, and `git show 222f98d072:CMakeLists.txt`'s
  `test_nr_pdcch_coreset_map` target already lacked `nr_passive_acq_state.c`. The gap predates
  commit 222f98d072 (start of this plan's tasks) — **pre-existing (b)**.
- Fix: added `nr_passive_acq_state.c` + `nr_tdd_pattern.c` (its own dependency, same pattern
  already used by `test_nr_passive_acq_state`) via `target_sources`.

### 2. `test_nr_pdcch_dci_length_sweep` — real gap, CAUSED BY THIS PLAN (a)
`nr_pdsch_config_sweep.c`'s `qm_table_mask()` (line ~179) calls `nr_pdsch_qm_of_mcs()`
(`nr_pdsch_qm_oracle.c`). This target compiles `nr_pdsch_config_sweep.c` directly.
- Evidence: at 222f98d072, `nr_pdsch_config_sweep.c` had **zero** calls to
  `nr_pdsch_qm_of_mcs`; commit `2b2b0c2fbd` ("Technique D: modulation-order oracle...",
  descendant of 222f98d072, part of Task 6 Qm oracle) added the call. The same commit's merge
  (`4656c952a9`) updated the **sibling** `test_nr_pdsch_config_sweep` target (which compiles the
  same `nr_pdsch_config_sweep.c`) to add `nr_pdsch_qm_oracle.c`, but missed this target —
  **caused by this plan's tasks (a)**, a missed-sibling-target oversight.
- Fix: added `nr_pdsch_qm_oracle.c` via `target_sources`.

### 3. `test_nr_pdcch_blind_monitor` / `test_nr_pdcch_ul_interp_sweep` — NOT actually broken
Rebuilding this target alone succeeded immediately with no source changes: its library
(`nr_pdcch_blind_monitor`, `CMakeLists.txt:1464`) already contains both
`nr_passive_acq_state.c` and `nr_pdsch_qm_oracle.c`, so all symbols resolve. Their "Not Run"
status in the original report was `make tests` (no `-k`) aborting on gaps #1/#2 above before
ever reaching these targets in the build graph — not a link failure of their own.
`test_nr_pdcch_ul_interp_sweep` is only an `add_custom_target` alias `DEPENDS
test_nr_pdcch_blind_monitor`, so the same explanation covers it.

### 4. `test_nr_ue_ra_procedures` — NEW failure surfaced once #1/#2 were fixed, OUT OF SCOPE, PRE-EXISTING
Once `make -k tests` got past the fixed targets it hit a 5th, previously-unreported link
failure: `libMAC_UE_NR.a(config_ue.c.o)` has undefined references to
`nr_pdcch_blind_monitor_set_tda_common`, `nr_pdcch_sib1_prior_set`, and
`nr_passive_acq_note_sib1_tdd` (all three defined in the `nr_pdcch_blind_monitor` library's
sources). `openair2/LAYER2/NR_MAC_UE/tests/CMakeLists.txt`'s `test_nr_ue_ra_procedures` target
links `MAC_UE_NR` but never the `nr_pdcch_blind_monitor` library.
- Evidence: all three calls already exist in `config_ue.c` at 222f98d072 (count=1 each before
  any of this plan's tasks), and `git log --oneline -- openair2/LAYER2/NR_MAC_UE/tests/CMakeLists.txt`
  shows the file was last touched by an unrelated stub-related commit, never to add this link.
  **Pre-existing (b)**, and outside the brief's named 4 targets / this lane's file territory
  (`openair2/LAYER2/NR_MAC_UE` is MAC/RRC production code, not the NR_UE_TRANSPORT sensing
  files this task scoped). **Not fixed**, per the brief's instruction not to expand into
  unscoped PHY/RT-adjacent chains — reported only. The likely fix (add `nr_pdcch_blind_monitor`
  to its `target_link_libraries`, same class as #1/#2) is a one-line change but belongs to
  whoever owns that test/lane.

### `test_vrtsim_cirdb` — FAILS at runtime, PRE-EXISTING/UNRELATED, not fixed
Ran with `--output-on-failure`. First sub-test (`CIRDBDelayDL/0`) passes; the second
(`CIRDBDelayDL/1`) fails: the client process hits
`Assertion (fd != -1) failed! ... shm_open() failed: errno 2, No such file or directory` in
`common/utils/shm_iq_channel/shm_td_iq_channel.c:116`, then the server times out waiting.
This looks like an inter-process shared-memory setup/teardown race between the two
sub-processes the test spawns, not a code-path this plan touched.
- Evidence: `git log 222f98d072..HEAD` returns **zero** commits for
  `radio/vrtsim/tests/test_vrtsim_cirdb.cpp`, the `radio/vrtsim/` tree, or
  `common/utils/shm_iq_channel/` — nothing since this plan's tasks began touched any of its
  code paths. **Pre-existing/unrelated — not fixed**, per the brief.

## Commit
`3eb1b070c3` on `sdd/agn-fix2` — "CMake: link the sources test targets now need (qm oracle,
passive acq state)". 1 file changed (`CMakeLists.txt`), 12 insertions, 0 deletions. No
production code touched, no test assertions relaxed, no stubs added.

## Verification
- Built each of the 4 named targets individually (`lane-make.sh fix2 <target>`) — all link
  and pass.
- Built the full `tests` meta-target with `make -k` (to get past the unrelated, out-of-scope
  #4 failure) — everything else built.
- Full `ctest` from `$B`:

```
100% tests passed for the 6 PDCCH-family tests targeted by this task:
  test_nr_dl_adaptive .............. Passed
  test_nr_pdcch_ul_field_sweep ...... Passed
  test_nr_pdcch_ul_interp_sweep ..... Passed
  test_nr_pdcch_blind_monitor ....... Passed
  test_nr_pdcch_coreset_map ......... Passed
  test_nr_pdcch_dci_length_sweep .... Passed

Full suite: 98% tests passed, 2 tests failed out of 112
  100 - test_nr_ue_ra_procedures (Not Run)   <- pre-existing, out of scope, unfixed
  109 - test_vrtsim_cirdb (Failed)           <- pre-existing, unrelated, unfixed
Total Test time (real) = 144.19 sec
```

All other 110 tests passed.
