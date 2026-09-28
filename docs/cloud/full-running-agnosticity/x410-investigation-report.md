# X410 / sens6 health investigation (2026-09-25, 20:07-20:35 UTC)

Scope: find X410 or host-side problems that would stall or silently invalidate the upcoming OTA
validation runs. **No gNB was on air, so every streaming result below is VOID for decode. It covers the RF/host path only.**
Times are UTC (sens6 runs in UTC; local CEST = UTC+2). No hardware, persistent network or MPM
configuration was changed. `usrp-hwd` was not restarted, because it was not wedged.
Raw test logs are on sens6 in `/tmp/x410inv/` (the .dat captures were deleted after analysis).

## Bottom line

- **The transport is healthy right now.** A 300 s run at 4 ch x 122.88 MS/s lost nothing: 0 drops,
  0 overruns, 0 sequence errors, NIC `rx_missed/out_of_buffer/discards` delta 0, and the mgmt RTT
  never went above 0.315 ms. The X410 is settled (booted 18:37:52, about 1.5 h before the tests),
  unclaimed, and its RF chains are healthy (all 4 noise floors are within 1 dB of each other on
  TERMINATION).
- **Three things will break or silently void OTA runs unless they are fixed first:**
  1. The data NIC is running at its thermal limit: 99-102 °C during the tests, crit 105 °C, 108 °C peak this boot, and today's warning is the first one in the journal since July.
  2. dnsmasq is disabled at boot, so the X410's mgmt IP depends on someone re-running a manual script after every sens6 boot.
  3. The launcher at `/home/sens/NICOLA/captures/run_arm.sh`, which today's journal shows was used for the salt_* runs, is stale. It pins the IRQs of the wrong NIC port. The in-tree copy is fixed.

## Findings

