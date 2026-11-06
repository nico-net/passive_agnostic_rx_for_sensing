# Stitching narrow same-slot DL grants into a wideband CFR: investigation (2026-11-06)

Branch twomodes/single1, build of the s1 campaign (binary 20:34, worktree HEAD 0ecddf993c). Investigation only: no receiver code, no new env/CLI/-D flag, no per-cell constant. Cells are never merged: 415 (Vodafone, 217 PRB) and 923 (TIM, 217 PRB), single PCI, sensing mode, X410 34B2AF1, 4 RX, gain 38, DGX GB10.

Evidence labels: [MEASURED OTA] = measured in the runs named; [OTA LOW LOAD] = captured 23:13-23:30 (night, low traffic), so counts of multi-user slots are lower than daytime, geometry statistics are not load-dependent; [SYNTHETIC-GT] = real CFR cut and corrupted by us, truth known; [INFERRED]; [NOT TESTED].

## 1. Answer in four lines

1. Same-slot stitching almost never creates a wideband record that did not exist: it adds 0.0-0.6 points to the 25 ms coverage on both cells (section 2). [MEASURED OTA, records of s1_415N1-5, s1_923N1-5]
2. Physically, H*W_u of different users is not one channel. On 415 the antenna-signature (W-invariant check) is NOT preserved across the seam between two users (coherence 0.48-0.62 vs 0.87-0.93 for the same-user edge control), so no scalar alignment can make it one channel. On 923 (rank 1) the signature is preserved and a per-segment scalar is enough to recover the delay profile, but only if the segments touch (gap 0). [MEASURED OTA, section 3]
3. The only alignment that needs no side information (boundary continuity, gap 0) recovers a known scalar to 0.02-0.08 rad / 0.2-0.5 dB on real wide grants cut in two [SYNTHETIC-GT], and on real 923 multi-user chains lowers the delay-profile distance to the CSI-RS truth from the random-phase null 0.18 to 0.12 (floor of a single segment 0.09). It is a modest, not a full, recovery. [MEASURED OTA]
4. Recommendation: do not build a stitched CFR record now. Gain is negligible, the 415 case is physically invalid, the 923 case works only for touching segments. If the idea is pursued, the better lever is the cross-slot pooling of a static channel with alignment by overlapping PRBs (section 6), [NOT TESTED].

## 2. Opportunity (part A) [MEASURED OTA, existing records, no new run]

Tool: tools/stitch/opp.py, output tools/stitch/opp_N.jsonl. Per DL slot, per user, the PRBs of the CFR actually exported (valid_mask of the pdsch_dmrs / pdsch_data rs records); union over users; "single wide" = one user with >= 128 valid PRB; "stitched-only" = union >= 128 with no single user >= 128. Coverage bins of 50/200/2000 slots (25 ms/100 ms/1 s), fraction of bins with a wide slot; first value = single wide only, second = with stitched-only slots added. All runs are the NEW (N) arm of the s1 campaign, one row per run, never summed. The run N5 on both cells is a higher-load run.

415 (Vodafone):

| run | slots with single wide CFR | stitched-only slots | 25 ms cov | 100 ms cov | 1 s cov | users / stitched slot p50/p90/max | contiguous runs p50/p90/max | gap PRB p50/p90/max |
|---|---|---|---|---|---|---|---|---|
| N1 | 4061 | 48 | 0.108 -> 0.109 | 0.215 -> 0.218 | 0.616 -> 0.625 | 2/3/3 | 1/7/8 | 16/32/80 |
| N2 | 8900 | 281 | 0.216 -> 0.220 | 0.359 -> 0.365 | 0.673 -> 0.677 | 2/2/3 | 4/5/13 | 16/32/80 |
| N3 | 9483 | 74 | 0.161 -> 0.162 | 0.275 -> 0.275 | 0.747 -> 0.747 | 2/2/3 | 2/5/8 | 16/32/80 |
| N4 | 5288 | 28 | 0.159 -> 0.160 | 0.313 -> 0.314 | 0.677 -> 0.682 | 2/2/3 | 4/6/15 | 16/32/48 |
| N5 | 54365 | 639 | 0.561 -> 0.562 | 0.750 -> 0.751 | 0.955 -> 0.955 | 2/2/3 | 5/19/34 | 2/20/80 |

