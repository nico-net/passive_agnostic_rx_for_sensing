# MPCI2 -- 2-PCI OTA 2026-11-02 (o1B): late lock of cell 1, decode p99 ~25 ms. Analysis only, no code change, no worktree.
Build: candgpuA e4729721b0 (wt/rr-candgpuA, build dir as found). Host: DGX Spark aarch64. All replays: sensing cfg ue.passive.ota.sense.dgx.cfg, MPCI_20261025.env (+ISAC_SIB1_CACHE=0, ISAC_USRP_* stripped, no
--thread-pool arg = OTA cmdline), speed 1.0, 100 s, under heavy.sh + td_measure.lock, g5_fast_pub loops the capture (2 s wideband = 50 loops, 4 s direct = 25 loops; each loop is a new ring epoch).
Recipe (copied to docs/superpowers/reports/fix1102/mpci2/): wide.sh (wideband arm; `CTL=1` = control arm), solo1.sh (direct solo, = night/replay1.sh with MPCI_20261025.env), an.py (per-cell sync/CRC/e2e), batch*.sh.

## Limits of the replay design [CODE-READ + OFFLINE VERIFIED]
- A solo run cannot be fed from the wideband capture: with a 1-cell inventory the receiver takes the direct path (no MPCI_SPAN) and the shm_iq device asserts on the 245.76 MS/s ring
  (shm_iq.c:247, decim != 1); a dummy out-of-span second cell does not change that (needs >= 2 served cells). So the "solo" arms are the direct-tuned 4 s captures raw_pci415/raw_pci923_4s_1102_0951
  (09:51, 8 min after the OTA run, different data from the 11:47 wideband capture), and the "control" arm is those two direct captures as the two cell rings of ONE process (no channelizer, device shm_iq).
- Replay loops reset the ring epoch every 2 s / 4 s, so "syncOK" counts include one re-lock per loop; fails/lock ratios are comparable between arms only inside the same loop length class.
- c0 in the wideband arm often locks a bad SSB (see F5) and the receiver exits rc=3 ("RFSTALL -- stopping capture") because a replay cannot re-init a radio; 7 of 10 wideband runs ended that way at ~75 s.
  Their c0 shows no grants. Valid wideband decode runs: w2_3, e3 (CPU channelizer), w2b_3. Direct arms: 0 of 12 ended that way.
- First attempt used ISAC_SCAN_ANT=1 by mistake (appended after the env file; OTA has =4): discarded (runs_ant1/runs_invalid).

## Per cell, 100 s replays [OFFLINE VERIFIED, DGX replay] (CRC = rv0 non-SI, probe 0; e2e = EVMGRANT e2e us p50/p99; scanq/async from last ISAC_METRICS)
| arm / run | cell | locks / failed searches | grants | CRC % | dec_us p50/p99 | tot p50/p99 | qwait p50/p99 | pre p50/p99 | async drop % | scanq_drop_full |
|---|---|---|---|---|---|---|---|---|---|---|
| solo direct s415_1 (cold) | 415 | 23/4 | 26581 | 71.0 | 3165/29582 | 6916/36024 | 1511/8388 | 57/8970 | 16.8 | 0 |
| solo direct s415_2 | 415 | 23/4 | 41392 | 76.9 | 876/3886 | 2282/13577 | 134/5456 | 50/5702 | 9.4 | 97 |
| solo direct s415_3 | 415 | 23/4 | 42561 | 63.1 | 887/3813 | 2105/12183 | 125/4855 | 50/5688 | 5.2 | 91 |
| solo direct s923_1 (cold) | 923 | 23/0 | 19529 | 63.9 | 2524/24445 | 5756/27755 | 454/7976 | 52/5219 | 7.2 | 83 |
| solo direct s923_2 | 923 | 19/4 | 20980 | 65.2 | 990/12336 | 2190/16909 | 137/6711 | 48/2500 | 2.5 | 0 |
| solo direct s923_3 | 923 | 23/1 | 19971 | 78.4 | 2212/12083 | 3786/16457 | 179/7135 | 50/4942 | 4.7 | 143 |
| control 2-cell direct c2_1 | 415 | 21/3 | 39398 | 67.3 | 877/3861 | 2702/12009 | 114/3244 | 58/4845 | 0.6 | 1704 |
| control c2_2 | 415 | 22/4 | 33183 | 61.6 | 939/4622 | 2841/17207 | 126/5957 | 60/8769 | 3.6 | 2286 |
| control c2_3 | 415 | 21/4 | 33900 | 76.9 | 861/5615 | 2524/12712 | 118/3974 | 53/5605 | 0.4 | 39 |
| control c2_1/2/3 | 923 | 11/8, 17/9, 0/15 | 0, 10, 0 | - | - | - | - | - | - | - |
| wideband w2_3 | 415 | 10/0 | 42135 | 52.9 | 1078/11766 | 3973/26425 | 182/6885 | 75/15321 | 20.8 | 8436 |
| wideband e3 (CPU channelizer) | 415 | 22/0 | 12786 | 77.4 | 1185/14919 | 4575/21296 | 868/11392 | 101/2570 | 16.8 | 9886 |
| wideband w2b_3 | 415 | 6/0 | 23107 | 70.2 | 5987/50386 | 13682/56183 | 3776/8864 | 81/32907 | 51.9 | 15753 |
| wideband w2_3 / e3 / w2b_3 | 923 | 0/14, 2/26, 0/15 | 0 | - | - | - | - | - | - | - |
OTA o1B (09:43, 300 s) for reference [MEASURED, DGX+X410 OTA]: 415 tot 3232/28548 dec 1040/25668; 923 locked at line 447675/821153, tot 6036/37526 dec 1426/25634. OTA solo 415 tot 2179/11143, dec 768/6013; 923 tot 1691/7287, dec 736/3853.
Data-mix-independent view (grants TBS < 20 kB, dec_us p50/p99): solo 742/2341; control 751/2236, 792/3176, 702/2416; wideband w2_3 901/7725, e3 1140/14578, w2b_3 5416/21021.
Grant mix (TBS >= 50 kB): solo 16.3 %, control 15.4/16.8/18.4 %, w2_3 14.5 %, w2b_3 15.7 % (e3 2.8 %): the mix is not the cause.
Cold first runs (s415_1, s923_1) show dec p99 24-30 ms in a DIRECT solo too: single replay p99 has a cold-start tail; compare warm runs / n>=3.

