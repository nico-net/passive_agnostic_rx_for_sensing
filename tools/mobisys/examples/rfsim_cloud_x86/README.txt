REAL rfsim example of the tools/mobisys exporter. Evidence label: [SIM VERIFIED, cloud x86 rfsim]  (upper bound, NOT OTA).

Host:      cloud x86 (Intel Xeon 2.8 GHz, 4 cores, 16 GB, no GPU, no IPv6), NOT the DGX.
Bed:       OAI gNB `nr-softmodem --phy-test --noS1 -D 0xff -O tests/passive_rx/gnb.sa.rfsim.conf --rfsim -m 9 -n 0 -M 106 -l 1
           --log_config.nr_mac_log_level debug` (virtual UE RNTI 0x1234, no core network, no separate test UE) + passive receiver
           `nr-uesoftmodem --passive-rx --rfsim -O tests/passive_rx/ue.passive.bwp.agn.conf -C 3319680000 -r 106 --ssb 516
           --numerology 1 --band 78 --ue-nb-ant-rx 4 --sensing.pdcch_blind_monitor_scan_thread 1:8:-1` (+ ISAC_PDSCH_CORES=1,2,3,0
           ISAC_PUSCH_CORES=2 ISAC_UL_SNAP_CORE=3 so the default core pins do not assert on 4 CPUs).
PRB / RX:  106 PRB, 4 RX (the 1-RX attempts behaved identically).
Commit:    b91ca51 (binaries built from the working tree at that commit, cloud x86, -DOAI_USRP=OFF).
Pipeline:  receiver 2>&1 | python3 tools/mobisys/export.py --experiment-mode lab|commercial --host-label "cloud x86"
Duration:  lab 401 s (manifest duration_s), commercial 106 s.

IMPORTANT LIMITATION (honest): on this host the receiver never reached TB decoding. DCI 1_1 of RNTI 0x1234 was found and
length-locked (46 b, UL 0_1 len 39 banked), but "DL layout requires TB-CRC evidence" never resolved and pdschq_queued stayed 0
in every run (1 RX and 4 RX, up to 400 s). So there are NO rx_grants, retry_events, cell_aggregates or latency_histograms, and
all TB recovery / CRC / e2e latency figures are N/A; 9 plots are not drawn (plots/MISSING.txt). This example validates the
exporter on real lines and DCI-level ground truth only. Not investigated here (C code untouched).

Contents:
  lab/            lab export. dci_attempts, system_telemetry, slot_health are TRUNCATED to the first 3 parts of the 401 s run;
                  manifest.json and summary.json still describe the full run (rows_written 101480, 0 parse errors, 0 queue drops).
  lab/groundtruth/summary.json   groundtruth.py on THIS truncated copy vs the full gNB log (gNB log not shipped, ~100 MB):
                  gnb_grants 6290, tp 5980, fn 310, fp 0 (the first slots precede receiver sync).
  groundtruth_full_401s_summary.json   same tool on the full lab export: gnb_grants 86998, DCI precision 1.000, recall 0.9962
                  (CI95 0.9958-0.9966), F1 0.998, false alarms 0/1000 slots, offset 0 slots; TB classes: 86667 dropped_undecidable
                  (DCI found, no TB decode), 331 missed, 0 correct_tb.
  commercial/     commercial export (106 s, K=10, 60 s windows): no RNTI, no per-grant rows. Privacy grep: RNTI 4660 / 0x1234 /
                  1234 absent from every file (parsed parquet values and raw bytes).
  plots/          cpu_rss, discovery_milestones, groundtruth_classes (png + csv + json only).
ISAC_METRICS rows carry the new keys: t_real_ns, proc_{utime_us,stime_us,maxrss_kb,minflt,majflt}, cfg_epoch, cfg_identity_gen,
retry_yield_{chains,yields,gated}, recov_* (all zero here; async recovery not exercised).
