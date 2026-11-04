# Traffic-mode OTA A/B, 2026-11-04 night

Label: [OTA LOW LOAD] (02:10-03:40 CET), FUNCTIONAL ONLY. Bound verdicts are provisional; the A/B must be repeated in daytime (>= 08:30) before any verdict.
Host: DGX (GB10) + X410 34B2AF1, 4 RX, gain 38, DUR 240 s, SETTLE 45 s, ISAC_SIB1_CACHE=0, MPCI_20261025.env. Build (commit at HEAD when run; NOT provable from the runs, see notes): 1d0b5cf108 fix(twomodes): UL never reopens DL, dci/decode join keys, null bits when unavailable, consistent DCI counters (same BUILD for both arms; L = no mode key, T = mode "traffic").
Configs: tests/passive_rx/traffic/{legacy,traffic}_ota.dgx.cfg (differ by one line). Script: tests/passive_rx/traffic/ota_traffic_ab.sh. Scorer: tools/traffic/score_traffic_ab.py.
Campaign dir: /home/nicola/NICOLA/ota_runs/tm_ota_20261104_021017 (runs.tsv, score.txt). Never pooled across cells/cases. No tuning.

Smoke (SMOKE=1, DUR 60, PCI 415, tmT0S415_1_020804): sync PCI 415, 597 type=dci records (all layout_state=fixed, 0 solved in 60 s), traffic keys present, dci_accepted == dci_rec_generated (543 in the last periodic ISAC_METRICS line; the record file holds 597, i.e. records >= last-metrics counters; dci_rec_persisted == generated was checked, e.g. tmT1B 71555/71555), no assert/crash, RX_EXIT=130 (clean SIGINT, same as legacy runs).

## runs.tsv (all 18 runs PCI OK, RX_EXIT=130, no assertion/segfault/backtrace in any run.log)
```
tmL1S415	/home/nicola/NICOLA/ota_runs/tmL1S415_1_021017	415	OK
tmT1S415	/home/nicola/NICOLA/ota_runs/tmT1S415_1_021513	415	OK
tmL2S415	/home/nicola/NICOLA/ota_runs/tmL2S415_1_022009	415	OK
tmT2S415	/home/nicola/NICOLA/ota_runs/tmT2S415_1_022508	415	OK
tmL3S415	/home/nicola/NICOLA/ota_runs/tmL3S415_1_023005	415	OK
tmT3S415	/home/nicola/NICOLA/ota_runs/tmT3S415_1_023500	415	OK
tmL1S923	/home/nicola/NICOLA/ota_runs/tmL1S923_1_023956	923	OK
tmT1S923	/home/nicola/NICOLA/ota_runs/tmT1S923_1_024458	923	OK
tmL2S923	/home/nicola/NICOLA/ota_runs/tmL2S923_1_024956	923	OK
tmT2S923	/home/nicola/NICOLA/ota_runs/tmT2S923_1_025457	923	OK
tmL3S923	/home/nicola/NICOLA/ota_runs/tmL3S923_1_025956	923	OK
tmT3S923	/home/nicola/NICOLA/ota_runs/tmT3S923_1_030501	923	OK
tmL1B	/home/nicola/NICOLA/ota_runs/tmL1B_1_031001	415 923	OK
tmT1B	/home/nicola/NICOLA/ota_runs/tmT1B_1_031508	415 923	OK
tmL2B	/home/nicola/NICOLA/ota_runs/tmL2B_1_032015	415 923	OK
tmT2B	/home/nicola/NICOLA/ota_runs/tmT2B_1_032523	415 923	OK
tmL3B	/home/nicola/NICOLA/ota_runs/tmL3B_1_033029	415 923	OK
tmT3B	/home/nicola/NICOLA/ota_runs/tmT3B_1_033533	415 923	OK
```

