# STARTUP-DROPS: persistent DPDK drop regime at receiver start (2026-10-09)

Branch `csd/startup-drops` (from `csd/otatest` 60f38885ea, with `csd/rxr` 7296a4d9e3 RXR-NOCTRL merged).
Host: DGX GB10 spark-74c3 (aarch64) with X410 34B2AF1 over DPDK, 4 x 122.88 MS/s, gain 38, 4 RX, PCI 415.
Evidence labels follow PROJECT_MEMORY §0.1. Every OTA claim below is for this host and this cell only.

## Verdict

**Root cause [INFERRED, strong]: missing CPU isolation for the RX-critical threads on the 2026-10-08 boot, not memory traffic from the cell search.**

- Every failing start of 2026-10-08 ran on boot `ac6a4f3b` (10-08 12:39 to 10-09 00:38). That boot's command line had `isolcpus=17-19 nohz_full=17-19 rcu_nocbs=17-19 irqaffinity=0-16` [OFFLINE, journalctl --list-boots + kernel command line per boot].
- The ring producer (`ISAC_USRP_RING_CORE=16`) and `uhd_ctrl` (core 14 or 15) therefore ran on cores that took IRQs and that the kernel load-balanced. Up to the 10-09 10:00 edit of `t26d.env`, one passive PDSCH consumer was also pinned to core 15 (`ISAC_PDSCH_CORES=8,9,15,2,3`).
- OAI's actors run at SCHED_FIFO 97 with no pinning (`DL__actor` x4, `UL__actor` x2, `SYNC__actor`, `ru_start_thread`, `TASK_RRC_NRUE` at 50). Today they roam every housekeeping core [OTA LIVE, per-thread placement sampled every 1 s]. On the old boot core 16 was a housekeeping core, so a FIFO 97 actor could land on it and preempt the producer (FIFO 95) for as long as it ran. Cell search and the start of the dedicated configuration are exactly when those actors are busiest.
- Old boot: 100 of 134 4-RX radio runs had sequence-error drops ("D"), and 21 of the 25 evening t26c2/t26d runs [OFFLINE, `tf_table.txt`].
- Current boot (`isolcpus=15-19 nohz_full=15-19 rcu_nocbs=15-19 irqaffinity=0-14`): 8 of 8 starts had zero drops [OTA LIVE]. These used the same `build_t26` receiver binary, the t26d env, the t26d cfg (`"2:32:6"`), `TPOOL=0,1` and gain 38. The other measures were also clean: NIC `rx_out_of_buffer`=0 and `rx_discards_phy`=0, no `USRP_RING_RECOVER`, no RFSTALL, and sync at +3.37 s every time.
- Chance check: if the old boot's 75 % drop rate still applied, 0 of 8 would happen with probability 0.25^8 ≈ 1.5e-5.
- Confounders (why this is INFERRED, not VERIFIED):
  - Today's host was cool (max zone about 58 °C) and idle (agents frozen, no mlocked replays).
  - The PDSCH consumer moved off core 15 at 10:00.
  - The device library carried the NOCTRL merge. No fault happened, so its recovery path never ran.
  - I did not re-create the old placement on purpose. That is the decisive A/B, listed under "Orchestrator validation" below.

**Ruled out or demoted:**

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Memory-bandwidth burst from the cell search | Demoted | [OTA LIVE] vmstat shows no allocation after the stream starts: about 1.15 GB (the ring prefault) is faulted about 2 s BEFORE the first packet, and free memory stays flat while streaming. Cell search ran 2.3 s with no drop. [CODE-READ] The PSS search is a cache-resident time-domain correlation. |
| DPDK lcore overloaded by per-packet futex wakes | Disproved | [OTA LIVE] perf on CPU18 (`dpdk-io_18`): 0 context switches/s and 0–1 % kernel cycles. |
| NIC or PCIe buffer drops | Not seen on the good boot | [OTA LIVE] `rx_out_of_buffer`, `rx_discards_phy` and `rx_prio*_buf_discard` all stayed at 0 over ~11 M packets per run. No NIC data exists for the old boot. |
| UHD io-service queue full | Disproved | [OFFLINE] "Dropping packet: No space in recv queue" never appears in any of the 663 run logs. Drops sit below UHD's queue. |

