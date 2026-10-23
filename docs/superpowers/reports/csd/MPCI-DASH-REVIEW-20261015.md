# MPCI-DASH-REVIEW-20261015: review, integration, DGX offline validation and first OTA of csd/mpci and csd/dashboard

Branch `csd/mpci-int` @ 57248bf5e0 (worktree `rr-csd-mpr`). It contains `csd/otatest` 26d3028b5d (merged 2026-10-15),
`csd/mpci`, the dashboard commits of `csd/dashboard`, and 13 review commits. Host: DGX Spark (GB10, aarch64, sm_121).
Build: `build_t26` (RelWithDebInfo, ENABLE_LDPC_CUDA=ON, OAI_USRP=ON). Work was done on 2026-10-13, frozen, and finished on 2026-10-15.

Labels follow PROJECT_MEMORY §0.1:
- `[MEASURED, DGX ...]` and `[OFFLINE VERIFIED, DGX aarch64]`: this session.
- `[CLOUD CLAIM]`: a statement of the cloud reports (MPCI-*, DASHBOARD-20261013, cloud x86) that was not re-measured.
- OTA: `[MEASURED, DGX OTA, X410 34B2AF1, 2026-10-15 03:23-03:45, LOW LOAD]`. Night load is not a validation.

Replays use 4 RX, 0.25x and the `ota/t26f.env` working env unless stated. Cells, captures and OTA runs are never pooled. n is 1-3 per arm: indicative only.

## 0. Summary

**Integration and flags-off behaviour**
- Build and ctest pass: only the known ARM failures remain.
- Static TLS is unchanged (2.41 MiB).
- Flags off, the receive path is unchanged in code; replay non-regression shows no detectable change (section 3.3).

**Offline (replay of the c3610 capture)**
- M1 fan-out: two cells of two operators (WindTre PCI 508, Iliad PCI 403) decode in parallel from one shm ring.
  - They reach the same sync, MIB and SIB1, the same CORESET bank and CSI-RS state, and comparable CRC-OK rates.
  - GPU `check_bad` is 0 and there are 0 overruns once the filter fix is in.
- An apparent ~17 % decode-volume loss of the fan-out arm turned out to be a replay pacing artefact, not the shm path. A fan arm paced like `raw_iq` matches the solo run.

**First OTA (LOW LOAD)**
- One `shm_iq_uhd_pub` claim of the X410 fed two receivers at once through the ring:
  - WindTre PCI 508 read the wideband ring directly (zero shift).
  - Iliad PCI 249 went through the GPU channelizer (decim 4).
  - Both decoded; 0 publisher overflows, 0 ring overruns for 508 (with a 1.09 s ring).
- A 245.76 MS/s window with Vodafone PCI 415 and TIM PCI 923 (80 MHz apart) synced both cells and decoded their SIB1. But the GPU channelizer could not keep up at that rate: 22 overruns, run aborted.

**Blocking for production multi-cell at 4 RX**
- Per-cell DDC on the GB10 CPU is far below real time: one 4-channel cell is at 0.71x real time with the mixer only and 0.30x at decim 4.
- So a shifted cell cannot consume the ring directly at 1x, and the GPU channelizer is the only shifted-cell path.
- The channelizer sits on the host-copy / GPU time-slice limit:
  - 1 cell at 122.88 MS/s: OK on air.
  - 2 cells at 245.76 MS/s: not OK on air.
- A zero-shift cell (cell centre = ring centre) is a plain memcpy and is fine.

**Found and fixed (13 commits, section 2)**
1. The UHD publisher ignored usrp_lib's X4x0 `rxshift` (cells would see a +12 dB digital level).
2. The UHD publisher broke the ring on every chunk with this X410's frozen timestamps (found on air).
3. The consumer filter design ignored the cell grid (a 175-tap stage that caused overruns).
4. The launcher could not pass the OTA receiver flags or per-cell core lists.
5. The dashboard attributed an unrelated receiver process to a run.
6. Added: P99AGE parsing, a file publisher, the fan-out replay harness and an OTA multi-cell runner.

## 1. Integration