## Facts
F1 [OFFLINE VERIFIED, OTA log, line-order split] c0 (415) decode in o1B before cell 1 ever locked (cell 1 only searching): dec 970/28281, tot 2911/30227 (n=28851); after cell 1 locked: dec 1311/15977, tot 4190/22400.
   The 25 ms p99 is therefore not "second cell's decode load".
F2 [OFFLINE VERIFIED, DGX replay] Two cells in one process WITHOUT the channelizer (control): c0 dec p99 3.9/4.6/5.6 ms, small-grant dec p99 2.2-3.2 ms = solo. Through the wideband path (GPU or CPU channelizer):
   small-grant dec p99 7.7 / 14.6 / 21 ms. The growth sits in dec (x3) and pre (x2.7, IQ copy + FEP group) and in arr2enq p50 (1.7 vs 1.3 ms); qwait p99 not systematically worse. CPU channelizer did not help (p99 14.9, 21 overruns).
F3 [MEASURED, OTA + replay logs] Channelizer: GPU backend, ~16 % duty (run_us_attrib 1.56 s per 10 s), mean 44 us per block, but chan_work_max 24.6 ms (OTA), 32.6 / 36.1 ms (replays) per block (CPU backend 10 ms);
   overruns_seen=0, DISC_TICKET contended 35.7k of 255k grants. GPU util 59 % (GPU channelizer) vs 20 % (CPU). GPUPASS in o1B: run_us p99 16 ms, gspan p99 8 ms, ovf_waits 2450, fallback 5328 vs solo 8/4 ms, 260, 472 (OTA).
   In w2_3 the GPUPASS p99 was equal to solo (4 ms) while dec p99 was 11.8 ms: the growth is not only GPU pass time.
F4 [OFFLINE VERIFIED] Cell 1 (923) PBCH acquisition: fails also solo (OTA solo 923 fails before lock: 4, 0, 0, 4, 7, 0, 3, 6, 91; solo 415: 0). Replay per-search success: solo direct 19-23 locks / 0-4 fails;
   2-cell direct 11/8, 17/9, 0/15; wideband 0-2 locks / 10-26 fails. In every case the first scans show fo_pss ~ -fo_seed (+4.4..5.5 kHz) and fail; the CFO ladder step 2208 locks in direct solo, not in the wideband runs. 
   The pinned search only ever fails at PBCH (PSS peak/avg 25-33 dB in all arms): data/SNR/CFO-seed at the PBCH stage, not detection.
F5 [OFFLINE VERIFIED] After a lock, tracking PBCH is 100 % ok in all direct runs (pbch_ok 50/50 per window, 0 errors) but in wideband runs c0 often locks a SSB whose tracking PBCH then fails 50/50 (OTA: ssb index 5 at frame 940;
   replays: index 2 or 5; index 4 locks track). 8 failed windows -> "RFSTALL PBCH lock lost (timing runaway)" -> OTA: "recovered by full device re-init" x3 in the first 30 s (log lines 4714/8793/12058), each restarting BOTH cells
   (receive-stream discontinuity, SFO reset, cell 1 search restarts); replay: exit rc=3. This explains "both cells re-synced together several times" and part of the late lock of cell 1 in o1B.
   SYNC_SCAN is 475 ms per failed search (6 s with the ladder), so ~28 failed searches = ~2.5 min of search before the first lock, the observed half run.

## Root cause status (honest)
(1) Not established. Established: the loss is in the PBCH stage of a weak cell, present solo, worse with 2 cells without channelizer, worst through the wideband path, plus the bad-lock/re-init episodes of cell 0 that only occur in the wideband path.
(2) Not established. Established: it is a property of the wideband path (not of two cells, not of the data mix, not cell-1 decode, not the CPU/GPU backend choice). Mechanism unknown: either the channelized samples degrade SSB/PBCH SNR and the
    tail (fidelity), or the live channelizer's runtime (GPU stream sharing, 25-36 ms block stalls, memory traffic) perturbs decode (load). The two predict different outcomes of the plan below.
No fix implemented (task C: root cause not clear).

## ONE plan (with stop condition)
Build the missing pre-channelized control FROM the wideband data: run the receiver's own channelizer offline (iq_chan CPU backend, test_iq_chan style tool, one cell per output dir, 11:47 capture) to produce
nrcell_pci415 / nrcell_pci923 raw captures, publish them through the existing control arm (CTL=1, two direct rings, no live channelizer), n=3, 100 s.
Decision: (a) control-from-channelizer behaves like the direct control (c1 locks, no bad c0 lock, small-grant dec p99 <= 3.2 ms) -> the samples are fine, the cost is the live channelizer (profile GPU stream/priority, block size and
the 25-36 ms stalls next); (b) it behaves like the wideband arm (c1 fails, bad locks) -> the channelized samples degrade SSB decoding: bench SSB-band SNR/phase through iq_chan (decim 2, 70 dB half-band, NCO +/-39.99 MHz, 923 band edge at
the span edge 3799.05) against the direct capture, test-first. STOP: after this single experiment, whichever branch.
