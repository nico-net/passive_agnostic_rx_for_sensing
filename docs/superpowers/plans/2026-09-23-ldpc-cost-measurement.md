# LDPC decode cost measurement (Task 1)

**Command run** (on `sens6`, `cmake_targets/ran_build/build/`):
```
for i in 1 2 3 4 5; do ./ldpctest -l 8448 -r 22 -d 25 -n 1 2>&1 | grep 'ldpc_decoder:'; done
```

**Ruling on parameters:** the plan's Step 1 assumed `--BG`/`--ZC`/`--kprime` flags exist; they do not
— `ldpctest`'s real CLI (read from `openair1/PHY/CODING/TESTBENCH/ldpctest.c`'s `getopt` string) is
`-l Kprime -r nom_rate -d denom_rate -n n_trials -i max_iterations -q qbits`, with BG selected
automatically from Kprime/rate. `tb_size_lbrm=159749` (the LBRM circular-buffer size captured live
from the gNB's real dedicated grant this session) is a different quantity from `-l Kprime` (one
code-block's own systematic bit count) — deriving an exact Kprime from it needs the real segmentation
formula, not attempted here. Used `-l 8448` (BG1's maximum code-block size, a real, large-transport-
block-representative value) with rate 22/25, matching leftover `ldpctest_BG_0_Zc_0_rate_22-25_
Kprime_8448_*.txt` output files already present in the build directory from a prior run on this exact
host — i.e. an already-validated, not newly-guessed, configuration. **This is a representative large-
TB benchmark, not a precise reproduction of this cell's specific transport block.**

**Raw result:** each invocation ran a full internal SNR sweep (not a single decode as the plan
assumed), producing 344 total `ldpc_decoder:` timing lines across the 5 invocations. The first ~15-20
lines show a warmup transient (62.4, 43.6, 35.6, 34.7 µs, decreasing), after which the value settles
tightly.

**Steady-state mean, last 100 samples:** **33.71 µs** per decode (range ~33.5-35.8 µs, one visible
outlier cluster at ~35.6-35.8 µs, likely a scheduling jitter, not a distinct regime).

**Conclusion for hypothesis-search feasibility:** at ~34 µs/decode, running 2-5 speculative full LDPC
decode attempts per grant (the arithmetically-pre-filtered count discussed in this session, well below
the naive full-Cartesian-product count) costs roughly 70-170 µs total per grant during the brief
discovery window before a cell-wide parameter locks — this is compatible with the existing deferred
PDSCH consumer-thread architecture (`pdcch_blind_monitor_pdsch`'s doc string: "6 consumers... queue was
dropping 93% full at 2", i.e. this pipeline is already built to absorb per-grant decode costs off the
RT thread) and is far below the ~500 µs/slot budget even if it somehow ran in-line, which it does not.
This number is much lower than the ~0.1-1 ms this session extrapolated from a different, unrelated
prior finding ("UL PUSCH decode running in-line, 1065 µs/grant vs a 500 µs slot") before this
measurement was taken — that 1065 µs figure evidently includes far more than just the LDPC decode step
itself (FEP, channel estimation, demod, rate-matching), which this measurement isolates. Do not re-use
the 0.1-1 ms figure after this; use 33.71 µs (steady-state, this exact config) instead, and note the
Kprime/rate caveat above if a reader needs a number specific to this cell's real TBS rather than a
representative large-TB case.