- **csd/dashboard.** Its three dashboard commits were cherry-picked (`a830d6a40b`, `1456bbc3e3`, `f86ad494d1`), not the branch itself.
  - The branch base `d21811d53a` (origin/adaptive-rx-UL-DL) carries the 2026-10-07 sa-bed merge, which is not in csd/otatest:
    - `571bd335d4` blind DM-RS identity solver, default ON (`ISAC_NID_SOLVE=0` is the kill switch);
    - a tracker refute rule, walk changes, and OAI gNB/UE edits.
  - Merging it would change flags-off behaviour of the DGX line, so it is left out.
  - The PROJECT_MEMORY hunk of `213cee5bbc` was dropped.
  - **Decision for you:** whether the sa-bed solver enters csd/otatest, with its own A/B.
- **csd/mpci.** Merged (`45818e7a73`). One conflict, in an include block of `nr_gpu_budget.c`: both kept.
- **csd/otatest 26d3028b5d.** Merged (`006b0973e7`). Three conflicts, both behaviours kept:
  - `nr_gpu_budget.c`: MPCI M3 publish plus `nr_gpu_budget_rt_active()`.
  - SIB1 cache and idsweep dump paths: per-cell `ISAC_CELL_INSTANCE` paths kept. The otatest sticky 01777 now lives in `nr_cell_mkdirs()`, so `/tmp/passive_rx` gets the same mode and `/tmp/passive_rx/<tag>` too.
  - `INDEX.md`: both rows.
- **sens6 frozen gate:** `git diff --quiet sens6-frozen-2026-09-30 -- ...` = 0.

## 2. Review findings (ranked)

### Fixed

**H1 — UHD publisher omitted the X4x0 rxshift** (`f2b0d1c67f`). `[CODE-READ]` + test.
- usrp_lib shifts X4x0 samples right by 2 (`srai_epi16`); `shm_iq_uhd_pub` wrote raw sc16. Every shm cell would have run 12 dB hotter than a single-process receiver at the same gain (gain 40 already saturates the PBCH channel estimate at this site).
- Now `--rx-shift` (default 2) runs in each recv thread. Unit test added.

**H2 — UHD publisher broke the ring on every chunk on this X410** (`94812fe218`). `[MEASURED, DGX OTA, 03:23]`.
- With the publisher alone: `inplace=7 copied=122712` in 60 s, and the ring head was about 2x the sample count. The 34B2AF1 stamps do not advance (USRP_TS_FROZEN); on air they even stepped back 68 ticks per 65536-sample chunk. Every consumer would have seen a timestamp jump per chunk.
- Fix: `pub_tick_track()` derives ticks from the sample count once a stamp falls behind.
- After the fix: `inplace=65582 copied=1` (the first write), ring head equals the sample count, overflows/timeouts/resyncs all 0.

**H3 — consumer passband ignored the cell grid** (`70665462c7`).
- The NR UE never sets `openair0_config_t.rx_bw`, so the filter fell back to 0.95 x rate. A 51-PRB cell at decim 4 got 19+175 taps (MPCI-M1 assumed 15+27, because its tests always pass `rx_bw`).
- Effect: 31 `SHMIQ_OVERRUN`, 26 % of the input dropped and 2 re-syncs on the 403 cell in replay.
- Now the passband is `num_rb_dl` x 12 x 30 kHz (taps 15+23): 0 overruns. The `SHMIQ_WARN` window check now uses the same width (it used to warn for every shifted cell).

**H4 — launcher could not run an OTA cell** (`ea742f970a`).
- Cell command lines had no `--passive-rx`, `--band`, `--ntn-initial-time-drift 0`, `-A` etc.
- Per-cell core lists could not be passed (`,` is the field separator), so all cells pinned their threads to the env's cores whatever the taskset mask.
- Added `CELL_ARGS`, and `+` inside a value now becomes `,`.

**H5 — dashboard attributed another receiver process to a run** (`10e0a0b18c`).
- `--offline` on `ab_LP_1_211542` showed the command line, cfg and env of an unrelated replay from another worktree. A multi-cell view would show cell 0's process for every cell.
- Now attribution goes only to the receiver whose stdout/stderr is the run log, or to the only root receiver on the host. Re-checked on the real OTA run `ab_K3`: config is empty as it should be while an unrelated receiver runs.

**M1 — dashboard did not parse P99AGE** (`913af341d3`). The csd/otatest line is now parsed and shown on the Performance page; tested on a real replay log.

