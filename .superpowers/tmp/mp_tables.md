
### 200 MHz (245.76 MS/s), mode T 

| arm | exit | cpu cores | GPU util mean/p95 | chan overruns / discont | chan_work_max / gap_max ms | SHMIQ ov (per ring) | UHD ovf / RFSTALL / RXDISCONT | asserts | SENSE_SCHED fail/started | pthread fail |
|---|---|---|---|---|---|---|---|---|---|---|
| mp2T_P1 | RX_EXIT=130 | 11.116 | 57/90 | 0 / 0 | 45.4 / 45.4 | pci415:3 pci923:3 spare1:0 | 0 / 0 / 6 | 0 | 0/0 | 0 |
| mp2T_S415 | RX_EXIT=130 | 6.429 | 45/96 | - / - | - / - | - | 0 / 0 / 0 | 0 | 0/0 | 0 |
| mp2T_P2 | RX_EXIT=130 | 11.850 | 55/90 | 0 / 0 | 18.1 / 18.1 | pci415:0 pci923:1 spare1:0 | 0 / 0 / 1 | 0 | 0/0 | 0 |
| mp2T_S923 | RX_EXIT=130 | 7.292 | 46/94 | - / - | - / - | - | 0 / 0 / 0 | 0 | 0/0 | 0 |
| mp2T_P3 | RX_EXIT=130 | 11.544 | 56/93 | 0 / 0 | 22.9 / 22.9 | pci415:3 pci923:4 spare1:0 | 0 / 0 / 7 | 0 | 0/0 | 0 |

#### cell 415 

| metric | mp2T_S415 | mp2T_P1 | mp2T_P2 | mp2T_P3 |
|---|---|---|---|---|
| active window s | 210 | 200 | 200 | 200 |
| t PBCH lock s | 3.4 | 15.4 | 13.3 | 12.7 |
| t SIB1 s | 13.4 | 25.4 | 23.3 | 22.7 |
| t first record s | - | - | - | - |
| sync losses / pbch locks | 0/1 | 3/4 | 0/1 | 3/4 |
| DCI accepts/s (1_0 / 1_1 / 0_0 / 0_1) | 235.2 (7.6 / 227.6 / 0.0 / 487.5) | 418.8 (3.6 / 415.2 / 0.1 / 449.8) | 215.3 (3.2 / 212.1 / 0.0 / 384.3) | 225.4 (4.5 / 220.8 / 0.0 / 352.2) |
| distinct RNTI (top) | 93 (0x29d8:630 0xe4b8:477 0xcafc:119) | 47 (0x29d8:859 0x9cb3:686 0x5db:430) | 60 (0x29d8:781 0x7dd3:154 0x2eab:153) | 57 (0x42b0:1125 0xd53b:176 0x5cdc:114) |
| grants decoded/s | 13.9 | 24.3 | 13.2 | 16.7 |
| CRC-ok % (all) | 72.1 | 66.3 | 70.6 | 64.8 |
| dec>=51 /s (crc-ok/s) | 5.7 (4.6) | 9.1 (7.6) | 5.6 (4.5) | 4.8 (4.3) |
| dec>=128 /s (crc-ok/s) | 3.4 (2.8) | 5.2 (4.4) | 3.2 (2.6) | 2.6 (2.3) |
| dec_us p50/p99 ms all | 0.96/8.86 (n=2917) | 1.53/43.99 (n=4863) | 1.29/23.45 (n=2639) | 1.96/30.79 (n=3346) |
| dec_us p50/p99 ms >=128 | 0.73/1.85 (n=708) | 1.03/9.17 (n=1050) | 1.07/4.62 (n=632) | 1.09/10.90 (n=520) |
| scanq full/stale/epoch % of queued | 0.0/0.0/0.0 (q=336049) | 0.4/3.4/0.0 (q=318629) | 4.7/5.2/0.0 (q=320111) | 0.1/0.9/0.0 (q=318485) |
| pdschq full/stale % of queued | 0.0/0.2 (q=11291) | 0.0/12.7 (q=8090) | 0.0/3.4 (q=6117) | 0.0/7.6 (q=7358) |
| DA async drop % data / dmrs | - / - | - / - | - / - | - / - |
| cfr_drop / rec_drop / obs_dropped | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| records total / cfr exported dmrs+data | 151749 / 0 | 173744 / 0 | 119925 / 0 | 115519 / 0 |

#### cell 923 

