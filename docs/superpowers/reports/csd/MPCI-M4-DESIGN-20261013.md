# MPCI-M4 2026-10-13 (DESIGN ONLY): multi-radio / multi-window

Status: [PLANNED]. Nothing here is implemented or run. Builds on M1 (shm ring per window), M2 (per-cell instance
keying), M3 (cross-cell GPU/CPU arbiter). Site numbers are from FRONTLIVE/GRIDCARRIER (DGX, OTA captures) and only
used to size the problem [ANALYTICAL].

## Problem
n78 at the site spans 3560-3772 MHz (508/4 at 3590, 403 at 3625, 675 at 3426, 415 at 3680, 923 at 3732): more than one
122.88 MS/s window (usable ±55 MHz after the DDC transition, M1). Cells outside the current window are either
(1) served by another window of the same radio (X410: 4 RX channels, each with its own LO/DDC in the FPGA),
(2) served by another radio, or (3) visited by retuning (time-sharing one window).

## Objects (maps onto PROJECT_MEMORY §18)
- **Window** = one shm ring (M1): `{ring name, radio id, channel subset, centre, rate, rf_epoch}`; produced by exactly one
  publisher process. Ring names are per window (`isac_iq_w<k>`), so M1 needs no change for several windows.
- **Cell** = one receiver process bound to one window (M2 tag, `ISAC_SHM_IQ_NAME`). A cell can move between windows
  only by restart in this design (M1 consumers already survive a retune of their own window via `rf_epoch`).
- **Planner** (new, python, orchestrator side): input `cells.json` (fastscan) + radio inventory; output a window plan
  and the per-cell launch specs for `mpci_launch.sh`.

## Window planning (static first)
1. Interval cover: each cell is an interval [C − BW/2, C + BW/2]. Greedy minimum cover with windows of width 110 MHz
   (sorted by left edge; open a window at the leftmost uncovered edge) — optimal for interval cover.
2. Assign windows to (radio, channel subset): X410 channels can be tuned independently (UHD `set_rx_freq` per channel;
   `usrp_set_rx_freq_all` today retunes all channels together — needs a per-channel path, and the ring publisher must
   then publish per-channel-subset rings: one publisher per subset, or one publisher process writing K rings).
   Trade-off per profile (§20): `4×windowA` (4-RX MRC/AoA for every cell) vs `2×A + 2×B` (2 RX per cell, two windows)
   vs `1×{A,B,C,D}` (1 RX per cell). The plan states it; the receiver rank/MRC (ISAC_RX_*) follows the channel count.
3. If windows > available channel subsets → time-sharing (below) for the lowest-value windows.

## Time-shared windows (retune policy)
- Publisher retunes its radio between window centres on a schedule; every retune bumps `rf_epoch`; samples between
  the retune command and LO settling are not published (`valid_from` after settle; settle time measured on the X410,
  [NOT RUN]; order 1 ms on UHD class radios).
- Consumers of the window that left are paused, not killed: their reads block (no new samples on their ring) — the
  heartbeat timeout must then be per-window "dwell aware" (the publisher writes `dwell_until` into the header; the
  consumer extends its timeout). On return: the consumer sees a forward timestamp jump = the receiver's existing gap
  path (G11 reacquisition: cheap SSB/PBCH revalidation, keep CellContext, PROJECT_MEMORY §19/§20).
- Dwell scheduling by value and staleness: dwell ≥ 2 SSB periods + the cell's slot pattern for grant capture
  (≥ 40 ms typical); revisit order = weighted round robin with weight = (traffic value) × (evidence staleness), and a
  cap on the fraction of time any window is absent. Cells needing continuous tracking (sensing targets) get a
  dedicated window; time-sharing only for discovery/maintenance.
- Tracking state across retunes: timing drift is ppm-level (X410 −4.8 kHz common offset at n78) → after a 100 ms
  absence the timing error is a few samples; CFO is stable; the receiver's rebase guards must accept a gap without
  global resync. That is the G11 reacquisition path and must be measured per cell (outage in sample time).

## Multiple radios
- Each radio = its own publisher process + ring(s); clock domains differ: ring timestamps are per radio. For cross-cell
  sensing fusion the rings need a common time base: X410 PPS/10 MHz shared reference + `set_time_next_pps` so
  `base_tick` is comparable; record the radio id and time source in the ring header (header version 2).
- NIC/DPDK: one claim per radio; DPDK lcores per publisher (core map per radio); the publisher is the only process
  with UHD/DPDK state, so the M2 `run_arm.sh` hazards (pkill, rm -rf dpdk) only concern publishers.

## Compute budget (M3 extension)
- The arbiter stays per host: GPU budget is global across windows and radios; cells of a time-shared window that is
  away consume nothing, so their share is borrowed (M3's work-conserving rule).
- DDC cost scales with cells × channels × input rate (M1: ~0.17 CPU-s/s per channel for ≤ 133-PRB cells on x86):
  4 cells × 4 ch on 122.88 MS/s rings is ~3 cores of DDC alone → move the DDC to the publisher side as a shared
  polyphase channelizer (one FFT filter bank per window, per-cell outputs written to per-cell rings) or to the FPGA
  (RFNoC DDC per channel), once more than ~3 cells share a window.

## Validation plan
1. Offline: planner unit tests (interval cover, profile assignment); a synthetic two-window raw_iq replay
   (two recordings at different centres played by two publishers) with consumers on each ring.
2. rfsim: two gNBs at different carriers are not mixable in one rfsim stream → use two publishers (one per gNB) =
   the two-window case directly; time-sharing tested by a publisher that alternates between two raw_iq files.
3. OTA (DGX): window A (508/4/403) and window B (415) on 2+2 channels of the X410, then the time-shared variant;
   per-cell metrics vs single-cell baselines, outage per return in sample time, RFSTALL/overruns.

## Changes needed (for the implementation task)
`usrp_lib.cpp` per-channel retune; publisher writing K rings (channel subsets) or one publisher per subset; ring header
v2 (radio id, time source, dwell_until); consumer dwell-aware timeout; planner script; launcher multi-ring support.