**Tooling added.**
- `shm_iq_file_pub` (offline stand-in for `shm_iq_uhd_pub`): `0603ba2b21`.
- `fanout_replay.sh` and `mpci_runsum.py`: `18a6149cbf`.
- `ota_mpci.sh` (OTA multi-cell runner): `e4a6be0ef6`, `57248bf5e0`.

### Open, high

**O1 — Per-cell CPU DDC is not real time on the GB10** `[MEASURED, DGX GB10, shm_iq_chan --bench, 1 thread]`.

| 4-channel cell | real-time factor | CPU (core-s per s of input) |
|---|---|---|
| 122.88 MS/s, mixer only (decim 1) | 0.71x | 1.27 |
| 122.88 MS/s, decim 4 | 0.30x | 2.76 |
| 245.76 MS/s, decim 2 | 0.18x | 5.16 |

- These are 3-4x below the `[CLOUD CLAIM]` x86 numbers.
- So a cell that is not at the ring centre cannot consume the ring directly at 1x. Only zero-shift cells (memcpy) and channelized cells are viable.
- Lever: fuse the mixer and first stage (the float scratch round trip is about 24 B per sample) and vectorise for NEON.

**O2 — GPU channelizer is limited by host copies and GPU time-slicing.**

| condition | per-block result |
|---|---|
| idle, 2 cells (`[MEASURED, DGX GB10 bench]`) | 2.05x real time at 245.76 MS/s; 3.15x at 122.88 MS/s |
| idle, 4 cells | 0.98-1.18x (one host thread saturated) |
| idle, 491.52 MS/s | 0.66x |
| live replay, 2 receivers, 0.25x | GPU 0.71 ms per block, 7x the idle 0.1 ms |
| on air, 1 cell at 122.88 MS/s | 0.80-1.34 ms per 2.0 ms block, no overrun |
| on air, 2 cells at 245.76 MS/s | 1.18 ms per 1.0 ms block, 22 overruns, aborted |

- Levers: CUDA stream priority or an SM partition for the channelizer (it is RT-class but invisible to `nr_gpu_budget`/M3), ring-to-pinned copy without staging, double buffering, MPS.

**O3 — Overrun cascade** `[MEASURED, DGX OTA 03:32]`.
- With the default 273 ms ring, one stall of the 508 cell turned into 31 overruns, each followed by a re-sync that was itself slower than real time.
- A 1.09 s ring (`--capacity 2^27`, 2 GB) gave 0 overruns.
- Make 2^27 the publisher default at 122.88 MS/s; consider keeping state on a jump instead of a full re-sync.

**O4 — Multi-streamer resync on a benign count mismatch** `[MEASURED, DGX OTA 03:39]`.
- At 245.76 MS/s with chunk 65536, one channel's recv returned 57252 vs 59240 samples. The lockstep treated it as MISMATCH and restarted all streamers, which cascaded into DPDK drops ("D"), overflows and the abort.
- With `--chunk 131072`, 30 s and then 150 s ran clean (0 overflows).
- Fix candidate: publish min(count) and keep the remainder, instead of restarting.

**O5 — RAM: about 2 full cells per DGX** `[MEASURED, DGX replay + OTA]`.
- Per receiver: anon RSS about 13 GB (217-PRB cell) or 3.6 GB (51-PRB cell), plus GPU 11 / 9 GB.
- Publisher ring: 0.5-2 GB.
- On air, MemAvailable fell from 110 GB to 70 GB with 2 cells (508 + 249) and to 55 GB with 2 x 217-PRB cells.
- With the 16 GB floor and the 48 GB OTA gate, about 3 cells is the ceiling. `mpci_auto.py` does not check RAM.

### Open, medium

**O6 — M3 arbiter busy refusals** `[MEASURED, DGX replay, n=1]`.
- Cross-cell discovery yields went from 0.59 M to 7.25 M; cell CPU rose about 10 % (44.0 vs 38.9-40.1 s).
- Callers poll `discovery_begin`; the arbiter should return a back-off time.
- Budgets were respected (summed discovery 15.9 % + 2.6 %, under the 30 % budget) and neither cell starved: both kept banks and nID passes. The rule "any other cell's RT in flight blocks discovery" grows with N cells (GPU-exact PDSCH publishes RT on every grant).

**O7 — Core budget under the t26f core map** (section 4). The planner splits cores evenly but sets no `ISAC_*_CORE` pins, and the `scan_thread` core is a cfg field shared by all cells. `fanout_replay.sh` and `ota_mpci.sh` work around both; `mpci_auto.py` does not.