## Scorer output [OTA LOW LOAD] (fix round 1 scorer)
```

== case B cell PCI 415: L runs=3 T runs=3
  queued    L mean=316915.0 sd=2122.5 | T mean=320074.3 sd=23.7
  acc       L mean=n/a sd=n/a | T mean=2387.7 sd=44.0
  gen       L mean=n/a sd=n/a | T mean=2387.7 sd=44.0
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=2.7 sd=4.6 | T mean=0.0 sd=0.0
  drop      L mean=11717.7 sd=12396.5 | T mean=4357.7 sd=4800.7
  drop_pct  L mean=3.7 sd=3.9 | T mean=1.4 sd=1.5
  cpu       L mean=10.3 sd=0.2 | T mean=10.2 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=55.0 sd=4.6
  per-run L: q=314548 drop=4361 (1.4%) cpu=10.257; q=317548 drop=4762 (1.5%) cpu=10.124; q=318649 drop=26030 (8.2%) cpu=10.503
  per-run T: q=320094 drop=1813 (0.6%) cpu=10.175; q=320081 drop=1365 (0.4%) cpu=10.088; q=320048 drop=9895 (3.1%) cpu=10.343
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=0.0 L=2.7 info
  scanq drop_pct<=L+sd   PASS T=1.4% L=3.7% sd=3.9
  cpu T<L-sd             FAIL T=10.2 L=10.3 sd=0.2 cores (process)
  field agreement>=99%   n/a  n/a OTA

== case B cell PCI 923: L runs=3 T runs=3
  queued    L mean=316066.3 sd=2626.0 | T mean=708285.7 sd=1919.9
  acc       L mean=n/a sd=n/a | T mean=50503.7 sd=18253.2
  gen       L mean=n/a sd=n/a | T mean=50503.7 sd=18253.2
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=14.7 sd=4.7 | T mean=14.7 sd=3.1
  drop      L mean=525.3 sd=139.5 | T mean=7126.0 sd=4247.2
  drop_pct  L mean=0.2 sd=0.0 | T mean=1.0 sd=0.6
  cpu       L mean=10.3 sd=0.2 | T mean=10.2 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=34.3 sd=7.2
  per-run L: q=313437 drop=630 (0.2%) cpu=10.257; q=316073 drop=367 (0.1%) cpu=10.124; q=318689 drop=579 (0.2%) cpu=10.503
  per-run T: q=708487 drop=3370 (0.5%) cpu=10.175; q=710097 drop=6273 (0.9%) cpu=10.088; q=706273 drop=11735 (1.7%) cpu=10.343
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=14.7 L=14.7 info
  scanq drop_pct<=L+sd   FAIL T=1.0% L=0.2% sd=0.0
  cpu T<L-sd             FAIL T=10.2 L=10.3 sd=0.2 cores (process)
  field agreement>=99%   n/a  n/a OTA

== case S415 cell PCI 415: L runs=3 T runs=3
  queued    L mean=336051.7 sd=11.6 | T mean=445175.3 sd=188965.6
  acc       L mean=n/a sd=n/a | T mean=6756.0 sd=6589.9
  gen       L mean=n/a sd=n/a | T mean=6756.0 sd=6589.9
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=1.0 sd=1.0 | T mean=6.3 sd=11.0
  drop      L mean=90.3 sd=37.9 | T mean=45.7 sd=27.2
  drop_pct  L mean=0.0 sd=0.0 | T mean=0.0 sd=0.0
  cpu       L mean=5.9 sd=0.2 | T mean=5.8 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=21.3 sd=6.8
  per-run L: q=336065 drop=71 (0.0%) cpu=5.973; q=336046 drop=66 (0.0%) cpu=5.751; q=336044 drop=134 (0.0%) cpu=6.06
  per-run T: q=663374 drop=67 (0.0%) cpu=5.883; q=336090 drop=15 (0.0%) cpu=5.709; q=336062 drop=55 (0.0%) cpu=5.688
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=6.3 L=1.0 info
  scanq drop_pct<=L+sd   PASS T=0.0% L=0.0% sd=0.0
  cpu T<L-sd             PASS T=5.8 L=5.9 sd=0.2 cores (process)
  field agreement>=99%   n/a  n/a OTA

== case S923 cell PCI 923: L runs=3 T runs=3
  queued    L mean=330720.7 sd=9244.6 | T mean=732743.0 sd=13702.8
  acc       L mean=n/a sd=n/a | T mean=38039.7 sd=5304.5
  gen       L mean=n/a sd=n/a | T mean=38039.7 sd=5304.5
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=24.0 sd=7.9 | T mean=26.3 sd=3.1
  drop      L mean=10310.0 sd=17763.0 | T mean=42602.0 sd=70611.5
  drop_pct  L mean=3.1 sd=5.3 | T mean=5.9 sd=9.8
  cpu       L mean=6.5 sd=0.3 | T mean=6.1 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=20.0 sd=5.3
  per-run L: q=336076 drop=30821 (9.2%) cpu=6.203; q=320046 drop=55 (0.0%) cpu=6.782; q=336040 drop=54 (0.0%) cpu=6.53
  per-run T: q=744821 drop=165 (0.0%) cpu=6.029; q=717852 drop=124114 (17.3%) cpu=6.057; q=735556 drop=3527 (0.5%) cpu=6.259
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=26.3 L=24.0 info
  scanq drop_pct<=L+sd   PASS T=5.9% L=3.1% sd=5.3
  cpu T<L-sd             PASS T=6.1 L=6.5 sd=0.3 cores (process)
  field agreement>=99%   n/a  n/a OTA

failed bounds: 3
```

