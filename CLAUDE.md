# CLAUDE.md — passive agnostic 5G NR receiver (OAI UE based)

Read `PROJECT_MEMORY.md` completely before doing anything; it is the single source of truth.

Hard rules:
- sens6 is FROZEN: never edit tests/passive_rx/captures/*, tests/passive_rx/*.conf, tests/passive_rx/sens6_host_snapshot_2026-09-30/*.
  Check: git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30
- gNB logs/configs are validation ground truth only, never receiver input.
- Evidence labels (PROJECT_MEMORY §0.1) on every claim; never merge results across hosts/cells/bandwidths.
- Never build while nr-uesoftmodem is running (pgrep -x nr-uesoftmodem).
- Stop a receiver with SIGINT, never SIGKILL (X410 claim hygiene).
- ISAC_RX_BRANCH_FO must stay unset (never enable at rank > 1).
- git add <explicit paths>; never -A, never stash.

Build (DGX, aarch64): cmake_targets/ran_build/build, Ninja:
  ninja nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests && ctest -j4
Build (cloud x86): same without oai_usrpdevif, with -DOAI_USRP=OFF: ninja nr-uesoftmodem rfsimulator params_libconfig nr-softmodem tests && ctest -j4
Evidence labels for non-DGX hosts must name the host (e.g. cloud x86).
Known ARM ctest failures: dft_test, test_nr_modulation, test_nr_pusch_ra0_qam256, test_nr_pusch_ra0_qam64 (intermittent).
Regression: tests/passive_rx/dgx/rfsim_regress.sh (106 PRB baseline: CONVERGED, CRC >= 98 %, drop_full <= 1 %).
Plan in progress: docs/superpowers/plans/2026-10-01-dgx-next-steps.md
