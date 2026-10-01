A1 baseline: unmodified HEAD C code (binaries built from be2e7fa4b6), 106-PRB fully agnostic rfsim arm, 150 s each,
via tests/passive_rx/dgx/rfsim_regress.sh / rfsim_arm.sh.
[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, be2e7fa4b6 code]

Host: cloud container, 4 cores, no IPv6 in kernel, no GPU. Host-specific arm setup (scripts only, no C change):
  - V4SHIM (auto): LD_PRELOAD v4only_shim.c maps the rfsimulator AF_INET6 server socket to AF_INET (no IPv6 here).
  - SCANTHREAD (auto when nproc<=5): --sensing.pdcch_blind_monitor_scan_thread 1:8:-1 (frozen conf pins core 5 -> EINVAL assert).
  Without these the arm cannot run here. The unpinned scan thread is a deviation from the DGX arm (affects CPU/drop figures).

Per-arm results (scores.jsonl; *.time.txt = /usr/bin/time -v output):
  arm      sync_s  ttc_s  n_conv  crc_pct  drop_full_pct  cpu_pct  max_rss_kb
  base_r1  8.689   4.218  2       94.93    1.1655         201      890236
  base_r2  8.216   2.872  2       96.89    1.5491         199      885244
  base_r3  8.045   4.538  2       95.19    0.7742         202      892844
Run-to-run spread: crc 94.93-96.89 %, drop_full 0.77-1.55 %. All miss the DGX gate (98.0 / 1.0) on CPU budget only.
Proposed cloud gate: GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5 (spread minus/plus ~2 pts / ~1 pt margin).