**O8 — Radio facts no longer reach the cell logs.** Shm cells have no CHPOW/RFCHAN/RFSTALL lines, so the dashboard Radio tile is empty for them. The publisher's `SHMIQ_UHD_STATS` is not parsed.

**O9 — SIB1 sibling file written non-atomically.** `/tmp/passive_rx/sib1_sibling_<ssb>.bin` is shared across cell processes by design, but written with `fopen("wb")` and `fwrite`. A sibling reading at the same moment can see a truncated file. Use write-then-rename. This is otatest code, not changed here.

**O10 — Timed tune late.** At 4 x 122.88 MS/s the tune of 4 channels took longer than 0.5 s, so channels were tuned unaligned. This matters only for AoA phase.

### Open, low

- The OTA scripts write no `label.txt`/`env.txt`/`cmd.txt` for the dashboard.
- Headless screenshots were not possible: no Chromium or Playwright on the host.
- Some test warnings (unused-result) remain in `nr_mpci_sched_test`.

### Checked, no issue

**M2 isolation** `[CODE-READ]`.
- Process-per-cell isolates all statics and TLS.
- Host-global paths are tagged: coresets file, SIB1 cache, idsweep, pdcch/pbch fixtures, stats log. Metrics JSON carries `"cell"` only when the tag is set.
- The remaining untagged `/tmp` paths are off the NR receive path, except the intentional SIB1 sibling file (O9).

**Flags off** `[CODE-READ]`. Every hook is guarded by an env check or a NULL handle: `shm_iq_pub_maybe_wrap`, `nr_cell_path`, the mpci handle, metrics publish.

**Static TLS** `[MEASURED, readelf]`. 0x269100 in both the merged binary and the otatest binary.

**No per-cell or operator constants** in mpci, monitor or supervisor code (grep); only documentation examples.

**Dashboard** `[OFFLINE VERIFIED + MEASURED]`.
- Binds 127.0.0.1 and is read-only (tests pass).
- On real OTA and replay logs every parser matches its family: sync, MIB, SIB1, carrier, bank, SS, scrambling, DCI length, CSIRS_BLIND, summary, PDSCHQ, PUSCHQ, LDPCDIAG, pusch, RFCENSUS, EVMGRANT, ULLAYOUT (LOCK/BWP_LOCK/CRC/STATS), PDTIM/PDTIMPCT/PDTIMCLS/PDTIMCLSST, CHPOW, and now P99AGE.
- CPU while a live replay wrote its log, with a page polling every 2 s: 0.61 % of one core over 120 s.

## 3. Validation (DGX)

### 3.1 Build and ctest

`[OFFLINE VERIFIED, DGX aarch64, merged tree 2026-10-15]`
- ctest: 239/243 pass. The 4 failures are all known: `test_nr_pusch_ra0_qam256`, `test_nr_td_grantwork`, `dft_test`, `test_nr_modulation`.
- New tests pass:
  - `test_shm_iq` (with the passband case);
  - `test_shm_iq_chan`, `test_shm_iq_chan_selftest`;
  - `test_shm_iq_uhd_core` (rx_shift, tick tracking);
  - `test_nr_mpci_sched`, `test_nr_cell_instance`;
  - `test_mpci_launch` (6), `test_mpci_auto`;
  - `test_nr_gpu_budget`, `test_nr_passive_acq_state`.
- Monitor tests: 48/48.
- `shm_iq_chan --selftest --gpu`: max diff 1 LSB, 129 of 786432 samples differ, 0.10 ms per 65536-sample block.

### 3.2 Static TLS

0x269100 (2.41 MiB) in both binaries, under 3 MB.

### 3.3 Flags-off non-regression

`[MEASURED, DGX replay, t26f, 0.25x]`. Base = otatest binary (rr-csd-b build of a9e3883aaa, and a second build of 26d3028b5d in this build dir). Int = csd/mpci-int.

| capture | base PDSCH crc_ok / decoded | int crc_ok / decoded | RNTIs | check_bad |
|---|---|---|---|---|
| tim (TIM 923, -C 3759990000 --ssb 233) | 2962/8595, 2992/8861, 3540/8681 (a9e3); 3158/8494, 2982/8712 (26d3) | 3920/8168, 3977/8391, 2876/9432 | 12 every run | 0 |
| b (Vodafone 415) | 2754/10212 | 2784/10346 | 43 / 42 | 0 |

