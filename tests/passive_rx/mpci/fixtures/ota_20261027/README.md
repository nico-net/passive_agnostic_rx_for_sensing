# OTA log excerpts, 2026-10-26/27 (DGX GB10 + X410 34B2AF1, DEIB Milan, 4 RX, 300 s runs)

Excerpts: first 60000 lines + every summary / multi-PCI line (MPCI_*, SFO reset, CORE_*, SHUTDOWN, SCANTIME, GPU_*, SHMIQ_*,
IQ_CHAN*, ISAC_METRICS, PDSCHQ, UL_PIPELINE, RFCENSUS, RXDISCONT, Got synch, length locked), original order, gzip.
Fixtures for the per-cell scorers and the dashboard. Cell lines carry `[c<k>] ` after an ANSI colour prefix.

| file | build | cells | capture |
|---|---|---|---|
| mp27_P1_1_115718 | mpci-main 1a78e0f001 | 2: Vodafone PCI 415 (c0) + TIM PCI 923 (c1) | one 245.76 MS/s capture, CPU channelizer (147 overruns) |
| mp27_Q1_1_121424 | mpci-main 1a78e0f001 | 2: Vodafone PCI 415 (c0) + Iliad PCI 403 (c1) | one 122.88 MS/s capture, CPU channelizer (23 overruns) |
| mp27_S1_1_115134 | mpci-main 1a78e0f001 | 1: Vodafone PCI 415 (N=1, no tag) | direct radio path |
| me_TN1_1_220653 | main 57df6abb85 | 1: TIM PCI 923 (N=1, no tag) | direct radio path, evening |