923 (TIM):

| run | slots with single wide CFR | stitched-only slots | 25 ms cov | 100 ms cov | 1 s cov | users / stitched slot p50/p90/max | contiguous runs p50/p90/max | gap PRB p50/p90/max |
|---|---|---|---|---|---|---|---|---|
| N1 | 639 | 23 | 0.052 -> 0.054 | 0.175 -> 0.181 | 0.791 -> 0.809 | 2/3/3 | 3/8/14 | 3/16/80 |
| N2 | 1261 | 39 | 0.074 -> 0.076 | 0.212 -> 0.217 | 0.779 -> 0.784 | 2/2/3 | 2/5/12 | 5/80/80 |
| N3 | 5154 | 175 | 0.224 -> 0.230 | 0.526 -> 0.533 | 0.943 -> 0.943 | 2/3/4 | 2/6/25 | 2/64/83 |
| N4 | 3605 | 28 | 0.114 -> 0.115 | 0.237 -> 0.238 | 0.640 -> 0.644 | 2/2/4 | 1/8/21 | 2/32/80 |
| N5 | 1222 | 19 | 0.035 -> 0.035 | 0.089 -> 0.091 | 0.415 -> 0.415 | 2/2/2 | 1/5/17 | 3/16/32 |

Findings:
- Stitched-only slots are 0.5-3.6 % of the slots that already have a wide record (415: 28-639 slots; 923: 19-175 slots per run). The coverage gain is at most +0.006 (25 ms), +0.007 (100 ms), +0.018 (1 s). [MEASURED OTA]
- A stitched slot has 2 users at the median, at most 4. The union is in median 1-5 contiguous runs (up to 34), with gaps between runs of 16-32 PRB at the median and up to 80-83. The share of stitched slots whose union is one contiguous run (all users touching) is 4-52 % on 415 and 35-79 % on 923 (N1-N5, in the table order of the runs); the rest are fragmented. [MEASURED OTA]
- The users of one slot share the DM-RS symbol positions in 100 % of the stitched-only slots (415: 48/48, 281/281, 74/74, 28/28, 638/639; 923: all) as the symbol-level coverage of the same grant allocation; layers differ between the users in 45-94 % of them (layer 0 of each is exported). [MEASURED OTA]
- Same numbers when the grants proven by crc==1 are added (potential, not only exported CFR): 16-299 stitched-only slots, +0.000-0.006 on 25 ms coverage; excluding SI-RNTI grants shifts the single-wide 25 ms coverage by +0.005 (checked on 415 N1 only). [MEASURED OTA]
- For reference only (it is not same-slot stitching): the union of CFR PRBs pooled over a 25/100 ms bin would reach >= 128 PRB in clearly more bins, e.g. 923 N1 100 ms 0.175 -> 0.908, 415 N1 100 ms 0.215 -> 0.746 (tools/stitch/opp_N.jsonl, key xslot_pool_ge128_*). The pooled union mixes users and time; it is an upper bound on a cross-slot idea, not a result. [MEASURED OTA]

## 3. Coherence (part B) [MEASURED OTA, OTA LOW LOAD]

Captures: 3 x 120 s per cell with the same tune/gain/env as the s1 campaign (tools/stitch/capture.sh, config tests/passive_rx/sensing/sensing_stitch.dgx.cfg = sensing_ota.dgx.cfg + cfr_path + sources csi_rs,pdsch_dmrs). Runs: /home/nicola/NICOLA/ota_runs/stch_415_{1,2,3}_1_* (PCI 415 only) and stch_923_{1,2,3}_1_* (PCI 923 only); all exit 130 (SIGINT), lock order heavy.lock then td_measure.lock, no other receiver. CFR frame format (NRCF v1): header 48 B (frame_id@8, src@16, n_ant@17, keep@20, abs_slot@24, noise@36), k u16[keep], l u8[keep], pad to 4, 4 x keep complex64; a record links to a frame by cfr_frame_id. Reader: tools/stitch/cfr.py.

