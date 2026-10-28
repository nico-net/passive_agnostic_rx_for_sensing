# MPCI-FEEDSTART (2026-10-28): multi-cell start-up deadlock between the RU start and the feed reader

Branch `mpci/c-feedstart` (worktree `/home/nicola/NICOLA/wt/rr-mpci-feedstart`), base mpci-main @ 6362bc70f3. Host: DGX GB10 aarch64, shared.
Build: own dir, `-DOAI_USRP=ON -DOAI_SIMU=ON -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121 -DNB_ANTENNAS_RX=4 -DENABLE_TESTS=ON`,
nice 19, -j4, `flock -s`. GPU runs: `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps`, serial. No radio, no OTA, no receiver run.
NO FLAGS: no env var, no option, no -D switch. N=1: untouched (the feed exists only with a radio and > 1 admitted cell).

## STOP result

| STOP item | result |
|---|---|
| root cause with evidence | MET, below `[MEASURED log facts + CODE-READ + OFFLINE VERIFIED reproduction]` |
| start test fails on base | MET `[OFFLINE VERIFIED, DGX GB10 aarch64 unit]`: base 6362bc70f3 + test: CPU 3/3 runs and GPU 3/3 runs FAIL at start 1 (reader 200 us late), `reads=0 consumed=0`, radio backlog 250 ms within 250 ms (the final test version re-checked on the base: CPU 2/2, GPU 1/1 FAIL the same way) |
| passes 50/50 after the fix | MET `[OFFLINE VERIFIED, DGX GB10 aarch64 unit]`: CPU 3 runs x 50/50 (RU start max 5.0-11.7 ms, radio backlog max 4.2-10.0 ms), GPU 3 runs x 50/50 (RU start max 3.9-8.9 ms, backlog max 3.2-8.8 ms; limit 250 ms) |
| ctest no new failures vs the known list | MET `[OFFLINE VERIFIED, DGX GB10 aarch64, CUDA build, shared host]`, section 5 |
| nr-uesoftmodem links | MET: `ninja -k 0 nr-uesoftmodem nr-uesoftmodem-optional-modules oai_usrpdevif shm_iq tests` rc 0, `liboai_device.so -> liboai_usrpdevif.so` |

## 1. Facts `[MEASURED, DGX+X410 34B2AF1, ota_runs/mp27*, mp28n*]`
- Failing runs (mp27e_P2, mp27e_Q1, mp28n_P1/P2/Q1): `MPCI_FEED started ... backend=gpu`, then `USRP_RING timed restart` and
  `USRP_RING_RECOVER attempt=1 reason=2 ... index=122880000` (Q1: 61440000) = exactly one ring. No `SHMIQ_READY` line at all; `SHMIQ no ring /nrcell_pci415
  after 30 s` (shm_iq.c device_init); `IQ_FEED_READ_ERROR` only after the assert. Working runs (mp27_P1, mp27e_P1): `SHMIQ_READY` for both rings right after
  `ru_start_thread`.
- `[CODE-READ]` usrp_lib.cpp ring producer: reason 2 = `w + n - oldest > ring_cap` with `oldest = max(ring_read, epoch_start)`; at the first fault w = ring_cap,
  so `ring_read < n` (n = chunk = max_num_samps x 8, ~16k samples). The feed reads 65536 per call and every completed read (copy or ZEROCOPY path, ZEROCOPY=1
  in MPCI_20261025.env) advances `ring_read` by 65536: **the feed reader completed zero reads**. Each later epoch repeats it (index = k x ring).
- `[CODE-READ]` the only blocking point of the reader before `trx_read` is `nr_shutdown_radio_enter(0)` in `nrue_feed_read` (shared radio lifetime gate).
  `nrue_ru_start` takes the same gate **exclusively** (`nr_shutdown_radio_enter(1)`) around `nrue_ru_start_impl`, whose ring RU 0 calls shm_iq `device_init`,
  which loops (usleep 10 ms, up to 30 s) until its cell ring has `head != 0` (first channelizer write). That write needs wideband input, i.e. a reader read.

## 2. Root cause
Start-up lock-order deadlock: the RU start holds the exclusive radio gate while waiting for an event (the first cell-ring write) that only the feed reader can
cause, and the reader needs that gate (shared) for every radio read. Whether it deadlocks depends only on which thread reaches the gate first after
`iq_feed_start` returns: main goes straight from `mpci_feed_start` to `ru_start_thread`; if the newly created reader thread has not entered its first read by
then, the RU start wins and nothing ever drains the radio (`reads=0`, usrp ring full, restarts, 30 s assert). If the reader is already inside its first read
(blocked until the PPS-timed stream start), glibc hands the gate to the waiting writer only after that read, the channelizer writes from that block and
device_init returns: the runs that worked.
- Why mostly GPU / since chanpool `[HYPOTHESIS, not measured]`: the trigger is the reader's first-scheduling latency relative to the main thread (sub-ms
  window). CHANPOOL moved the reader to its own RT-role CPU and removed the CPU channelizer's pinned threads; the GPU backend changes what runs on the
  host around the start. The fix does not depend on which.
- Reproduced without a radio `[OFFLINE VERIFIED]`: with the reader 200 us late, base stalls every time with `reads=0`, the OTA signature, on both backends.
- The same mechanism is G5's "For G6" item (a channelizer block larger than one feed read never writes before the RU attaches): covered by the same fix
  (the reader keeps reading until the first write exists).

