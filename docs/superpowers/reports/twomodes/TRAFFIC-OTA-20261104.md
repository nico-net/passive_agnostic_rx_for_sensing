# Traffic-mode OTA A/B, 2026-11-04 night

Label: [OTA LOW LOAD] (02:10-03:40 CET), FUNCTIONAL ONLY. Bound verdicts are provisional; the A/B must be repeated in daytime (>= 08:30) before any verdict.
Host: DGX (GB10) + X410 34B2AF1, 4 RX, gain 38, DUR 240 s, SETTLE 45 s, ISAC_SIB1_CACHE=0, MPCI_20261025.env. Build: 1d0b5cf108 fix(twomodes): UL never reopens DL, dci/decode join keys, null bits when unavailable, consistent DCI counters (same BUILD for both arms; L = no mode key, T = mode "traffic").
Configs: tests/passive_rx/traffic/{legacy,traffic}_ota.dgx.cfg (differ by one line). Script: tests/passive_rx/traffic/ota_traffic_ab.sh. Scorer: tools/traffic/score_traffic_ab.py.
Campaign dir: /home/nicola/NICOLA/ota_runs/tm_ota_20261104_021017 (runs.tsv, score.txt). Never pooled across cells/cases. No tuning.

Smoke (SMOKE=1, DUR 60, PCI 415, tmT0S415_1_020804): sync PCI 415, 597 type=dci records (all layout_state=fixed, 0 solved in 60 s), traffic keys present, dci_accepted == dci_rec_generated (543), no assert/crash, RX_EXIT=130 (clean SIGINT, same as legacy runs).

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

## Scorer output [OTA LOW LOAD]
```

== case B cell PCI 415: L runs=3 T runs=3
  acc       L mean=n/a sd=n/a | T mean=2387.7 sd=44.0
  gen       L mean=n/a sd=n/a | T mean=2387.7 sd=44.0
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=2.7 sd=4.6 | T mean=0.0 sd=0.0
  drop      L mean=11717.7 sd=12396.5 | T mean=4357.7 sd=4800.7
  cpu       L mean=10.3 sd=0.2 | T mean=10.2 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=55.0 sd=4.6
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved>=90%L           FAIL T=0.0 L=2.7 (0%)
  scanq_drop<=L+sd       PASS T=4357.7 L=11717.7 sd=12396.5
  cpu T<L                PASS T=10.2 L=10.29 cores
  field agreement>=99%   n/a  n/a OTA

== case B cell PCI 923: L runs=3 T runs=3
  acc       L mean=n/a sd=n/a | T mean=50503.7 sd=18253.2
  gen       L mean=n/a sd=n/a | T mean=50503.7 sd=18253.2
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=14.7 sd=4.7 | T mean=14.7 sd=3.1
  drop      L mean=525.3 sd=139.5 | T mean=7126.0 sd=4247.2
  cpu       L mean=10.3 sd=0.2 | T mean=10.2 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=34.3 sd=7.2
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved>=90%L           PASS T=14.7 L=14.7 (100%)
  scanq_drop<=L+sd       FAIL T=7126.0 L=525.3 sd=139.5
  cpu T<L                PASS T=10.2 L=10.29 cores
  field agreement>=99%   n/a  n/a OTA

== case S415 cell PCI 415: L runs=3 T runs=3
  acc       L mean=n/a sd=n/a | T mean=6756.0 sd=6589.9
  gen       L mean=n/a sd=n/a | T mean=6756.0 sd=6589.9
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=1.0 sd=1.0 | T mean=6.3 sd=11.0
  drop      L mean=90.3 sd=37.9 | T mean=45.7 sd=27.2
  cpu       L mean=5.9 sd=0.2 | T mean=5.8 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=21.3 sd=6.8
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved>=90%L           PASS T=6.3 L=1.0 (633%)
  scanq_drop<=L+sd       PASS T=45.7 L=90.3 sd=37.9
  cpu T<L                PASS T=5.76 L=5.93 cores
  field agreement>=99%   n/a  n/a OTA

== case S923 cell PCI 923: L runs=3 T runs=3
  acc       L mean=n/a sd=n/a | T mean=38039.7 sd=5304.5
  gen       L mean=n/a sd=n/a | T mean=38039.7 sd=5304.5
  lost      L mean=n/a sd=n/a | T mean=0.0 sd=0.0
  solved    L mean=24.0 sd=7.9 | T mean=26.3 sd=3.1
  drop      L mean=10310.0 sd=17763.0 | T mean=42602.0 sd=70611.5
  cpu       L mean=6.5 sd=0.3 | T mean=6.1 sd=0.1
  emit_p99  L mean=n/a sd=n/a | T mean=20.0 sd=5.3
  accepted==generated    PASS 0/3 runs differ
  rec_lost==0            PASS lost=[0, 0, 0]
  solved>=90%L           PASS T=26.3 L=24.0 (110%)
  scanq_drop<=L+sd       FAIL T=42602.0 L=10310.0 sd=17763.0
  cpu T<L                PASS T=6.12 L=6.5 cores
  field agreement>=99%   n/a  n/a OTA

failed bounds: 3
```

## Failed bounds (3 of 20 scored, all provisional)
1. B / PCI 415: solved RNTIs T=0.0 vs L=2.7 (L sd 4.6). In the pair run the 415 cell had almost no 1_1 traffic; T found only fixed-layout DCIs. Single 415 shows T=6.3 vs L=1.0. Low counts at night; not interpretable.
2. B / PCI 923: scanq_drop T=7126 vs L=525+139.5 sd.
3. S923: scanq_drop T=42602 (sd 70611, one or more runs far above the rest) vs L=10310+17763 sd. T drop is higher on the busy 923 cell in both the single and the pair case. This needs the daytime repeat and a look at per-run values before any conclusion; no cause is claimed here.

## Notes and concerns
- Passing here: dci_accepted == dci_rec_generated in all 18 T cell-runs, dci_rec_lost == 0 everywhere, CPU T < L in every case (differences small: 5.76 vs 5.93, 6.12 vs 6.50 cores; 10.2 vs 10.3 for the pair) [OTA LOW LOAD]. dci_emit_us_p99 20-55 us (reported only).
- Definitions: legacy dci_accepted_* keys do not exist (shown n/a). "Solved RNTIs": T = distinct rnti of type=dci records with layout_state solved; L = distinct rnti (excluding 65535) of type=grant records (legacy has no layout_state), so the bound compares different quantities. CPU is process-level mean cores from /proc (5 s samples, whole run incl. sync), so in the pair both cells show the same value. scanq_drop = full+stale+epoch.
- Field agreement: n/a OTA (needs identical (abs_slot,rnti) across arms).
- Arms are alternated but cells carry live traffic that differs between runs (sd is large for drop counts).
- Frozen check (sens6): exit 0.
