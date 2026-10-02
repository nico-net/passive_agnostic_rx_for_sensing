# BC9 clean A/B [MEASURED, DGX rfsim 106 PRB 1 RX, host idle] -- HEAD 0232f351c3 (code = 1a6be6155b)

Interleaved on/off, 150 s arms (rfsim_arm.sh, default 106 PRB bed). on = default (BC9 runtime path), off = ISAC_TD_DCI_ADJ=0. Precheck before every run: 1-min load < 2 and no nr_td_sim process (see idle_ab_run.log). No SIB1 in this phy-test bed: TDD / adjacency exclusions inert in every on arm (tdd_known=0 adj_rows=0).

| arm | run | CRC % | drop_full % | decoded | tda0 ttc s | tda2 ttc s | tda0 winner trials | seg_fail | ldpc_zero_tb | f_S | certified passes by k0 [0,1,2,3,>=4] | wrong-k0 alarms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| off | r1 | 97.29 | 0.0595 | 55510 | 2.689 | 21.805 | 128 | 1199 | 269 | None | None | None |
| on | r1 | 97.57 | 0.1011 | 55313 | 2.147 | 22.556 | 110 | 1038 | 259 | 0.185 | 28,0,0,0,0 | 0 |
| off | r2 | 95.9 | 0.0835 | 55068 | 4.658 | 22.078 | 126 | 1791 | 402 | None | None | None |
| on | r2 | 95.25 | 0.0728 | 54809 | 5.651 | 20.644 | 141 | 2094 | 465 | 0.2505 | 46,0,0,0,0 | 0 |
| off | r3 | 95.9 | 0.0353 | 55139 | 4.595 | 20.616 | 125 | 1825 | 369 | None | None | None |
| on | r3 | 97.33 | 0.0625 | 55096 | 2.544 | 19.685 | 125 | 1148 | 277 | 0.205 | 36,0,0,0,0 | 0 |
| off | r4 | 97.27 | 0.0277 | 55183 | 2.412 | 23.542 | 125 | 1163 | 289 | None | None | None |
| on | r4 | 95.26 | 0.0436 | 55195 | 5.699 | 21.974 | 142 | 2122 | 428 | 0.245 | 42,0,0,0,0 | 0 |
| off | r5 | 97.44 | 0.0639 | 57885 | 2.552 | 19.97 | 125 | 1168 | 264 | None | None | None |
| on | r5 | 97.0 | 0.0488 | 55191 | 3.005 | 23.469 | 141 | 1313 | 302 | 0.249 | 40,0,0,0,0 | 0 |

| metric | arm | min | median | max |
|---|---|---|---|---|
| crc | on | 95.25 | 97.0 | 97.57 |
| crc | off | 95.9 | 97.27 | 97.44 |
| trials | on | 110 | 141 | 142 |
| trials | off | 125 | 125 | 128 |
| ttc0 | on | 2.147 | 3.005 | 5.699 |
| ttc0 | off | 2.412 | 2.689 | 4.658 |
| ttc2 | on | 19.685 | 21.974 | 23.469 |
| ttc2 | off | 19.97 | 21.805 | 23.542 |
| zero | on | 259 | 302 | 465 |
| zero | off | 264 | 289 | 402 |
| segf | on | 1038 | 1313 | 2122 |
| segf | off | 1163 | 1199 | 1825 |
| drop | on | 0.0436 | 0.0625 | 0.1011 |
| drop | off | 0.0277 | 0.0595 | 0.0835 |
Mann-Whitney crc: U(on)=11.0 exact two-sided p=0.794 (n=5,5)
Mann-Whitney trials: U(on)=16.5 exact two-sided p=0.468 (n=5,5)
Mann-Whitney ttc0: U(on)=14.0 exact two-sided p=0.841 (n=5,5)
rank4: crc 100.0 (3005/3005)

Exact Mann-Whitney (two-sided, mid-rank ties, n=5 vs 5). Rank-4 pin49r4 [MEASURED, DGX rfsim rank-4 pin49r4 273 PRB 4 RX, host idle] on the same binary, default arm.