- Earlier, with the t26b env on 2026-10-13: b 1810, 1468 (base) vs 1686, 2300 (int); tim 19 vs 22 crc_ok.
- The int spread overlaps the base spread on every capture.
- The first int pair on tim (3920/3977 vs 2962/2992) looked like a difference. It came from run order and spread: base rebuilt from 26d3 gave 3158/2982, and int gave 2876.
- Sync, SIB1, banks, CSI-RS and check_bad are identical in kind.

### 3.4 M1 fan-out on c3610

`[MEASURED, DGX replay l0942_c3610x8, t26f, 0.25x]`. Cells A = WindTre PCI 508 (217 grid, shift -19.81 MHz, decim 1) and B = Iliad PCI 403 (51 grid, decim 4). Core pins are identical in solo and fan arms.

| arm | A crc_ok/decoded | A p99 us | B crc_ok/decoded | B p99 us | overruns | min MemAvailable |
|---|---|---|---|---|---|---|
| solo raw_iq (2 runs) | 225/903, 250/879 | 715, 565 | 92/154 (raw_iq decim 4: 133 s CPU) | - | - | 86 / 99 GB |
| fan A+B (2 runs) | 192/722, 190/742 | 9285, 6065 | 139/197, 136/187 | 13785, 5055 | 0 | 73 GB |
| fan A only | 192/740 | 605 | - | - | 0 | 86 GB |
| fan A only, publisher 0.125x | 243/935 | 555 | - | - | 0 | 86 GB |
| fan B only | - | - | 101/168 | 975 | 0 | 99 GB |
| fan + M3 arbiter | 185/749 | 4895 | 156/205 | 4965 | 0 | 73 GB |
| GPU channelizer | 164/625 | 2465 | 223/245 | 4195 | chan 5 (backlog), cells 5 discont | 72 GB |

- In every arm: sync PCI 508/403, SIB1 decoded, B bank and CSI-RS like solo, check_bad 0.
- **A's volume gap (about 740 vs 890 decoded) is pacing, not the shm path.** `raw_iq` pacing is elastic (sleep after each read: 0.13x effective). The file publisher is wall-anchored like a radio. At 0.125x the fan arm matches solo (243/935).
- **Running two cells together raises A's p99 from 0.6 to 6-9 ms** (GPU contexts time-sliced, shared CPU).
- **CPU (cell process):** A 31-38 s, B 30-33 s, publisher 4.5-4.9 s, CPU channelizer 33 s (2 cells; real time at 1x not possible on 2 threads: 1.02 ms per 0.5 ms block).
- **GPU memory:** A 11.0 GB, B 9.1 GB, channelizer 0.18 GB.

The 2026-10-13 runs (t26b env, pre-merge binary) agree: fan A 192-199/736-749, B 105-153, sched 192/139.

### 3.5 Channelizer benchmarks

`[MEASURED, DGX GB10, synthetic noise, 4 ch]`

| config | CPU 1 thread | CPU 3 threads | GPU block 65536 | GPU block 262144 |
|---|---|---|---|---|
| 122.88 MS/s, 1 cell D=1 + shift | 0.71x | - | - | - |
| 122.88 MS/s, 1 cell D=4 | 0.30x | - | - | - |
| 122.88 MS/s, 2 cells | - | - | 3.16x | 3.15x |
| 245.76 MS/s, 1 cell D=4 | - | 0.31x | 3.13x | 3.34x |
| 245.76 MS/s, 2 cells | - | 0.19x | 2.05x | 2.05x |
| 245.76 MS/s, 4 cells | - | 0.12x | 0.98x | 1.18x |
| 491.52 MS/s, 4 cells D=8 | - | - | - | 0.66x |

### 3.6 M3 arbiter

`[MEASURED, DGX replay, n=1 on each day]`
- Per-cell discovery shares stayed within the global 30 %: A 15.6-18.3 %, B 5.2-6.0 % of wall time over 1 s windows (mpci_sched_stat).
- The ACQUIRE cell (B) got its slices.
- No starvation: B nID passes 17 vs 15 without the arbiter; bank and CSI-RS unchanged.
- Cost: see O6.

