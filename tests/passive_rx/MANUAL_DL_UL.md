# Manual X410 receiver with the sensing pipeline

Build the target repository, not the acquisition experiment:

```sh
cmake --build cmake_targets/ran_build/build --target nr-uesoftmodem oai_usrpdevif -j4
sudo -n env DURATION=120 bash tests/passive_rx/run_manual_x410.sh
```

The launcher refuses an occupied radio or dashboard port. It never changes the NIC,
resets the X410, starts a transmitter, or reads gNB configuration. The existing
`run_adaptive_receive_test.sh` and detector source are unchanged.

This is explicitly a deployment-specific manual profile: carrier, bandwidth,
numerology, SSB, CORESET and UL layout are supplied. CFO/drift CLI seeds are the
existing manual values, not newly measured acquisition results. Automatic carrier
acquisition and layout sweeps are off. DL enables DCI 1_0 and an explicitly
reconciled 47-bit DCI 1_1 layout: no BWP-indicator bit and one TDA-index bit.
The two TDA entries (S=1, L=13/7), additional DMRS position 2, and MCS table 1
come from the prior passive CRC-backed capture `auto_ul_evidence_retry.G8BYYc`,
configuration `e65ce1703ffffe66`, not gNB configuration. They are a manual
waveform interpretation, not a claim that every dedicated RRC field is known.
Uplink retains the existing manual 0_1 layout. CSI-RS is not invented without its
resource configuration. The detector stays enabled; this test does not validate
array calibration, target geometry, or range/Doppler scoring.

Outputs include copied configuration, binary checksums, NIC counter values,
receiver logs, raw sensing reports, stop reason and a separate cleanup status.
The watchdog compares values read from sysfs, not the apparent sysfs file size.
RF faults, incomplete captures and forced shutdowns must not be used to claim CRC
performance. `RF_VALID_REQUIRES_DL_UL_CRC_EVIDENCE` is not a decoding pass verdict.
Dashboard percentages use completed transport blocks; queue drops stay separate.

The dashboard remains available after the bounded capture and marks old data stale.
For Windows through the existing VPN:

```powershell
ssh -N -L 8083:127.0.0.1:8083 sens@128.178.122.140
```

Open `http://localhost:8083/`. Set `MONITOR_PORT` when another dashboard uses 8083.

## Multi-branch, gain trim, and tracker options

Unset, every one of these leaves the rendered `receiver.conf` byte-identical to the
legacy single-branch conf — they are all opt-in overrides on the RENDERED COPY only,
never on the tracked `adaptive_manual_dlul.conf`.

```sh
# 4 independent passive RX branches (X410's 4 channels), each with its own
# SensingEngine, own per-branch CRC, own reports_bN.jsonl:
sudo -n env RX_BRANCHES=0,1,2,3 DISABLE_HIERARCHICAL_TRACKER=1 DURATION=120 \
  MONITOR_PORT=8083 bash tests/passive_rx/run_manual_x410.sh

# Same, with a per-channel RX gain trim (dB added to --ue-rxgain per branch,
# comma list br0,br1,br2,br3):
sudo -n env RX_BRANCHES=0,1,2,3 DISABLE_HIERARCHICAL_TRACKER=1 \
  RX_GAIN_TRIM=16,19,0,0 DURATION=120 MONITOR_PORT=8083 \
  bash tests/passive_rx/run_manual_x410.sh
```