What the DM-RS frame contains: the decoder-derived estimate (cfr_est decoder / decoder_seg) of layer 0, 6 comb RE per PRB (k step 2) on one DM-RS symbol, 4 RX. Within one PRB it is smooth to 0.011-0.04 rad (MAD of the phase step between neighbouring comb RE), across a PRB boundary the step has MAD 0.04-0.12 rad: the estimate is effectively per PRB, so the boundary figures contain the estimator noise as well as any precoder jump.

### 3.1 Within one user's wide grant (tools/stitch/a1_within.py, records >= 128 valid PRB)
Measure at each adjacent comb pair across a PRB boundary: dphi = phase of the 4-antenna spatial correlation, coh = |correlation| / norms (1 = same antenna signature). "frac coh<0.9" = share of boundaries with a visible change. Rows nl1 / nl>1 (layer 0 of multi-layer grants), by boundary class.

| run | boundary | nl1 n, frac coh<0.9 | nl>1 n, frac coh<0.9 |
|---|---|---|---|
| 415_1 | odd PRB / even not mult. of 4 / mult. of 4 not 16 / mult. of 16 | 4048 0.037 / 2024 0.072 / 1534 0.073 / 483 0.075 | 47908 0.042 / 23954 0.052 / 18165 0.105 / 5736 0.083 |
| 415_2 | same | 1180 0.003 / 590 0.005 / 447 0.043 / 141 0.043 | 52616 0.005 / 26308 0.048 / 19941 0.056 / 6288 0.054 |
| 415_3 | same | 7972 0.015 / 3986 0.145 / 3021 0.182 / 950 0.177 | 98588 0.020 / 49294 0.025 / 37384 0.029 / 11810 0.027 |
| 923_1 | same | 25532 0.001 / 12766 0.007 / 9686 0.007 / 3061 0.008 | 235636 0.002 / 117818 0.010 / 89244 0.010 / 28008 0.009 |
| 923_2 | same | 27936 0.000 / 13968 0.023 / 10596 0.029 / 3347 0.020 | 155456 0.001 / 77728 0.008 / 58852 0.011 / 18451 0.013 |
| 923_3 | same | 144672 0.001 / 72336 0.002 / 54839 0.003 / 17276 0.003 | 777668 0.002 / 388834 0.002 / 294667 0.004 / 92669 0.004 |

- On 415 the changes sit at EVEN PRB boundaries (a precoder granularity of 2 or 4 PRB, depending on the user and time); odd boundaries are clean, and there is no extra excess at multiples of 16 over multiples of 4. This does not reproduce the "16 RB" granularity of CSIRS-CLASS-20261105.md in this build and window; the two analyses use different statistics and I did not reconcile them. [MEASURED OTA; discrepancy open]
- On 923 the excess at even boundaries is 0-3 % and the phase step MAD does not rise at the boundaries (0.04-0.05 at every boundary class vs 0.011 inside a PRB): the precoder jump between PRGs is not visible in the 4-RX signature, as expected for a rank-1 channel. [MEASURED OTA]
- A jump is rare: even on 415 only 5-18 % of the PRG boundaries show a drop of the signature coherence below 0.9. So a single wide grant is mostly coherent and the per-PRG jump is the exception. [MEASURED OTA]

### 3.2 Between users in the same slot (tools/stitch/a2_users.py, out/a2_all_with_edge_control.txt)
Seam = last RE of the lower user, first RE of the next user, same DM-RS symbol, k gap <= 4 (touching). Edge control = same user, edge RE vs the RE one PRB inside (so edge-estimate noise is not blamed on the user change).