| # | Sev | Symptom | Evidence | Likely cause | Impact on OTA runs | Recommended fix | Needs user? |
|---|-----|---------|----------|--------------|--------------------|-----------------|-------------|
| 1 | **HIGH** | The X410 data NIC (ConnectX-5, 02:00.0/.1) is at its thermal limit | `hwmon4 temp1_input` 99000-102000 while idle and while streaming (`long.temp`: 98-100 °C during the 300 s 4-ch run). `temp1_crit 105000`, `temp1_highest 108000`. Kernel: `15:49:26 mlx5_core 0000:02:00.1: temp_warn ... High temperature on sensors with bit set 0x0 0x1`, cleared 19:51:49. `journalctl -k \| grep temp_warn` shows **only today's two events** in a journal going back to 2026-07-25. So this is NEW, and it appeared after today's 15:20-15:30 power-off, when hardware was handled | Insufficient airflow over a passively cooled 100G NIC. Something changed physically today (case, fan or cabling during the 15:20-15:30 downtime or the 18:37 recabling). Not traffic: the GPU is idle (37 °C, 9 W) and the temperature does not rise with 15.7 Gb/s of load | Above crit the mlx5 firmware throttles and eventually shuts down the port. That produces a mid-run stream loss that looks exactly like RFSTALL / an X410 stall. It did NOT happen in 300 s tonight, but the margin is about 3 °C | Restore or direct airflow over the NIC (case fan, slot spacing from the GPU) and check `temp1_input` settles well below 90 °C. Until then, have the launcher log `/sys/class/hwmon/hwmon4/temp1_input` (and `temp1_highest`) per run, and VOID any run that crosses 105 °C | **Yes** (physical) |
| 2 | **HIGH** | The X410 mgmt IP (192.168.1.140) only exists while dnsmasq is running, and dnsmasq does not start at boot | `systemctl is-enabled dnsmasq` gives `disabled`. The lease `1790404850 00:80:2f:36:f3:fc 192.168.1.140 ni-x4xx-327C1F2` expires **2026-09-26 06:40:50 UTC** (12 h lease). dnsmasq was started by hand at 18:40:56 via `/usr/local/sbin/enp128s31f6-mode server` (the netplan override was rewritten at 18:40). After today's 15:30 boot nothing served DHCP until then | Server mode is a manual toggle, and only the static-IP half (the netplan file) persists | After any sens6 reboot, the X410's mgmt IP disappears once the lease expires (up to 12 h later, possibly mid-run). The mgmt RPC then times out, which is the documented claim-loss / stall mode. Scripts that hardcode `.140` (`~/NICOLA/ota_loop.sh`, `MGMTA` default) fail outright. The in-tree `run_arm.sh` autodetects `mgmt_addr` and falls back to 192.168.20.2 (RPC over the data link), so it degrades more gracefully | `sudo systemctl enable dnsmasq` (and keep the netplan override), or give the X410 a static mgmt IP. Add "dnsmasq active + lease valid" to the launcher preflight | **Yes** (persistent config) |
| 3 | **HIGH** | The stale launcher `/home/sens/NICOLA/captures/run_arm.sh` pins the wrong NIC IRQs | That copy hardcodes `for i in 152 155 ... 167`. The live IRQs in this boot: `enp2s0f1np1` (in use) = `155 156 170-182`, `enp2s0f0np0` (no cable) = `153 154 157-169`. So 11 of its 14 pins land on the idle port. The live comp IRQs stay one per core on 0-13 (`156 aff=0`, `170 aff=1` ... `182 aff=13`), on top of the softmodem's CPUs 0-7. Today's journal (`tee /proc/irq/164..167/smp_affinity_list`, `PWD=/home/sens/NICOLA/captures`, `ip addr add ... dev enp2s0f1np1`) shows this copy launched today's runs. `nic_miss` in those runs: salt_full4ant 41692, salt_full4ant2 52473 (burst at stream start, then stream dead), salt_full4ant5 22838, agnostic_lab_115315 22314. It also defaults to `MGMT=128.178.122.174` and a non-existent `NIC=enp129s0f0np0`. With that default NIC, the MTU and ring assertions and the `nic_miss` counter silently no-op (nic_miss reads 0). It waits only 40 s after an MPM restart (memory says 180 s) | A pre-2026-09-15 copy. The fix (`78590c7200`: IRQs from sysfs, autodetected MGMT, NIC `enp2s0f1np1`, 180 s settle) only exists in the in-tree copies (`agn-wt/*/tests/passive_rx/captures/run_arm.sh`, `multirx-clean-adaptive/...`) | This reproduces the documented stale-IRQ fault (NAPI starved, NIC drops, RFSTALL at stream start). Today's 4-antenna VOIDs with 22-52 k drops are consistent with it | Launch OTA runs only through the in-tree `run_arm.sh` (via `run_sensing.sh` / `ota_loop.sh`). Retire `~/NICOLA/captures/run_arm.sh`, or replace it with a wrapper that execs the in-tree copy | **Yes** (decide which launcher is canonical. I did not edit it) |
| 4 | MED | Receiver stop and retry paths use SIGKILL and short gaps | In-tree `run_arm.sh`: `exec timeout -k 20 $DUR` (SIGTERM, then **SIGKILL** after 20 s). The CFO and stall watchdogs use `sudo pkill -9 -x nr-uesoftmodem` (lines 255, 258). Between tries there is only about 2 s plus a probe, not the 60 s user rule. Today's aftermath: `salt_1ant_145919` got `RFSTALL USRP_RX_START ... ERROR_CODE_OVERFLOW (Out of sequence error)`, and `salt_1ant_c_151134` got `RPC call to 'claim' ... Someone tried to claim this device again (From: 192.168.1.1)`. The journal shows `kill -9 11827 ...` at 15:19:21 | Memory records: a SIGKILL wedges the X410 / leaves a stale claim; a stalled receiver ignores SIGTERM but exits on SIGINT; after a kill, wait 60 s and check `claimed: False` | The try after any watchdog kill is likely VOID (out-of-sequence overflow or a claim conflict), which burns tries and can wedge MPM | Use `timeout -s INT -k 60`, make the watchdogs send INT then TERM then KILL, and add `sleep 60` + a `uhd_find_devices ... claimed: False` check before each retry (`ota_loop.sh` already does the 60 s) | Yes (launcher change on the OTA path) |
| 5 | MED | UDP socket buffers are below what UHD asks for | UHD on every run: `recv buffer could not be resized ... Target 250000000, Actual 50000000`, and send `Actual 1048576`. `rmem_max=50000000` comes only from `/usr/lib/sysctl.d/50-uhd-usrp2.conf`. Nothing in `/etc/sysctl.d` sets rmem or wmem (memory said "250 MB tuned", which was not persistent) | The earlier tuning was set by hand and lost across reboots | Not binding at idle (the 300 s 4-ch run had 0 loss). It cuts the host-side burst tolerance to about 25 ms at 4 x 122.88 MS/s, and that tolerance is what absorbs CPU-contention hiccups | Add `net.core.rmem_max=250000000` and `net.core.wmem_max=250000000` to `/etc/sysctl.d/90-x410-net.conf` | **Yes** (persistent config) |
| 6 | MED | Several unexplained outages today, and neither box can explain them afterwards | sens6 boots: `-3` 14:14:00 to 14:14:05 (journal stops 5 s into boot), `-2` stops abruptly at 14:37:02 (no shutdown sequence, last entry an ssh login, no vmcore). **The X410 dropped both links at 14:31:03** (`mlx5 ... enp2s0f1np1: Link down` + `e1000e enp128s31f6: NIC Link is Down` in the same second) during capture `salt_full4ant2`. That stream had already died at 14:27:01 (52 k NIC drops, then `rx_packets` flat; `rpc::timeout ... 'reclaim'` afterwards). The X410 was down 6 min before sens6 died. There was another X410 drop-and-reboot at 15:08:14 to 15:08:54 (40 s, the mgmt flap pattern of an X410 boot) and a drop at 15:19:13, followed by a user `poweroff` of sens6 at 15:20:21. From 15:30 (sens6 boot) the X410 was absent until 18:37-18:38, when the kernel shows QSFP `Cable unplugged/plugged` on both ports and mgmt negotiating `1000 Mbps Half Duplex` once (someone physically recabled). The X410 journal is volatile (`/var/log/journal` missing, `Storage=auto`), so it only has the current boot (since 18:37:52) | Undetermined. 14:31 (X410) followed by 14:37 (sens6) fits a shared power event or a manual power-cycle as well as a crash. The user said one reboot was theirs. The X410 side is unrecoverable | If the X410 drops out spontaneously during OTA, no evidence survives | Enable a persistent journal on the X410 (`mkdir -p /var/log/journal` with a small `SystemMaxUse`: its root fs is 77 % of 3.4 G) and kdump/pstore on sens6. Log X410 uptime at the start and end of each run, so a mid-run reboot is detected rather than read as a stall | **Yes** (persistent config on both hosts) |
| 7 | LOW | Corrected-bit errors on one lane of the QSFP28 DAC link | `rx_corrected_bits_phy` 1802 to 1893 over about 40 min, almost all `rx_err_lane_2_phy` (lanes 1/3: 1/14). About 7 bits per 30 s at idle, RS-FEC active, `rx_pcs_symbol_err_phy` / `rx_symbol_err_phy` = 0. Appeared after the 18:38 recabling | A marginal DAC seat or cable on lane 2, possibly made worse by the NIC temperature | None today (pre-FEC BER about 1e-11, far below the RS-FEC limit). Becomes relevant if uncorrectables appear | Reseat or replace the 2 m DAC when convenient, and watch `rx_pcs_symbol_err_phy` per run | Optional (physical) |
| 8 | LOW (known) | ch1/ch3 RX1 inputs receive about 8.7 dB less external power than ch0/ch2 | RMS at 30.72 MS/s, 1 s. TERMINATION @3450 MHz g40: ch0 -67.16, ch1 -66.55, ch2 -67.50, ch3 -66.68 dBFS (all within 1 dB, so the receive chains are healthy). RX1 @3450 g40: -63.76 / -67.04 / -64.74 / -66.93. RX1 @3540 g43: -61.72 / -65.43 / -61.11 / -65.29. Excess over the floor at 3540 is ch0 4.8e-7 vs ch1 6.5e-8 (−8.7 dB), and ch2 6.0e-7 vs ch3 8.1e-8 (−8.7 dB). No overflows, no stalls, DC below −94 dBFS, block power steady | Antenna, cable or connector side, not the X410 chains. Matches memory ("ch1/ch3 down 8-15 dB, PHYSICAL") | Four-antenna MRC and AoA are weighted toward 2 useful branches | Check the ch1/ch3 antennas and cables (VOID for decode: no lab gNB; 3540 energy is ambient) | Optional (physical) |
| 9 | INFO | Things that are healthy, or where earlier concerns are cleared | `uhd_find_devices`: `claimed: False`, `fpga: UC_200`. Probe: MPM 6.1, FPGA 11.0, UHD 4.10.0 on both sides, `clock/time_source=internal`, all 4 `lo_locked`. X410 temps: RFSoC 56 °C, PSU PCB 62 °C, PMBUS 80-81 °C, DBs 50-51 °C, fans present. usrp-hwd log clean (only the normal `metal: error` / `XRFdc_SetThresholdClrMode` boot noise). Mgmt ping 0.18 ms, data ping 1.1 ms, jumbo `ping -M do -s 8972` OK. Persistent tuning **survived the reboot**: MTU 9000 (NM profile), ring 8192/8192, governor `performance` on all 14 cores, backlog 5000. PCIe Gen3 x16, AER counters all zero. No rcu stall, OOM, MCE or hung task in any of today's 8 boots | n/a | n/a | n/a | No |