**Separate finding [OTA LIVE + CODE-READ]:**

- `uhd_ctrl_ep0001` causes an interrupt and context-switch storm. UHD's `chdr_ctrl_endpoint::recv_worker` loops `get_recv_buff(0)` and then `sleep_for(1 ns)` (`host/lib/rfnoc/chdr_ctrl_endpoint.cpp:158-166`).
- On core 15 that comes to 1.6–2.05 M context switches/s with 96 % kernel cycles (`uhd_ctrl` at 62 % CPU). Host-wide it is about 1.05–1.09 M cs/s and 540–560 k interrupts/s for the whole stream, in all 8 runs.
- On an isolated core this is harmless, as today shows. On a shared core it starves kernel threads, which fits the earlier "RCU stall on core 14" lesson. It must stay on an isolated core. The real fix is in UHD (back-off or a blocking wait), which is out of scope.

**Unexplained [INFERRED]:**

- With the same `build_t26` binary, `TPOOL=0,1` and t26c2/t26d, the 10-08 evening cell search took about 19 s (trashed_frames 1808–1960, including clean runs td_3_1_194239, td_4_1_194536 and te_2_1_222018).
- Today it took 2.3 s (trashed_frames=236, 8 of 8).
- Something slowed the search cores (0, 1) about 8x on the old host state. That fits contention or throttling on a non-isolated, IRQ-carrying layout, but it was not measured.

## Radio window [OTA LIVE, 2026-10-09 11:50–12:00, 566 s, one heavy.lock hold, agents frozen once]

8 runs through `starttest.sh` -> `ota.sh`: 60 s each, SIGINT only, X410 `claimed: False` afterwards, no receiver left alive.

- Common settings for all runs:
  - `BDIR=run_sd`: build_t26 with `liboai_usrpdevif.so` from this branch, so both arms have NOCTRL.
  - `CFG=` t26d cfg with "2:32:6", `TPOOL=0,1`, `$(cat t26d.env)`.
  - `ISAC_RX_RECOVER=1 ISAC_RX_RECOVER_NOCTRL=1 ISAC_RX_RECOVER_BACKOFF_MS=300 ISAC_RX_RECOVER_LEAD_MS=80 ISAC_RX_RECOVER_MAX=20 ISAC_RX_RECOVER_MAX_FAULTS=20 ISAC_RX_RECOVER_WINDOW_S=60 ISAC_USRP_RING_ZEROCOPY=1`.
- Arms, interleaved A1 B1 … A4 B4:
  - A = blocking producer.
  - B = `ISAC_USRP_RING_POLL=1`.
- Read-only monitors (housekeeping cores):
  - `ethtool -S` once per second;
  - `vmstat 1`;
  - `perf stat -A -C 0,1,5,15-19` (cycles:u/k, context-switches, l3d_cache_refill, stall_backend_mem, bus_access);
  - per-thread /proc stat;
  - timestamped log.

| run | poll | drops (D) | RING_RECOVER | RFSTALL | sync after 1st buffer | CSI-RS confirmed | CPU16 cs/s | CPU16 kernel | usrpRing CPU |
|---|---|---|---|---|---|---|---|---|---|
| A1 | off | 0 | 0 | 0 | +3.37 s | 42 | 28 k | 40 % | 39 % |
| B1 | on | 0 | 0 | 0 | +3.37 s | 23 | 0 | 1 % | 99 % |
| A2 | off | 0 | 0 | 0 | +3.37 s | 75 | 29 k | 40 % | 40 % |
| B2 | on | 0 | 0 | 0 | +3.36 s | 47 | 0 | 1 % | 99 % |
| A3 | off | 0 | 0 | 0 | +3.37 s | 57 | 30 k | 39 % | 39 % |
| B3 | on | 0 | 0 | 0 | +3.37 s | 36 | 0 | 1 % | 100 % |
| A4 | off | 0 | 0 | 0 | +3.37 s | 21 | 31 k | 39 % | 40 % |
| B4 | on | 0 | 0 | 0 | +3.37 s | 66 | 0 | 1 % | 99 % |