| metric | mp2T_S923 | mp2T_P1 | mp2T_P2 | mp2T_P3 |
|---|---|---|---|---|
| active window s | 210 | 200 | 200 | 200 |
| t PBCH lock s | 2.5 | 13.4 | 13.6 | 13.5 |
| t SIB1 s | 12.5 | 23.4 | 23.6 | 23.5 |
| t first record s | - | - | - | - |
| sync losses / pbch locks | 0/1 | 3/4 | 1/2 | 4/5 |
| DCI accepts/s (1_0 / 1_1 / 0_0 / 0_1) | 352.1 (9.0 / 343.1 / 0.0 / 243.7) | 511.9 (8.4 / 503.5 / 0.0 / 401.7) | 485.9 (9.1 / 476.8 / 0.0 / 342.8) | 339.8 (6.5 / 333.2 / 0.0 / 269.4) |
| distinct RNTI (top) | 58 (0x57a4:429 0x5c0a:415 0x5b4c:297) | 29 (0x5a64:960 0x4c96:935 0x4b5a:640) | 26 (0x5b4c:1161 0x468a:996 0x57a4:644) | 36 (0x4ebc:1183 0x57a4:510 0x5c24:315) |
| grants decoded/s | 17.0 | 27.3 | 26.4 | 16.4 |
| CRC-ok % (all) | 56.2 | 63.3 | 55.7 | 62.4 |
| dec>=51 /s (crc-ok/s) | 10.4 (6.5) | 15.2 (10.7) | 14.6 (9.0) | 9.4 (6.6) |
| dec>=128 /s (crc-ok/s) | 5.9 (3.8) | 8.6 (6.3) | 8.2 (5.2) | 5.3 (3.8) |
| dec_us p50/p99 ms all | 0.83/5.87 (n=3565) | 1.19/27.55 (n=5454) | 1.23/26.00 (n=5287) | 1.12/17.55 (n=3283) |
| dec_us p50/p99 ms >=128 | 0.80/2.43 (n=1246) | 1.08/16.42 (n=1722) | 1.07/7.12 (n=1639) | 1.01/8.39 (n=1052) |
| scanq full/stale/epoch % of queued | 0.0/0.0/0.0 (q=336043) | 0.4/1.1/0.0 (q=318784) | 0.7/2.0/0.0 (q=320038) | 0.0/0.6/0.0 (q=318408) |
| pdschq full/stale % of queued | 0.0/0.3 (q=49171) | 0.0/7.6 (q=55966) | 0.0/11.4 (q=50919) | 0.0/6.6 (q=37930) |
| DA async drop % data / dmrs | - / - | - / - | - / - | - / - |
| cfr_drop / rec_drop / obs_dropped | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| records total / cfr exported dmrs+data | 125118 / 0 | 182722 / 0 | 165740 / 0 | 121852 / 0 |

### 200 MHz (245.76 MS/s), mode S 

| arm | exit | cpu cores | GPU util mean/p95 | chan overruns / discont | chan_work_max / gap_max ms | SHMIQ ov (per ring) | UHD ovf / RFSTALL / RXDISCONT | asserts | SENSE_SCHED fail/started | pthread fail |
|---|---|---|---|---|---|---|---|---|---|---|
| mp2S_P1 | RX_EXIT=130 | 12.165 | 56/89 | 0 / 0 | 952.2 / 952.2 | pci415:5 pci923:6 spare1:0 | 0 / 0 / 11 | 0 | 0/1 | 0 |
| mp2S_S415 | RX_EXIT=130 | 7.569 | 51/95 | - / - | - / - | - | 0 / 0 / 0 | 0 | 0/1 | 0 |
| mp2S_P2 | RX_EXIT=130 | 11.913 | 53/82 | 0 / 2 | 952.2 / 952.2 | pci415:3 pci923:4 spare1:0 | 0 / 0 / 9 | 0 | 0/1 | 0 |
| mp2S_S923 | RX_EXIT=130 | 7.591 | 55/95 | - / - | - / - | - | 0 / 0 / 0 | 0 | 0/1 | 0 |
| mp2S_P3 | RX_EXIT=130 | 12.118 | 55/87 | 0 / 0 | 25.6 / 25.6 | pci415:3 pci923:4 spare1:0 | 0 / 0 / 7 | 0 | 0/1 | 0 |

#### cell 415 

