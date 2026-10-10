# UL-READY: is the UL pipeline ready, and does it decode sporadic grants? (csd/ul-ready, 2026-10-10)

Host: DGX GB10 (aarch64), worktree `rr-csd-ulr`, branch `csd/ul-ready` (from `csd/otatest` 0c57ede27e). Every build, unit test, replay and census went through `heavy.sh`. No radio, no hardware. Replays are CPU replays of the saved 4-RX IQ at speed 0.25 (`tests/passive_rx/ulready_replay.sh`: `t26l.env` minus radio-only flags, `ISAC_PDCCH_RBOFFSET_FIX=1` included, CORESET fixture `scanfast_coresets.txt`, UCI search `1024:11:0:32`). Captures are never merged: **b** = `raw_pci415_12s_b` (CAP_TICK 208029025), **n3** = `raw_pci415_3s_1010_1431` (CAP_TICK 194642213, CAP_SAMPLES 368640000). All results are [MEASURED, replay, DGX] unless labelled.

## Verdict (summary)

| Item | Verdict |
|---|---|
| DCI 0_1 parse (field order/widths vs TS 38.212 7.3.1.1.2) | **correct** [CODE-READ + unit-tested against an independently written bit string]; layout of the *cell* is not known to the receiver (see 1.3) |
| DCI 0_0 parse | correct [CODE-READ] (7.3.1.1.1: id1, RIV, TDRA 4, FH 1, MCS 5, NDI 1, RV 2, HARQ 4, TPC 2; non-zero padding rejected). No 0_0 grant occurs in capture b |
| Parameter extraction (FDRA, TDRA k2/S/L, MCS, NDI, RV, PID, ports to CDM groups/ports) | **OK** when the widths are right; the blind width/BWP search does **not** converge on this cell (1.3) |
| (a) TDRA S/L aliasing | solver: absent (`time_rank` tries S=1/L=13 first). Chain full-decode path: **present**, **fixed behind `ISAC_UL_RETRY=1`** |
| (b) CDM groups / ports | tables correct; width-split ambiguity gives 1 vs 2 CDM groups; **fixed behind `ISAC_UL_RETRY=1`** (retry with the other count) |
| (c) rate-matching geometry | DM-RS overhead, TBS and G consistent (absent). PT-RS absent for <= 4 PRB. **UCI: present, fixed** (UCI sweep false-pass rate, `ISAC_UL_UCI_SAFE=1`) |
| (d) 2-layer MMSE bias | **present** (same math as the DL bug) but **unreachable**: the receiver rejects multi-layer PUSCH. Not fixed (shared file, outside ownership) |
| (e) solver poisoning / inherited IDs | per-UE state: absent. Cell-global `prior_*`: harmless ranking only. **New defect found and fixed**: with `ISAC_UL_PUSCH_SOLVER=1` non-QPSK/wide grants were never decoded (`ISAC_UL_SOLVER_FALLTHROUGH=1`) |
| (f) CORESET rb_offset | applies to DCI 0_1 detection (shared `nr_rx_pdcch_symbol`); **flag is default off, `t26l.env` sets it**; keep it in the UL env |
| Chain runs on replay without errors | **yes**: b, 4 configurations, EOF reached, no assert/crash, DCI 0_1 -> parked -> queue -> FEP/chest/demod/LDPC runs |
| Sporadic CRC | **no credible one**. 3 TB CRC passes in 11 runs, all explained as false passes (16-bit CRC x UCI sweep); with the fix 0 in 3 runs. Strongest grants fail CRC. n3: no UL grant reached (DL lock too late) |

## 1. DCI 0_1 / 0_0 parsing

### 1.1 Field order and widths, code vs TS 38.212

