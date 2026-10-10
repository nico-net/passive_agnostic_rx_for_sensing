# UL-LAYOUT: blind UL BWP / DCI 0_1 layout lock without PUSCH CRCs (csd/ul-layout, 2026-10-10)

Host: DGX GB10 (aarch64), worktree `rr-csd-ull`, branch `csd/ul-layout` (from `csd/otatest` 28e4451001). Every build, test, replay and offline census went through `heavy.sh`, one at a time. No radio, no hardware. Replays: `tests/passive_rx/ulready_replay.sh` (t26l.env minus radio-only flags, CORESET fixture `scanfast_coresets.txt`, speed 0.25, ISAC_MLOCK=0). Captures are reported separately: **b** = `raw_pci415_12s_b` (CAP_TICK 208029025), **old** = `raw_pci415_12s_20261008` (CAP_TICK 210036277). Labels: [MEASURED, replay, DGX] unless stated.

## Verdict

| Item | Result |
|---|---|
| Lock converges to the true UL BWP | **yes: 216 PRB @ CRB 1** on capture b, in 3 of 3 runs of the final binary (b5, b6; b3 with the earlier vote rule), never to the DL BWP 217@0 |
| Field widths | id 1, BWP indicator 1, RIV 15, TDRA 4 (row 0, k2 = 2), FH 0, MCS/NDI/RV/HARQ at bit 21; len 44 tail = DAI 2, TPC 2, ant 3, SRS 2, DM-RS init 1, UL-SCH 1 (CP-OFDM, 1 CDM group without data), DM-RS additional position 1 |
| CDM ambiguity | **settled physically, no CRC**: the second comb of the DM-RS symbols carries data energy (cdm1 votes 5-9, cdm2 0 per run) -> 1 CDM group -> reading A for len 44; reading B (2 CDM groups) kept as the logged runner-up |
| ULDCI after the lock | rb/k2 of **every** post-lock ULDCI = the 216@1, k2=2 reading of its raw payload (713/713 b5, 354/354 b6, 751/751 b3); census-strong grants decoded after the lock sit on the census PRBs (3/3 b5, 4/4 b3) |
| Convergence | 7.7-8.4 s after the first DCI 0_1 (capture frames 1084-1150 of 1199): set by when this position first shows detectable PUSCH DM-RS (ULDBG: all strong grants are in frames > 1040). Bit stage (fit) at ~256 DCIs, 1.2-2.1 s after the first DCI of that length |
| False lock | none: wrong-slot-DM-RS control 0 votes in 2399 and 4287 probes, no lock; old capture (no detectable DM-RS) no lock in 2 runs; true runs 18 strong detections in ~2500 probes |
| PUSCH CRC | still 0 (link budget, ULDBG), not required by the lock |
| Extra (scope addition) | UL 2-layer MMSE bias fixed behind `ISAC_PUSCH_MMSE_MODE=2` (separate commit, section 6) |

## 1. Method (`nr_pdcch_ul_layout_lock.c`, flag `ISAC_UL_LAYOUT_LOCK=1`, default off)

Stage 1, bits, per DCI length, on the bank of CRC-verified C-RNTI DCI 0_1 payloads (1024 per length, fitted once at 256; eval thread every 100 ms, never on the PDCCH thread):
- **MCS/HARQ offset**: for every offset, fraction of consecutive same-RNTI grants whose HARQ pid advances by 1..3 (mod 16) plus fraction of RV = 0. True offset 21 scores 1.73-1.88, the runner-up (one bit off) 1.17-1.45 [MEASURED, b and old]. A constant region scores 0 in the first term (fixed after a first version that counted step 0).
- **Prefix split** (BWP ind | RIV | TDRA | FH): every split with constant BWP-indicator bits, a high-entropy RIV LSB and >= 98 % RIV validity for some candidate N is kept as a hypothesis (up to 8; 2-5 survive here, e.g. 1/15/4/0 and 2/14/4/0). Their RIV widths define which N enter stage 2.
- **Tail split**: every spec-legal split (DAI1 1/2, DAI2 0/2, SRI 0-2, CP ant 3/4 or TP ant 2/3, SRS 2, CSI 0-6, beta 0/2, init) that reads >= 97 % of the bank through `nr_pdcch_blind_extract_01()`; ranked by a configuration prior (plain configuration first), kept up to 4. The non-spec "CP with 2-bit antenna ports" reading (ULREADY B) is allowed but penalised.
- **k2 per TDRA index**: smallest k2 that moves >= 95 % of the DCI slots onto slots that never carry an accepted PDCCH (DL or UL format). Slots 6/7/16/17 -> 8/9/18/19, k2 = 2. A first threshold (2 % of the busiest slot) called the special slot 7 free on the old capture (173 UL DCIs vs ~20k DL accepts) and gave k2 = 1; fixed (>= 3 DCIs and 0.2 %), unit-tested.

