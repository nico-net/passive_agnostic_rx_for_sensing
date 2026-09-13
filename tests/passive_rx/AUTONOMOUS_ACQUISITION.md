# Autonomous passive acquisition

`auto_acquire.py` scans the repository's standard FR1 synchronization raster.
Frequency limits, maximum RF bandwidth, CFO search extent, gain, and device
addresses describe the receiver/search domain. They are not discovered cell facts.
No gNB files, scheduler hints, saved CFO/STO values, or external timing are used.

## Acquisition contract

1. Try standard band/SCS hypotheses in bounded RF windows with `--ue-scan-carrier`.
2. Search coarse CFO hypotheses on independent IQ copies, refine with PSS/SSS,
   and require PBCH CRC. Derive frame timing from the detected SSB.
3. Decode MIB/SIB1. Probe mode publishes facts without applying trial-center
   geometry to the PHY. An SSB without SIB1 is not full carrier acquisition.
4. Derive Point A, actual grid center, PRBs, bandwidth, and numerology from those
   broadcasts. Recenter an edge-of-window SSB once if SIB1 was not recovered.
5. Cleanly stop the probe, reopen at the discovered geometry, measure CFO/STO
   afresh, and revalidate the broadcasts before publishing `ACQUIRED`.
6. Keep the receiver and dashboard running until interrupted (or the optional
   duration expires). Target detection remains disabled with `ISAC_SYNC_ONLY=1`.

FR1 geometry follows [TS 38.211 sections 4.4.4.2 and 7.4.3.1](https://www.etsi.org/deliver/etsi_ts/138200_138299/138211/18.02.00_60/ts_138211v180200p.pdf):

```text
SCS = 15000 * 2**mu
SSB_low = SS_ref - 120 * SCS
Point_A = SSB_low - (12 * offsetToPointA + kSSB) * 15000
carrier_center = Point_A + (offsetToCarrier + carrierBandwidth / 2) * 12 * SCS
```

CFO is a frequency offset in Hz. STO is the observed frame-boundary displacement
in samples, not propagation delay. Sampling-clock drift is separate: its initial
seed is zero and the existing timing loop learns it online. CFO/frequency is
**not** used as a sampling-drift estimate. No synchronization estimator is disabled
by the target-detection switch.

## Invocation on sens6

Rebuild `nr-uesoftmodem` with the native acquisition changes before running.
The launcher requires four receive branches and preserves the existing sens6 CPU
placement; it does not reconfigure the NIC, radio firmware, gNB, or active UE.
Stop the existing receiver and port-8081 monitor first, or select a free monitor
port. Existing radio owners are refused rather than killed implicitly.

Example search interval (a search domain, not an assertion of band/carrier):

```bash
sudo python3 tests/passive_rx/auto_acquire.py \
  --start-hz 3300000000 --stop-hz 3800000000 \
  --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174 \
  --out /home/sens/NICOLA/captures/autonomous-$(date +%Y%m%d-%H%M%S)
```

Windows VPN dashboard access:

```powershell
ssh -N -L 8081:127.0.0.1:8081 sens@128.178.122.140
```

Open `http://localhost:8081/`. The dashboard has no CFR/decoder samples during
probing. `acquisition.json` and terminal events distinguish scanning, probing,
reacquisition, acquired, unsupported, inconclusive, stopped, and VOID states.
`run.log` remains the dashboard's live log for the entire session.

`candidate.json` is probe-derived geometry. `acquired.json` contains independently
revalidated SSB/MIB/SIB1 facts and fresh CFO/STO from the capture process.
Per-attempt logs, hypotheses, commands, and NIC loss counters are retained.

## Scope and failure handling

- Supported capture: terrestrial FR1, same 15/30 kHz SSB/DL/UL numerology,
  co-channel TDD, equal DL/UL grid widths, zero offsetToCarrier, integer-aligned SSB.
- FDD, FR2/NTN, mixed numerologies, nonzero carrier offsets, and asymmetric grids
  are explicitly refused. The existing PHY contracts must be extended first.
- A finite probe deadline without SIB1 is INCONCLUSIVE, not proof of no cell.
- RFSTALL, discontinuities, NIC missed samples, UHD startup errors, unexpected
  receiver exit, and forced SIGKILL are VOID. No CRC metrics from them are valid.
- No automated X410 reboot or retry after forced termination. This launcher does
  not fix the separately observed X410 first-buffer/management-portal failure.
- Automatic acquisition is not proof of >=60% DL/UL CRC. CRC performance must be
  measured separately over valid RF intervals, including dropped-work accounting.
- Enabling `csi_rs` does not discover its dedicated resource map. CSI-RS discovery,
  dedicated configuration coverage, and unsupported UL/HARQ modes remain separate.
- New code has not been built or OTA-tested as part of this implementation task.