### 3.7 Dashboard

See "Checked, no issue". Runs exercised: OTA `ab_LP_1_211542` and `ab_K3_1_111423` (offline) and a live replay. Screenshots: not possible on this host.

## 4. Multi-PCI under the t26f core map (how instances must fit)

GB10 cores (sd_t26ul.cfg header):
- big Cortex-X925: 5-9, 15-19 (17-19 DPDK, 15 uhd_ctrl, 16 ring producer);
- little Cortex-A725: 0-4, 10-14.

One t26f cell uses:

| role | cores |
|---|---|
| UL consumers | 0, 1 |
| DM-RS scan | 2 |
| PDSCH | 7, 8, 9 (big) + 3, 4 (little) |
| scan | 6, 7 (big) |
| USS | 12 |
| NID GPU | 13 |
| GPU worker | 10 |
| GPU partitions | discovery 8 SMs / rt |

That is the whole host for one cell.

With N = 2 shm cells each cell gets about half:
- Cell A: scan 5 (+6), PDSCH 7 + 3, DM-RS 2, UL 0, USS/NID/worker unpinned inside its mask 0,2,3,5,6,7,12.
- Cell B: scan 8 (+9), PDSCH 4 + 11, DM-RS 13, UL 1, mask 1,4,8,9,11,13,14.
- Channelizer main thread: 10.
- Publisher: uhd_ctrl 15, recv threads unpinned. Cores 16-19 are free for the publisher, since no usrp_lib ring runs in shm mode.

The two cells also share the GPU partitions; the M3 arbiter paces only discovery. Per-cell decode p99 rose from about 0.6 ms (one process) to 6-9 ms (two cells), in replay and on air.

Rule: N <= 2 full cells per DGX at t26f quality, limited by big cores, RAM (O5) and GPU time-slicing. A third, light cell (51 PRB, or co-channel with zero shift) is possible on little cores at reduced scan/PDSCH capacity. `ota_mpci.sh` implements this split; `mpci_auto.py` must learn it (O7).

## 5. OTA

### 5.1 What ran tonight

`[MEASURED, DGX OTA, X410 34B2AF1, 2026-10-15, LOW LOAD, t26f, sd_t26ul, binary csd/mpci-int 94812fe218/57248bf5e0]`. Not a validation: night load, n = 1 per arm.

| run | config | result |
|---|---|---|
| `mpci_pub_032341` | publisher alone, 4 x 122.88 @ 3590.19 MHz, 60 s | 0 overflows; frozen-stamp defect H2 (copied=122712) |
| `mpci_pub_032755` | same, after the H2 fix, 30 s | inplace=65582, copied=1, 0 overflows/timeouts/resyncs |
| `ab_M508s1_1_032856` | single process, WindTre 3590190000/517/-4842, 150 s | PCI 508, crc_ok 2172/5463 (39.8 %), p99 605 us |
| `ab_B403s1_1_033512` | single process, 3630000000/18/2164, -r 51, 60 s | PCI 249 (Iliad 222-50; 403 not seen tonight), 273/315 |
| `mpci_AB_033207` | publisher + 508 direct (TARGET 508) + 403 via GPU channelizer (TARGET 403), ring 273 ms | 508: 1397/4343, 31 overruns and re-syncs (O3); 403: never synced (absent tonight, see B403s1) |
| `mpci_AB_033650` | same, ring 1.09 s, second cell blind | 508: 1735/3969 (43.7 %), 0 overruns, p99 7.8 ms; Iliad PCI 249: 739/810 (91 %), 3 overruns; channelizer 1.34 ms per 2 ms block, 0 overruns; publisher clean; MemAvailable 70 GB |
| `mpci_pub245_034025` / `_034119` | publisher alone, 4 x 245.76 @ 3720 MHz, 30 s | single streamer 6 overflows; multi-streamer with chunk 131072 clean |
| `mpci_VT_033950` | 245.76 window, Vodafone 415 + TIM 923 via channelizer, chunk 65536 | publisher MISMATCH/restarts, aborted (O4) |
| `mpci_VT_034233` | same, chunk 131072 | publisher clean; both cells synced (PCI 415, 923) and decoded SIB1/SI (249/251, 169/169); channelizer 1.18 ms per 1.0 ms block, 22 overruns, aborted after about 33 s (O2) |

The X410 was released (unclaimed) after every run. SIGINT only.

