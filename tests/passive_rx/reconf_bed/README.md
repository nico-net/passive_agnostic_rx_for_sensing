# reconf_bed: live reconfiguration under ONE running passive receiver (DGX rfsim, phy-test)

Status: written 2026-10-30, NOT RUN (an OTA campaign held the radio and `/tmp/td_measure.lock`). Thresholds are
PROVISIONAL, scorer logic is [OFFLINE VERIFIED] only on synthetic lines (`test_score_reconf.py`). Nothing here is a measurement.

## Design (why no relay is needed)
The rfsim passive client survives a server restart [CODE-READ radio/rfsimulator/simulator.cpp]:
`socketError` (:411) sets `reconnect_pending` for `passive_client` (set at :1833 when `--passive-rx`); `rfsimulator_read` calls
`startClient` again on the reader thread (:1589); `startClient` retries connect every 1 s until the new server accepts (:962-996), rebases
`nextRxTstamp` to the new server's first timestamp, forces `nextRxTstamp != previous` so the UE sees RXDISCONT (:1025-1031), and logs
`rfsim passive client reconnected after N ms` (:1034), which the scorer requires. TX is muted and a silence block drives the new
server's clock (:1060, :1599-1613). So the bed is: gNB A (phy-test) + passive RX -> SIGINT gNB A -> gNB B with a changed cell on the same
port -> same RX process continues. The receiver is never restarted, so its CONFIG_EPOCH state, solver and bank are the thing under test.
Fully blind receiver cfg: `tests/passive_rx/dgx/ue.passive.auto.100mhz.dgx.cfg` (autoconf/autodiscover/full_auto, no cell hints); the
bed/passive_nochan.cfg style cfgs pin PCI-0 monitor hints and would make S1 meaningless. `ISAC_RECONF` is set by main() under `--passive-rx`
(PROJECT_MEMORY section 25 note); the OTA env also sets it.

Fallback if a future change breaks client reconnect: use `tests/passive_rx/sa_bed/` (R13: Open5GS SA, `run_arm.sh`, `score_r13.py`,
scenarios cell_restart/same_cell_restart_size_change). Open5GS install, only if `which open5gs-amfd` is empty (sudo allowed; NOT executed):
`sudo add-apt-repository -y ppa:open5gs/latest && sudo apt-get update && sudo apt-get install -y open5gs mongodb-org` (or `mongodb`),
then `sudo systemctl enable --now open5gs-amfd open5gs-smfd open5gs-upfd open5gs-nrfd open5gs-udmd open5gs-udrd open5gs-ausfd`,
set PLMN 001/06 and add the subscriber from `sa_bed/open5gs-subscriber.cfg`; follow `sa_bed/RUNBOOK.md`.

## Files
- `run_reconf.sh <out_dir> <S0|S1|S2|S2c|S3> [dur=240]`: one scenario. WARM=90 s on gNB A (`-m 9 -l 4 -M 273`, frozen 273-PRB rank-4 conf), SIGINT,
  wait exit, GAP=3 s, gNB B. Receiver: 4 RX, 273 PRB, `-C 3750000000 --ssb 1478`, env from `PENV` (default MPCI_20261025.env, `ISAC_METRICS_PATH` added),
  `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps`. Only SIGINT is used; if gNB A ignores it for 90 s the run aborts (no SIGKILL). Refuses if any nr-softmodem/
  nr-uesoftmodem runs or if the caller does not hold the lock (`ALLOW_NOLOCK=1` overrides).
- `gnb_pci1.cfg` (physCellId 0->1), `gnb_tdd_6d3u.cfg` (DL slots 7->6, UL slots 2->3), `gnb_ss0_1.cfg` (searchSpaceZero 0->1): sed copies of the frozen conf, one line each (verified by diff).
- `stamp.py` wall-stamps the receiver log; `score_reconf.py`; `test_score_reconf.py`.

## Commands (never while the OTA campaign runs; caller takes the lock)
    cd /home/nicola/NICOLA/wt/rr-mpci-main
    for S in S0 S1 S2 S2c S3; do
      flock -x /tmp/td_measure.lock tests/passive_rx/reconf_bed/run_reconf.sh /tmp/reconf_$S $S 240
      python3 -I tests/passive_rx/reconf_bed/score_reconf.py /tmp/reconf_$S
    done
    python3 -I tests/passive_rx/reconf_bed/test_score_reconf.py      # scorer unit test, no hardware
Build first per CLAUDE.md (DGX); idle host. Label results `[MEASURED, DGX rfsim 4 RX, n=1, <commit>]`; repeat n>=3 before any claim.

## Scenarios
| id | change at gNB B | validates | required epoch |
|----|-----------------|-----------|----------------|
| S0 | none (restart only) | continuity path, false-reset avoidance, decode resumes | none of CELL_IDENTITY_CHANGE / SIB1_CHANGE / MIB_CHANGE / HARD_RESET; a SOFT or HARD_REVERIFY CONTINUITY_LOSS/RX_RECOVERY is expected (WARN if absent) |
| S1 | PCI 0->1 | HARD_RESET/CELL_IDENTITY_CHANGE, `PDSCH_SOLVER reset on new cell identity`, SIB1 re-decode, DCI length lock, `ISAC_METRICS pci==1`, CRC-ok | HARD_RESET + CELL_IDENTITY_CHANGE within 30 s of gNB B ready |
| S2 | TDD pattern, same PCI | SIB1 semantic-hash change path, no false identity reset, re-lock | HARD_* SIB1_CHANGE (or MIB_CHANGE), no CELL_IDENTITY_CHANGE |
| S2c | MIB searchSpaceZero, same PCI | MIB-hash path (pdcch-ConfigSIB1 changes SS0 occasions; CORESET0 index not changed, table value not verified by me) | HARD_* MIB_CHANGE or SIB1_CHANGE |
| S3 | `-m 16 -l 2` | scheduler-only change: no cell epoch, solver re-solves rank/MCS; DEDICATED_CHANGE_SUSPECTED optional (INFO) | no HARD_RESET / identity; >=80 % of post-recovery CRC-ok grants show Nl=2 and mcs=16 |

## Criteria and thresholds (provisional)
`--lenient` turns FAIL into WARN (use for S2/S2c while the SIB1/MIB detection of a LOST-and-reacquired cell is uncharacterised).
- baseline_decode >= 200 CRC-ok (probe=0) grants in 30 s before change (the cell is decodable).
- no_false_hard_before_change = 0 HARD epochs after 25 s settle; false_soft_rate <= 5/min (WARN; audit OTA max 28/min, CSIRS_MAP_CHANGE).
- client_reconnected: reconnect line within gNB-B delay + 30 s; receiver_no_crash.
- no_stale_credit_in_gap = 0 CRC-ok grants from 1 s after old server exit until gNB B ready (no samples exist, so any credit is stale work).
- recovery_crc_ok: first CRC-ok after the expected epoch (or gNB B ready) <= 15 s (S0), 45 s (S1/S2/S2c), 30 s (S3). Basis: audit RX-discontinuity
  recovery p95 1.1 s / max 1.3 s with state kept [MEASURED OTA 2026-10-30]; a new cell needs SSB+MIB+SIB1+CORESET verify+solve, budget 3x. INFERENCE.
- recovery_sustained >= 50 CRC-ok in the 20 s after first; tail_crc_fraction >= 90 % over the last 30 s (>= 50 grants).
- S1/S2: dci_length_lock within 45 s after the epoch. epoch_drop_counters are INFO (drops are the stale-work rejection).