Stage 2, physics, cell-wide. Candidate BWPs: N in {carrier-0..5, DL BWP-0..5, standard sizes} with a fitted RIV width, start 0..5, inside the carrier (63 hypotheses here: 212-217, 189, 160/162, 133/135, 106/107). The DL BWP is only one member of the set and gets no prior. Each grant of a fitted length is booked as a **probe** at its PUSCH slot (DCI slot + k2). The PUSCH worker (`nr_pusch_passive_layout_probe()`, nr_pusch_passive_decode.c) FFTs symbols l0, 7, 11, l0+1, 12 of the slot snapshot and computes, for every hypothesis' PRB range (<= 16 PRB), the CP-OFDM DM-RS MRC SNR (PCI-scrambled Gold, port 0, comb 0, CRB-0 reference, per-PRB coherent with a +-256-sample delay search, same estimator as `tools/uldbg/ulphys.py dmrs_est`). No decode, no CRC. Votes and lock:
- a probe votes when its best hypothesis reaches 6 dB;
- **degenerate mappings vote for nobody**: for an L-PRB grant, (N, s) and (N-+1, s-+(L-1)) give identical PRBs; a detection there cannot separate them. With "first in list" tie-breaking (run b4) the families 215@2/214@0 and 214@3/213@1 collected 4 + 2 systematic votes and the lock stalled at 7 vs 4; with this rule the runner-up drops to 1;
- collision guard: two decoded grants of the same slot whose detections fall on the same PRBs (under the vote's, the leader's or the other grant's best hypothesis) are the same signal, both votes dropped (all UEs share the PCI scrambling);
- lock: leader >= 6 votes, >= 2x the runner-up, >= 2 distinct RNTIs. Then each fitted length is built into a `nr_pdcch_blind_ul_opts_t` and every later DCI of that length is read with it (`hyp_generation = NR_UL_LAYOUT_TAG`), booked to the normal chain (DL-lock ticket still required). Lengths fitted after the lock are locked on fit.
- The same probes measure (strong detections, >= 8 dB) the comb-1 RE energy of the DM-RS symbols against the data-symbol RE energy: about 0 dB = data in the second comb = 1 CDM group without data; << 0 = 2 groups. The tail whose decoded CDM count matches the majority is chosen, the other is logged as runner-up. Per-symbol SNR of symbols 7 and 11 votes the DM-RS additional position (default 1). Gold detections per length are CP-OFDM evidence (needs >= 5 and above the control rate) against TP readings.

Before the lock, with the flag on, only probes reach the PUSCH worker (U3/width readings are still logged as ULDCI but not decoded). Without the flag nothing changes.

Log lines: `ULLAYOUT FIT` (per length: offset score, prefixes, best/next tail, k2), `ULLAYOUT BWP_LOCK` (votes, runner-up, others, control votes, discarded, tied, add-pos votes, first/lock abs slot, t_conv_ms, confidence), `ULLAYOUT LOCK len=..` (BWP, widths, tail, runner tail, CDM evidence), periodic `ULLAYOUT STATS`. Diagnostics: `ISAC_UL_LAYOUT_LOG=1` (one `ULLAYOUT_PROBE` line per probe), `ISAC_UL_LAYOUT_CONTROL=1` (validation only: every hypothesis scored with the wrong slot's DM-RS).

## 2. Offline evidence that drove the design [OFFLINE VERIFIED, capture b, `tools/uldbg/bwp_hyp.py`, ULDCI payloads of replay b_ft, frame offset 487]

DM-RS MRC SNR of the <= 8-PRB grants (4067) at the PRBs each hypothesis predicts:

| hypothesis | > 3 dB | > 6 dB | > 10 dB |
|---|---:|---:|---:|
| 216@1 | 24 | 11 | 6 |
| 216@2 / 216@0 (1-PRB shift, 2/3 overlap) | 22 / 14 | 9 / 9 | 3 / 4 |
| 215@1 / 215@2 / 214@2 | 17 / 15 / 9 | 7 / 6 / 4 | 2 / 1 / 3 |
| **217@0 (the DL BWP)** | **0** | **0** | **0** |
| control (216@1, two frames later) | 1 | 1 | 0 |

Strong detections under wrong hypotheses are other UEs' PUSCH: e.g. dbc2 abs 31277 under 215@2 lands on PRB 136-138, exactly where 2152 sits under 216@1 in the same slot. Under 215@2 every L-PRB grant moves by exactly L PRB, i.e. onto the next contiguously packed allocation: that is the structural confusion the degenerate-mapping and collision rules address.

CDM groups [OFFLINE VERIFIED, `ulphys`, energy rel. noise of the same PRBs two frames later]: on the strongest grants the second comb of the DM-RS symbols is as loud as the data: 5aea 16.4 dB pilot 10.9 / comb-1 10.4 / data 11.1 dB; e513 12.3 dB 6.8 / 6.6 / 7.4; 2152 11.6 dB 5.5 / 4.8 / 5.4; 3383 9.9 dB 4.4 / 3.4 / 4.5. Data in the DM-RS symbols => 1 CDM group without data => reading A (ant `000`), not B. Symbol 0 is occupied (S = 0) and symbol 13 too (L = 14).

Length 42: 3 of 918 grants > 3 dB Gold DM-RS (all 37f3), 0 > 6 dB, control 0: too weak to decide the waveform. The spec-consistent reading is TP (a CP-OFDM 42-bit tail does not fit TS 38.212 with a 3-bit antenna field), so the lock keeps A42-TP with the CP reading as runner-up. [INFERENCE]

## 3. Replays, capture b (final binary unless noted)

Env: UL chain flags of ULREADY + `ISAC_UL_LAYOUT_LOCK=1 ISAC_UL_LAYOUT_LOG=1 ISAC_UL_DBG_LOG=1`. Check: `tools/uldbg/ullayout_check.py run.log`.

| run | lock | votes leader / runner | first DCI 0_1 -> lock | probes | post-lock ULDCI = 216@1/k2=2 reading | CDM votes len 44 | add-pos votes |
|---|---|---|---|---:|---|---|---|
| b5 | 216@1 at abs 31719 (capture frame 1098) | 6 / 1 (212@4) | 7.66 s | 3144 | 713 / 713 | cdm1 5, cdm2 0 | 0/8/0 -> 1 |
| b6 | 216@1 at abs 32759 (frame 1150) | 6 / 1 (217@0) | 8.43 s | 2335 | 354 / 354 | cdm1 8, cdm2 0 | 1/9/0 -> 1 |
| b3 (earlier vote rule) | 216@1 at abs 31439 (frame 1084) | 6 / 3 (215@2) | 7.96 s | 2498 | 751 / 751 | cdm1 9, cdm2 0 | 0/15/0 -> 1 |
| b4 (earlier vote rule) | **none** (7 vs 4 at EOF) | - | - | 2771 | - | cdm1 13 | - |
| b2 (first version) | 216@1, then crash (section 5) | 6 / 3 | 7.93 s | 3971 | - | cdm1 14 | 10/10/0 (tie, fixed) |

Run-to-run variation comes from DL discovery timing (which RNTIs are admitted when; ULREADY saw the same). Census cross-check (b3/b5): every census-strong grant whose DCI was admitted after the lock appears in ULDCI with exactly the census PRBs (b3: 115+3, 65+3, 191+3, 176+3; b5: 3 of 3). Post-lock the normal chain decodes (UCI-safe, retry); 0 TB CRC as expected from the link budget. Pipeline: pre-lock probes cost ~1-1.3 ms thread CPU each, 0.04-0.06 core, 0 queue drops; post-lock decodes bring the worker to 0.07-0.12 core with queue-full drops 0-140 of 2400-3500.

Bit stage on capture b: len 44 fitted at 257 DCIs (1.2-2.1 s after its first DCI), len 42 at 256; both mcs_off 21, prefix {1, 15, 4, 0} among the survivors, k2 2.

## 4. False-lock analysis

- **Noise** [MEASURED, b, `ISAC_UL_LAYOUT_CONTROL=1`, runs ctl1 and ctl2]: the best of 63 hypotheses with the wrong slot's DM-RS reached 6 dB in 0 of 2581 logged probes (>= 4 dB: 21); ctl2: 4287 probes, 0 votes, no lock. Real probes on the same capture: 18 of 2498 >= 6 dB (14 >= 8, 11 >= 10). Per-probe noise vote rate < 0.12 % (95 % bound); a wrong lock by noise needs 6 votes on one of ~15 non-degenerate hypotheses with 2x margin and 2 RNTIs: negligible (Poisson estimate < 1e-5 per 10k probes). The in-receiver control at the argmax (ctl_votes) was 0 in every run.
- **No signal** [MEASURED, old capture, runs old1/old2]: layout fitted (len 44, same offsets/tail, k2 2 after the fix), 1771-1860 probes, best SNR max 5.4 dB, 0 votes, no lock. The receiver stays in search instead of guessing.
- **Other UEs' PUSCH** (the real risk): strong detections that belong to undecoded UEs land on wrong hypotheses. With the final rules the runner-up held 1 vote at lock time (b5, b6) against 6; before the degenerate-mapping rule it reached 3-4 (b3, b4). The 2x margin plus 2-RNTI rule did not lock wrongly in any of the 8 replays. Residual risk on a cell where one wrong hypothesis is systematically non-degenerate and packed next to a strong undecoded UE: possible, needs more runs on other cells; the BWP_LOCK line logs the runner-up and the confidence (0.86 b5/b6) to watch for it. [INFERENCE]
- A TB CRC would be a stronger tie-break for the CDM tail and could confirm the BWP; none is available here (link budget, ULDBG). The runner-up tail is logged and `ISAC_UL_RETRY=1` decodes the other CDM count.

## 5. Other changes and findings

- **Crash fixed** [MEASURED, b2]: with a TP (length 42) grant the ISAC_UL_RETRY CDM alternate (1 group) handed the gNB IDFT a half-symbol size (18 REs for 3 PRB): `Assertion sizeidx` in `oai_dfts_neon.c`, receiver exit. The retry now skips the CDM alternate for transform precoding, and `nr_pusch_passive_decode()` rejects TP with one CDM group (`UNSUPPORTED`). Only reachable with a TP reading (ULREADY's flags could reach it through U3 readings too).
- UL-DISCOVERY width/U3 feedback is skipped for layout-tagged grants (they belong to no width search; the feedback would otherwise log UL_FEEDBACK_REJECT per grant).
- Known limits: hypotheses use the first prefix with a given RIV width (a frequency-hopping variant of the same width is not scored); probes only for <= 16-PRB allocations; the BWP is assumed common to all lengths/UEs of the cell; after the lock no more probes run (no re-lock: a reset needs a new identity/dedicated epoch or a PCI change).

## 6. Scope addition: unbiased 2-layer UL MMSE (`ISAC_PUSCH_MMSE_MODE=2`, separate commit)

[CODE-READ] In `nr_ulsch_demodulation.c inner_rx()` 2-layer PUSCH uses the max-log ML detector for Qm <= 6 (no threshold bias) and stock `nr_mmse_2layers()` for 256QAM. That function outputs `adj(G + sI) H^H y` but thresholds `det(G + sI) * QAM_n`, so amplitudes shrink against their thresholds, and both layers share one threshold (the DL defect QAM-FIX fixed).

Fix: new function `nr_mmse_2layers_unbiased()` in `nr_compute_llr.c` (stock function untouched, still used by the gNB and the DL): float ZF per RE, per-layer gain `w_l = 1/[G^-1]_ll`, output `w_l x_l = z_l - (G_lm/G_mm) z_m`, per-layer thresholds `w_l * QAM_n1/n2/n3` in `ch_mag[l]/ch_magb[l]/ch_magc[l]` (on the 1-layer `|h|^2` scale, so `nr_compute_llr()` is unchanged). `inner_rx()` calls it only when `ISAC_PUSCH_MMSE_MODE=2` (read once); unset, the stock call is executed exactly as before (flag-off output identical by construction: an unchanged function behind one branch).

Unit test `test_nr_pusch_mmse2` (synthetic 2-layer 256QAM, 4 RX, through OAI's own `nr_channel_compensation()`) [MEASURED, DGX]:
- correlated layers (0.8), diagonal load 10 % of G_ll: **kappa stock 0.857, mode 2 1.003**;
- mode 2 kappa within 0.04 of 1 at correlation 0, 0.5, 0.9, and > 99 % of noiseless coordinates recovered;
- stock path deterministic and unchanged (byte compare of two stock runs).

Multi-layer UL stays **rejected at DCI parse**: a non-zero precoding code point needs TS 38.212 Tables 7.3.1.1.2-2..5, selected by the UE's SRS port count, maxRank and codebookSubset, none of which is on the air; guessing would produce a confident wrong layer count, DM-RS port set and TBS. Not lifted (not safe without that configuration). The fix therefore matters to the gNB-shared path and to any future multi-layer UL, not to this cell's current grants.

Tests [MEASURED, DGX]: test_nr_pusch_mmse2, test_dlsch_fixed_point, test_nr_llr_norm, test_llr, test_llr_no_avx_256, test_nr_pusch_ul_disc, test_nr_pusch_solver, test_nr_pdcch_blind_monitor (incl. 3 new UlLayoutLock tests), test_nr_pusch_ra0_qpsk/qam16/multidmrs/short_segments/late_typeb/type2/fdra_collision pass; test_nr_pusch_ra0_qam64 and qam256 fail (the known ARM failures in CLAUDE.md; 1-layer, not through this code). `nr_ulsim` does not link in this fork (undefined `nr_ue_diag_producer_absolute_slot`, pre-existing), so no 2-layer link-level simulation was run.

## 7. Exact OTA env (orchestrator)

Config `tests/passive_rx/dgx/ue.passive.ota.t26c.dgx.cfg` with `pdcch_blind_monitor_ul_uci = "1024:11:0:32";` in `sensing = {...}` (as ULREADY). Environment = `ota/t26m.env` (already has ISAC_PDCCH_RBOFFSET_FIX=1, ISAC_RX_MRC_MODE=1) plus:

```
ISAC_UL_PUSCH_CHAIN=1 ISAC_UL_PUSCH_SOLVER=1 ISAC_UL_SOLVER_FALLTHROUGH=1 ISAC_UL_RETRY=1 ISAC_UL_UCI_SAFE=1 ISAC_UL_DCI_LOG=1 ISAC_UL_LAYOUT_LOCK=1
```

Optional: `ISAC_UL_LAYOUT_LOG=1` (per-probe lines, ~350/s before the lock), `ISAC_UL_DBG_LOG=1` (raw payloads, for `tools/uldbg/ullayout_check.py`), `ISAC_PUSCH_MMSE_MODE=2` (only matters for 2-layer 256QAM, which this receiver does not admit). Do not set `ISAC_UL_FORCE_OPTS` or `ISAC_UL_LAYOUT_CONTROL`. Expect `ULLAYOUT FIT` within a few seconds of the first DCI 0_1, `ULLAYOUT BWP_LOCK bwp=216@1` once >= 6 strong PUSCH DM-RS detections from >= 2 RNTIs have been seen (at the capture-b position: 8 s; earlier with more UL gain), then `ULLAYOUT LOCK len=44/42` and ULDCI lines with the locked reading. If no lock appears, ULLAYOUT STATS shows the vote tally; the UL path then only spends probe CPU (~0.05 core).

## 8. Files

- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.{c,h}` (new): bank, bit fit, k2, hypotheses, votes, lock, opts.
- `nr_pdcch_blind_monitor_rt.c`: bank/lock call on every UL DCI, probe booking, locked reading, PDCCH slot occupancy, epoch reset (all under the flag).
- `nr_pusch_passive_decode.{c,h}`: DM-RS probe scorer; TP one-CDM guard.
- `nr_pusch_passive_queue.c`: probe branch; no U3 feedback for tagged grants; TP retry fix.
- `nr_pusch_passive_monitor_rt.c`: probes bypass UE-state gates and SRS hooks; ULLAYOUT STATS in the periodic dump.
- `tests/nr_pdcch_blind_monitor_test.cc`: UlLayoutLock.* (RIV, bit fit incl. k2 cases, lock without CRC + no lock on noise).
- `tools/uldbg/ullayout_check.py`, `tools/uldbg/bwp_hyp.py`.
- MMSE commit: `nr_compute_llr.{c,h}`, `nr_ulsch_demodulation.c`, `openair1/PHY/NR_TRANSPORT/tests/nr_pusch_mmse2_test.cc`, `CMakeLists.txt`.
- Replay logs (untracked): `replays/replay_ull_*` in the worktree.