## Failed bounds (3, all provisional, [OTA LOW LOAD])
1. Pair, PCI 923: scanq drop_pct T 1.0 % vs L 0.2 % (sd 0.0). Per run T 0.5/0.9/1.7 %, L 0.1-0.2 %.
2. Pair, PCI 415 and pair, PCI 923: cpu T < L - sd fails (10.2 vs 10.3 cores, L sd 0.2), i.e. not distinguishable.
Passing on every cell: dci_accepted == dci_rec_generated (12/12 T cell-runs), dci_rec_lost == 0. S415 and S923 drop_pct and cpu pass, but S923 L/T drop_pct are bursty (sd > mean; T run 2 17.3 %, L run 1 9.2 %), so these passes are weak.
Solved RNTIs: n/a (not comparable), excluded from the failed-bound count. Field agreement: n/a OTA.

## Notes and open items (no cause claimed for any)
- Solved-RNTI definitions differ between arms (T: distinct RNTIs of type=dci with layout_state solved; L: distinct non-65535 RNTIs of type=grant, DL+UL, 1_0 C-RNTI included), so the row is info only.
- CPU confound: T does less work (no CFR/PRACH/SRS in traffic mode, less LDPC; e.g. tmT3S923 vs tmL3S923 ldpc_tb_cpu 14.6k vs 66.2k, sense_rec_count 43.9k vs 366.8k). CPU is process-level (5 s /proc samples, whole run incl. sync); in the pair both cells show the same number.
- OPEN: scanq_queued on PCI 923 is 2.1-2.2x higher in T than L (single 717.9-744.8k vs 320.0-336.1k; pair cell 706-710k vs 313-319k); T1S415 (the run with 1_1 traffic) also doubled (663.4k). Offered scan load differs between arms; see the [CODE] item below. This is why the bound is on drop/queued.
- [CODE] T enables the pre-existing early-start path (no DM-RS source; es_global_ok() in nr_pdcch_blind_monitor_rt.c, early_post called from nr-ue.c) -> about 2 scan jobs per banked slot; this explains the 2.1-2.2x scanq_queued. Raw drop counts are not comparable across arms; drop_pct is the comparable metric. Whether early-job drops lose any DCI (the end-of-slot job re-runs banks not marked done) is not measured.
- OPEN: scanq_drop_epoch is 0 in all T cell-runs but 5-69 in the L pair cell-runs; unexplained.
- Runs embed the stale configure-time hash df9b20eab7 in the session record; the build at 1d0b5cf108 is plausible (binary built after the last source edit, before the commit) but not provable from the runs.
- Order aliasing: L always runs first in each L/T pair, so slow drift in the live cells is aliased into the arm. Alternate the order (T first on even repeats) in the daytime repeat.
- Live traffic differs run to run (3 runs per arm, bursty drops), so means/sd of drops are weak; use the per-run lines.
- 415: 1_1 was absent in 5 of 6 T runs (only fixed 1_0 records); "no 1_1 traffic" cannot be told from "no dedicated CORESET discovered" with these data.
- Frozen check (sens6): exit 0.

## Daytime repeat 2026-11-04 08:00-09:33 [MEASURED OTA], build 4759260bbf (final fixes; early start off in traffic mode)

Campaign /home/nicola/NICOLA/ota_runs/tm_ota_20261104_080000, 18/18 runs pci=OK, no assert/segfault. Run folders may embed a newer git hash: the binary was not rebuilt during the campaign (heavy.lock held exclusive).

