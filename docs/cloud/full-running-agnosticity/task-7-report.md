# Task 7 report: Result-preserving speed-up of the joint solver

Lane: `joint` (`/home/sens/NICOLA/agn-wt/joint`, branch `sdd/agn-joint`, host sens6).

## What was done

1. **Golden-hash characterisation test** added to `tests/nr_pdcch_joint_solve_test.cc`
   (`JointSolve.OptimisationKeepsResultsBitIdentical`), exactly as specified in the brief.
   Ran on the unmodified solver: `JOINT GOLDEN HASH = 0x745164fa85b81a5b`. Replaced the
   `UINT64_C(0x0)` placeholder with that value, rebuilt, confirmed PASS. Committed alone
   (commit `013718a41a`).
2. **Applied all four optimisations (a)-(d) from the brief, verbatim**, inside
   `nr_pdcch_joint_solve()` only (left the similar loop in `nr_pdcch_joint_model_new_full`
   at line ~164 untouched, per the brief):
   - (a) Branchless MRIP elimination (mask XOR instead of `if (v_get(...))`).
   - (b) Branchless Gauss-Jordan elimination (mask XOR instead of `if (q != c && v_get(...))`).
   - (c) `memset(T, 0, sizeof(T))` gated on `order >= 1` (order 0 never reads `T`).
   - (d) `uint8_t inI[NR_PDCCH_JOINT_MAX_E]` (1,728-byte AL16-sized array) replaced by an
     `ebits inIb` bitmask set via `e_flip`; both remaining reads (`WO`/`W2O` loop and the
     `EVAL` macro) changed to `e_get(&inIb, i)`. `qsort` call untouched.
3. **Verified results unchanged**: `test_nr_pdcch_joint_solve` (12/12 including the golden-hash
   test) and `test_nr_pdcch_joint_live` both PASS after the change — bit-identical outputs
   across the fixed AL1/AL2, signal/noise, order 0-2 corpus.
4. **Benchmarked** via a lane-local copy of `/home/sens/NICOLA/bench_joint` at
   `/home/sens/NICOLA/agn-wt/joint-bench/` (`B=` retargeted to the lane build dir, `D=` to the
   copy), built against the base-commit (unmodified) solver via `git stash` and again against
   the optimised solver via `git stash pop`, so both binaries link the exact same harness/flags
   against only the solver file differing.
5. **Kept the change**: 1-thread time at 1,080 candidates dropped 55,426.5 -> 34,391.3 us
   (-38.0%), well past the >=20% bar. Committed the solver change (commit `96789d2cf7`).

## Before (base commit, unmodified solver)

```
AL1 order-0 joint solve, A=51 (K=67 unknowns), E=108 coded bits, Es/N0 6 dB, 1 in 20 candidates real
Per-occasion wall time in us (median). Slot budget at 30 kHz = 500 us.

cands    |   CPU 1thr   CPU 6thr  CPU 12thr |    GPU e2e GPU kernel
1        |        6.6       17.8       27.6 |      733.0      715.8
5        |      232.7       18.8       68.1 |     1371.0     1353.7
45       |     2282.1      456.0      661.1 |     2175.1     2157.6
135      |     7077.8     1429.7     1944.4 |     2642.9     2620.6
1080     |    55426.5    10487.7    11471.6 |     2692.3     2639.1
36720    |  1910194.8   345473.5   273881.7 |    46717.8    45370.6
111510   |  5785415.3  1052631.0   872382.2 |   148508.9   144456.3
```

(Reference point from memory `joint-solver-gpu-vs-cpu-benchmark`: 54,339 us 1-thread /
10,501 us 6-thread at 1,080 candidates — this re-measurement is within normal run-to-run noise.)

## After (optimised solver, commit 96789d2cf7)

```
AL1 order-0 joint solve, A=51 (K=67 unknowns), E=108 coded bits, Es/N0 6 dB, 1 in 20 candidates real
Per-occasion wall time in us (median). Slot budget at 30 kHz = 500 us.

cands    |   CPU 1thr   CPU 6thr  CPU 12thr |    GPU e2e GPU kernel
1        |       12.8       22.5       32.9 |      724.0      707.6
5        |      126.8       23.4       48.0 |     1387.6     1371.1
45       |     1440.7      267.2      415.0 |     2167.1     2151.4
135      |     4350.1      947.7     1180.4 |     2643.1     2624.2
1080     |    34391.3     6777.7     7931.4 |     2692.2     2636.2
36720    |  1176646.2   219919.2   203768.6 |    46717.9    45374.1
111510   |  3580311.9   687457.9   547003.4 |   148840.9   144786.5
```

## Headline numbers

| cands  | 1thr before | 1thr after | delta   |
|--------|-------------|------------|---------|
| 1080   | 55,426.5 us | 34,391.3 us| -38.0%  |
| 36720  | 1,910,194.8 us | 1,176,646.2 us | -38.4% |
| 111510 | 5,785,415.3 us | 3,580,311.9 us | -38.1% |

Improvement holds consistently across candidate counts and thread counts (6-thread @1080:
10,487.7 -> 6,777.7 us, -35.4%; 12-thread @1080: 11,471.6 -> 7,931.4 us, -30.9%). GPU numbers
unaffected, as expected (GPU path doesn't use this CPU code). At very low candidate counts
(1, 5) the 1-thread numbers are noisy/inconsistent in direction (dominated by fixed per-call
overhead, not the elimination loops the brief targeted) — irrelevant to the pass/fail criterion,
which is specified at 1,080 candidates.

## Commits (branch `sdd/agn-joint`, worktree `/home/sens/NICOLA/agn-wt/joint`)

- `013718a41a` — Joint solver: golden-hash characterisation test before optimising
- `96789d2cf7` — Joint solver: branchless GF(2) elimination; skip order-0 work it never reads

## Concerns / notes

- No capture (`nr-uesoftmodem`) was running at any point during this task; builds proceeded
  per the shared rule.
- The golden hash (`0x745164fa85b81a5b`) was recorded once, before any optimisation, and never
  changed — all four optimisations (a)-(d) applied together still produce bit-identical results,
  so no bisection of individual steps was needed.
- Bench directory `/home/sens/NICOLA/agn-wt/joint-bench/` is a working copy left in place (per
  the brief's Step 5 instructions); it is outside the tracked repo tree (bench_joint itself is
  untracked scratch infrastructure, matching the original `/home/sens/NICOLA/bench_joint`).