| metric | mp2S_S415 | mp2S_P1 | mp2S_P2 | mp2S_P3 |
|---|---|---|---|---|
| active window s | 210 | 200 | 200 | 200 |
| t PBCH lock s | 2.5 | 14.7 | 13.3 | 14.8 |
| t SIB1 s | 12.5 | 24.7 | 23.3 | 24.8 |
| t first record s | 2.5 | 14.7 | 13.2 | 14.8 |
| sync losses / pbch locks | 0/1 | 5/6 | 4/5 | 3/4 |
| DCI accepts/s (1_0 / 1_1 / 0_0 / 0_1) | 261.7 (0.0 / 0.0 / 0.0 / 0.0) | 327.7 (0.0 / 0.0 / 0.0 / 0.0) | 257.5 (0.0 / 0.0 / 0.0 / 0.0) | 143.3 (0.0 / 0.0 / 0.0 / 0.0) |
| distinct RNTI (top) | 109 (0x8093:4756 0xcfdc:2426 0x6358:2083) | 56 (0x1203:1505 0x387c:1165 0x2905:1132) | 45 (0xc538:3294 0x80e4:1070 0x18e4:740) | 47 (0x2a2d:3040 0x3ed:839 0x7665:661) |
| grants decoded/s | 191.3 | 66.7 | 50.0 | 39.7 |
| CRC-ok % (all) | 71.4 | 57.4 | 63.7 | 64.7 |
| dec>=51 /s (crc-ok/s) | 93.9 (75.1) | 16.0 (11.2) | 13.0 (10.5) | 6.2 (4.5) |
| dec>=128 /s (crc-ok/s) | 54.5 (43.6) | 7.2 (5.5) | 6.2 (5.3) | 3.1 (2.3) |
| dec_us p50/p99 ms all | 0.76/6.57 (n=40167) | 1.77/29.34 (n=13334) | 2.31/42.60 (n=10006) | 2.63/40.95 (n=7946) |
| dec_us p50/p99 ms >=128 | 0.68/2.31 (n=11445) | 1.24/10.84 (n=1450) | 1.43/10.61 (n=1241) | 1.22/8.05 (n=612) |
| scanq full/stale/epoch % of queued | 0.0/0.1/0.0 (q=336066) | 3.0/5.4/0.0 (q=317346) | 0.5/4.2/0.0 (q=314065) | 2.3/7.6/0.0 (q=318484) |
| pdschq full/stale % of queued | 0.0/0.9 (q=51674) | 0.0/10.1 (q=20835) | 0.0/17.1 (q=20573) | 0.0/15.6 (q=21014) |
| DA async drop % data / dmrs | 2.4 / - | 0.0 / - | 9.1 / - | 0.0 / - |
| cfr_drop / rec_drop / obs_dropped | 51 / 0 / 0 | 3463 / 0 / 0 | 4633 / 0 / 0 | 1597 / 0 / 0 |
| records total / cfr exported dmrs+data | 1057176 / 92819 | 733558 / 2126 | 719055 / 5343 | 673438 / 4804 |
| rec/s pdsch_dmrs n | v51 | v128 | 199.3 | 24.0 | 15.9 (noqual 3) | 5.7 | 2.2 | 1.4 (noqual 1) | 15.3 | 5.2 | 3.7 (noqual 7) | 17.6 | 8.5 | 6.4 (noqual 3) |
| rec/s pdsch_data n | v51 | v128 | 245.5 | 23.7 | 10.9 (noqual 4) | 6.2 | 0.7 | 0.3 (noqual 8) | 11.7 | 2.0 | 1.2 (noqual 19) | 6.5 | 1.0 | 0.5 (noqual 9) |
| rec/s ssb n | v51 | v128 | 1219.7 | 0.0 | 0.0 (noqual 4) | 1218.2 | 0.0 | 0.0 (noqual 0) | 1213.9 | 0.0 | 0.0 (noqual 29) | 1207.4 | 0.0 | 0.0 (noqual 5) |
| rec/s csirs n | v51 | v128 | 485.9 | 297.1 | 0.0 (noqual 38) | 532.3 | 267.4 | 0.0 (noqual 3508) | 575.7 | 316.6 | 0.0 (noqual 4662) | 433.2 | 282.9 | 0.0 (noqual 1611) |
| SENSE_SCHED lines (cell) | 55 | 53 | 52 | 52 |

#### cell 923 