## RF/host path tests (VOID for decode, RF/host path only)

All tests ran with `benchmark_rate --rx_cpu sc16` (OAI's host format) against the `RX1`/default antenna, from
`/usr/local/lib/uhd/examples`, as the unprivileged user (so `--priority high` could not take effect:
"Unable to set the thread priority"). The governor was `performance`. Compile activity was checked before each
test.

| Test | Samples expected / received | Drops | Overruns | Seq err | NIC missed/oob/discard Δ | NIC temp | CPU load during |
|------|-----------------------------|-------|----------|---------|--------------------------|----------|-----------------|
| 122.88 MS/s x1, 30 s | 3.686e9 / 3686282114 | 0 | 0 | 0 | 0/0/0 | 100-101 °C | idle |
| 122.88 MS/s x4, 60 s | 29.491e9 / 29490720412 | 0 | 0 | 0 | 0/0/0 | 98-100 °C | an agent's `make -j4` started at 20:17:54, part-way through. Still 0 loss |
| 30.72 MS/s x4, 30 s | 3.686e9 / 3686319824 | 0 | 0 | 0 | 0/0/0 | 95-96 °C | under that `make -j4` |
| **122.88 MS/s x4, 300 s** + 5 Hz mgmt ping | 147.456e9 / 147452632284 | 0 | 0 | 0 | 0/0/0 | 98-100 °C | idle, no compile throughout (30/30 samples `cc=0`); mgmt RTT max 0.315 ms, 0 lost |
| 4-ch capture, TERMINATION / RX1@3450 / RX1@3540 | see finding 8 | none in the logs | | | | | idle |

Rates: 122.88 MS/s is the 273-PRB rate (208 of 209 X410 captures in `~/NICOLA/captures` logged
`sample_rate 122880000`). The X410's master clock is 245.76 MS/s, so the 51-PRB rate is 30.72 MS/s
(/8). 23.04 MS/s is not reachable at this MCR.

Caveat: `benchmark_rate` is a pure sink. These results clear the NIC, link, MPM claim and FPGA path at full
4-channel rate. They do not show that `nr-uesoftmodem` itself drains fast enough, which depends on
thread and IRQ placement (finding 3).

## What I did NOT do / deferred

- **No `nr-uesoftmodem` streaming test.** Without a cell it adds nothing over `benchmark_rate` for the
  RF path. With the stale launcher it would reproduce finding 3 rather than measure the device.
- **No test of how the NIC behaves above 105 °C.** I could not force it and would not try. The thermal risk
  is inferred from `temp1_highest=108000` and the kernel warning, not observed as packet loss.
- **Why the 14:31 X410 dropout and the 14:37 sens6 stop happened: not determinable.** There is no X410 journal
  for that boot and no vmcore on sens6.
- No persistent config changes (dnsmasq enable, sysctl, X410 journald) and no launcher edits. These are
  listed as fixes for the user.
- A long run (≥20 min) to catch intermittent mgmt-RTT spikes or claim loss was not done. The 300 s run was clean,
  and the mgmt link is now a direct sens6 to X410 segment rather than the campus LAN from the 2026-08-23 incident.

## Pre-run checklist derived from this (for the OTA operator)

1. `systemctl is-active dnsmasq` is active and the X410 lease is valid (`/var/lib/misc/dnsmasq.leases`).
2. `cat /sys/class/hwmon/hwmon4/temp1_input` is well below 105000. Record `temp1_highest` before and after the run.
3. Launch only via the in-tree `run_arm.sh`. Confirm the preflight prints IRQs from sysfs, not `(n set)` from the old list.
4. `uhd_find_devices --args addr=192.168.20.2` shows `claimed: False`. Wait 60 s after any killed run, and 180 s after any MPM restart.
5. Record the X410 `uptime` at the start and end of each run, so a mid-run X410 reboot is caught (finding 6).