### 5.2 Steps for the next (daytime) window

Commands are from `rr-csd-mpr`. Build first, with no receiver running:

    ninja shm_iq shm_iq_chan shm_iq_uhd_pub nr-uesoftmodem

Take the lock and set `HEAVY_HELD=1`:

    flock -x /home/nicola/NICOLA/wt/rr-orchestration/locks/heavy.lock env HEAVY_HELD=1 ...

1. **Publisher only (30 s).**

       OUT=<dir> FREQ=3590190000 DUR=30 CAPACITY=134217728 tests/passive_rx/mpci/ota_mpci.sh

   - Expect in `pub.log`: `SHMIQ_UHD_STREAM ... rx_shift=2`, `SHMIQ_UHD_TS_FROZEN` (4 lines), and `SHMIQ_UHD_END ... overflows=0 timeouts=0 resyncs=0 copied=1`.
   - Abort if `copied` > 2 or there are overflows: do not continue.
2. **Daytime A/B, alternated, n >= 2 per arm, 150 s each.**
   - Single-process baseline: `SPECS="M508s:t26f:sd_t26ul:3590190000:517:-4842" ota/ota_window_gen.sh` with `BDIR=<rr-csd-mpr build_t26>`.
   - Two-cell shm run:

         OUT=<dir> FREQ=3590190000 DUR=150 CAPACITY=134217728 CHAN_CELLS="pB:3630000000:30720000:18360000" tests/passive_rx/mpci/ota_mpci.sh \
           "pA:3590190000:217:517:-4842:0+2+3+5+6+7+12:7+3:5:2:0:ISAC_TARGET_PCI=508" "pB:3630000000:51:18:2164:1+4+8+9+11+13+14:4+11:8:13:1"

   - Expect per cell: `SHMIQ_READY ... decim=1`, sync, SIB1, `SHMIQ_END overruns=0`. Channelizer: `SHMIQ_CHAN_STAT ... overruns=0`, proc_ms below block_ms.
   - Compare pA with the single-process M508s run on the same day: crc_ok %, p99, banks, check_bad.
   - Read cell logs with `mpci_runsum.py`.
3. **Optional third, co-channel cell.** PCI 4 at zero shift: same geometry as pA with `ISAC_TARGET_PCI=4` and its own small core set. Watch MemAvailable >= 48 GB at the start and >= 16 GB during the run.
4. **Do not use the 245.76 MS/s channelizer window for production** until O2 is fixed. If you want to re-measure it, run 245.76 with `PUB_EXTRA="--chunk 131072"` and abort on more than 20 channelizer overruns (the runner does this).

Abort criteria (built into `ota_mpci.sh`):
- publisher not READY in 60 s, `SHMIQ_UHD_FAIL`, or more than 3 restarts;
- MemAvailable < 16 GB;
- channelizer overruns > 20.

Also stop if any cell logs repeated `SHMIQ_OVERRUN`, or if `SHMIQ_PRODUCER_LOST` appears while the publisher is alive.

After every run check `abort.txt` for "X410 released". If it says "NOT released", wait 60 s and check `uhd_find_devices`; never SIGKILL.

## 6. Merge recommendation

- **Dashboard commits and the review fixes:** ready to merge into csd/otatest. Flags-off behaviour is unchanged, all receiver hooks are guarded by env, and the fixes were tested on air.
- **Multi-PCI:** M1 ring, M2 tags, M3 arbiter and the publisher are usable for one or two cells with zero-shift or single-channelized cells.
  - Not yet production at 4 RX for shifted cells: O1 (CPU DDC) and O2 (GPU channelizer).
  - O3 (make the 2^27 ring the default) and O4 (multi-streamer resync) should be fixed before relying on the supervisor's wideband default (`mpci_auto.py` defaults to 245.76 MS/s with the channelizer, which failed on air tonight).
- **Not merged and needing your decision:** the sa-bed solver base of csd/dashboard (section 1).

## 7. Inherited, not re-measured

`[CLOUD CLAIM]`:
- x86 DDC throughput;
- rfsim fan-out results;
- M4 multi-window and dwell behaviour (per-channel LO and dwell on the X410 were not run);
- the supervisor `pass2`/`watch` behaviour on a live radio;
- PPS time source.

The M4 dwell and per-channel LO were not tested on air tonight.
