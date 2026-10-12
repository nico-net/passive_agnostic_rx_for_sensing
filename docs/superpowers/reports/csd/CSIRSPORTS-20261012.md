# CSIRSPORTS 2026-10-12: 12/16/24/32-port CSI-RS handling (branch csd/csirs-ports, base csd/otatest ebb50d1dea)

Evidence labels (PROJECT_MEMORY 0.1): [code-audit], [unit], [replay DGX GB10, rr-csd-cps build, t26v.env, speed 0.25, other agents loading the host], not OTA.
Host DGX aarch64 only; TIM (PCI 923) and Vodafone (PCI 415) results are never merged.

## 1. Audit [code-audit]
Worst case: 32 ports, 4 RX, 273 PRB (FFT 4096, 14 symbols).

| item | scales with | worst case | state |
|---|---|---|---|
| `ue->nr_csi_info->csi_rs_generated_signal` | ports x samples_per_slot | was 12 buffers; 32 x 123 kB = 3.9 MB | BUG fixed: 32 buffers with ISAC_CSIRS_PORTS_GT8=1 (`csi_rs_generated_ports`); flag off stays 12 |
| `nr_ue_csi_rs_procedures` (own CSI-RS path) | ports | no port guard, heap overrun for rows 11-18 | now guarded like the monitor path |
| `noise_real/noise_imag` VLAs (channel estimation) | ant x ports x RB x 2 x 2 B | 140 kB stack | moved to per-thread heap scratch (slot 3) |
| `mod_csi` VLA in generator | 14 x 16*RB/2 x 2 B | 61 kB stack | left (bounded, independent of ports) |
| LS/estimate/rx scratch (`csirs_scratch`) | ant x ports x FFT | 4*32*4096*4 B = 2 MB each, heap, grow-on-demand | already heap (f51f967bf2) |
| `isac_h/isac_k/isac_l` static `__thread` | NR_ISAC_CSIRS_MAX_ANT x 275 | 18 kB | unchanged; TLS of nr-uesoftmodem = 0x256498 B = 2.4 MB (< 3 MB) |
| blind_rt reference planes | 32 planes x FFT x 14 | 7 MB per thread heap, only in discovery mode | already sized for 32 ports |
| disc sweep map (`begin[33]`, GPU map capacity rb*1024 RE) | ports, RE per RB (row 18: 256) | fits | no change |
| rate matching (`overlap_csi_symbol`, `csi_resource_re_map`) | row | generic for rows 1-18 | verified vs table 7.4.1.5.3-1, no change |
Also: `nr_get_csi_rs_signal` (meas_count 0) and `calc_power_csirs` (size 0) divided by zero when no RB has the density parity; both guarded.
Note: the old skip was `ports > NR_MAX_CSI_PORTS (12)`, i.e. 16/24/32-port rows; rows <= 12 ports were already handled.

## 2. Port handling
Generator, discovery hypotheses (rows 1-18), sweep maps and rate-matching maps already follow table 7.4.1.5.3-1 (CDM4/CDM8 weights, k'/l', density 0.5/1). The missing piece was the capture/estimation buffers. With ISAC_CSIRS_PORTS_GT8=1 the monitor and own-CSI-RS paths now run LS estimation for 16/24/32 ports (CDM despreading by the generated weights); sensing export stays port 0 (unchanged). Flag off: identical behaviour (the skip message now reads "ports > 12 buffers").

## 3. Validation
[unit] `test_nr_csirs_ports` (new, ctest): all 18 rows at 273 PRB with exactly `ports` valid plane pointers (any dataF[p>=ports] write segfaults) plus a canary after each plane; checks port count, per-port RE count = CDM size per RB, union = `ports` RE/RB, occupied symbols per table, orthogonality inside CDM groups: PASS. `test_nr_csirs_blind_synth` builds.
[replay DGX] 12 s captures, t26v.env, B = rr-csd-cps build, flag off / on / on+ISAC_SEGV_BT=1 (last ISAC_METRICS line):

| run | confirmed | candidates | pdschq_crc_ok / decoded | crash |
|---|---|---|---|---|
| TIM off | 6 | 1243 | 384 / 6440 | 0 (row-12 16-port skip logged) |
| TIM on | 6 | 1202 | 413 / 6354 | 0 (no skip) |
| TIM on+BT | 2 | 964 | 363 / 4293 | 0 |
| TIM on (repeat) | 2 | 785 | 252 / 4044 | 0 |
| TIM off (repeat) | 4 | 1008 | 282 / 3961 | 0 |
| VF off | 24 | 1493 | 1858 / 8605 | 0 |
| VF on | 23 | 1351 | 1442 / 9634 | 0 |
| VF on+BT | 22 | 1426 | 1992 / 8899 | 0 |

Reading: no crash, no heap error with 16-port row 12 captured (TIM); CSI-RS confirmations and CRC differences between on and off are inside the run-to-run spread seen between identical configs (TIM off 6 vs 4 confirmed, 384 vs 282 CRC; VF on 1442 vs on+BT 1992), because replays at speed 0.25 share the host with other agents. This shows "not worse" only at that noise level; it is not a precise A/B. The 16-port capture adds CPU work per matching slot (not measured separately).

## OTA env delta
None required. Optional: add `ISAC_CSIRS_PORTS_GT8=1` to the OTA env for cells with >12-port CSI-RS (TIM PCI 923 row 12). Default off. Costs 3.9 MB heap at 273 PRB.
