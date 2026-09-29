# Round 3 OCUDU bed lane

## Current bounded status — 2026-09-27

Inventory and dry-run planning only. Receiver tree is local
`/home/sens/NICOLA/rfsim-integ3`, detached at
`239eb144acd398968d5869a75e083e540ddefa26`; initially clean. No receiver source changes, lock acquisition,
build, radio run, capture, or commit.

### Fixture / route inventory

- The clean tree contains OCUDU container/deployment YAMLs, but no tracked
  `tests/passive_rx/ocudu/` directory, OCUDU passive harness, or OCUDU-specific passive config.
- `cmake_targets/ran_build/build` is absent. The tree therefore has no configured receiver build directory
  or candidate binary to pin. Do not infer build success from another tree.
- OCUDU knob/harness specifications were read from local docs:
  `ocudu-test-knobs.md` SHA256 `6d47342bc3eff381aecb540be89d2fe75e5eb30171026840352749e4fd16fa94`,
  `ocudu-harness.md` SHA256 `d1eaa79756111ee17abf09511204d745bf1a0dcd7818cf9f1703c0e7a4dde3a7`,
  Round-3 lanes SHA256 `286f346bd3b10f08a2937b5bdf0dfc88a3e2c4ac366efa6715ddacfd1248bf66`.
- Routing risks that must remain fail-closed: the documented srsUE PUSCH transmitter does not implement
  NR transform precoding (progress.md records `repos/srs-ue/lib/src/phy/phch/pusch_nr.c:758-762`); the
  `test_ue --nof_ues` executable/config/provenance route is not present in the clean receiver tree; and
  qam256 needs `SRSUE_ADVERTISE_256QAM=1` to cross the harness's sudo boundary, which the harness notes
  still needs wiring. These are route blockers, not receiver failures.
- Never use the dirty `/home/sens/NICOLA/rfsim-integ` or alter gNB/srsUE trees for this lane.

### Matrix plan and offline checks

Added `tests/passive_rx/ocudu_bed_matrix.py` plus
`tests/passive_rx/test_ocudu_bed_matrix.py` in this tree only. The declarative plan is 18 feature arms,
each with three exact 300-second paired alternations `baseline, feature` (108 runs, 9 hours of dwell,
excluding startup/cleanup). It separates DL/UL RA-0 and dynamicSwitch, VRB IL n2/n4, DM-RS type 2,
transform precoding, DL/UL scrambling ID0 and ID1 with matching NSCID, DL/UL type-B TDA, qam256, AL16,
forced HARQ retransmission, and multi-UE. The plan explicitly marks transform precoding and multi-UE
unroutable; a matrix launch must refuse until those routes and feature-specific validation evidence are
reviewed. Each live arm must still have gNB logs/pcaps for ground-truth validation only, a named receiver
feature line, C-RNTI CRC > 0 (PUSCH CRC > 0 for UL), and the handover's n>=3 alternation. No parameter
from gNB truth may configure the passive receiver.

Test-first check outcome: initial structural run exposed one plan-validator continuity bug (1 failing,
3 passing); removed the invalid cross-feature continuity assertion. Retest is 4/4 passing; plan emits
108 alternating runs. `git diff --check` is clean. This is a plan-only runner module, not yet an executable
build/capture launcher: the missing receiver build directory, absent OCUDU fixture/harness in this tree,
and unresolved active-UE routes prevent a safe launch contract. Any later launcher must be separately
reviewed, exact-pin every executable/config/harness/scorer before the first arm, test fail-stop behavior,
and serialize both heavy build and all captures using the host `radio_bed.lock`. No lock/build/radio was
used here, per coordinator hold.

Files / hashes:

- `rfsim-integ3/tests/passive_rx/ocudu_bed_matrix.py` — SHA256
  `570b12f45c73743cdc3b848e40ad8adf756e39bc0628dc53ea344f709aa2e5df`.
- `rfsim-integ3/tests/passive_rx/test_ocudu_bed_matrix.py` — SHA256
  `efafb781362e69fa00b54004e06a5c31753b3dc5671115c7ec065b3c802688af`.

G1-G6 and live G4 are not run; this checkpoint is not a pass and authorizes no capture.
