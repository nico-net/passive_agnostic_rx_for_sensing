# HEALTH-GRID-20261016: per-class receiver health (Phase 1 Task 1)

Tool tests/passive_rx/health_grid.py (a07a919933). Class = (tbl, mcs, PRB bucket, Nl, nsym); margin = goal_metrics true SNR - required SNR; qualified = margin >= 2 dB; probes, rv>0, reserved MCS and noise-floor grants excluded; RECOVLATE rescues credited; min_n = 50 (fewer = UNVERIFIED); cross80 = smallest margin 0..12 dB from which the class decodes >= 80 %; loss = cross80 - 2 (floor 0), "-" = never reaches 80 %. Sonnet agent; saved by the orchestrator. Full per-class outputs: session scratchpad out_Vota.txt, out_Tota.txt, out_Vrep.txt, out_Trep.txt (agg.py).
Labels: [MEASURED, DGX OTA 2026-10-16, V, aVX1/2 + xVaXE1-3 + qVaE1-3, 8 logs, n=192865 grants, 858 classes]; [MEASURED, DGX OTA 2026-10-16, T, aTX1/2 + xTaXE1-3 + qTaE1-3, 8 logs, n=75072, 443 classes]; [MEASURED, DGX replay raw_pci415_12s_b, t26aXE, 8 logs, n=9100]; [MEASURED, DGX replay raw_pci923_tim_12s_tile3, t26aXE, 4 logs, n=2296]. Replays = the same 12 s capture repeated.

## 1. Verdict
| set | PASS | FAIL | UNVERIFIED | health |
|---|---|---|---|---|
| V OTA | 125 | 100 | 633 | FAIL |
| T OTA | 66 | 23 | 354 | FAIL |
| V replay | 14 | 4 | 178 | FAIL |
| T replay | 7 | 2 | 65 | FAIL |

## 2. Aggregates (OTA)
V per (tbl, mcs): 0/0 88.6 PASS; 1/0 92.5; 1/1 85.2; 1/2 86.0; 1/3 90.6; **1/4 75.1 FAIL (cross80 5)**; 1/5 87.4; 1/6 87.0; 1/7 88.0; 1/8 81.8; 1/9 80.8; 1/10 81.7; 1/11 88.6; **1/12 73.5, 1/13 73.9, 1/15 75.0, 1/16 72.5, 1/17 61.8, 1/18 55.5 FAIL**; 1/14 83.4; 1/19-27 UNVERIFIED.
V per width: rb 1-8 100; 9-24 94.3; 25-51 85.4; **52-106 78.5 (cross80 3); 107-162 74.5 (never); 163-217 68.6 (cross80 10)**. Nl 1 85.0, 2 88.0, 3 91.9 PASS; Nl 4 43 grants UNVERIFIED. nsym 7 100; **9 0.0 (n 95); 10 11.8 (n 51)**; 11 89.4; 12 89.8; 13 84.8; **14 77.8 FAIL**.
T per (tbl, mcs): 0/0 100; 1/0 96.5; **1/1 74.8 FAIL (cross80 7)**; 1/2 82.2; 1/3 80.4; 1/4-1/11 89.9-97.3 PASS; 1/12 81.5; **1/13 74.8, 1/14 71.1, 1/15 70.6, 1/16 65.0, 1/17 69.1, 1/18 44.4 FAIL**.
T per width: 1-8 100; 9-24 94.1; 25-51 91.2; 52-106 83.9; 107-162 80.5; **163-217 78.5 FAIL**. Nl 1-3 92-98 PASS (no Nl 4). **nsym 9 0.0 (n 190)**; 10 100; 11 83.1; 13 94.5 (no nsym 12/14).
Replays: V 1/3 77.6 FAIL, 1/8 79.2 FAIL, rb 107-162 73.1 and 163-217 79.6 FAIL, nsym 9 0.0; T rb 52-106 19.2, 107-162 4.6, 163-217 5.0 FAIL, 1/15 and 1/16 0.0, Nl 1 64.1 FAIL.

