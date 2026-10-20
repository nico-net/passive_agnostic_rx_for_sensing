# Dashboard always on -- 2026-10-20 (csd/int16-stage2, phase 1 of 2)

Evidence labels: [CODE-READ], [BUILD] DGX aarch64 build_t26, [CTEST] DGX, [LOGCHECK] real stage2 OTA logs ab_s2TM1 (TIM) and ab_s2VM1 (Vodafone), parsed offline, [MEASURED] DGX idle host, synthetic log feed. No OTA, no hardware.

## What it does
Every `nr-uesoftmodem --passive-rx` start launches the dashboard (tests/passive_rx/monitor/monitor.py, Python 3 stdlib, read-only on the run files). No flag. Log line at start:
`SENSING: DASHBOARD http://127.0.0.1:8080 (run <dir>)`.

Laptop: `ssh -L 8080:127.0.0.1:8080 spark-74c3`, then http://127.0.0.1:8080 . It binds 127.0.0.1 only.

## How it starts (common/utils/nr_dashboard.c, called in main() once logging is up)
- Run dir = directory of stdout (/proc/self/fd/1 -> run.log); `--log` is that file; `--metrics` is ISAC_METRICS_PATH when set. If stdout is not a regular file: `--follow ~/NICOLA/ota_runs/*` (newest dir).
- fork + second fork (no zombie, receiver never waits), setsid, all fds above 2 closed (no GPU/radio fd leaks), stdin/out/err to /dev/null then to `<run dir>/dashboard.log` (/tmp/dashboard.log in follow mode).
- Privileges: running as root, it setgroups/setgid/setuid to SUDO_UID/SUDO_GID (fallback: owner of the run dir; if that is root too, no dashboard) and verifies root cannot be regained, else exits without exec. Never python as root.
- CPU: sched_setaffinity to core 2 (not assigned anywhere in code defaults; spare in the site map), SCHED_IDLE (not RT) plus nice 19, RLIMIT_AS 4 GiB.
- Single instance: monitor.py `--single` holds flock on /tmp/rxdash-<uid>-<port>.lock containing "pid run". Same run already served: the new process exits (reuse). Different run: SIGTERM the old one, take over (a new receiver start replaces the old dashboard).
- Lifetime: the dashboard is an independent session leader; it stays up after the receiver exits (finished run stays inspectable) until the next receiver start replaces it. Chosen over a systemd service: no host change, no install step, works on every host the binary runs on. A user systemd unit is not prepared or installed (not needed).
- Failure isolation: all of it is fork/exec of a separate process; any failure (no python, no monitor.py, bind error, crash) logs one `SENSING: DASHBOARD not started: ...` line (or only dashboard.log) and the receiver goes on. Fork happens in main() before the thread pool starts.

## Cost [MEASURED]
Live tail of a real TIM stage2 log fed at 370 KB/s (real run was about 210 KB/s): 72 ticks in 60 s = 1.2 % of one core (core 2, SCHED_IDLE), RSS 25 MB (HWM 24.9 MB); history deques are bounded. Poll period 1 s (monitor default). First start on an existing large log pays one catch-up parse (111 MB log, about 40 s, same core).

## Parser changes against the cleaned build
- [LOGCHECK] 48 regexes against both logs. All producers still exist in the source ([CODE-READ] by grep). Zero-hit regexes (ACQ MISMATCH/HYPOTHESIS, RETUNE, bank REMOVED, ULLAYOUT_CRC_LOCK/PRIOR/STATS/reset on some runs, Technique D CONVERGED, CMDLINE, RFSTALL) are event-driven lines absent from these healthy runs, not removed ones. ULLAYOUT PRIOR_* has no producer in the current source (dead pattern, harmless).
- EVMGRANT: the existing regexes already match the new line (dmrs_snr, skip, gp, li, es, e2e, stg appended). New: `e2e=` total (slot arrival to CRC) is parsed into the DL aggregate (`e2e_us` p50/p90/p99 in the snapshot; not yet drawn in the UI). Test added with a real line.
- Hints naming removed flags (ISAC_PDCCH_TIMING, ISAC_PDTIM_CLASS, ISAC_OBS_PATH, ISAC_UL_LAYOUT_LOCK, "set ISAC_USRP_CHPOW_S") reworded.
- isac_flags.json regenerated with gen_flags.py: 67 ISAC_* names that production code reads (test-only sources excluded); the old 600+ entry table is gone. Names set by main() via setenv (ISAC_LOG_ASYNC, ISAC_RECONF) are not literal getenv strings and are not listed.
- Targets updated 2026-10-20 to the goals: decode p50 < 0.5 ms and p99 < 3 ms, DL CRC-OK amber below 80 %, scan-queue full drops < 1 %.

## Tests
- tests/passive_rx/monitor: test_monitor + test_health, 47 tests OK.
- ctest nr_dashboard_spawn (common/utils/tests/test_nr_dashboard.py + nr_dashboard_test.c fake receiver, fake run dirs, random port): non-root uid, pinned core, nice 19, SCHED_IDLE, own session, serving HTTP 200, second start with the same run leaves exactly one instance, start with another run replaces it. Passed.
- [BUILD] `ninja -j6 -k 0 nr-uesoftmodem oai_usrpdevif tests params_libconfig rfsimulator`: only the 3 known link failures (nr_pusch_ra0sim, test_nr_ssb_rate_match_prod, test_nr_csi_rb_extent). [CTEST] -j4 without nr_td_sim_test: 13 failures, all known (ra0 x9, ssb_rate_match_prod, csi_rb_extent, dft_test, test_nr_modulation).
- Not verified: the root to SUDO_UID drop in the real binary under sudo (the test harness ran unprivileged; code path is the same, root branch untested), and a real OTA start.
