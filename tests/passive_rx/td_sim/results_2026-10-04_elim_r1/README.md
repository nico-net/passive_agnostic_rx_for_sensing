# CB0 elimination channel — fix round 1 simulator gate (2026-10-03/04)

`[SIMULATED, DGX host (shared with other agents; no lock per the operator lock policy for nr_td_sim; load 6-9 during the run), nr_td_sim @2c51f9c4e4]`
(binary byte-identical to the one built from that commit). Seed 1, acq 500 x 4 RNTIs per cell, v2 slot model, BC6 common flags (`gate_elim.json`).
**4 RX only: the 1-RX arms were dropped by operator decision (2026-10-03).** Main matrix 64 cells: arm (off / elim) x prior / fb2 x oracle 0/1 x TDD
none/DDDSU x rho (persist) 0.5/0.9 x snr_rho 0/0.9; 14 supplementary cells (subset B 64/128, trap-family exemption off, CUDA CB0 + CPU fallback,
crc-false 1e-3 stress, K38 rank-1-only, margin 0, rank-SNR + HARQ gain). first = RNTI 1 of an acquisition, later = RNTIs 2-4 (median / mean / p95
in seconds of decided RNTIs). cb0_per_grant = CB0 decodes per CB0-admissible grant (GPU workload); cb0_inadmissible = retransmission grants (no CB0).
`premise_arm.jsonl`: the discriminating premise-violation arm (v1 model, near-perfect MCS twin te 0.05, CB0 margin -6 dB, TB-only HARQ gain 6 dB
on wrongly admitted retransmissions, rank-SNR +3 dB, mu 8 dB, cap 600 s, acq 200 x 1 RNTI), flags as listed.
Run 2026-10-03 23:59 -> 2026-10-04 00:33 CEST (main + supp), premise arm in parallel (8 processes total).
