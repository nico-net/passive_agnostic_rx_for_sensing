# RXR: receive recovery and direct ring read

## Implementation

- [CODE-READ] `radio/USRP/usrp_lib.cpp:1008,1043,1895` gates producer recovery on `ISAC_RX_RECOVER=1` (default off). Overflow and other UHD metadata errors, receive exceptions, startup timeouts, missing timestamps, and ring overruns enter the same bounded stop/drain/restart path. `ISAC_RX_RECOVER_MAX` defaults to 20; a second of valid samples resets the consecutive failure count. Restarts use `stream_now=true` so this application does not issue its former `get_time_now()` control read after a fault. The producer retains a monotonic logical write index, publishes a new epoch and hardware timestamp origin on the first valid packet, and abandons unread samples at the boundary. The reader skips that boundary, rejects a slot if a fault overtakes conversion, and leaves the existing exit-3 shutdown in place only after the retry limit. Index/retry accounting is in `radio/USRP/usrp_ring_recovery.h:9`.
- [CODE-READ] `executables/nr-ue.c:274,1239,1735`, `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:549,920`, and `openair1/PHY/NR_UE_TRANSPORT/pss_nr.c:219` detect either a hardware timestamp gap or a driver epoch change, drop the current slot, clear timing, and run a PSS/SSS search around the previously decoded PSS sample position on the learned PCI and SSB. A miss falls back to the ordinary full-capture search. The soft configuration epoch invalidates in-flight IQ consumers while retaining learned dedicated state; legacy behavior remains under the flag-off path. Successful reacquisition logs `RX_RECOVER epoch=.. gap_samples=.. resync_ms=..`.
- [CODE-READ] `radio/USRP/usrp_ring_view.h:10` and `usrp_lib.cpp:2146` gate the direct read on `ISAC_USRP_RING_ZEROCOPY=1` (default off). The reader shifts sc16 samples directly from a circular ring window into PHY `rxdata`, including the wrap, without the intermediate ring-to-temporary-buffer copy. The format-shift write into `rxdata` remains necessary. `ISAC_USRP_RING_TRAFFIC_METRICS=1` appends cumulative `usrp_ring_copy_bytes` and `usrp_ring_direct_bytes` to `ISAC_METRICS` (`openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics_json.c:182`); take deltas between metric timestamps to get bytes/s. Flag-off JSON remains unchanged.
- [CODE-READ] `ISAC_USRP_RING_PKTS` already permits 1–64 packets per `recv()` and defaults to 8. The supplied OTA benchmark with one packet had zero drops; no larger default is justified by that evidence.
- [CODE-READ] The raw-IQ device already had `ISAC_RAW_IQ_GAP_AT_S` and `ISAC_RAW_IQ_GAP_S` test-only injection. RXR uses it without changing the replay device.
- [CODE-READ] The CSI-RS discovery contract (`nr_csirs_disc.h`) was not changed.

## Verification

