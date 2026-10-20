# PDCCH arm: PAR_BANKS=1 + 3 scan threads vs all-on, OTA 2026-10-16

Arms: all-on t26aXE (ab_x*, 14:21-14:40) vs t26aXPE = t26aXE + ISAC_PDCCH_PAR_BANKS=1 (helpers unpinned) + cfg sd_t26ul3s (3 scan threads, cores 6..8; core 7 and 8 shared with PDSCH consumers 0/1) (ab_p*, 14:40-14:58). n=3 per cell, not interleaved. Parser: scratchpad cmp_p.py (= cmp.py). Labels [MEASURED, DGX OTA 2026-10-16, <cell>, <arm>, n=3].

| run | grants | CRC-OK % | tot p99 us | a2e p50/p99 us | scan wait p99 | scan run p50/p99 | over-slot passes | bank pass mean |
|---|---|---|---|---|---|---|---|---|
| xV 1/2/3 | 23.3k/37.7k/27.3k | 47.3/40.7/40.9 | 11509/10645/12304 | 623/694/906, 2745/3400/4522 | 850/1750/2350 | 400-550 / 1450-2400 | 2.9/4.7/4.6 % | 158/182/166 |
| pV 1/2/3 | 32.8k/52.0k/29.7k | 44.1/42.7/45.8 | 10472/10009/7958 | 1042/1088/819, 4249/4560/4232 | 1900/2100/1600 | 500-650 / 2700-3050 | 7.3/13.3/7.7 % | -/272/219 |
| xT 1/2/3 | 32.7k/21.7k/34.0k | 48.9/30.2/35.7 | 7284/8147/8491 | 621/680/619, 3110/4461/3281 | 1700/2450/1750 | 400-500 / 2000-2650 | 12.3/13.5/14.5 % | no banks |
| pT 1/2/3 | 17.2k/23.6k/2.8k | 30.6/45.8/63.4 | 9124/7495/11040 | 903/645/736, 6792/4553/4379 | 4000/2400/1200 | 250-650 / 2900-5050 | 30.1/12.5/6.5 % | no banks |

## Verdict
- The PDCCH change made the scan SLOWER, not faster: scan run p99 1.5-2.4 -> 2.7-3.1 ms (V), bank pass mean 158-182 -> 219-272 us, over-slot passes x1.6-2.8, a2e p50 V 0.69 -> 1.04 ms, a2e p99 worse on both cells. a2e is now the top or second p99 component (V 9-30 %, T 49-52 % in runs 1-2).
- T has no banked CORESET passes, so PAR_BANKS does nothing there; the T change is the 3rd scan thread only.
- Most likely cause [INFERRED]: CPU contention: the 3rd scan thread sits on core 8 with PDSCH consumer 1 (+ the unpinned pdcchJanon at ~70 %), the helpers are unpinned. Not proven.
- DL tot p99 V lower (10.0-10.5, 8.0 vs 10.6-12.3 ms) with less qwait, but the arms are not interleaved and loads differ; not attributable to the PDCCH change.
- pTaXP3 decoded only 2.8k grants (others 17-34k): unexplained, check before reuse.
- Next: pin scan threads/helpers to free cores (10, 12, 14) and pdcchJanon away from PDSCH cores, interleaved A/B; or skip to early-start / GPU-batch scan.
