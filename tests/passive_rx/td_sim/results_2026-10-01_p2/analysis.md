# P2 gate: run record and analysis (hand-written; `summary.md` is the generated part)

All numbers `[SIMULATED, DGX host, nr_td_sim @02524c44ff]` unless labelled otherwise.

## Run record

- Simulator `nr_td_sim` built at commit 02524c44ff (branch td/convergence-levers). Commands:
  `python3 tests/passive_rx/td_sim/gate_p2.py run --sim nr_td_sim --matrix tests/passive_rx/td_sim/gate_p2.json --raw RAW -j 8`
  then `python3 tests/passive_rx/td_sim/gate_p2.py summarize --matrix tests/passive_rx/td_sim/gate_p2.json --raw RAW
  --out tests/passive_rx/td_sim/results_2026-10-01_p2 --label "SIMULATED, DGX host, nr_td_sim @02524c44ff"`. `summarize`
  writes summary.md and per_config.jsonl, including the wrong-winner table and the admitted-fail-share figure.
- Wall time [MEASURED, DGX host, 8 processes]: oracle 1 half 7 s; oracle 0 half 2396 s.
- Acquisitions (4 RNTIs each) per (cell, rx) and arm: oracle 1 = 260 x 72 configs = 18 720; oracle 0 = 20 x 72 = 1 440
  (cut for compute); total 20 160 (80 640 overall per arm).
- Twins: brief asked {0, 3}; the simulator caps physical twins at 2, so twins 2 (physical) and 0 (UNPHYSICAL stress arm).
- probe_inconclusive left at its default (0.1); the spec 5.4 probe-cost ablation was not run (moot given the FAIL).
- Pairing is per RNTI REALISATION (same seed: truth, channel draws, engine RNG seed), not per trajectory: P2 changes the
  engine's evidence, so the hypothesis sequences diverge at both oracle settings.
- Raw per-RNTI output (196 MB) is not committed.

## Why P2 produces wrong winners in the blind arm

Every wrong P2 winner differs from the truth only in mcs_table (a physical twin: any other wrong hypothesis always fails
in the model and cannot reach SWEEP_MIN_RATE). All 932 occur at twins 2, 920 at table_exercise 0.5, none at K 4.

Mechanism. Probe slots go to the first uncleared survivors in score order (nr_pdsch_config_sweep.c:669-681, with the
cleared-hypothesis skip at :671; ordering key :519 includes w_obs and w_probe). P2 failures clear the dead hypotheses
quickly, so when K-1 is smaller than the number of survivors, the truth -- ranked first by correct side information --
is probed far more than its twins (~30x its fair share in the reproduction below: truth_full ~282 vs truth_kl_trials
~4641). The truth's KL rate p.F/(F + (1-p).P) (F full-TB decodes, P probes, p its pass probability) is therefore deflated
below a rarely probed twin. The hot exploit (:564-573) is not the cause: it is capped at trials[hot] < 64 (:571).

Supporting evidence:
- Reproduction (reviewer, re-run here): seed 308001, NSA-like, 4 RX, oracle 0, twins 2, te 0.5, K 2, mu 8, catalogue 16,
  acq 5: P2 with all levers 9/20 wrong (8 undecidable); with w_obs 0: 0/20 wrong (3 undecidable). Reviewer: w_probe 0 or
  w_obs-only also give 0 wrong but more undecidable.
- K 4 gives 0 wrong (all survivors probed equally).
- Bare P2 (gate 0, weights 0, field book 0, prior 0; seed 7, acq 12, blind, catalogue 16, mu 8, 1 RX, K 2, te 0.5) gave 0/48 wrong.
- Correlational only: in the 932 wrong RNTIs a median 92 % of the truth's KL trials are admitted probe FAILs, against 6 %
  in the 9 089 correct twins-2 oracle-0 RNTIs (summary.md, last section).

With oracle 1 the Qm oracle prunes the twins after two sightings, so no twin is left to win: this masks the bias, it does
not remove it.
