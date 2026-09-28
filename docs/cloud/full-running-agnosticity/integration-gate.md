# Integration gate: sdd/gap-dmrs2 → sdd/integration

## Merge

- **Merge SHA: `66176b7250`** on branch `sdd/integration`, worktree `/home/sens/NICOLA/agn-wt/integ`.
- Merged `sdd/gap-dmrs2 @ ccc60caa6b` into `sdd/integration` (previous tip `d15ecd8a67`,
  itself `Merge sdd/gap-harq into sdd/integration`).
- One conflict, pre-resolved by the coordinator before I started:
  `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc`.
- Verification performed:
  - `git diff d15ecd8a67 -- <file>` (resolution vs "ours"/harq-integrated tip): showed only
    ADDITIONS — dmrs2's new tests `Dci11Type2AntennaPortsDecodeViaTables3And4`,
    `UlDmrsMaskLookupSupportsMaxLength2`, `UlAntennaPortsAcceptsMaxLength2AndDoublesTheDmrsSymbolCount`,
    plus dmrs2's rework of `UlAntennaPortsCodePointOutsideItsTableIsRejected` →
    `UlAntennaPortsType1DecodesViaReverseTable` + new `UlAntennaPortsType2DecodesViaReverseTable`.
  - `git diff ccc60caa6b -- <file>` (resolution vs dmrs2 tip): confirmed harq's renames/adds are
    present — `Dci10CrntiAcceptsTheReservedMcsRangeAsAPossibleRetransmission`,
    `Dci01AcceptsTheReservedUlMcsRangeAsAPossibleRetransmission`,
    `UlMcsExtractionNoLongerGatesOnTheReservedRange`,
    `Dci00ScramblingIdsAreAlwaysThePciNeverTheDedicatedEstimate`, the "Gap item 1"
    FDRA/dynamicSwitch test block, `Dci10RaRntiRejectsTheReservedMcsRange`, and the
    background-discovery-pass-yield test.
  - No leftover `<<<<<<<`/`=======`/`>>>>>>>` markers.
  - No duplicate `TEST`/`TEST_F` names (the one name collision, `MatchesTheSpecFormula`, is two
    different pre-existing test suites, `Dci10Size` and `Dci00Size` — not a real duplicate).
- Diff stat vs both parents: 33 files changed, 1942 insertions(+), 511 deletions(-).
- Committed with `git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc && git commit`,
  message `Merge sdd/gap-dmrs2 @ ccc60caa6b into sdd/integration` + the context.md attribution trailer.

## Build dir

- No `integ` build dir existed. Replicated `/home/sens/NICOLA/agn-wt/val/cmake_targets/ran_build/build`'s
  CMake cache by extracting all non-INTERNAL `BOOL`/`STRING`/`PATH`/`FILEPATH` entries (293) into an
  init-cache script (`/tmp/integ_initcache.cmake` on sens6), excluding `FETCHCONTENT_BASE_DIR` and any
  value referencing the `agn-wt/val` path (left to regenerate fresh for the new build dir).
  Deliberately did NOT `cp -a` the val build dir, per the lane doc's binutils/nvcc stale-`.cu.o` warning.
- Configured fresh: `cmake -C /tmp/integ_initcache.cmake -G 'Unix Makefiles' /home/sens/NICOLA/agn-wt/integ`
  inside a new `/home/sens/NICOLA/agn-wt/integ/cmake_targets/ran_build/build`. Configure succeeded
  (only a benign CMP0167/Boost dev warning).
- Verified option parity: diffing val's vs integ's cache (BOOL/STRING keys, excluding path/dir noise)
  showed only 3 benign additions (compiler paths cmake records under a different key on a fresh
  configure) — zero option divergence. Same generator (Unix Makefiles), same key options
  (`ENABLE_TESTS=ON`, `ENABLE_ISAC_SENSING=ON`, `ENABLE_LDPC_CUDA=ON`, `OAI_USRP=ON`,
  `NB_ANTENNAS_RX/TX=4`, `T_TRACER=ON`, `CMAKE_BUILD_TYPE=RelWithDebInfo`, AVX2/GFNI, etc.).
- `ctest -N` lists 114 registered tests (saved to `/tmp/ctest_names.txt` on sens6).
- Before every build/configure step, confirmed `pgrep -x nr-uesoftmodem` was empty (no capture
  running). Another lane (`gap-ocudu-dl`) had its own build running concurrently in ITS OWN build
  dir — that's not a capture and not this lane's build dir, so it did not block per the lane rules.

## Build

- Launched in background on sens6: `lane-make.sh integ nr-uesoftmodem nr-softmodem rfsimulator
  oai_usrpdevif params_libconfig tests` (the `tests` meta-target builds every ctest executable).
  Log: `/tmp/integ_build.log`.

*(This section will be filled in with PASS/FAIL + timing once the build completes.)*

## ctest (full run)

*(pending)*

## 9 named suites × 3 shuffle seeds

*(pending)*

## Triage / regressions

*(pending)*