| run | adjacent seams | seam coh median, frac<0.9 | seam amplitude step MAD | seam phase step MAD (uniform random = 2.17) | edge control coh median, frac<0.9 |
|---|---|---|---|---|---|
| 415_1 | 70 | 0.522, 0.986 | 7.5 dB | 1.08 rad | 0.873, 0.607 |
| 415_2 | 40 | 0.618, 0.900 | 15.6 dB | 2.38 rad | 0.934, 0.356 |
| 415_3 | 61 | 0.483, 0.918 | 4.9 dB | 0.76 rad | 0.924, 0.428 |
| 923_1 | 2640 | 0.881, 0.568 | 6.1 dB | 1.77 rad | 0.966, 0.171 |
| 923_2 | 812 | 0.879, 0.578 | 7.3 dB | 1.63 rad | 0.964, 0.190 |
| 923_3 | 3989 | 0.891, 0.536 | 7.1 dB | 1.05 rad | 0.949, 0.250 |

- Two users touching in a slot are rare on 415 (40-70 seams per 120 s run) and common on 923 (800-4000).
- The seam is a complete unknown scalar between users (phase step as wide as uniform noise on 415_2, amplitude steps 5-16 dB). Alignment (any method) is required; no alignment is a null. [MEASURED OTA]
- 415: the antenna signature changes at the seam by far more than edge noise explains (0.48-0.62 vs 0.87-0.93). The channel seen through W_u is not the channel seen through W_v (no rank-1 structure at our antennas). Scalar alignment can only remove a scalar, so it cannot make the two a single channel. [MEASURED OTA; the cause (multipath beams, layer-0 definition for nl>1) was not separated]
- 923: seam coherence 0.88-0.89 vs edge control 0.95-0.97: slightly different, mostly consistent with noise plus rank 1. [MEASURED OTA]

### 3.3 Non-precoded CSI-RS reference on 923 (tools/stitch/a4_csirs.py)
CSI-RS frames: 217 PRB, 1 RE per PRB, resources fd=2 l0=5 and l0=9 (two single-port RE patterns; 7000-7100 frames per 120 s), 4 RX, SNR 18-20 dB. 415 has only 52-PRB CSI-RS frames, so no such reference there. [MEASURED OTA]
- Stationarity: the per-PRB signature coherence between two occasions of the same resource is 0.998 at 20 ms, 100 ms, 500 ms and 1 s lag, in all three runs. The channel at our position is static over at least a second (no coherence-time limit on the nearest occasion; the delay-profile distance between occasions has a noise floor 0.10-0.13, flat in the lag).
- A wide user's H*W_u (nl=1..4) against the nearest CSI-RS (<= 40 ms): per-PRB signature coherence median 0.97 (n=2569 / 1802 / 8976 for runs 1/2/3), delay-profile distance (noise floor removed, 0 = identical) median 0.06 (p90 0.15-0.17). So on 923 the antenna signature and the delay profile of H*W_u are the same as H's: W-invariant in practice (single dominant path). [MEASURED OTA]
- Consequence: on 923 a narrow single grant already gives the AoA/signature without any stitching; what a narrow grant lacks is only the delay resolution.

## 4. Alignment methods, evaluated

The delay-profile distance used below is the total variation between the normalised power-delay profiles (0 identical, 1 disjoint); delay peak error is in bins of 5.4 ns on the PRB grid.

(i) None. Real 923 chains (adjacent users, CSI-RS within 40 ms, run 3, n in the table): delay-profile distance to the CSI-RS truth 0.111-0.136 for 48-128 PRB, 0.095 for 128-300 PRB (the 128-300 PRB row includes chains containing a user already >= 128 PRB).