- `RX_BRANCHES` (comma list of `0`-`3`): activates that subset of the X410's 4
  channels as independent branches, each with its own `SensingEngine` instance.
  Requires `--ue-nb-ant-rx 4` (already hardcoded in the launcher's invocation).
- `DISABLE_HIERARCHICAL_TRACKER=1`: turns off only the optional global/multi-static
  ENU tracker. Needed with this conf because `adaptive_manual_dlul.conf` carries no
  surveyed `rx_pos_*`/`tx_pos_*` (both default `(0,0,0)`), and
  `SensingEngine`'s constructor throws `"hierarchical ENU tracking requires surveyed
  noncoincident Tx/Rx"` for every branch, unconditionally, if this is left on — the
  sensing engine then never reaches `enabled.store(true)` and no `reports_bN.jsonl`
  is ever produced, live or replay (`nr_isac_init()` runs before the replay/live
  branch in `nr-uesoftmodem.c`, so it affects both identically). This does not
  invent a position; it disables the one feature that needs a surveyed one, while
  leaving per-branch local detection/tracking on. Live-verified 2026-09-13:
  `engines=4` built, `reports_b0..b3.jsonl` produced with real per-branch content.
- `RX_GAIN_TRIM=d0,d1,d2,d3`: per-channel dB added to `--ue-rxgain`, clamped to the
  channel's UHD gain range and confirmed via readback in the log (`RX gain trim =
  [...] dB`, `Actual RX gain: ...`). **Mechanically verified working, but its
  causal effect on a weak branch's CRC is NOT established** — per-branch CRC on
  this rig swings by 60-80 percentage points between adjacent runs of the SAME
  config (see `adaptive_RX_pipeline_progress.md`, session 2026-09-13), dominated by
  a per-run CFO/sync-lock effect, not a stable physical gain imbalance. Don't trust
  a 1-run-per-arm comparison; this rig's own standard is ≥5 runs per arm.

## Debugging common X410 problems

All of the following were actually hit live on 2026-09-13; none are hypothetical.

**Radio busy / "BLOCKED: radio lock held" or "another modem or radio utility is
running"**: something else is using the X410 — check before assuming a bug:
```sh
pgrep -a -x nr-uesoftmodem
timeout 8 uhd_find_devices --args="type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174,serial=327C1F2" 2>&1 | grep -i claim
```
Never `pkill`/reset another session's process. Wait for `pgrep` to go empty and
`claimed: False`. **If `uhd_find_devices` reports "No UHD Devices Found" (not
"claimed") right after another session releases it, that's the known ~180s MPM
settle gap, not a fault** — poll every ~15s rather than failing immediately.
Note: another concurrent test loop on this host (`adaptive-rx-UL-DL` worktree,
captures named `ab2_*`) has been observed to `kill -9` any `nr-uesoftmodem` PID it
finds before starting its own next arm, with no ownership check. An unexplained
`exit=137`/`Killed` on an otherwise-healthy run may be that, not a new bug — check
`sudo journalctl --since "<window>" | grep "kill -9"` for a `PWD=.../ab2_*` line
naming your PID before assuming a real failure.

**`verdict=VOID_RF_OR_ASSERT`, `RFSTALL ... USRP_RX_START ... ERROR_CODE_OVERFLOW`**:
an RX overflow right at stream start, before any data flows. Intermittent, not
persistent — just retry (radio-availability check above, then relaunch). If the
same RFSTALL happens mid-run instead (`grep RFSTALL "$OUT/run.log"` finds it after
real PDSCHQ-BRANCH lines already exist), any per-branch CRC/decode data from AFTER
that point is corrupted (IQ was invalid) and must be discarded — data from BEFORE
the RFSTALL is still valid.

**`verdict=VOID_SHUTDOWN_TIMEOUT`, `cleanup=FORCED_KILL`**: the receiver didn't
shut down within the launcher's ~108s budget after SIGINT. Root-caused
2026-09-13 via a live gdb backtrace (not a deadlock): with multiple branches, the
main thread's `SensingEngine::stop()` for one engine can be waiting on that
engine's consumer thread, which is itself waiting on a worker thread that is
*actively executing* a slow, unaccelerated (`cuda_backend=0x0`) `detect_clean()` /
`refine()` detection pass. The whole process runs `taskset -c 0-7` — 8 logical
cores shared between real-time PHY threads and up to 8 SensingEngine worker
threads across 4 engines — so under backlog (e.g. after a mid-run RFSTALL) this can
legitimately exceed even a generous shutdown budget. This is real CPU contention,
not a hang to "fix" by killing harder. If it recurs, read
`$OUT/shutdown_stall_backtrace.txt` (captured automatically at the old ~18s mark,
before the extended wait) — it has the full thread-by-thread state, including
which engine and which stage (`accumulation_run`/`processing_run`/`detect_clean`)
each thread was in. **A `VOID_SHUTDOWN_TIMEOUT` run's decode/CRC data is still
valid** as long as no RFSTALL occurred during the run — only the shutdown itself
was slow, not the capture.

**Dashboard port already in use / `OSError: [Errno 98] Address already in use`**:
a previous run's dashboard is still bound (by design — it stays up after its
capture ends so results remain viewable). Check `ss -tlnp | grep 127.0.0.1:8`,
pick an unused `MONITOR_PORT`.
