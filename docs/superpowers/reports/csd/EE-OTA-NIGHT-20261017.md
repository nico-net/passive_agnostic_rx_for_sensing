# On-air A/B: t26bE vs t26eE, 2026-10-17 04:57-05:09 (LOW LOAD) — not usable

B = t26bE; E = t26eE = bE + ISAC_GPU_LDPC_CL_MIN_CB=4, NC_MIN_CB=8, LDPC_SM=1 + ISAC_PDSCH_GATHER_FAST=1 + ISAC_PDSCH_HOST_FAST=1. Binary csd/int16 f5d846211e. n=3 per cell, interleaved, all synced.
[MEASURED, DGX OTA 2026-10-17 ~05:00 LOW LOAD, cell, arm, n=3] Traffic collapsed: n_real 49-2856 per run (TB 169-275; TE 49-7179; VB 354-2856; VE 374-1363). With so few grants p99 is a 1-in-100 sample of tens of grants (e.g. TB1 p99 21.5 ms from 194 grants). crc_q 94.6-100 everywhere, drops <= 0.1 %.
Decision: no conclusion; the comparison moves to the daytime run (t26aXE vs t26bE vs t26eE, n >= 3 per cell).