Data: `/home/nicola/NICOLA/ota_runs/startup_drops_1009_1150/` (`summary.txt`, per-run `nic.txt`, `vmstat.txt`, `perf.txt`, `threads.txt`, `tslog.txt`, the scripts, and `tf_table.txt`, the history of all 628 radio runs). Run logs: `ota_runs/sd_{A,B}{1..4}_1_*`.

**RXR-NOCTRL on air [OTA LIVE]:**

- No stream fault occurred in 8 starts, so the recovery path was not exercised.
- There are no `USRP_RING timed restart` lines, so no `time_read_ms` sample.
- There is no "Control operation timed out", no `gap_samples`, and no unsynced restart loop.
- Validated only as "does no harm on a clean start (8/8)". Exercising it needs a fault, for example the reproduction run below.

## Code on csd/startup-drops (all flags default off; with them unset, behaviour is unchanged)

All changes are in `radio/USRP/usrp_lib.cpp` and `radio/USRP/usrp_ring_recovery.h`.

1. `ISAC_USRP_RING_POLL=1`: once streaming, the producer calls `recv()` with a zero timeout. UHD then never parks the producer on a condition variable, and an empty poll retries.
   - `ISAC_USRP_RING_POLL_TIMEOUT_MS` (default 100) of continuous silence is still a receive-timeout fault, as before.
   - The reader is still woken once per `chunk` samples, not on every call.
   - Pure classifier: `usrp_ring_poll_classify()`.
   - Effect [OTA LIVE]: CPU16 goes from 28–31 k cs/s and 40 % kernel to 0 cs/s and 1 % kernel. The producer becomes a 100 % spinner on its isolated core. Drop rate is 0/4 with and 0/4 without, so this is not the root-cause fix. It removes the producer's kernel exposure and makes it immune to wake-up latency.
   - Use only on an isolated core.
2. `ISAC_USRP_RING_PRIO=N` (default 95): FIFO priority of the producer. 98 puts it above every OAI FIFO 97 thread, so a roaming actor can no longer preempt it. This is the direct guard against the inferred mechanism. Not yet tested on air.
3. `ISAC_USRP_ISOLATION_CHECK=1`: before streaming, logs `SENSING: USRP_ISOLATION ... core N isolated=0 nohz_full=0 -- startup drop risk` for any ring or `uhd_ctrl` core missing from `/sys/devices/system/cpu/{isolated,nohz_full}`. It only warns and never refuses. Pure parser: `usrp_cpulist_mask()`.

**Tests [OFFLINE VERIFIED, DGX aarch64]:**

- `tests/passive_rx/rxr/test_usrp_ring.cpp` gains `poll_classify_test` and `cpulist_test`. Command: `g++ -std=c++17 -O2 -Wall -I . tests/passive_rx/rxr/test_usrp_ring.cpp -o /tmp/t && /tmp/t`. It passes, and the existing RXR tests still pass.
- `ninja -j6 oai_usrpdevif` builds in `cmake_targets/ran_build/build_sd` (heavy.sh).
- The overlay run dir `cmake_targets/ran_build/run_sd` symlinks build_t26's libraries plus this `liboai_usrpdevif.so`. Point `BDIR` at it; `liboai_device.so` must point at the local `liboai_usrpdevif.so`.
- The radio window ran a library build with only the POLL change, before PRIO and the isolation check were added. The final library (all three flags) built and passed the unit tests but has not been on air.

## Recommendations (orchestrator)