`nr_pdcch_blind_extract_01()` (`nr_pdcch_blind_monitor.c`, field walk after `blind_ul_field_bits()`): identifier 1, carrier 0/3, UL/SUL 0/1, BWP indicator 0-2, FDRA (type 1 `ceil(log2(N(N+1)/2))`, type 0 `N_RBG`, dynamic `max+1`; `nr_fdra_bits`), TDRA `ceil(log2(rows))` or 4 for the default table, frequency hopping 0/1, MCS 5, NDI 1, RV 2, HARQ 4, 1st DAI 1/2, 2nd DAI 0/2, TPC 2, SRI, precoding, antenna ports, SRS request, CSI request, CBG, PTRS-DMRS, beta offset, DM-RS init, UL-SCH indicator. This is exactly the 7.3.1.1.2 order. UL-SCH indicator 0 (CSI only) and identifier 1 are rejected. TDRA default table = Table 6.1.2.1.1-2 with j = {1,1,2,3} (`g_table_6_1_2_1_1_2`, `g_ul_tda_j`): verified row by row. Antenna ports use `decode_dci_antenna_ports_val()` (Tables 7.3.1.1.2-6..-11): rank 1 type 1 = {1,p0},{1,p1},{2,p0},{2,p1},{2,p2},{2,p3} (3 bits), TP table {2,p0..p3} (2 bits).

