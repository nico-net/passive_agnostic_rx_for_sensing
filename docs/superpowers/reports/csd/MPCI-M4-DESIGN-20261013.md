# MPCI-M4 2026-10-13 (DESIGN ONLY): multi-radio / multi-window

Status: radio side IMPLEMENTED 2026-10-13 (see "Implementation status" at the end); planner/launcher not part of it. Not run on any radio. Builds on M1 (shm ring per window), M2 (per-cell instance
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

## Implementation status (radio side, 2026-10-13)
All default OFF; with ISAC_SHM_IQ_PUB unset nothing is wrapped; with a plain `ISAC_SHM_IQ_PUB=name` the M1 single ring is
written with identical samples (test `plain single ring`). Code: `radio/COMMON/shm_iq_ring.{h,c}`, `shm_iq_pub.c`, `shm_iq.c`;
tests: `radio/COMMON/tests/test_shm_iq.c` (ctest `test_shm_iq`).

| item | status | label |
|---|---|---|
| Header v2 (`SHM_IQ_VERSION 2`): `radio_id` (ISAC_SHM_IQ_RADIO_ID, else cfg `sdr_addrs`, else `pid<N>`), `time_source` (cfg `time_source` internal->free, external/gpsdo->pps; override ISAC_SHM_IQ_TIME_SOURCE=free\|pps), `away`, `dwell_until_ns`. Appended after v1 fields; consumer accepts v1 and v2, refuses v3. SHMIQ_READY gains `radio=` `time_source=`. | implemented | [OFFLINE VERIFIED, cloud x86 4c] |
| Channel-subset rings: `ISAC_SHM_IQ_PUB=ringA:0-1,ringB:2+3` (`-` range, `+` list); ring channel k = radio channel subset[k]; consumers see only their subset. | implemented | [OFFLINE VERIFIED, cloud x86 4c, fake 4-ch device, distinct tone per channel] |
| Per-channel LO: `ISAC_SHM_IQ_PUB_FREQ=ringA=3598e6,ringB=3680e6`. The publisher writes the per-channel frequencies into `cfg->rx_freq[ch]` and calls the device `set_freq`. **`usrp_lib.cpp` is unchanged**: `usrp_set_rx_freq_all` already tunes `cfg->rx_freq[i]` per UHD channel (so no cloud-x86 build impact). A UE-initiated retune (CFO, carrier) adds the same delta to every window. | implemented in the publisher; UHD side unchanged | [CODE-READ] for usrp_lib, [OFFLINE VERIFIED] fake-device LO log; [NOT RUN] on X410 |
| Dwell scheduler: `ISAC_SHM_IQ_DWELL=ringA@3598e6:200,ringB@3680e6:100` (ms; rings must share one channel subset), `ISAC_SHM_IQ_SETTLE_US` (1000). Away ring: `away=1`, `dwell_until_ns` = expected return, nothing written; return = `shm_iq_ring_return` (epoch bump, new run) at the first post-settle write; the settle window (sample time from the first read after the retune) is discarded. Retune happens in the read wrapper between two reads (the device `set_freq` itself is synchronous in UHD, so one read is delayed by the retune). | implemented | [OFFLINE VERIFIED, cloud x86 4c, fake device: alternating LO sequence, away flags, epochs bumped, forward gaps, run start >= settle ticks, ring content only its own tone] |
| Consumer dwell-aware timeout: PRODUCER_LOST is not declared while `away && now < dwell_until_ns + timeout`. | implemented | [OFFLINE VERIFIED, cloud x86 4c: dead producer pid + stale heartbeat: away -> consumer resumes with a forward timestamp jump; control without `away` -> exit 3] |
| X410 behaviour (per-channel set_rx_freq, LO sharing, settle time, overruns) | not testable here | [NOT RUN] |

Review round (same day) [OFFLINE VERIFIED, cloud x86 4c; ctest test_shm_iq 15/15 runs incl. 5 under 6 busy loops, ASAN/UBSAN + TSAN clean on publisher state]:
fixed (1) the publisher no longer rewrites the UE's `cfg->rx_freq[]` (set_freq gets a copy): a device reload reset the UE
delta to a managed LO (next CFO/carrier retune moved every window by f_UE - f_ringA), and a dwell retune racing a UE retune
could corrupt the delta; (2) dwell retune vs UE retune serialised by a mutex (state + device set_freq); (3) settle discard now
also covers the measured blocking time of set_freq (samples captured while the LO moved were published); (4) a device reload
while a window is away now gives that window a timestamp jump of its absence (the return refreshed the heartbeat, so the
first backward write jumped by 1 sample); `away` clears at the first write after the fresh heartbeat; (5) refused instead of
mislabelled: overlapping subsets on different LOs or a non-dwell ring on dwell channels, duplicate ring names, malformed
channel lists, per-ring LO with ISAC_RX_CHAN_MAP, dwell <= 2 x settle; a single DWELL entry is a static window (LO was never
set); a read with fewer channels than a subset index writes zeros instead of reading past `b[]`. Open: (a) the dwell retune
blocks the UE read thread for `retune_us` (X410 full LO tune may be ms) -> host overflow risk [NOT RUN]; (b) samples still in
the host buffer at the retune are counted as post-retune: raise SETTLE_US if the backlog exceeds it; (c) usrp_set_rx_freq_all
re-issues set_rx_freq on unchanged channels at every dwell/CFO retune (possible NCO phase step on the static windows) [CODE-READ];
(d) the publisher UE's own loops (AGC gain, CFO retune) act on whatever window it currently sees and move every window;
(e) a v1 consumer binary refuses v2 rings (`version != 1`): upgrade producer and consumers together.

Uncertainties to resolve on the DGX (UHD docs knowledge, not verified): (a) X410 ZBX has 2 RX/TX ports per daughterboard and
LOs may be shared between channels of one board (the ZBX has independent RX LOs per channel in UHD 4.x docs, but with the
RX1 port or LO-sharing/`lo_source` settings two channels may be forced to one LO; set_rx_freq on the second channel may then
retune the first or return the shared frequency); (b) ISAC_RX_CHAN_MAP permutes OAI antenna -> UHD channel while `rx_freq[i]`
is applied to UHD channel i: use subsets in UHD order or no map; (c) the NCO-only shortcut in `usrp_set_rx_freq_all`
(< 0.25 x rate) gives a DDC-only retune for small steps and a full LO retune for large ones: settle times differ.

### DGX validation steps (publisher UE on the X410, `nr-uesoftmodem ... -C <window A centre>`)
1. Per-channel LO, static: `ISAC_SHM_IQ_PUB=wA:0-1,wB:2-3 ISAC_SHM_IQ_PUB_FREQ=wA=<fA>,wB=<fB>` (fA/fB two window centres from
   the scanner output, >= 100 MHz apart). Expect `SHMIQ_PUB_READY` x2, UHD log `Actual RX frequency` per channel = fA,fA,fB,fB.
   Check: `usrp->get_rx_freq(i)` for i=0..3; consumers on wA and wB each decode their cell; no `rx ch1 NCO-only retune landed` warnings;
   RFSTALL/overruns unchanged vs one window (compare the usrp overflow counters).
2. LO sharing: repeat with the RX1 port/antenna per channel; if tuning ch2 moves ch0/1 (`get_rx_freq` differs from request) the pairs cannot sit on different LOs: then use the
   time-shared mode or one board per window.
3. Time-shared: `ISAC_SHM_IQ_PUB=wA,wB ISAC_SHM_IQ_DWELL=wA@<fA>:200,wB@<fB>:100 ISAC_SHM_IQ_SETTLE_US=1000` (one or two channels).
   Expect `SHMIQ_PUB_DWELL from=/wA to=/wB center_hz=... retune_us=...` every dwell, consumers `SHMIQ_DISCONT` once per return, never `SHMIQ_PRODUCER_LOST`.
   Measure: `retune_us` (blocking time of set_freq) and the LO settle time (raise SETTLE_US until the first samples of a return
   are clean: no AGC/gain-step or phase transient in the cell's first slots); overruns/`O` counters and RFSTALL during retunes;
   per-cell outage per return in sample time = (first ring timestamp after return - last before leaving) - from the consumer's
   `SHMIQ_DISCONT skipped_in` and the receiver's reacquisition time (G11 path) to first decoded grant; fraction of time away vs `dwell`.
4. Metadata: `SHMIQ_READY ... radio=<sdr_addrs or pid> time_source=pps` when the cfg has `time_source=external`/gpsdo; two radios with PPS must report equal-comparable ticks (check `set_time_next_pps`).

## Wideband publisher (2026-10-13; [CODE-READ] + compile only, everything UHD is [NOT RUN])

Primary mode: ONE wideband window (4 antennas, one LO) published by `shm_iq_uhd_pub` (radio/USRP/shm_iq_uhd_pub.cpp), one `shm_iq`
receiver process per cell on top (each with its own DDC). The per-channel-window/dwell publisher (UE hook) stays the fallback.
Compile check: [OFFLINE VERIFIED, cloud x86, g++ 13 + libuhd-dev 4.6, standalone compile, links]; non-UHD step
(`shm_iq_uhd_core.c`: reserve -> recv into ring slots -> commit, gap/copy path, error counters) [OFFLINE VERIFIED, cloud x86,
ctest `test_shm_iq_uhd_core`, fake recv source: in-place path, first write, overflow gap, ring wrap, tick<->index mapping].

### Zero-copy recv
`shm_iq_ring_reserve()` raises `reserve` and hands out the slot pointers at `head` (count clipped at the ring end, so a recv never
wraps; UHD keeps the rest of the packet for the next call). UHD `recv()` writes into them; `shm_iq_ring_commit(tick, got)` publishes
`head` if the timestamp continues the run. Only the first write and an overflow gap (timestamp jump) take the copy path via
`shm_iq_ring_write()` (data staged, then written at the new index so consumers see `valid_from`/gap exactly as with the UE hook).
Safety: consumers already treat anything below `reserve - capacity` as overwritten (seqlock), so a consumer that lags by almost a
full ring may lose a chunk while the producer is inside `recv()`; the effective lag budget is `capacity - chunk` (default chunk 65536).

### Build (DGX, aarch64)
    cd cmake_targets/ran_build/build   # configured with -DOAI_USRP=ON; never while nr-uesoftmodem runs (pgrep -x nr-uesoftmodem)
    ninja shm_iq_uhd_pub shm_iq                # -> build/shm_iq_uhd_pub

### Run (245.76 MS/s x 4, CG_400 image loaded; MCR is left at the image default 491.52 MHz unless --mcr is given)
    ISAC_UHD_CTRL_CORES=<two cores != rx-core, != DPDK lcores> \
    ./shm_iq_uhd_pub --args "type=x4xx,addr=192.168.10.2,mgmt_addr=192.168.30.2,use_dpdk=1,dpdk_num_mbufs=65536" \
        --rate 245760000 --freq <centre from the scanner> --gain 38 --channels 4 --antenna RX1 --ring isac_iq \
        --rx-core <X925 core> --duration 600
Device args = the survey's (tests/passive_rx/survey/survey_capture.sh). Check first that `--rate 245760000` is accepted on CG_400
(the tool aborts with `SHMIQ_UHD_FAIL actual rate ... != requested` otherwise; usrp_lib.cpp notes CG has no DDC, so a rate other than
the MCR may need `--mcr 245760000` if the image allows it; benchmark_rate at 4 x 245.76 passed, copy its master_clock_rate).
Consumer cell (any number, own process): `ISAC_SHM_IQ_NAME=isac_iq ISAC_SHM_IQ_CELL=c675 ISAC_CELL_INSTANCE=... nr-uesoftmodem` with `device = { name = "shm_iq"; }` (see MPCI-M1/M2).
Stop with SIGINT (`kill -INT`): the tool issues STREAM_MODE_STOP_CONTINUOUS, drains, releases the device (X410 claim hygiene); never SIGKILL.

### Expected log lines
    SHMIQ_UHD_MCR 491520000   SHMIQ_UHD_CH uhd_ch=0 rate=245760000.000 freq=... bw=... ant=RX1   SHMIQ_UHD_LO ch=0 locked=1 (x4)
    SHMIQ_UHD_READY ring=/isac_iq channels=4 rate=245760000 center_hz=... capacity=33554432 bytes=536875008 radio=<serial> time_source=free
    SHMIQ_UHD_STATS t=10.0 samples=2.4e9 rate_meas=245760000.0 overflows=0 timeouts=0 ring_head=...   (every 1 s)
    SHMIQ_UHD_END samples=... overflows=0 timeouts=0 inplace=... copied=1       (copied=1 is the first write; more = gaps)
    on overflow: SHMIQ_UHD_OVERFLOW n=<count>   (consumers then log one SHMIQ_DISCONT)

### Acceptance (10 min) [NOT RUN]
overflows=0 and timeouts=0 in `SHMIQ_UHD_END`; `copied=1`; `rate_meas` within 1e-4 of the requested rate (cumulative from the first
sample); every consumer `SHMIQ_OVERRUN` count 0 and no `SHMIQ_PRODUCER_LOST`; all four `lo_locked` true; RFSTALL-style counters
(`rx_bytes` vs `rate x t x 16`) agree. Then repeat with 2-3 consumers concurrently and compare overflows.

### 491.52 MS/s step (400 MHz, needs the 2nd QSFP28 cable) [NOT RUN]
UHD multi-link: pass both ports in the device args, e.g. `addr=192.168.10.2,second_addr=192.168.20.2,mgmt_addr=192.168.30.2,use_dpdk=1`
(X4xx 100 GbE: `addr` = QSFP port 0, `second_addr` = port 1; UHD round-robins the RX channels over the links; verify with
`benchmark_rate --args "<same args>" --rx_rate 491.52e6 --rx_subdev ...` first, already the user's procedure). Then
`--rate 491520000 --channels 4`; raise `--capacity` to 67108864 (2^26 samples = 136 ms; 1 GB) since each consumer now has to sustain
7.9 GB/s of reads. DPDK needs one lcore per link plus the rx-core and ctrl cores kept distinct: size `ISAC_UHD_CTRL_CORES` and
`dpdk_corelist` for two links (usrp_lib.cpp found uhd_ctrl at FIFO 97 colliding with a DPDK worker on the same core: half the starts never recycled an mbuf).

### Memory-bandwidth / CPU budget
Rate x 4 B x channels: 3.93 GB/s (4 x 245.76 MS/s), 7.86 GB/s (4 x 491.52). Producer writes it once (UHD copies mbuf -> ring slot; the
NIC DMA into mbufs is a second write, so ~2 x the rate on the memory bus plus 1 x read of the mbuf) = ~12 GB/s at 245.76 and ~24 GB/s
at 491.52 for the producer alone. Each consumer reads the full 4-channel ring once (DDC reads the window it needs: still all
channels): + 3.9 / 7.9 GB/s per cell. With 5 cells: ~40 GB/s at 245.76, ~63 GB/s at 491.52, against the Spark's LPDDR5X (~273 GB/s
peak, ~200 practical, shared with the GPU): fits at 245.76, need measuring at 491.52. CPU: one core busy-polling in recv (memcpy-bound),
pin it (`--rx-core`, SCHED_OTHER; no RT priority set by the tool) with the uhd_ctrl cores separate.
Each ring slot copy is not counted twice: nothing is staged except after a gap.

### Open points
Not hardware-tested: DPDK lcore layout of this binary (it uses UHD's own thread config, `use_dpdk=1` in the args, run as root with
hugepages like capture_raw); whether `ISAC_UHD_CTRL_CORES` finds the `uhd_ctrl*` threads before they exist (it is applied right after the stream start command);
`--time-source pps` uses `set_time_source("external")` + `set_time_next_pps(0)` (untested on the X410 sync cabling).
