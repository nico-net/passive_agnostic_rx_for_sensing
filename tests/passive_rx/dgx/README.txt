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
Cross-check (fix_r4, a still-running arm, excluded; not used for calibration, idleness not certified, other codes): 30 further rfsim logs in /tmp
(bc9_ab, bc9_gate*, bc9_fix1*, bc7_default, bc7b_k42 ab/gate) all give post CRC 100.00, ttc0 <= 6.8 s, ttc2 <= 23.5 s,
overall CRC >= 93.4; all PASS with the defaults below. Pre-K39 logs (bc7_legacy) PASS as well (ttc0 0.8, ttc2 12).

Rule and chosen defaults (each overridable by the named env var):
  GATE_NCTX_MIN=2             both bed contexts (tda0, tda2) converge: 10/10 observed.
  GATE_POSTCONV_CRC_MIN=99.5  observed min 100.00 over >= 41k grants (rule "min - small margin, >= 98.0"; the data
                              supports a tighter floor; one failed grant in 41k is 0.002 %). A real post-conv
                              regression of >= 0.5 % trips it. Lower it per host if a loaded host shows noise.
  GATE_TTC_MAX_TDA0=12        max observed 5.706 s x 2 = 11.4, rounded up.
  GATE_TTC_MAX_TDA2=47        max observed 23.477 s x 2 = 46.95, rounded up.
  GATE_CRC_FLOOR=93.0         observed overall-CRC min 95.25 minus ~2.2 points; the 30-run cross-check min is 93.4.
                              Not meaningless: overall CRC = 100 - search-phase failures, so 93.0 trips when search
                              failures exceed ~1.5x the worst idle run (a ~7 % failure share vs 2.4-4.75 % observed).
  GATE_DROP_MAX=1.0           unchanged (max observed 0.10 %).
Per-run PASS/FAIL line prints every criterion as name=value<op>limit. Re-score any arm dir with
  python3 score_rx.py --gate <arm_dir>      (GATE_MODE=legacy for the old criterion)
Limits: calibrated on an idle DGX; a loaded or other host must re-baseline (never merge across hosts). The
ttc bounds assume the 150 s arm: tda2 must converge before ~47 s so that the post window is non-empty.
