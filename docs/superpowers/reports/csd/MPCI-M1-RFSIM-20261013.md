# MPCI M1 (shared-memory IQ fan-out) on rfsim, 2026-10-13

Host: cloud x86 Xeon 4c, 15 GB, no GPU. Label base: `[SIM VERIFIED, cloud x86 Xeon 4c, rfsim 106 PRB, @4bf6394 + uncommitted M1 test script]`
unless stated (runs 2,3,7 used the pre-review binary, 53354e0/e052bbd).

## Commands
```
flock /tmp/claude-0/ninja.lock tests/passive_rx/mpci/rfsim_shm_fanout.sh <OUT> <secs> [N_consumers]
# env: PIN=1 (taskset gNB 0 / pub 1 / consumers 2-3), NICE_PUB, PUBEXTRA (extra pub-only flags), RING, CONSUMER_DELAY
```
gNB `nr-softmodem --phy-test --rfsim -m 9 -n 0 -M 106 -l 1` (gnb.sa.rfsim.conf, V4 shim); publisher = reference passive UE
(`--rfsim -C 3319680000 -r 106 --ssb 516`) + `ISAC_SHM_IQ_PUB=mpci_ring`; consumer = same args, no `--rfsim`, cfg = ue.passive.bwp.agn.conf +
`device={name="shm_iq"}; loader={shm_iq={shlibpath=<build>}}`, `ISAC_SHM_IQ_NAME=mpci_ring ISAC_SHM_IQ_CELL=c<i>`, own cwd/metrics, `ISAC_CORE_PLAN=0`.
Consumer attaches 20 s after the publisher.

## Results
| Check | Result | Label |
|---|---|---|
| Consumer attaches, PBCH sync, PCI | READY/START, `Initial sync successful, PCI: 0`, same MIB (coreset0=11 k_ssb=31, no CORESET#0 on this SSB, so no SIB1: phy-test bed has none) as publisher | run6/run9 |
| Ring D=1 passthrough | `SHMIQ_TUNE rx_hz=3319680000 ring_center=3319680004 shift=-4 Hz decim=1`; `SHMIQ_END reads=219907 overruns=0 dropped_in=0 discont=0` (240 s) | run6 |
| DCI detection | consumer saw 12417 of the publisher's 12418 distinct (sfn,slot,fmt,cce) rnti=0x1234 DCIs, 0 consumer-only | run3 (old binary) |
| 2 consumers + pub | both SYNC PCI 0; c1 reads=106486, c2 reads=160891, overruns=0 discont=0 (150 s). CPU user+sys: pub 190 s, c1 66 s, c2 99 s | run9 |
| PDSCH CRC (full decode, pub + consumer) | pub 96-97 % (46305/48019) but consumer 3.6-11 % and Technique D never CONVERGED | run2/3/7 |

## Caveat: the CRC equivalence is NOT demonstrated
gNB (~0.7 core) + a full decoding UE (~2 cores) leaves < 1 core on this 4-core box; the consumer got 70-77 % of one core, was starved
(`PDSCHQ dropped stale=25160`, `max_lag_slots=447`, `scanq_drop_full` 165/175) and never converged; its sys time is high (20 us nanosleep poll in
`shm_iq.c` read_iq, 1m27 sys vs 2m16 user in run7). Pinning the publisher to 1 core (run4) made the whole bed crawl (2.4 G samples in 290 s).
Neither a degraded publisher (`PUBEXTRA` with blind-monitor autoconf off, which skips PDSCH decoding) nor nice helps (RT SCHED_FIFO threads ignore nice).
A fair CRC comparison needs >= 6 cores (or a DGX run): [NOT RUN].

## Anomaly (unexplained, 1 of 3 runs on @4bf6394)
run8 (light pub, full consumer, 270 s): consumer exited ~60 s after pub start with `SHMIQ_PRODUCER_LOST pid=12250 pos=4334102705` and
`Exiting OAI softmodem: shm IQ producer lost` (shm_iq.c:144) although the publisher was alive and busy to the end (pub CPU 4m25 user).
Heartbeat was stale > 60 s while the pid existed: the publisher apparently stopped publishing (or stopped updating the heartbeat) after `SHMIQ_PUB_RETUNE epoch=1`
(last SHMIQ line in pub log). Not reproduced in run6/run9. Evidence: scratchpad run8/{pub,c1}/rx.log.

## Goal 2 (D=2): skipped
rfsim gNB 106 PRB at 61.44 MS/s; k_ssb=31 (>=24) and the SSB at `--ssb 516` is 516 PRB-subcarriers-offset in a 106 PRB grid; the 51-PRB window (30.72 MS/s) would have to be
centred on the SSB at the grid edge, and this bed's MIB has no CORESET#0 (phy-test), so the criterion "SSB/CORESET#0 fit in 51 PRB" cannot be evaluated or exercised. [NOT RUN]