Unit tests (new, `UlReadyDci01.*` in `nr_pdcch_blind_monitor_test.cc`, payloads written out bit by bit, **not** through the test's own packer): length 44 CP-OFDM layout, length 42 TP layout, UL-SCH-indicator-0 and DL-identifier rejection. 3/3 pass; the whole suite (243 tests) passes.

### 1.2 Consistency on the replay logs (capture b, `ISAC_UL_DBG_LOG=1`, 4170 DCIs)

`tools/uldbg/dci_fields.py` (new). Two DCI lengths occur, 42 (1856 grants, 8 RNTIs) and 44 (2314 grants, 10 RNTIs). Bit-statistics fit the layout `id1 | bwp-ind 1 | RIV 15 | TDRA 4 | MCS 5 | NDI 1 | RV 2 | HARQ 4 | DAI 2 | TPC 2 | ant | SRS 2 | [DM-RS init 1] | UL-SCH 1`:

| check | len 44 | len 42 |
|---|---|---|
| identifier / BWP indicator / UL-SCH indicator | 0 / 1 / 1 in 100 % | same |
| TDRA index | 0 in 100 % (2314/2314) | 0 in 100 % |
| RV | 0: 2155, 2: 155, 1/3: 4 | 0: 1702, 2: 154 |
| TPC | 1 in 99 % | 1 in 100 % |
| DAI | 3 in 94 % | 3 in 89 % |
| SRS request | 0 | 0 |
| RIV valid inside BWP (N=216/217/273) | 100 % | 100 % |
| MCS | 0-27, none reserved; 69 % >= 20 | 0-27, none reserved; 68 % >= 20 |
| HARQ pid step between consecutive grants of an RNTI in 0..3 | 2243/2304 (97 %) | 1714/1848 (93 %) |
| NDI toggles on pid reuse | 1755 toggled / 399 unchanged (268 of those RV0 = duplicate detections) | 1351 / 395 (262 RV0) |
| DCI slot in frame | 6,7,16,17 only | 6,7,16,17 only |

With TDRA index 0 always and k2 = 2 (the ULDBG-validated row S0/L14/k2=2) the PUSCH lands in slots 8, 9, 18, 19 = exactly the UL slots of DDDDDDDSUU. HARQ pids count up cyclically and NDI toggles once per 16-pid cycle: the HARQ/NDI/RV fields are certainly at the right offsets.

DCI 1_1 reference (DL, `ISAC_DCI_RAW_LOG=1`, 5423 DCIs, length 49, 45 RNTIs): fitted `id1 | bwp 1 | RIV 15 | TDRA 3 | MCS 5 | NDI 1 | RV 2 | HARQ 4 | DAI 2 | TPC 2 | PUCCH-res 3 | K1 3 | ant 4 | SRS 2 | DM-RS init 1` = 49. TDRA 0..5, RV mostly 0, K1 0..7, ant ports {2,7,3,0,9,8}, DCI slots only in DL slots 0-6/10-16, HARQ pid step 0..3 in 4006/5378 (74 %; several UEs interleave), NDI toggles 3121 / unchanged 1773 (1681 RV0). Consistent; one wart: RIV max 32767 (all-ones) appears and is a PDCCH-order pattern the DL code already excludes.

### 1.3 What is NOT solved: the cell layout

* The tail of both layouts has two readings with the same length. A: `DAI 2 | TPC 2 | ant 3 | SRS 2 | DM-RS init 1` (len 44, ant `000` = 1 CDM group port 0) and len 42 as TP (2-bit ant, no init bit). B: `DAI 1 | TPC 2 | ant 2 ('10' = 2 CDM groups port 0) | SRS 2 | [CSI 2 for len 44] | DM-RS init 1`. The census (3 of the 24 strongest DM-RS grants are len 42 and coherent with the Gold ID 415) favours CP-OFDM for both, i.e. B, but the receiver can only settle it with a TB CRC. [INFERENCE]
* The UL BWP is **216 PRB starting at CRB 1** (the physics census predicts the grants with N=216, start 1). The U3 catalogue ranks the DL BWP (217@0) first. With N=217 the RIV decodes to different start/length (e.g. RIV 435 -> 1+3 PRB vs 3+3). In the blind replays the logged readings vary per grant (tda=8, 12, rb=5+130, ...): `UL_CONFIG_STATS ... feedback[ok=0 bad=1525] locked=0`. The width/BWP discovery **did not converge** on capture b in any run. [MEASURED]
* `nr_dci01_layout_observe()` (the statistical layout resolver) is a no-op while the UL BWP is unknown (`nr_pdcch_blind_monitor_rt.c:1517`, `ul_bwp_size == 0`), so exactly the information that pins the layout here (constant TDRA bits, HARQ counter, NDI toggling) is not used in blind mode. Recommended next step: run the resolver on the *length-locked raw payload bank* without a BWP (bit-statistics: constant bits, HARQ counter, NDI toggle). Not done tonight.

### 1.4 Smaller findings (not fixed, documented)

* `nr_pdcch_dci01_layout_sweep.c:43,50`: the group sums put the CSI request into the pre-antenna-ports group and the SRS request into the post group. In 7.3.1.1.2 both follow the antenna ports (SRS then CSI). Totals are unaffected; the `ant_ports` offset is wrong by the CSI width (`ap_valid_rows` is 0, so it only skews diagnostics histograms). Cosmetic.
* `nr_pdcch_ul_field_sweep.c` axes: SRI 0-2 bits (non-codebook with 3-4 SRS resources needs up to 4), precoding 0-4 bits (4-port codebook needs up to 6), CBG {0,2} (spec 0,2,4,6,8). Coverage gaps for multi-port UEs only; rank > 1 is rejected anyway.
* Default antenna-ports width in `blind_ul_field_bits()` is 2; the CP-OFDM type-1/len-1 field is 3 bits (`ul_ant_bits()`).

## 2. DL defect classes on the UL chain

**(a) TDRA S/L aliasing.** Solver: `nr_pusch_solver.c:119` `time_rank[] = {1,13},{0,14},{2,12},...` tries S=1/L=13 first: absent. The chain's full-decode path (`nr_pusch_passive_queue.c`) decoded only the reading's own S/L, and DM-RS discovery searches symbol masks, not data span: present. Fix: `ISAC_UL_RETRY=1` retries a CRC-failed full decode of an allocation of <= `ISAC_UL_RETRY_MAX_RB` PRB (default 12) with S=1/L=13 (and the other CDM count, (b)). `ISAC_UL_RETRY_SNR_DB` adds an SNR gate, but `snr_db` is not populated by the PUSCH path (always 0.0), so the gate is off by default (a first version with a default of 5 dB never fired: runs up to `h_*` used it, only `i_hypA_retry` has the retry active). Measured cost with the retry active: 8661 decode calls for 3006 queued grants (2.9x), worker 0.26 core, queue-full 2.8 %. No UL_RETRY success occurred.

**(b) CDM groups/ports.** `fill_pusch_pdu` -> `num_dmrs_cdm_grps_no_data`; the gNB extraction (`nr_ulsch_demodulation.c:161,265`), G (`nr_ulsch_decoding.c:116`) and the TBS DM-RS term (`nr_pusch_passive_decode.c:1150`) agree: data in the other comb of a DM-RS symbol when the count is 1. TBS uses `6*cdm*n_dmrs_sym` per PRB = 38.214 6.1.4.2 N_DMRS^PRB. The field-width ambiguity (1.3) makes the group count a hypothesis: `ISAC_UL_RETRY=1` retries with the other count (1 <-> 2).

**(c) Rate-matching geometry.** DM-RS overhead: consistent as above. PT-RS: PTRS-DMRS association is 0 bits in these DCIs and PT-RS is absent for N_RB <= 4 (38.214 Table 6.2.3.1-2), which covers every detectable grant (3 PRB); wider grants with PT-RS would be mis-rate-matched (no PT-RS sweep exists; documented limit). **UCI multiplexing is the real defect.** DAI = 3 in ~94 % of grants, so most PUSCH carry HARQ-ACK. The UCI footprint search (`nr_pusch_passive_decode.c:~1645`) sweeps RE = 1..1024 for CSI and ACK (2048 LDPC decodes) on every 32nd failed grant. A TB <= 3824 bits has a 16-bit CRC, so each exploring grant has a ~3 % false-pass chance. Measured: **1 CRC pass (run A) and 2 CRC passes (run A2)** in two replays of the explicit-layout configuration, e.g. `rnti=0x59ab mcs=14 prb=43+3 uci_re=223 tb=000000...` and `rnti=0xe513 mcs=24 uci_re=174`, with a TB whose first 20 bytes are zero, and the first one's DM-RS SNR at the predicted location was -7.7 dB (control -8.4 dB): not decodes. Fix `ISAC_UL_UCI_SAFE=1`: footprint capped at 25 % of the coded bits' REs, and a sweep win only *learns* the footprint (grant stays CRC_FAIL, counter `unconfirmed`); only a pass at a learned footprint (one trial) is reported. With it: 0 false passes in 2 runs (24k-47k UCI trials); one learned footprint still produced two zero-TB "rescues" (excluded by the existing zero-TB guard).

**(d) 2-layer MMSE.** `nr_mmse_2layers` (`nr_compute_llr.c:2758`, called at `nr_ulsch_demodulation.c:417` for 2 layers and QAM256 only; <= 64QAM uses ML LLR). Term by term: the diagonal load `noise_var>>shift` is added to a and d before `nr_det_HhH` (line 2926-2940), so `determ_fin = det(G+sI)`; the output is `d*z0 - b*z1` = `adj(G+sI) H^H y`, whose wanted-symbol coefficient is `det(G+sI) - s*(G_jj+s)`, while both layers' thresholds `ch_mag/b/c` are `det(G+sI)*QAM_n` (one shared value for both layers). That is **the same inconsistency QAM-FIX found on the DL** (shrunken output vs. unshrunken thresholds, no per-layer reliability). **Present, but unreachable in this receiver**: `nr_pdcch_blind_extract_01` rejects any non-zero precoding code point and `blind_ul_finish` rejects nrOfLayers > 1 ("multi-layer PUSCH not supported"). Not fixed: the function lives in the shared `nr_compute_llr.c` (not an owned file) and has no passive caller. If multi-layer UL is ever enabled, port QAM-FIX mode 2.

**(e) Solver poisoning / inherited IDs.** Per-UE: IDs are keyed by identity ticket (rnti, identity_gen, incarnation, epoch) in `nr_pusch_passive_ul_ids.c` and `ue_for()`; probe calls suppress confirmation; SOLVED needs a CRC pass on a different DCI hash and slot (2^-32 for 32-bit-CRC TBs; note for 16-bit CRC one-CB probes the bound is 2^-32 only if the CRC is 16 bit twice, which is what the code assumes). Priors (PCI, DL PDSCH ID, DL PDCCH ID) are ranking hints. Cell-global `prior_*` (`nr_pusch_solver.c:313`) is applied to every UE as a *probe*, never as a confirmation: harmless. Reset on identity/dedicated epoch change (`nr_pusch_solver_reset`). **New real defect (e2)**: with `ISAC_UL_PUSCH_SOLVER=1` the queue's solver branch consumed every ticketed grant and then `continue`d; while unsolved it probes only `one_cb_qpsk()` grants (<= 48 PRB, QPSK, one CB), so the 3-PRB MCS 17-27 grants (the only detectable PUSCH here, 64/256QAM) were never fully decoded. Fix `ISAC_UL_SOLVER_FALLTHROUGH=1`: a grant outside the probe class for which the solver issued no full decode also takes the ordinary full-decode path. (Without the fix the recommended "CHAIN=1 + SOLVER=1" env could not decode them.)

**(f) rb_offset.** `dci_nr.c:242` `nr_pdcch_coreset_group_rbs()` is called by `nr_pdcch_extract_rbs_single()` (line 318) from `nr_rx_pdcch_symbol()`, the single PDCCH symbol path of the blind monitor for all formats; DCI 0_1 candidates are decoded from the same LLRs, so the fix applies to 0_1 detection too. Flag `ISAC_PDCCH_RBOFFSET_FIX` is **default off**; `t26l.env` has it set. The UL env below inherits t26l.env, so it is on.

## 3. Replays (UL chain)

Common: `ISAC_UL_PUSCH_CHAIN=1`, t26l baseline. Capture b. "queue full" = ring overflow drops.

| run | config | UL DCIs admitted | full-decode results (CRC ok) | queue queued / decoded / full / stale | worker CPU (core) | notes |
|---|---|---:|---|---|---|---|
| b_ft | blind U3 + SOLVER + FALLTHROUGH + RETRY | 5208 | 1646 (0) | 3146 / 1528 / 704 / 157 | 27.6 s / 81 s = 0.34 | 354 solver probes, 0 CRC; `locked=0`, `bad=1525` |
| f_hypA (v1) | explicit layout A, chain without `hyp` ticket | 3280 | 0 (queue 0: `no_hyp=2980`) | 0 | 0.0008 | exposed that explicit mode needs a non-zero `hyp_generation` (fixed) |
| f_hypA | explicit layout A (len 44, BWP 1+216, TDRA 0 k2=2 S0/L14, DM-RS add-pos 1, ant 3 bits, qam64) | 2988 | 2031 (**1**, false pass) | 2516 / 1843 / 507 / 157 | 26.6 s = 0.33 | mean 13.1 ms/grant |
| f_hypA2 | same, rerun | 2962 | 2068 (**2**, false passes) | 2518 / 1907 / 541 / 70 | 19.3 s = 0.24 | non-deterministic counts run to run (DL discovery timing) |
| g_hypA1 | same | 1832 | 1410 (0) | 1579 / 1334 / 190 / 53 | | |
| f_hypB | explicit layout B (DAI1, ant 2, CSI 2) | 1666 | 1324 (0) | 1373 / 1194 / 128 / 51 | 0.17 | |
| f_hypAq | layout A with qam256 | 1348 | 1126 (0) | 1031 / 939 / 73 / 18 | 0.09 | |
| h_hypA_safe | layout A + `ISAC_UL_UCI_SAFE=1` | 2421 | 2099 (0) | 2147 / 2017 / 86 / 41 | 0.16 | 24514 UCI trials |
| h_blind_safe | blind + SOLVER + FALLTHROUGH + RETRY(gate off by mistake: never fired) + SAFE | 3330 | 1192 (0) | 2204 / 1119 / 199 / 125 | 19.0 s = 0.24 | 1880 solver grants, 380 probes, 0 CRC; 46893 UCI trials |
| i_hypA_retry | explicit layout A + RETRY (active) + SAFE (final code) | 3582 | 3191 (0) | 3139 / 3006 / 88 / 42 | 20.8 s = 0.26 | 8661 decode calls (primary + 2 alternates), 62906 UCI trials, 0 UL_RETRY success |

* Stability: all runs reach `RAW_IQ_EOF`, no assert, no segfault, no allocation failure; the worker joins cleanly.
* Pipeline: DCI 0_1 -> `UL_FIRST_0_1` -> grant book -> queue -> snapshot -> FEP/chest/demod/LDPC -> `UL_PUSCH_CRC` runs on every config. Blind mode feeds it *wrong interpretations* (1.3); explicit mode feeds it the right ones.
* CPU: mean 6.5-15 ms thread CPU per full decode (max 652 ms with UCI exploration), worker 0.1-0.34 core. **Queue-full drops 3-22 %** (largest with explicit layout and no SAFE: the UCI sweep dominates; 2.8 % in the final configuration). Prioritising the queue by DM-RS energy is the cheap further fix, not done.
* Strongest grants (census DM-RS MRC SNR, matched to `UL_PUSCH_CRC` by RNTI and target slot over the six explicit-layout runs): e513 abs 30786 (12.3 dB, MCS 21) and abs 30646 (9.4 dB, MCS 24) reached the decoder in 5/5 runs and failed the segment CRC every time; 2152 (MCS 18-19 at 11.6, 11.3, 10.1, 7.4 dB), 3383 (MCS 27, 9.9/6.9 dB), ac03, d093, 5d93 also failed whenever they were decoded. The two strongest, 5aea (16.4 dB MCS 23, 11.9 dB MCS 24), **never reached the decoder in any explicit-layout run** (the DL side had not locked that RNTI in time, so the DCIs were not admitted; the blind run b_ft had 246 DCIs and 99 results for 5aea but under wrong interpretations). The best DM-RS SNRs are below what MCS 18-24 64QAM needs (about 15-24 dB), consistent with ULDBG. Wrong rate matching (PT-RS absent, UCI amount unknown) cannot be excluded for them, but nothing in the data demands it.
* n3 (3 s capture): PBCH/SIB1 lock, 31 CSS accepts, DCI 1_1 length lock for rnti 0xe402 only at the very end (3 JOINT_ANON hits); zero C-RNTI DCIs, **0 UL DCIs, 0 PUSCH**. The 3 s of data is shorter than the DL discovery needs; the UL chain was idle (0.0006 core). No UL result for n3.

## 4. Sporadic CRC: no

Zero credible TB CRC. The 3 observed passes are false passes of the 16-bit TB CRC under the UCI sweep (rate ~ trials x 2^-16: 2 passes per ~2000 grants with hundreds of thousands of trials), with noise-level pilots and zero TB prefixes. `ISAC_UL_UCI_SAFE=1` removes them. A real sporadic decode would need a grant above ~15-20 dB DM-RS SNR with the right layout; the capture has one grant at 16.4 dB (MCS 23, rnti 5aea) and it never reached the decoder in the explicit-layout runs (RNTI not DL-locked in time).

## 5. Flags added (all default off, flag-off path unchanged)

| flag | file | effect |
|---|---|---|
| `ISAC_UL_DCI_LOG=1` | `nr_pdcch_blind_monitor_rt.c` | one compact `ULDCI` line per admitted UL DCI (below) |
| `ISAC_UL_SOLVER_FALLTHROUGH=1` | `nr_pusch_passive_queue.c` | non-probe-class grants still get the ordinary full decode under the solver |
| `ISAC_UL_RETRY=1`, `ISAC_UL_RETRY_MAX_RB` (12), `ISAC_UL_RETRY_SNR_DB` (off) | `nr_pusch_passive_queue.c` | retry CRC-failed full decode with other CDM count and S1/L13 |
| `ISAC_UL_UCI_SAFE=1` | `nr_pusch_passive_decode.c` | cap UCI sweep at 25 % of REs, sweep wins only learn footprints |
| `ISAC_UL_FORCE_OPTS=1` | `nr_pdcch_blind_monitor_rt.c` | EXPERIMENT: read UL DCIs with the explicit `pdcch_blind_monitor_ul_*` layout (not blind; label results "explicit-layout assisted") |

Also: `UL_PUSCH_CRC` now appends `snr_db delay seg tbs cdm tb` (always-on, log only; `snr_db` is 0 in this path).

### ULDCI line format (for `tools/ota_stats.py`)

```
[PHY]    SENSING: ULDCI rnti=0x37f3 abs=16837 fs=841.17 fmt=0_1 len=42 k2=2 rb=5+130 mcs=2 ndi=0 rv=0 pid=0 tda=8
```
`rnti` C-RNTI (hex); `abs` source absolute slot of the DCI (mu=1: 20 slots/frame); `fs` frame.slot of the DCI; `fmt` `0_1` or `0_0`; `len` DCI payload bits; `k2` slots to the PUSCH (PUSCH slot = abs + k2); `rb` absolute start+length in PRB (BWP start added); `mcs ndi rv pid tda` as decoded (TDRA row index). Fields are the receiver's own reading: in blind mode the layout may be a hypothesis (so `rb/mcs/tda` can be wrong while `rnti/abs/len/fs` are exact); count UL UEs by distinct `rnti`, activity by `abs`, grants/s by lines, and use `len`+`pid`/`ndi` for HARQ statistics. About 350 lines/s at this cell's rate. `ISAC_UL_DBG_LOG=1` additionally prints the raw payload (`ULDBG_DCI ... payload=0x..`); `tools/uldbg/dci_fields.py` consumes it.

## 6. OTA env to enable the UL chain (orchestrator)

Base: `ota/t26l.env` unchanged (it already has `ISAC_PDCCH_RBOFFSET_FIX=1`, `ISAC_RX_MRC_MODE=1`), config `ue.passive.ota.t26c.dgx.cfg` plus, in `sensing = {...}`, `pdcch_blind_monitor_ul_uci = "1024:11:0:32";` (without it no UCI recovery; with `ISAC_UL_UCI_SAFE=1` it is safe). Add:

```
ISAC_UL_PUSCH_CHAIN=1 ISAC_UL_PUSCH_SOLVER=1 ISAC_UL_SOLVER_FALLTHROUGH=1 ISAC_UL_RETRY=1 ISAC_UL_UCI_SAFE=1 ISAC_UL_DCI_LOG=1
```
(`ISAC_UL_PUSCH_SOLVER=1` implies the chain.) Optional diagnostics: `ISAC_UL_DBG_LOG=1` (raw payloads), `ISAC_DCI_RAW_LOG=1` (DL raw payloads). Do **not** set `ISAC_UL_FORCE_OPTS` on air unless running the explicit-layout experiment, which also needs `pdcch_blind_monitor_dci01 = "1:44"`, `_ul_bwp = "1:216"`, `_ul_tda`, `_ul_dmrs = "0:1:1"`, `_ul_misc`, `_ul_dci_bits` as in `tests/passive_rx/dgx/ue.passive.ota.ulready.hypA.replay.cfg` (these values come from the receiver's own UL census of this cell, not from a gNB log; they are specific to PCI 415 at this position). Expect: `UL_FIRST_0_1`, `UL_PUSCH_CRC`, `PUSCHQ`, `UL_PIPELINE worker_cpu_ms` lines; queue-full drops of several percent; ~0.25-0.35 core for the UL worker.

## 7. Files

Code: `nr_pusch_passive_queue.c`, `nr_pusch_passive_decode.c`, `nr_pusch_passive_monitor_rt.c`, `nr_pdcch_blind_monitor_rt.c` (all `openair1/PHY/NR_UE_TRANSPORT/`). Tests: `tests/nr_pdcch_blind_monitor_test.cc` (`UlReadyDci01.*`). Tools: `tools/uldbg/dci_fields.py`, `tests/passive_rx/ulready_replay.sh`, configs `tests/passive_rx/dgx/ue.passive.ota.ulready*.replay.cfg`. Replay logs: `replays/replay_*` in the worktree (untracked).

## 8. Open items

1. Blind UL layout/BWP convergence (1.3): run the bit-statistics resolver on the raw payload bank without a BWP. This is the single blocker for a blind first PUSCH, independent of link budget.
2. Prioritise the PUSCH queue by DM-RS energy / drop the weakest grants first.
3. `snr_db` is not set in the PUSCH decode path; an SNR gate for the retry needs it.
4. A position/antenna with >= 15 dB more UL gain and a working RX2 (ULDBG).