## 3. Fix (commit 2 on the branch)
- `iq_feed_start` (radio/COMMON/iq_feed.c/.h) returns only once the feed is **live**: every running (not paused) cell ring holds its first block. The wait
  polls (1 ms), holds no lock, and ends early with the threads stopped on: reader stopped (`-EIO`), `cfg.cancelled()` (`-ECANCELED`, receiver passes
  `nr_shutdown_requested`), or `IQ_FEED_LIVE_TIMEOUT_S` = 30 s (the existing shm_iq attach limit; `-ETIMEDOUT`, line `IQ_FEED_NOT_LIVE ... reads=`).
  New stat `live_ms`.
- `shm_iq_ring_live()` (shm_iq_ring.c/.h): `head != 0 || (version >= 2 && away)`, the exact predicate shm_iq `device_init` waits for; device_init now calls
  it (same behaviour), so the two cannot diverge. head never decreases, so once iq_feed_start returned no RU's device_init can wait on data under the gate.
- Receiver (`executables/nr-uesoftmodem.c`, `mpci_feed_start`): `.cancelled = nr_shutdown_requested`; -ECANCELED returns (the RU start then sees the shutdown);
  -EIO / -ETIMEDOUT / other -> `mpci_feed_fail` with a named reason (exit before RX instead of 30 s of usrp restarts and an assert under the gate).
  `MPCI_FEED started ...` gains ` live_ms=<ms> reads=<n>` (multi-cell line only).
- Not changed: gate semantics, nr-ue-ru.c, usrp_lib, channelizer backends, the N=1 path.

## 4. Test `test_iq_feed_start` (radio/COMMON/tests/test_iq_feed_start.c, cmake `# ---- FEEDSTART ----` in mpci_cloud.cmake)
Real order of the receiver without a radio: fake device that delivers at full rate from its timed stream start, blocking like a radio, behind the real
`nr_shutdown_radio_enter(0)` (as nrue_feed_read); a separate "PPS" thread starts the stream once the feed threads exist (+0/2/20 ms; the X410 starts later);
`iq_feed_start` with the CPU backend (Q geometry 415 d1 + 403 d4 at 122.88 MS/s x 4) or the GPU backend (P geometry 415 d2 + 923 d2 at 245.76 MS/s x 4);
CHAN leases from a real `nr_core_pool` (cores = this process's affinity, derived) through the receiver's lease hooks; "RU start" = exclusive gate around
the real libshm_iq.so `device_init` + `trx_start` of both cell rings; then 8 reads per cell under the shared gate. Reader first-scheduling latency 0 / 200 /
1000 / 3000 us. Fail = RU start not done in 5 s or radio backlog > 250 ms (half the receiver's 500 ms usrp ring); a stall prints FAIL and exits.
ctest: `test_iq_feed_start_cpu`, `test_iq_feed_start_gpu` (RUN_SERIAL, MPS dir, 77 = no device/module).

## 5. Build / ctest `[OFFLINE VERIFIED, DGX GB10 aarch64, CUDA build, shared host]`
`ctest -j4 --timeout 900` (flock -s, nice 19, MPS dir): 350 run, 15 failed = known list (`dft_test`, `test_nr_modulation`, `test_nr_pusch_ra0_qam64`,
`test_nr_pusch_ra0_qam256`, `test_nr_ssb_rate_match_prod`, `test_ldpc_cuda_pool_cpu`, `nr_td_sim_test`) + `test_nr_pusch_uci_*` x6 "Not Run" (binary not in
`tests`, pre-existing, also in the G6/A8 baselines) + `test_nr_spin_handoff` and `test_mpci_a6_ul_cells`: both passed 3/3 on rerun (load timing; spin
handoff is the known wake-latency bench of the G6 report; neither touches the changed files). New: `test_iq_feed_start_cpu` (35.6 s) and
`test_iq_feed_start_gpu` (4.8 s) pass; `test_iq_feed`, `test_shm_iq`, `test_iq_chan`, `test_iq_chan_soak5`, `test_iq_chan_gpu(_discrete)` pass.

## 6. Open items
- `[CODE-READ]` the CPU backend's `iq_chan_start` runs its k-derivation bench (~0.4 s here) after the radio was started and before the reader exists; the
  X410's PPS-timed start (>= 1 s) covers it today, a radio that streams at once would buffer that long (500 ms ring). Not changed (GPU backend: no bench).
- G6 open item unchanged: `nrue_ru_restart_rx`/`reinit` take the exclusive gate while a cell RU read may wait on a paused ring (shared holder waiting on the
  producer, not on the gate: no deadlock with glibc's reader-preferring rwlock, `[CODE-READ]`).

## 7. OTA check for the orchestrator
Build this branch (no receiver of ours running): `flock -s /tmp/td_measure.lock nice -n 19 ninja -k 0 -j8 nr-uesoftmodem nr-uesoftmodem-optional-modules
oai_usrpdevif shm_iq params_libconfig` + `ln -sf liboai_usrpdevif.so liboai_device.so`. Run `rr-orchestration/ota/run_mpci_ota_20261028n.sh` with this build
(P1, P2, Q1; the start is what is tested, 150-300 s each is enough), ideally 3 starts per pair. Pass per run: `MPCI_FEED started ... backend=gpu live_ms=<x>
reads=<n>` with reads > 0, then `SHMIQ_READY` for every cell ring, no `USRP_RING_RECOVER reason=2` / `timed restart` in the first 10 s, no `SHMIQ no ring`,
no `IQ_FEED_NOT_LIVE`; both cells reach sync. Record live_ms (expected about the PPS start latency, ~1 s). Labels
`[MEASURED, DGX+X410 34B2AF1, 2026-10-28 <time>, cell PCI/centre/BW, n=..]`, one row per cell.