(ii) Per-segment scalar by boundary continuity (tools/stitch/a3_align.py `align`): a local linear fit (log amplitude and unwrapped phase vs subcarrier) over the one PRB at each side of the seam on the projection onto the left edge antenna direction, scalars chained from the lowest segment (gauge s0 = 1). Edge length M = 6 RE (1 PRB) chosen on stch_415_1, then reported on the other five runs; larger M (12-96) was worse because the linear model does not hold over wider edges (stch_415_1: phase error 0.12 / 0.22 / 0.38 / 0.54 rad for M = 12 / 24 / 48 / 96 against 0.05 for M = 6).
- Impossible without touching segments: the extrapolation across a gap fails already at 4 PRB (phase error median 0.16-0.78 rad, p90 0.4-2.1) and is no better than none at 16 PRB (phase 0.72-1.7 rad, amplitude 6-24 dB, CFR error aligned 0.45-0.69 vs none 0.49-0.66). Stated limit: only gap 0 is aligned; 415's median stitched-only gap is 16 PRB, so the method applies only to the touching minority (4-52 % of the 415 stitched slots, 35-79 % of the 923 ones). [SYNTHETIC-GT, section 5]

(iii) Per-segment scalar fitted to the non-precoded CSI-RS (923 only; needs a reference H at a nearby occasion, which the 20 ms/1 s stationarity above supports): one complex scalar per segment fitted on that segment's own PRBs; the shape that remains is an honest test, the scalar is not.

(iv) W-invariant quantities, no alignment: the per-PRB 4-RX signature. On 923 it matches the CSI-RS (0.97) for every user, so AoA is available from any single grant and needs no stitching. On 415 the signature differs between users (section 3.2), so it is a property of W_u and cannot be pooled across users. A magnitude-only delay estimate was not evaluated [NOT TESTED]; for a single-dominant-path channel (923) the magnitude carries no delay information, so it would not help there.

Real 923 chains, delay-profile distance to the CSI-RS truth on the same PRBs (tools/stitch/a5_real_stitch.py; run 3 values, runs 1 and 2 give the same ordering, see out/stch_923_*.txt). "Floor" = each segment alone against the CSI-RS on its own PRBs.

| chain size (PRB) | n | aligned (ii) | CSI-RS-fit scalar (iii) | none (i) | random scalars (null) | single-segment floor | peak error aligned / iii / none / random (bins) |
|---|---|---|---|---|---|---|---|
| 48-96 | 817 | 0.117 | 0.099 | 0.136 | 0.177 | 0.093 | 1 / 0 / 1 / 2 |
| 96-128 | 276 | 0.107 | 0.103 | 0.129 | 0.152 | 0.090 | 1 / 0 / 1 / 1 |
| 128-300 | 1057 | 0.091 | 0.084 | 0.095 | 0.112 | 0.077 | 0 / 0 / 0 / 0 |
| 128-300, stitched-only (no segment >= 128) | 186 | 0.101 | 0.097 | 0.111 | 0.129 | 0.087 | 1 / 0 / 1 / 1 |

Runs 1 and 2 for the stitched-only 128-300 row: n = 48 and 23, aligned 0.120 / 0.103, iii 0.109 / 0.094, none 0.139 / 0.117, random 0.153 / 0.138, floor 0.092 / 0.101. [MEASURED OTA]
Reading: the ordering random > none > aligned > CSI-RS-fit > floor holds in every row of every run, but all methods stay within 0.01-0.04 of the floor and the peak delay is already right to one bin without any alignment, because the 923 channel is dominated by one path. What alignment buys on 923 is the removal of the spurious delay-profile spreading that random phases create (0.129 -> 0.101 in the stitched-only row), not a new capability. A delay-profile metric that is insensitive to the scalars is the limitation of this evaluation; the CFR itself cannot be scored against the CSI-RS without refitting the scalars (circular).

## 5. Ground truth on real wide grants (tools/stitch/a3_align.py) [SYNTHETIC-GT]
Real single-user wide records (>= 128 contiguous valid PRB; n = 135 / 472 / 689 for 415 runs 1/2/3, 400 sampled per case) are cut into S segments at random odd positions (inside a PRG) or on multiples of 4 PRB (PRG boundary), each segment multiplied by a random complex scalar (amplitude 0.5-2, phase uniform), then aligned; the intact record is the truth. Errors are the worst segment scalar after fixing the gauge on segment 0, the CFR relative error after the best single global scalar, and the delay-profile distance to the intact record. Null: no alignment (i). Median over the cuts:

| cell, run | cut | segments, gap | scalar phase err p50 / p90 (rad) | amplitude err p50 / p90 (dB) | CFR rel. error aligned vs none | delay-profile dist. aligned vs none |
|---|---|---|---|---|---|---|
| 415_1 | odd | 2, gap 0 | 0.05 / 0.14 | 0.3 / 1.4 | 0.03 vs 0.57 | 0.009 vs 0.181 |
| 415_1 | odd | 3, gap 0 | 0.08 / 0.20 | 0.6 / 1.6 | 0.05 vs 0.76 | 0.015 vs 0.249 |
| 415_1 | PRG (x4) | 2, gap 0 | 0.08 / 0.25 | 0.5 / 1.8 | 0.06 vs 0.60 | 0.017 vs 0.194 |
| 415_2 | odd | 2, gap 0 | 0.02 / 0.11 | 0.2 / 0.7 | 0.01 vs 0.56 | 0.005 vs 0.194 |
| 415_3 | odd | 2, gap 0 | 0.01 / 0.02 | 0.1 / 0.2 | 0.01 vs 0.57 | 0.002 vs 0.188 |
| 923_1 | odd | 2, gap 0 | 0.03 / 0.07 | 0.2 / 0.8 | 0.02 vs 0.64 | 0.006 vs 0.244 |
| 923_3 | odd | 3, gap 0 | 0.03 / 0.09 | 0.3 / 0.7 | 0.03 vs 0.80 | 0.008 vs 0.307 |
| 415_2 | odd | 2, gap 4 PRB | 0.43 / 2.35 | 3.4 / 10.3 | 0.27 vs 0.59 | 0.089 vs 0.205 |
| 923_3 | odd | 2, gap 4 PRB | 0.16 / 0.44 | 1.5 / 4.2 | 0.13 vs 0.61 | 0.041 vs 0.236 |
| 415_2 | odd | 2, gap 16 PRB | 1.43 / 2.75 | 14 / 56 | 0.59 vs 0.54 | 0.200 vs 0.195 |
| 923_3 | odd | 2, gap 16 PRB | 0.72 / 1.87 | 6.5 / 18.6 | 0.47 vs 0.61 | 0.156 vs 0.238 |

(all cells and cases: out/stch_*.txt.) With added white noise at 10 dB / 5 dB SNR on top of the measured one (415_1, gap 0, 2 segments), the phase error rises to 0.12 / 0.22 rad and the CFR error to 0.31 / 0.50, still below the unaligned 0.66 / 0.73 (tested on the 415_1 run only). Limits of this test: both segments come from one user and one W, so no real inter-user jump is present (it measures the estimator and the method, not the physics of section 3.2); the PRG-boundary cuts include the rare real jump (5-18 % of boundaries).

Null test against a good-looking artefact: two halves from DIFFERENT real records (different user and time) are spliced and aligned the same way. The method always makes the seam continuous by construction, so the seam appearance proves nothing: the seam signature coherence is 0.46 / 0.72 / 0.73 for 415 runs 1/2/3 (versus 0.98 for a true same-user boundary), the delay profile of the splice is 0.26-0.48 from either half. On 923 the spliced seam coherence is 0.95-0.97 (rank 1 makes any two records look alike), the delay-profile distance is again 0.21-0.26 from both halves, and the seam coherence on its own is therefore not a validity test on 923; a reference (CSI-RS) or a gap-0 constraint is.

## 6. What is physically valid, and what is not
Valid:
- Per-user, per-PRB records as today (H*W_u of one user with its own provenance).
- On 923-like rank-1 cells: AoA/antenna signature from any single grant, no stitching needed (coherence 0.97 vs CSI-RS).
- On touching segments (gap 0), a stitched delay profile after boundary-continuity alignment, labelled as such; accuracy as in sections 4 and 5; the validity condition is a signature coherence at the seam comparable to the edge control (415 fails it by a wide margin; 923 passes it on average, 54-58 % of seams fall below 0.9 against 17-25 % of the controls, so the check has to be per seam).
Not valid:
- A coherent channel H assembled from different users' H*W_u without a proof of alignment. No such proof exists on 415 (signature changes), and on 923 the alignment is proven only for touching segments against a CSI-RS reference.
- Any stitched CFR across a gap >= 4 PRB (the scalar cannot be recovered: section 5).
- Any use of the stitched delay profile as H on a cell with no rank-1 evidence. Even a perfect scalar alignment would give a channel weighted per path by each user's W_u, which is not the cell channel unless it is single-path or the W_u agree.