```

== case B cell PCI 415: L runs=3 T runs=3
  queued    L mean=315602.3 sd=4342.3 | T mean=319244.3 sd=1347.3
  solved    L mean=46.0 sd=24.2 | T mean=67.3 sd=29.7
  drop      L mean=120604.3 sd=103271.0 | T mean=89343.3 sd=121914.2
  drop_pct  L mean=38.0 sd=32.4 | T mean=27.9 sd=38.0
  cpu       L mean=11.5 sd=0.5 | T mean=11.4 sd=0.9
  emit_p99  L mean=n/a sd=n/a | T mean=28.7 sd=6.4
  per-run L: q=310596 drop=25436 (8.2%) cpu=11.287; q=318347 drop=105962 (33.3%) cpu=12.131; q=317864 drop=230415 (72.5%) cpu=11.145
  per-run T: q=318456 drop=20156 (6.3%) cpu=11.764; q=320800 drop=230111 (71.7%) cpu=10.322; q=318477 drop=17763 (5.6%) cpu=11.969
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=67.3 L=46.0 info
  scanq drop_pct<=L+sd   PASS T=27.9% L=38.0% sd=32.4
  cpu T<L-sd             FAIL T=11.4 L=11.5 sd=0.5 cores (process)
  field agreement>=99%   n/a  n/a OTA

== case B cell PCI 923: L runs=3 T runs=3
  queued    L mean=314111.7 sd=5528.0 | T mean=318956.0 sd=933.6
  solved    L mean=56.0 sd=9.0 | T mean=60.0 sd=4.6
  drop      L mean=78605.0 sd=50459.6 | T mean=38221.0 sd=13165.3
  drop_pct  L mean=24.8 sd=15.8 | T mean=12.0 sd=4.1
  cpu       L mean=11.5 sd=0.5 | T mean=11.4 sd=0.9
  emit_p99  L mean=n/a sd=n/a | T mean=31.0 sd=4.0
  per-run L: q=307764 drop=20352 (6.6%) cpu=11.287; q=317868 drop=108784 (34.2%) cpu=12.131; q=316703 drop=106679 (33.7%) cpu=11.145
  per-run T: q=318407 drop=23095 (7.3%) cpu=11.764; q=320034 drop=47099 (14.7%) cpu=10.322; q=318427 drop=44469 (14.0%) cpu=11.969
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=60.0 L=56.0 info
  scanq drop_pct<=L+sd   PASS T=12.0% L=24.8% sd=15.8
  cpu T<L-sd             FAIL T=11.4 L=11.5 sd=0.5 cores (process)
  field agreement>=99%   n/a  n/a OTA

== case S415 cell PCI 415: L runs=3 T runs=3
  queued    L mean=336586.0 sd=867.8 | T mean=336060.0 sd=29.3
  solved    L mean=128.0 sd=54.1 | T mean=157.7 sd=14.8
  drop      L mean=58655.7 sd=100860.2 | T mean=90.3 sd=58.9
  drop_pct  L mean=17.4 sd=29.9 | T mean=0.0 sd=0.0
  cpu       L mean=6.6 sd=0.7 | T mean=6.7 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=14.3 sd=0.6
  per-run L: q=337588 drop=175119 (51.9%) cpu=5.778; q=336089 drop=371 (0.1%) cpu=6.942; q=336081 drop=477 (0.1%) cpu=7.096
  per-run T: q=336050 drop=28 (0.0%) cpu=6.772; q=336093 drop=145 (0.0%) cpu=6.643; q=336037 drop=98 (0.0%) cpu=6.687
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=157.7 L=128.0 info
  scanq drop_pct<=L+sd   PASS T=0.0% L=17.4% sd=29.9
  cpu T<L-sd             FAIL T=6.7 L=6.6 sd=0.7 cores (process)
  field agreement>=99%   n/a  n/a OTA

== case S923 cell PCI 923: L runs=3 T runs=3
  queued    L mean=336050.7 sd=3.2 | T mean=336054.3 sd=1.5
  solved    L mean=89.0 sd=6.9 | T mean=103.3 sd=6.5
  drop      L mean=8210.0 sd=6175.3 | T mean=362.7 sd=142.8
  drop_pct  L mean=2.4 sd=1.8 | T mean=0.1 sd=0.0
  cpu       L mean=7.6 sd=0.1 | T mean=7.4 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=13.7 sd=1.5
  per-run L: q=336053 drop=7864 (2.3%) cpu=7.629; q=336047 drop=14551 (4.3%) cpu=7.644; q=336052 drop=2215 (0.7%) cpu=7.447
  per-run T: q=336053 drop=330 (0.1%) cpu=7.511; q=336056 drop=519 (0.2%) cpu=7.289; q=336054 drop=239 (0.1%) cpu=7.495
  rec_lost==0            PASS lost=[0, 0, 0]
  solved RNTIs           n/a  n/a (not comparable); T=103.3 L=89.0 info
  scanq drop_pct<=L+sd   PASS T=0.1% L=2.4% sd=1.8
  cpu T<L-sd             PASS T=7.4 L=7.6 sd=0.1 cores (process)
  field agreement>=99%   n/a  n/a OTA

failed bounds: 3
```

Reading (per cell, not merged):
- Records: accepted == generated and lost == 0 in every T cell-run.
- scanq_queued now equal across arms (early start off in T), so drop_pct compares like for like.
- Single cells: T scan drop_pct 0.0-0.1 %; L 0.1-51.9 % (bursty).
- Pair: both arms overloaded (process ~11.5 cores); drop_pct T 6-72 % vs L 7-73 % on 415, T 7-15 % vs L 7-34 % on 923.
- CPU: T is not cheaper than L (S923 -0.2 cores; others within sd). Failed bounds: 3, all cpu T<L-sd. Cause not measured.