## 3. FAIL classes and cause hypotheses ([INFERRED] unless stated; the grid has no slot dimension)
V (100 classes; 70 at rb >= 52; all with n_q > 400 at rb >= 25, mostly Nl 2). Largest: (1,0,163-217,2,11) 1203 73.2 % never; (1,7,163-217,2,13) 1161 67.4 never; (1,4,25-51,2,11) 1036 74.8 loss 4; (1,8,163-217,2,13) 986 61.5 never; (1,6,163-217,2,13) 932 66.7; (1,0,52-106,2,11) 831 56.1; (1,4,52-106,2,11) 790 60.4 loss 5; (1,0,107-162,2,11) 747 60.8; (1,9,163-217,2,13) 734 61.2; (1,2,52-106,2,11) 686 56.1; (1,5,163-217,2,13) 670 70.0; (1,1,52-106,2,11) 623 41.4; (1,1,25-51,2,11) 521 44.5; (1,1,107-162,2,11) 453 38.0.
1. The loss grows monotonically with PRB width (94 -> 85 -> 79 -> 75 -> 69 %) and many wide classes never reach 80 % at any margin: unmodelled REs over the band (the undiscovered full-band non-TRS CSI-RS at abs%160 = 135, symbol 7; wide grants over the SSB RBs ~70 % vs 90 %; CSIFULLAB: ~30 % of failures decode without rate-matching around all CSI-RS) — not a margin effect.
2. nsym 11, MCS 0-4, rb >= 25, Nl 2: the same wide-grant mechanism plus the S ambiguity.
3. nsym 14 classes (S=0/L=14) fail with loss 1-3 dB where nsym 13 neighbours pass: TDRA S ambiguity (within the +-1.5 dB estimator uncertainty).
4. High MCS 12-18 at 25-217 PRB, loss 1-3 dB: receiver loss or estimator bias (multi-layer noise enhancement, chest error not modelled) — the rfsim sweep (known SNR) decides.
5. (0,0,25-51,1,9) 0.0 % (n_q 92), reproduces on the V replay (60, 0.0 %); V nsym 10 11.8 %: deterministic 0 % = a fixed unmodelled allocation or mapping; not explained by any DLCRC report.
T (23 classes): (1,1,9-24,2,11) 737 12.3 % loss 7; (1,2,9-24,2,11) 288 20.1; (1,3,9-24,2,11) 104 30.8: the periodic dead slots (slots 3/13 nsym 11, DLCRC-ROOTCAUSE 3, REPLAYS). MCS 11-17, Nl 2, nsym 13, 25-217 PRB, 49-78 %, loss 1-3: partly even-frame slots 8/18, rest within estimator uncertainty. (1,1,9-24,2,9) 77 0.0 %: same unexplained nsym-9 0 % as V.
T replay: (1,16,163-217,1,13) 152 0.0 %, (1,15,107-162,1,13) 113 0.0 % in all 4 replays: wide Nl 1 high-MCS dead on that capture (not seen on air) — offline test case.

## 4. NOT COVERED (no class with n_q >= 50), for the rfsim sweep (Task 2)
tbl0 MCS 1-28 (only MCS 0, nsym 7/9); tbl1 MCS 18-27; tbl2 MCS 0-28; Nl 4 (V 46 grants, none qualified); Nl 3 above 51 PRB; rb 218-273; nsym < 7 and 8. Plus the FAIL classes of unknown cause: wide Nl 2 nsym 13 MCS 5-17, nsym 11 MCS 0-4, nsym 14, nsym 9/10.

## 5. Caveats
Estimator correction is AWGN + ZF only (+-1.5 dB near the floor): losses of 1-3 dB are not attributable on air. Slot-based causes are cross-references, not measured here. Replay n is not independent.