- [OFFLINE VERIFIED, DGX aarch64 sandbox] `nr-uesoftmodem`, `oai_usrpdevif`, and `raw_iq` built with the configured CPU Ninja graph. The sandbox cannot regenerate CMake: CPM download fails DNS resolution and GTest is absent; the local generated build graph was retained for the build.
- [OFFLINE VERIFIED, DGX aarch64 sandbox] `ninja tests` completed 609/609 build steps. Chunked `ctest -E '^nr_td_sim_test$'` covered all 224 registered tests: 15 pre-existing disabled, 200 passed, nine documented ARM/sandbox failures. The failures were `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `time_management_tests` (socket denied), `dft_test`, `test_nr_modulation`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, and `nr_cuup_functional_test`. Chunk ranges were 1–56, 57–112, 113–168, and 169–224, with `-j4` except the last `-j1` chunk. The long `test_nr_pdsch_config_sweep` (528.50 s) and `test_nr_csirs_disc_fp` both passed.
- [OFFLINE VERIFIED, DGX aarch64 sandbox] `tests/passive_rx/rxr/test_usrp_ring.cpp` passes. A fake streamer injects randomized overflow, exception, and timeout events and verifies monotonic indices, epoch publication, and bounded retry exhaustion. The direct conversion test covers every wrap position and partial vector tail.
- [OFFLINE VERIFIED replay raw_pci415_12s_20261008, DGX aarch64 sandbox] A no-sudo replay at 0.25x with `ISAC_RX_RECOVER=1 ISAC_RAW_IQ_GAP_AT_S=5 ISAC_RAW_IQ_GAP_S=0.05` acquired PCI 415 before the injection. `RAW_IQ_GAP` skipped exactly 6,144,000 samples at recorded sample 614,459,946. `RXDISCONT` reported the same jump, dropped the slot, and reacquired PCI 415; `RX_RECOVER epoch=1 gap_samples=6144000 resync_ms=2189`. The passive metrics showed one sync loss and two PBCH locks. PDCCH accepts rose from 45 before the gap (absolute slot 8332) to 131 after it (slot 22172); PDSCH CRC OK rose from 0 to 41. The replay reached EOF with `consumed=1474558462`, `clipped=0`, and no RFSTALL. Log: `/tmp/rr-csd-a-replays/replay_rxr_gap_150852/run.log`.
- [CALCULATED, four channels at 122.88 MS/s, sc16] The removed staging copy moves 1,966,080,000 bytes/s in each direction, saving 3,932,160,000 bytes/s of CPU memory traffic at real-time rate. The ring reader's two passes (staging plus format shift) become one, halving its traffic from 7.864 to 3.932 GB/s. Counting the producer's ring write as well gives 9.830 to 5.898 GB/s, a 40% reduction in total host memory traffic. This is a model, not an OTA bandwidth measurement; the new metrics provide measured deltas on a live stream.

Focused test command: `g++ -std=c++17 -O2 -I . tests/passive_rx/rxr/test_usrp_ring.cpp -o /tmp/test_usrp_ring && /tmp/test_usrp_ring`.

Replay command (run without sudo under `flock -s /tmp/td_measure.lock nice -n 19`, with no OTA marker):

```sh
REPLAY_PREFIX='' REPLAY_OUT=/tmp/rr-csd-a-replays REPLAY_LAST=/tmp/rr-csd-a-replay-last \
BASECFG=/home/nicola/NICOLA/wt/rr-csd-a/tests/passive_rx/dgx/ue.passive.ota.dldedicated.dgx.cfg \
CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008 CAP_SAMPLES=1474560000 CAP_TICK=210036277 \
/home/nicola/NICOLA/wt/rr-orchestration/csd_replay_user.sh rxr_gap \
/home/nicola/NICOLA/wt/rr-csd-a/cmake_targets/ran_build/build 0.25 \
ISAC_CORE_PLAN=0 ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CUMULATIVE=1 ISAC_PDCCH_NID_BANK=1 \
ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 \
ISAC_DCI11_STAGE2=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_RX_RECOVER=1 \
ISAC_RAW_IQ_GAP_AT_S=5 ISAC_RAW_IQ_GAP_S=0.05
```

## UHD behavior and limits

- [CODE-READ + UHD documentation] RFNoC continuous-stream overflow recovery is initiated within UHD's RX streamer before the application receives overflow metadata. The public `recv()` documentation says the metadata error may arrive only after valid buffered samples are returned. No documented RX streamer option disables the internal radio restart/control operation. The application therefore catches the resulting exception and retries the stream; checking metadata cannot prevent an exception that UHD raises first. See [RFNoC radio overrun handling](https://files.ettus.com/manual/classuhd_1_1rfnoc_1_1radio__control.html) and [RX streamer error handling](https://files.ettus.com/manual/classuhd_1_1rx__streamer.html).
- [IMPLEMENTED, NOT VALIDATED OTA] Recovery from a persistent CTRLEP failure still needs X410 validation. After 20 consecutive failures, exit 3 remains intentional. The direct ring read still performs the required 16-bit sample shift and has not yet been timed on the X410.

## Deferred measurements

- [DEFERRED OTA, orchestrator] Compare 4-channel 122.88 MS/s starts and sustained decoding with `ISAC_RX_RECOVER=1 ISAC_USRP_RING_ZEROCOPY=1 ISAC_USRP_RING_TRAFFIC_METRICS=1 ISAC_METRICS=1`, using the existing dedicated OTA command/configuration. Record epochs, restart attempts, CTRLEP exceptions, PDCCH/PDSCH accepts before and after each fault, and the difference of cumulative traffic counters divided by metric interval. Repeat with `ISAC_USRP_RING_ZEROCOPY=0` for the copy baseline; no radio command was run here.