1. **Keep the 15-19 isolation boot** (`isolcpus=15-19 nohz_full=15-19 rcu_nocbs=15-19 irqaffinity=0-14`) and treat it as part of the receiver. After any reboot, check `cat /sys/devices/system/cpu/isolated` = `15-19` before a radio window, or run with `ISAC_USRP_ISOLATION_CHECK=1` and grep `USRP_ISOLATION`.
2. Never pin anything except the RX path (producer, `uhd_ctrl`, DPDK lcores) onto 15-19. Never put `uhd_ctrl` on a non-isolated core: its 1 ns sleep loop makes about 2 M cs/s.
3. Add `ISAC_USRP_RING_PRIO=98` to the OTA env. Optionally add `ISAC_USRP_RING_POLL=1`; it is cheap on isolated core 16.

**Orchestrator validation (exact env, one window, ≤ 6 runs of 60 s):**

```
cd /home/nicola/NICOLA/wt/rr-orchestration/ota
S=<dir holding ota_t26d.cfg = t26d.dgx.cfg with "2:32:6">
NOCTRL="ISAC_RX_RECOVER=1 ISAC_RX_RECOVER_NOCTRL=1 ISAC_RX_RECOVER_BACKOFF_MS=300 ISAC_RX_RECOVER_LEAD_MS=80 ISAC_RX_RECOVER_MAX=20 ISAC_RX_RECOVER_MAX_FAULTS=20 ISAC_RX_RECOVER_WINDOW_S=60 ISAC_USRP_RING_ZEROCOPY=1"
FIX="ISAC_USRP_RING_PRIO=98 ISAC_USRP_RING_POLL=1 ISAC_USRP_ISOLATION_CHECK=1"
B=/home/nicola/NICOLA/wt/rr-csd-j/cmake_targets/ran_build/run_sd   # rebuild build_sd first: ninja -j6 oai_usrpdevif
# (a) fix, normal layout, x3: expect 0 drops, USRP_ISOLATION "0 issue(s)"
BDIR=$B CFG=$S/ota_t26d.cfg TPOOL=0,1 DUR=60 ./starttest.sh sdfix 3 65536 $(cat t26d.env) $NOCTRL $FIX
# (b) DECISIVE reproduction, x2-3: producer on non-isolated core 14 (shared with FIFO 97 actors), no fix.
#     Expect drops if the root cause is right (also exercises NOCTRL recovery: check time_read_ms~0, gap_samples>0).
#     Keep uhd_ctrl on 15 (never move uhd_ctrl off an isolated core). Do not use POLL here (a FIFO spinner on a shared core).
BDIR=$B CFG=$S/ota_t26d.cfg TPOOL=0,1 DUR=60 ./starttest.sh sdrepro 2 65536 $(cat t26d.env) $NOCTRL ISAC_USRP_RING_CORE=14 ISAC_USRP_ISOLATION_CHECK=1
# (c) same as (b) + ISAC_USRP_RING_PRIO=98, x2: expect the drops of (b) to disappear
BDIR=$B CFG=$S/ota_t26d.cfg TPOOL=0,1 DUR=60 ./starttest.sh sdrepro_prio 2 65536 $(cat t26d.env) $NOCTRL ISAC_USRP_RING_CORE=14 ISAC_USRP_RING_PRIO=98
```

If (b) is clean as well, the isolation explanation is weakened. The next candidates are host thermal/power state and memory pressure (frozen mlocked replays), the two other differences from 10-08. Then add `cpufreq` and thermal sampling (`/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`) to the monitors.

## Incident (my fault, fixed)

- My monitor script started its log-tail processes from the window shell. Those processes inherited the fd of `locks/heavy.lock`, and the `tail -F` children were not killed at the end of each run.
- As a result the global heavy lock stayed held from about 12:00 to about 12:35 after the window had finished, and queued agent builds waited.
- I killed only my own leftover `tail`/`sd_mon.sh` processes, and the lock was released.
- The scripts in `ota_runs/startup_drops_1009_1150/` are fixed: `sd_mon.sh` now closes fd 9, `sd_window.sh` starts it with `9>&-`, and the tail is killed by pattern.
- The orchestrator's ota.sh/starttest.sh are unaffected.