| metric | mp2S_S923 | mp2S_P1 | mp2S_P2 | mp2S_P3 |
|---|---|---|---|---|
| active window s | 210 | 201 | 200 | 200 |
| t PBCH lock s | 2.5 | 13.2 | 13.5 | 13.5 |
| t SIB1 s | 12.5 | 23.2 | 23.5 | 23.5 |
| t first record s | 2.5 | 12.9 | 13.2 | 13.1 |
| sync losses / pbch locks | 0/1 | 6/7 | 5/6 | 4/5 |
| DCI accepts/s (1_0 / 1_1 / 0_0 / 0_1) | 575.4 (0.0 / 0.0 / 0.0 / 0.0) | 437.0 (0.0 / 0.0 / 0.0 / 0.0) | 531.4 (0.0 / 0.0 / 0.0 / 0.0) | 544.8 (0.0 / 0.0 / 0.0 / 0.0) |
| distinct RNTI (top) | 47 (0x515e:9823 0x543c:9712 0x4b00:7597) | 35 (0x4aca:4119 0x4ebc:1464 0x5062:1069) | 22 (0x5504:3056 0x53f4:856 0x515e:797) | 34 (0x5338:5246 0x528e:968 0x5baa:906) |
| grants decoded/s | 270.4 | 55.8 | 48.0 | 64.2 |
| CRC-ok % (all) | 74.7 | 44.9 | 36.8 | 51.3 |
| dec>=51 /s (crc-ok/s) | 178.7 (140.3) | 18.8 (9.9) | 21.5 (10.6) | 24.1 (14.8) |
| dec>=128 /s (crc-ok/s) | 103.2 (82.8) | 9.3 (5.5) | 9.9 (6.0) | 11.3 (7.7) |
| dec_us p50/p99 ms all | 0.72/3.10 (n=56787) | 2.00/26.48 (n=11193) | 1.90/18.55 (n=9597) | 1.63/17.44 (n=12848) |
| dec_us p50/p99 ms >=128 | 0.71/1.81 (n=21680) | 1.22/7.73 (n=1857) | 1.45/9.16 (n=1987) | 1.22/8.62 (n=2263) |
| scanq full/stale/epoch % of queued | 0.2/1.5/0.0 (q=336044) | 0.6/1.9/0.0 (q=318269) | 3.1/5.7/0.0 (q=313827) | 5.1/4.5/0.0 (q=318378) |
| pdschq full/stale % of queued | 0.0/0.7 (q=117654) | 0.0/14.8 (q=68069) | 0.0/17.6 (q=70207) | 0.0/8.7 (q=81653) |
| DA async drop % data / dmrs | 0.2 / - | 11.6 / - | 0.0 / - | 4.4 / - |
| cfr_drop / rec_drop / obs_dropped | 39 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| records total / cfr exported dmrs+data | 1143374 / 193219 | 218159 / 4198 | 228209 / 822 | 278295 / 9136 |
| rec/s pdsch_dmrs n | v51 | v128 | 373.7 | 19.0 | 7.8 (noqual 5) | 11.7 | 8.9 | 6.4 (noqual 0) | 1.2 | 1.1 | 0.5 (noqual 0) | 27.4 | 15.4 | 5.4 (noqual 0) |
| rec/s pdsch_data n | v51 | v128 | 564.0 | 9.3 | 5.1 (noqual 19) | 11.0 | 5.7 | 4.5 (noqual 0) | 3.0 | 0.5 | 0.3 (noqual 0) | 19.3 | 5.1 | 2.2 (noqual 0) |
| rec/s ssb n | v51 | v128 | 153.2 | 0.0 | 0.0 (noqual 3) | 153.0 | 0.0 | 0.0 (noqual 0) | 151.5 | 0.0 | 0.0 (noqual 0) | 151.9 | 0.0 | 0.0 (noqual 0) |
| rec/s csirs n | v51 | v128 | 101.7 | 101.7 | 101.7 (noqual 1) | 100.1 | 100.1 | 100.1 (noqual 0) | 100.4 | 100.4 | 100.3 (noqual 0) | 100.1 | 100.1 | 100.1 (noqual 0) |
| SENSE_SCHED lines (cell) | 55 | 0 | 0 | 0 |

### step 1 attempt m4S_attempt /home/nicola/NICOLA/ota_runs/m4S_attempt_1_165741 

span: [PHY]    SENSING: MPCI_SPAN ok=1 centre_hz=3720000000 rate=245760000 usable_mhz=196.608 span_mhz=[3640.950..3799.050] n_rx=4 served=2/3 cells=pci415:d2:-39.990,pci923:d2:+39.990 unserved=pci508:out_of_span device_rates=7 analog_mhz=400.0
unserved: ['MPCI_SPAN cell=2 id=pci508 unserved reason=out_of_span:']
admit: MPCI_ADMIT cells=3 of 5 reason=pool refuse=0 host_budget_mb=89228 host_used_mb=48187 gpu_budget_mb=-1 gpu_used_mb=0 nid_window=0 unserved=[3:pool 4:pool]
| arm | exit | cpu cores | GPU util mean/p95 | chan overruns / discont | chan_work_max / gap_max ms | SHMIQ ov (per ring) | UHD ovf / RFSTALL / RXDISCONT | asserts | SENSE_SCHED fail/started | pthread fail |
|---|---|---|---|---|---|---|---|---|---|---|
| m4S_attempt | RX_EXIT=130 | 9.740 | 44/83 | 0 / 0 | 26.5 / 26.5 | pci415:4 pci923:5 pci508:0 | 0 / 0 / 9 | 0 | 0/1 | 0 |
cell 0 pci 415 state CELL_CONFIGURED t_pbch 13.295423363 dec/s 76.08428637378637
cell 1 pci 923 state CELL_CONFIGURED t_pbch 14.099822727 dec/s 59.48364691256814
cell 2 pci None state None t_pbch None dec/s None
