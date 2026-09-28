# Task 12 report: PRB-bundling (PRG) hypothesis for channel estimation

**Status: DONE**

## Commit
`79c42a29ed` on `sdd/agn-prb` (worktree `sens6:/home/sens/NICOLA/agn-wt/prb`):
`Passive PDSCH: PRG-aware channel estimation, bundle size swept per RNTI`

Files touched:
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (modified)
- `openair1/PHY/NR_UE_TRANSPORT/nr_arm_sweep.h` (new)
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_arm_sweep_test.cc` (new)
- `CMakeLists.txt` (modified: registers `test_nr_arm_sweep`)

**Deviation from the brief's file list, called out explicitly**: the brief said "Modify:
nr_pdsch_passive_decode.c" only. Controller ruling R25 (given in the task prompt) additionally
required factoring the pick/latch core into one small helper shared with the VRB-L sweep, plus a
standalone unit test of that helper. `nr_pdsch_passive_decode.c` is not unit-test-friendly standalone
(too many OAI/PHY dependencies -- Task 11's own report flagged this as an open gap), so the helper
was pulled into a tiny header-only file (`nr_arm_sweep.h`, no PHY dependencies) that both the sweep
and a standalone gtest can include directly. This is the smallest change that satisfies R25's
explicit ask; no other files were touched.

## Design
A wrong PRG guess only changes channel-estimation *interpolation* boundaries -- never which REs are
read (unlike VRB-L, where a wrong bundle size means literally the wrong PRBs). So the whole hypothesis
lives inside `nr_pdsch_passive_decode()`:
- At function entry, if the caller left `freq_alloc->prg == 0` (true of every producer today) and no
  GPU-assisted decode is pending for this thread (`t_llr_ovr_n == 0` -- the GPU front end already
  computed its channel estimate assuming `prg==0` and bails to CPU for any nonzero `prg`), it picks
  this RNTI's current arm (`rnti_prg_pick`), maps it to a bundle size (0/2/4), and — only if nonzero —
  points the local `freq_alloc` at a stack copy with `.prg` overridden. Task 9's existing
  `nr_prb_segments()`/segmented-chest path (already exercised by RA-type-0/interleaved-VRB grants)
  then does the real work unmodified.
- Arm 0 (wideband) is a true no-op: `freq_alloc` is left pointing at the caller's own struct, so an
  unlatched RNTI sitting on arm 0 costs exactly two guard reads.
- After LDPC/CRC is known, right next to the existing VRB-L feed call, `rnti_prg_feed()` records the
  outcome; on latch it logs `SENSING: PRG rnti=0x%x prg=%u latched` (brief's exact log shape).

### Generic helper (R25)
`nr_arm_sweep.h` factors the Wilson-interval pick/latch algorithm (previously duplicated verbatim as
`vrbl_wilson`/`vrbl_sweep_pick`/`vrbl_sweep_feed`) into `nr_arm_sweep_wilson/_pick/_feed(..., n_arms)`,
operating on a fixed-size `nr_arm_sweep_t { uint32_t tr[3], ok[3]; int latched; }` (`NR_ARM_SWEEP_MAX
= 3`, no heap). `nr_vrbl_sweep_t`/`nr_prg_sweep_t` are now `typedef`s of it; `vrbl_sweep_pick/feed` and
the new `prg_sweep_pick/feed` are one-line wrappers pinning `n_arms` to 2 and 3 respectively. Byte-for-
byte identical algorithm to what Task 11 shipped for `n_arms=2` (verified: same constants, same
tie-break rules, same latch condition). PT-RS's own `nr_ptrs_sweep_t` (7 arms, different shape) was
left untouched, per the ruling.

`rnti_dec_t` gained a `nr_prg_sweep_t prg` field, seeded `latched = -1` in `rnti_dec()` next to the
existing `vrbl` init (memset already zeros `tr`/`ok`).

## Test summary
`ctest -R 'test_nr_pdsch|test_nr_hyp_sweep|test_nr_arm_sweep'` from the lane build dir: **6/6 pass**
(`test_nr_hyp_sweep`, `test_nr_pdsch_xoverhead`, `test_nr_pdsch_config_sweep` [29 s, 38/38 internal
cases], `test_nr_pdsch_prb_set`, `test_nr_arm_sweep` [new, 7/7 cases], `test_nr_pdsch_ptrs_unav`).
`nr-uesoftmodem` (full build, lane `prb`) is clean; the only warning is a pre-existing
`dmrs_first`-maybe-uninitialized note at line ~2720, unrelated to this change and already present
before it (confirmed against Task 11's report, which flagged the identical warning).

New `nr_arm_sweep_test.cc` cases (synthetic tr/ok sequences, no PHY dependencies):
1. `StartsAtArmZeroUntried` -- all arms virgin picks arm 0 (wideband default).
2. `ExploresEveryArmAtLeastOnceBeforeLatching` -- every arm gets >=1 trial before any latch chance.
3. `LatchesOnAClearWinner` (2-arm) -- clean 0%/100% split latches on the winner; further pick/feed
   calls after latch record zero additional trials (guard-cost requirement).
4. `LatchesOnAClearWinnerThreeArms` (3-arm, the PRG shape) -- wideband wins against two failing arms.
5. `NeverLatchesOnNoise` -- three genuinely equal (50/50/50) arms stay unlatched through 5000 rounds.
6. `SmallAdvantageDoesNotFalseLatchEarly` -- a noisy 55/45 edge does not latch before
   `NR_ARM_SWEEP_LATCH_MIN_OK` trials.
7. `PickReturnsLatchWithoutRecordingFurtherTrials` -- feeding a latched sweep is a pure no-op.

## Concerns
1. As with Task 11's VRB-L sweep, the actual wiring inside `nr_pdsch_passive_decode()` (the pick site,
   the local `freq_alloc` override, the feed site, the log line) is exercised only by the full
   `nr-uesoftmodem` build compiling -- there is no automated test that drives a real grant through it,
   because that function is not unit-test-friendly standalone. Risk is contained by the fact that the
   *only* new logic actually reachable inside that function is a two-line guard (`freq_alloc->prg == 0
   && t_llr_ovr_n == 0`) plus a struct copy; the pick/latch math itself is now the independently-tested
   `nr_arm_sweep.h`.
2. The PRG and VRB-L sweeps run as two independent univariate bandits over the same TB-CRC stream when
   a grant is both interleaved and prg-swept simultaneously (both `n_prb_list>0` and my override
   active). Each still converges to its own true answer since a hard CRC failure requires either
   parameter to be wrong, not both simultaneously wrong in only one dimension; a live cell that never
   exercises this combination cannot be a regression source, and the lab cell gives no signal on PRG at
   all so this cannot be validated against real per-PRG precoding until a capable cell is available.
3. Per instructions, did not run `nr-uesoftmodem` or touch the X410 -- verification is build + ctest
   only. Not pushed, per standing rules.

## Report path
`/home/sens/NICOLA/docs/superpowers/sdd/full-running-agnosticity/task-12-report.md` (this file).
