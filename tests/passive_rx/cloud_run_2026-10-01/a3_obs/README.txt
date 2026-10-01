Task A3: per-grant observation API (nr_passive_obs) -- rfsim validation, Step 9.
Evidence label: [SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 10cc6f0058]
(unit test: [OFFLINE VERIFIED, same host].) Not a DGX number; no OTA.

Setup: tests/passive_rx/dgx/rfsim_regress.sh 1, GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5, 150 s, 106 PRB, fully agnostic.
Runs (raw logs/JSONL under /tmp/claude-0/ev/a3, not committed):
  score_on.txt         ISAC_OBS_PATH + ISAC_METRICS_PATH set        gate PASS, crc 94.81 %, drop 1.79 %
  score_off.txt        no env vars                                 gate PASS, crc 94.87 %, drop 1.29 %
  score_throttled.txt  writer paused 60 s, ring 256 slots           gate PASS, crc 95.38 %, drop 1.64 %
                       (ISAC_OBS_TEST_WRITER_PAUSE_MS=60000 ISAC_OBS_TEST_RING_SLOTS=256, test hooks, inert when unset)
  (the HEAD baseline on this host is crc 94.9-96.9 %, drop 0.77-1.55 %; CRC with obs vs without differs by 0.06 pt.)
check_obs_on.txt: obs vs the last ISAC_METRICS snapshot (records with t_mono_ns <= snapshot):
  n_dl 14852 <= pdschq_decoded 14913 (layout-probe share 0.41 %); n_dl_crc_ok 14139 == pdschq_crc_ok 14139;
  obs_pushed == obs_written == 14852, obs_dropped == 0. The strong nr_passive_obs_stats overrides A2's weak stub
  (nm shows one T symbol; ISAC_METRICS obs_* nonzero).
check_obs_throttled.txt: obs_pushed 13129, obs_dropped 3591 (> 0) while decode CRC stayed 95.38 %: the writer never
  blocks decode. (that script's PASS_dropped_zero is expected to be false there.)
obs_sample.jsonl: 200 lines (first 100 and lines 14000-14100 of the obs_on run). All DL: rfsim phy-test has no PUSCH
  (pusch_try = 0), so the UL hook is compiled and reviewed but not exercised at runtime here.
check_obs.py: the check script.
Note: the on/off runs used the binary before the ISAC_OBS_TEST_RING_SLOTS hook was added to nr-uesoftmodem.c (env parse only).

Fix round 1 (review minors: io_errors/after_close counters, g_open_fast + nr_passive_obs_enabled() hook gating,
PRIO_INHERIT mutex, strtoul ring clamp, header notes): re-run at the fix commit with ISAC_OBS_PATH + ISAC_METRICS_PATH:
  score_fix.txt / check_obs_fix.txt: gate PASS, crc 95.94 %, drop 1.17 %; checks as above (see check_obs_fix.txt).
  unit tests 8/8 x3; full ctest: only test_thread-pool and nr_cuup_functional_test fail (known env).
  (obs_written trails obs_pushed by 2 at the snapshot: written now advances after the flush, by design.)
