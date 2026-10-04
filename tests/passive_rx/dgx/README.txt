DGX Spark (spark-74c3, aarch64) tools. NEW files only; the sens6 launchers under ../captures are frozen.
rfsim_regress.sh  - rfsim regression gate (Task A1)
run_rx_dgx.sh     - DGX receiver launcher with the X925 core map (Task A6)
coremap_dgx.env   - core map definition (Task A6)
Evidence of 2026-09-30/10-01 lives in ../dgx_host_snapshot_2026-09-30/.


Postconv gate calibration (rfsim_regress.sh, GATE_MODE=postconv default; GATE_MODE=legacy = old criterion)
-----------------------------------------------------------------------------------------------------------
Why: since the K39 fix (DM-RS presence no longer pins k0) the receiver separates k0 candidates by decoding, so the
search phase is longer (tda0 ttc ~2-6 s, tda2 ~20-23 s vs ~0.8 / ~12 s before) and its CRC failures pull overall CRC
to ~95-97.6 %, below the old 98 % gate, while the post-convergence phase is clean. Operator option (a): gate on
post-convergence CRC + per-context ttc bounds + a search-phase floor. (b) (an SA bed with SIB1) is separate.

Definitions (score_rx.py docstring is authoritative):
  ttc_s        CONVERGED time minus the first log line carrying "rnti=<that RNTI>" (first sighting of the RNTI).
  postconv_crc rx.log does not attribute PDSCH grants to contexts, so the window starts at the first periodic
               cumulative "PDSCHQ queued= decoded= crc_ok=" sample (every ~20 s) at or after the time the LAST
               context converged; CRC = (final ok - sample ok)/(final decoded - sample decoded). Up to ~20 s of
               post-convergence grants are therefore left out (conservative), and None (=> FAIL) if no grants follow.
  search CRC   cumulative ok/decoded at that same sample (reported, not gated; overall CRC is the gated floor).

[MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @0232f351c3 (code 1a6be6155b)] BC9 idle A/B, 150 s arms, rescored with
the new score_rx.py from /tmp/bc9_idle/*/rx/rx.log (arm = ISAC_TD_DCI_ADJ off/on; both are default-bed behaviour here
because no SIB1 => adjacency inert):
arm  run  CRC%   postCRC% srchCRC% ttc0_s  ttc2_s  zeroTB drop_full% postDec
off  r1   97.29  100.00   88.99  2.705  21.821   269  0.0595  41843
off  r2   95.90  100.00   83.54  4.666  22.086   402  0.0835  41358
off  r3   95.90  100.00   83.51  4.602  20.623   369  0.0353  41440
off  r4   97.27  100.00   89.09  2.421  23.551   289  0.0277  41384
off  r5   97.44  100.00   89.23  2.563  19.981   264  0.0639  44112
on   r1   97.57  100.00   90.28  2.154  22.563   259  0.1011  41491
on   r2   95.25  100.00   80.89  5.658  20.651   465  0.0728  41177
on   r3   97.33  100.00   89.23  2.558  19.699   277  0.0625  41453
on   r4   95.26  100.00   80.96  5.706  21.981   428  0.0436  41458
on   r5   97.00  100.00   87.99  3.013  23.477   302  0.0488  41396
Observed: overall CRC min 95.25 / max 97.57; post-convergence CRC 100.00 in 10/10 (>= 41177 grants each; no post-conv
regression); ttc0 max 5.706 s; ttc2 max 23.477 s; drop_full max 0.1011 %; 2 contexts (tda0, tda2) in 10/10.
Cross-check (fix_r4, a still-running arm, excluded; not used for calibration, idleness not certified, other codes): 32
further rfsim logs in /tmp (bc9_ab, bc9_gate*, bc9_fix1*, bc9_fix1b*, bc7_default, bc7_legacy, bc7b_k42 ab/gate): post CRC
100.00 everywhere, ttc0 <= 6.8 s, ttc2 <= 23.5 s, reopens 0. 30 PASS; 2 FAIL on the overall-CRC floor only (bc9_fix1_gate
r1 93.85, bc9_fix1b_gate r1 93.40; both are "loaded" runs per the BC9 report, so a correct FAIL for an idle-host gate).

Reopens: every idle run has exactly one CONVERGED line per context (0 re-convergences), so GATE_REOPENS_MAX=0.
Contexts are deduplicated by (rnti, tda); ttc and n_contexts use the FIRST convergence, later CONVERGED lines of the same
context are counted as `reopens`. The post window starts after the last context's FIRST convergence; a reopened
context's re-search therefore stays inside the window (stricter).

Chosen defaults (each overridable by the named env var), derived from the 10 idle runs above:
  GATE_NCTX_MIN=2             both bed contexts (tda0, tda2) converge: 10/10 observed.
  GATE_POSTCONV_CRC_MIN=99.8  observed min 100.00 over >= 41177 grants; 0.2 pt margin (~80 failed grants in 41k).
  GATE_POSTCONV_MIN_DEC=5000  minimum grants in the post window (observed min 41177, 8x margin); stops a tiny or empty
                              window from passing by luck.
  GATE_REOPENS_MAX=0          observed max 0.
  GATE_TTC_MAX_TDA0=8.6       1.5 x observed max 5.706 = 8.56, rounded up to 0.1 s.
  GATE_TTC_MAX_TDA2=35.3      1.5 x observed max 23.477 = 35.22, rounded up to 0.1 s.
  GATE_CRC_FLOOR=94.5         observed overall-CRC min 95.25 minus 0.75.
  GATE_DROP_MAX=1.0           unchanged (max observed 0.10 %).
A converged tda with no GATE_TTC_MAX_TDA<n> bound is unbounded (only tda0 and tda2 have defaults). GATE_TTC_MAX_TDA*
values must be numbers (clear error otherwise). Informational only, NOT gated: search_crc_pct (cumulative CRC at the
post-window baseline sample; 80.9-90.3 on the idle set) and ldpc_zero_tb (259-465).
False positives on the calibration set with these defaults: 0/10 (all 10 PASS). Per-run PASS/FAIL line prints every
criterion as name=value<op>limit. Re-score with  python3 score_rx.py --gate <arm_dir>  (GATE_MODE=legacy for the old one).

Sensitivity versus false-positive rate [MODEL on MEASURED, DGX rfsim 106 PRB 1 RX, host idle, @0232f351c3]
FP = idle runs (of 10) that FAIL. ttc bound = k x observed max; floor on overall CRC:
  ttc bound k      tda0 / tda2 bound   FP        overall-CRC floor   FP
  2.0 (first cut)  11.5 / 47.2         0/10      93.0                0/10
  1.5 (chosen)     8.6 / 35.3          0/10      94.5 (chosen)       0/10
  1.25             7.2 / 29.5          0/10      95.25 (=min)        0/10
  1.1              6.3 / 26.0          0/10      95.3                2/10
  1.0              5.8 / 23.6          0/10      96.0                4/10
  0.9              5.2 / 21.2          7/10
Tighter than 1.5x/94.5 buys little (rig run-to-run spread of ttc0 is 2.2-5.7 s), 1.0x is the zero-margin limit.

What the gate catches (model for slowdowns: search failures and ttc scale linearly with the slowdown factor k, i.e.
overall CRC' = 100 - k*(100 - CRC), ttc' = k*ttc; no receiver run was made, so this is arithmetic on the measured idle
runs, not a measurement of a slowed receiver). Post-window is ~75 % of grants.
  best run (on_r1, 97.57 %, ttc 2.15/22.6 s): 1.0x pass; 1.5x pass (96.35 %, 3.2/33.8 s); 2x FAIL (ttc2 45.1 s)
  median run (off_r4, 97.27 %, 2.42/23.6 s):  1.0x pass; 1.5x FAIL (ttc2 35.3+ s, borderline); 2x FAIL (ttc2 47 s)
  worst run (on_r2, 95.25 %, 5.66/20.7 s):    1.0x pass; 1.5x FAIL (floor 92.9 %); 2x FAIL (ttc0 11.3, ttc2 41.3, floor 90.5)
  Over all 10 runs: 1.5x slowdown fails 5/10, 2x slowdown fails 10/10 (reliable only at >= 2x).
  ~1 % decode regression in the post-convergence phase (post CRC 99.0 vs 100.00): FAIL in every run via
  postconv_crc (<99.8); the gate resolves post-conv regressions down to 0.2 pt. The overall CRC alone would show only
  ~0.75 pt of it (96.5-96.8 for typical runs), which is why post CRC is gated separately.
  Also caught: a context that never converges, converges twice (reopen), a post-window < 5000 grants, drop_full > 1 %.
What it does NOT catch: a search slowdown below ~1.5x (typical runs pass up to ~1.4x on ttc2); search-phase failure
counts that stay under the 94.5 floor; a ttc2 shift within 35.3 s; ttc/CRC on a different bed, host, bandwidth or
load (idle DGX 106 PRB 1 RX only; never merge); wrong-but-CRC-passing configurations (CRC is the only correctness
proxy); per-context CRC or the first ~20 s after convergence (rx.log has no per-context attribution; the post window
starts at a ~20 s PDSCHQ sample); anything beyond the 150 s arm.