## 7. Recommended design (if pursued): "stitched" record
Not recommended for the same-slot case at the measured gain (section 2). If the cross-slot variant is wanted, it should look like this:
- New record type `rs` with source `stitched`, never replacing pdsch_dmrs records; each carries `stitch_segments`: list of {cfr_frame_id, rnti, abs_slot, start_prb, n_prb, nl, scalar_re, scalar_im, align_method, seam_coh}, so the original per-user frames remain the data and the scalars are metadata; `evidence`: `aligned_gap0` / `aligned_ref_csirs` / `unaligned`; `valid_cfr_prbs` counts only aligned PRBs; `wideband` false unless the aligned run is >= 128 PRB contiguous.
- Alignment: gap 0 only, (ii); reject a seam below the edge-control signature coherence; reference-based (iii) only where a full-band non-precoded CSI-RS exists in the last N slots (923 only today).
- Cost [INFERRED, not measured]: the fit is O(24 RE x 4 RX) per seam, negligible next to the existing decoder-side CFR work; an extra frame per stitched slot (the aligned CFR, up to 217 PRB x 6 RE x 4 RX x 8 B = 41 kB for one DM-RS symbol) at 28-640 stitched slots per 210 s; no new sensing-API field if provenance goes in the record. The cost to the receiver is the retention of per-slot segments until the slot closes, which today's per-record path does not do.
- Better lever [NOT TESTED]: pool across slots for a static channel (923: coherence 0.998 up to 1 s) and align users through overlapping PRBs seen at different times (overlap gives the scalar ratio directly, without extrapolating over a gap); the pooled PRB union reaches >= 128 in 0.47-0.91 of the 100 ms bins on 923 (N1-N5) and 0.62-0.85 on 415 against 0.09-0.53 / 0.22-0.75 for single wide grants (opp_N.jsonl). On 415 the signature changes between users, so this works on 923-like cells only.

## 8. Open risks
- Night capture (23:13-23:30): fewer multi-user slots; the 415 seam counts (40-70 per run) are small. The geometry statistics (coherence, jump) should not depend on load, the seam counts do. [OTA LOW LOAD]
- Layer 0 is exported for nl>1; whether layer 0 of user A and of user B are the same beam is not established; the 415 seam incoherence may partly be this.
- The decoder-derived estimate is per PRB and smooth within it; the synthetic test shows the best case of continuity, real different-user seams have the estimator noise of two different decoders.
- The "PRG 2/4 PRB" finding on 415 contradicts the earlier 16-RB statement; unresolved.
- M = 6 was selected on stch_415_1 (same family of data as the evaluation) and then reported on five other runs; the added-noise sensitivity was checked on one run only.
- The delay-profile metric is insensitive on a single-path channel (923); no multipath cell with a non-precoded full-band reference exists in this data set.
- CSI-RS frames are two single-port RE patterns on 923; a multi-port true H (all 32 ports) was not reconstructed.

## 9. Files
Scripts: tools/stitch/{cfr.py, load.py, opp.py, a1_within.py, a2_users.py, a3_align.py, a4_csirs.py, a5_real_stitch.py, capture.sh}; outputs: tools/stitch/opp_N.jsonl, tools/stitch/out/*.txt; config: tests/passive_rx/sensing/sensing_stitch.dgx.cfg; raw captures (CFR frames and records, 0.2-0.5 GB per run, not committed): /home/nicola/NICOLA/ota_runs/stch_{415,923}_{1,2,3}_1_*. The sens6 frozen gate was checked (no diff).
