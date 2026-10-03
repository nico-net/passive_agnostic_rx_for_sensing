# CB0 elimination channel — simulator gate (2026-10-03)

`[SIMULATED, DGX host (host shared with other agents' builds/rfsim beds; runs under flock /tmp/td_measure.lock), nr_td_sim @5ffe81aab8]`.
Seed 1, acq 500 x 4 RNTIs per cell, v2 slot model, BC6 common flags (gate_elim.json). first = RNTI 1 of an acquisition,
later = RNTIs 2-4, cold = 1-2, steady = 3-4 (BC6 convention); seconds of decided RNTIs. Main matrix + supplementary
(stress crc-false 1e-3, K38 rank-1-only CB0, margin 0): `summary.md` / `rows.json` (`gate_elim.py --matrix gate_elim.json`,
started 18:35 CEST, load 3.42). Decoder arms (CUDA CB0, CPU fallback mid-context): `supp_decoder/` (`gate_elim_decoder.json`,
20:46 CEST, load 0.44). cb0_per_grant counts CB0 decodes run per processed grant (GPU workload), including batches the
engine's decoder pin dropped. cb0_bound = 1e-6 x RNTIs (analytical wrong-winner bound, independent of --crc-false).
Monte-Carlo resolution ~3/N = 1.5e-3 per RNTI (N = 2000); the 1e-6 claim is analytical. Report:
`/home/nicola/NICOLA/wt/td-levers/.superpowers/sdd/2026-10-01-technique-d-blind-convergence/task-ELIM-report.md`.
