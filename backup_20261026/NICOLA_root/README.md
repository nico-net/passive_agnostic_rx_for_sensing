# sens6 — OTA passive ISAC receiver (X410)

**This machine is the OTA rig.** It is the only host with the USRP X410 on a 10 GbE link.
The rfsim/simulation work happens on the other host; do not run simulation batches here.

Working tree for OTA: **`~/NICOLA/openairinterface5g-total-passive-ue/`** (branch
`total-passive-ue`). The `openairinterface5g/` tree here is on `develop` and is *not* what
the OTA captures were built from.

Restore points (both created 2026-08-03, nothing else touched):
- `c9f4fd4361` — working OTA passive-rx state, in the `total-passive-ue` worktree
- `4f2a7a14f5` — the `develop` tree snapshot

---

## 0. Hardware / link — read once

The X410 has two addresses. **Use the data port.**

| address | port | use |
|---|---|---|
| `192.168.10.2` | `sfp0`, 10 GbE, MTU 9000, cabled to `enp179s0f0np0` | **sample data — always use this** |
| `128.178.122.20` | RJ45 management, 1 GbE | management only |

The 1 GbE path cannot carry even 20 MHz (983 Mbit/s needed at `-r 51`, 3.93 Gbit/s at
`-r 273`). Any sync/timing result taken over it is invalid.

Sanity-check the link before a session:

```bash
uhd_usrp_probe --args addr=192.168.10.2
benchmark_rate --args addr=192.168.10.2 --rx_rate 122.88e6 --duration 10
# expect: 0 dropped / 0 overruns / 0 timeouts
```

**One RX overflow permanently stalls the stream** and OAI never restarts it — it looks like
a hang *after* a successful sync. If that happens, restart the process; don't debug it as a
protocol bug.

---

## 1. Build

Two traps here, both build-side only:

1. A source-built **UHD 4.8 in `/usr/local` shadows the distro `libuhd 4.10`**, and 4.8
   aborts on the X410 with `MPM major compat number mismatch`. `UHD_DIR` alone is **not**
   enough — `UHD_LIBRARIES` is a cached FILEPATH and stays at 4.8.
2. Building *all* targets fails at `nr_psbchsim` (physim targets link `libPHY_NR_UE.a` but
   not `NR_UE_ISAC`, so ISAC hook symbols are undefined). Pre-existing, unrelated to the X410.
   **Build the two targets explicitly.**

```bash
cd ~/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build

cmake . -DUHD_LIBRARIES=/usr/lib/x86_64-linux-gnu/libuhd.so \
        -DUHD_INCLUDE_DIRS=/usr/include

make -j8 nr-uesoftmodem oai_usrpdevif
```

> **Never build while a capture is running.** 8 compile jobs starve the radio thread; the
> capture silently becomes a dead-cell measurement. This has already produced one wrong
> conclusion in this project.

---

## 2. Run the passive receiver

`--passive-rx` never transmits and never attaches. It listens to whatever the cell is doing
for other UEs, so **the cell must have real traffic** — an idle cell gives you nothing to
sense with.

```bash
cd ~/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build

sudo ./nr-uesoftmodem \
  -r 273 --numerology 1 --band 78 \
  -C 3414990000 --ssb 165 \
  --ue-rxgain 85 --ue-fo-compensation \
  --passive-rx \
  --usrp-args "type=x4xx,addr=192.168.10.2" \
  --uecap_file ~/NICOLA/openairinterface5g-total-passive-ue/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml \
  -O ~/NICOLA/nrue.ota.sensing.conf \
  2>&1 | tee /tmp/ota_rx.log
```

**This exact line is verified** — taken from the `CMDLINE:` of the 2026-08-02 100 MHz capture
that produced `/tmp/ota_sensing/ota_reports.jsonl` (1551 CPIs). Note `type=x4xx` in
`--usrp-args`, and that the conf is **`nrue.ota.sensing.conf`** (not `nrue.ota.100mhz.conf`).

Add `--ue-nb-ant-rx 4` for AoA. The verified run above was **single-antenna, no bearings**.

**Always pin `--ssb`. Never use `--ue-scan-carrier` here.** A GSCN scan at 273 PRB starves
the RX thread, causes an overflow, and permanently stalls the stream (§0). It also segfaults
at `--ue-nb-ant-rx 4` (≈1.6 GB of simultaneous scan buffers, unchecked `malloc16`).

`--ssb` is `ssb_start_subcarrier`, **not an ARFCN**, and it is bandwidth- and
placement-dependent — carrying a value across a bandwidth change is what breaks sync.
Recorded values for this cell:

| bandwidth | `-r` | `-C` | `--ssb` |
|---|---|---|---|
| 100 MHz | 273 | 3414990000 | 165 |
| 20 MHz | 51 | 3374400000 | 186 |

Derive it if the cell moves: `(f_SSB − f_pointA)/SCS − 120`, then confirm against the UE's
own `"GSCN: ..., with SSB offset: N"` line.

Drop `--ue-nb-ant-rx 4` (or set 1) for a single-antenna run with no AoA.

### Check it is actually working

```bash
grep -c "Initial sync successful" /tmp/ota_rx.log     # want 1, not a re-acquire loop
grep -oE "occ\[[^]]*\]" /tmp/ota_rx.log | tail -3     # CFR sources per CPI
grep "blind PDCCH monitor summary" /tmp/ota_rx.log | tail -1
grep -c "uplink\|RA_" /tmp/ota_rx.log                 # MUST be 0 — it must never transmit
```

- `occ[csi=.. dmrs=.. data=.. blind=..]` — `dmrs=0` is *correct* on a passive run.
- In the blind-PDCCH summary, `cfr_submits` tracking `accepts` ≈ 1:1 is healthy;
  `cfr_submits=0` with nonzero `accepts` means every accept was a false CRC.

---

## 3. Config that must match the live cell

In `~/NICOLA/nrue.ota.sensing.conf` (the conf the verified run uses), `[sensing]`:

- `sources` — `"csi_rs"`, and add `"pdsch_dmrs_blind,pdsch_data"` once blind PDCCH is
  verified on the cell. Blind PDCCH is what raises the slow-time rate (13.2 ms → 3.8-4.8 ms
  spacing, i.e. `vel_max` ±6.6 → ±18-23 m/s).
- `csirs_monitor` — 11 colon-separated fields per resource, and **the values must match the
  DU log exactly**. A wrong `scramb_id`/`freq_domain` means Ĥ = Y/X uses the wrong X, which
  gives *garbage*, not a weak signal. Working line for this cell is in the conf; re-derive if
  the cell's band/BW/SCS/PCI changes.
- **Check the DU's `dci_aggregation_level` distribution before trusting blind PDCCH on a new
  deployment.** This cell sends 99.997 % at AL1; a scanner set to AL2 only produces a silent
  100 % false-accept stream, not an error.

---

## 4. Analyse the results

All scorers live in `tests/passive_rx/`. They read the receiver's own DetectionReport JSONL
and the `SENSING_CHANNEL gt:` lines (simulation ground truth — **OTA has no ground truth, so
the coverage/precision scorers below only apply to rfsim captures**; on OTA use them only for
axis/geometry diagnostics, §4.3).

### 4.1 Detection coverage and precision (needs ground truth)

```bash
cd ~/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx
python3 gt_score.py <run_dir>
```

Reports `resolvable %`, `coverage %` (fraction of CPIs detecting the target),
`in-band precision %`, `dets/CPI`.

**Always pair precision with its chance level** — a precision figure below chance is
meaningless, and one has already been found in this project:

```bash
python3 chance_level.py <run_dir>
```

### 4.2 Track-level

```bash
python3 score_passive_tracks.py <tracks.jsonl>          # confirmed-status only
python3 ../sensing_sim/score_emitted_tracks.py <...>    # confirmed + coasting
```

Report **both**. Coasting updates were measured at 0-4 % correct while often outnumbering
confirmed ones, so the confirmed-only figure misrepresents what a consumer receives.

### 4.3 Axis / geometry diagnostics (useful on OTA too)

```bash
python3 e0_axis.py <run_dir>    # is the target inside the zero-Doppler notch / aliased?
python3 d1_probe.py <run_dir>   # is target energy in the raster but undetected?
```

`e0_axis.py` prints, per CPI, where a given range-rate lands on the delivered Doppler axis in
**bins**, and splits CPIs into aliased / notched / resolvable. If the target sits below the
`zero_doppler_guard` (default 3 bins) it is undetectable by construction and no detector
tuning will recover it.

`d1_probe.py` compares the target's range-Doppler box against a control box at the same
ranges but an impossible velocity — separating "energy absent" (integration/clutter problem)
from "energy present but rejected" (detector problem).

### 4.4 Quick look at any capture, no tooling

```bash
python3 - <<'EOF'
import json, statistics as st
rs=[json.loads(l) for l in open('/tmp/ota_sensing_reports.jsonl') if l.strip()]
print('CPIs', len(rs))
print('vel_res  med %.3f m/s' % st.median([r['vel_res_mps'] for r in rs]))
print('vel_max  med %.1f m/s' % st.median([r['vel_max_mps'] for r in rs]))
print('N_dopp   med %.0f'     % st.median([2*r['vel_max_mps']/r['vel_res_mps'] for r in rs]))
print('dets     tot %d'       % sum(len(r.get('detections',[])) for r in rs))
EOF
```

> **Never quote the run summary's last-CPI geometry.** It prints the *last* CPI, which is
> routinely a far-tail outlier — this has caused at least two wrong conclusions. Always
> compute medians over all CPIs from the JSONL, as above.

---

## 5. Things already known — don't rediscover them

- **PBCH tracking fails on a loaded cell.** Traffic-gated and reproducible; it is *not*
  bandwidth-, X410-, gain- or SNR-related (all eliminated by measurement). Worked around for
  passive mode by not setting `RLM_out_of_sync` on a failed MIB re-decode. The underlying
  estimator defect is still open.
- **Data-aided PDSCH is blocked by RANK on this cell**, not by config: the cell runs
  `nof_antennas_dl: 4` and the commercial UE takes rank 4, while the passive decoder is
  single-layer only. Options: set `nof_antennas_dl: 1` on the DU, or stay on the blind DM-RS
  path — which already delivers the velocity axis.
- **rfsim CRC rates are an upper bound, not an OTA result.** rfsimulator hands every client
  identical downlink samples, so a passive receiver's channel equals the granted UE's by
  construction. Re-measure everything OTA.
- A **~40 m systematic range bias** is open and forces a 40 m scoring tolerance. Do not make
  absolute range-accuracy claims until it is fixed. Perturbing the channel cannot measure it
  (the UE's timing loop absorbs the change) — instrument the receive chain instead.

---

## 6. Measured state

### OTA, 100 MHz, 2026-08-02 (`/tmp/ota_sensing/ota_reports.jsonl`, 1551 CPIs)

| | measured |
|---|---|
| `range_res` | 3.05 m (confirms 273 PRB / 100 MHz) |
| `vel_res` | 0.104 m/s |
| **`vel_max`** | **±6.6 m/s** |
| `N_dopp` | 128, fixed every CPI |
| **dwell** | **median 848 ms** (p10 293 ms, p90 10.2 s) |
| detections | 19714 total, median 2.0/CPI, only 385 CPIs empty |

**This configuration can only see very slow targets, and that is a tuning choice worth
revisiting before the next campaign.** Two independent limits, both from the numbers above:

- **Doppler ambiguity**: `vel_max` ±6.6 m/s = ±24 km/h. Anything faster aliases. A car does
  not appear at its true velocity.
- **Range migration**: at an 848 ms dwell a target crosses one 3.05 m range bin at just
  **3.6 m/s**. Faster than that and its energy smears across range bins and is lost to
  coherent integration — a walking person (~1.5 m/s) is fine, a vehicle is not.

So the current OTA setup is effectively a pedestrian sensor. Both limits have the same cause
(`sources = "csi_rs"` alone gives a 13.2 ms slow-time spacing) and the same fix: enabling the
blind-PDCCH source raises the row rate to 3.8-4.8 ms spacing, which takes `vel_max` to
**±18-23 m/s** and lets the dwell come down into a range where vehicles survive integration.

### rfsim, other host, 2026-08-03

Single passive receiver: coverage 18-26 %, confirmed track precision 0-17 %, 2-receiver
fusion 0 tracks — coverage-starved, not a fusion defect (joint coverage is the *product* of
per-receiver coverages).

Note the two rigs sit at **opposite ends of the same dwell curve**: rfsim runs at 50-70 ms
(too short — measured optimum is 100-200 ms), OTA at 848 ms (too long for anything but a
pedestrian). Dwell is the parameter to get right on both.

Full record: `AGENT_HANDOFF.md` and `RESULTS_2026-08-03.md` on the other host.

---

# CFO ROOT CAUSE + FIX (2026-08-26) — READ THIS BEFORE TOUCHING SYNC OR PDSCH CRC

Branch `total-passive-rx-UL-DL` (was `passive-rx-only` at session start -- it changed underneath), tree `~/NICOLA/openairinterface5g-total-passive-ue/`.
**Committed: 1bd2581ac4, 2288170c07, bf6612c63b.** Builds clean. Full detail: `BRANCH_IMBALANCE_HARQ_PLAN.md` §11-14.

## 1. What was wrong (PROVEN, not a hypothesis)

**The 0 %/91 % passive PDSCH CRC bimodality is a mis-estimated carrier frequency offset.** It is
estimated ONCE at acquisition, sometimes lands wrong, and nothing ever revisits it.

| evidence | result |
|---|---|
| separation, 21 runs, zero exceptions | \|err\| < 600 Hz -> always decodes (14/14, 59-91 %); >= 600 Hz -> always EXACTLY 0.0 % (7/7). Fisher p = 8.6e-06 |
| positive control | remove `--ue-fo-compensation` (~13.7 kHz uncorrected) -> 0.0 % on **3/3** |
| natural reproduction | untouched baseline runs off by 2623 / 4150 / 4935 Hz -> 0.0 % |

**Why five sessions of probes missed it:** the DM-RS channel estimate is taken on the SAME rotated
symbols and absorbs the phase, so chest SNR reads healthy (22.8 dB on a dead run). The damage is ICI
across the 13 data symbols -- payload destroyed, pilots intact. Binary because the estimate either
lands or does not. 0 of 461807 segments converge, which is what ICI does and a weak link does not.

**The rig's true offset is -14390 Hz** (the trim loop's own fixed point). NOT -13872, which was the
median of scattered acquisition estimates. Score every error against -14390.

## 2. What is in the tree

| knob | default | status |
|---|---|---|
| `ISAC_RF_STALL_MAX_REINIT` | **4 (ON)** | VALIDATED. 4/4 recovered runs kept decoding; dwell 8 s -> 50-66 s |
| `ISAC_CFO_TRACK_HZ` | 0 (off) | the trim loop. Set 300 |
| `ISAC_CFO_TRACK_APPLY` | off | required before it retunes |
| adaptive AL ladder | on | learns the cell's distribution; 37 -> 26 candidates |
| `pdcch_blind_monitor_scan_thread` | 0 (off) | deferred scan, never re-measured after its serial-candidate fix |

Best measured config = **`ISAC_CFO_TRACK_HZ=300 ISAC_CFO_TRACK_APPLY=1`** (the "F2" behaviour, which
the tree is currently reverted to):

| | baseline | F2 |
|---|---|---|
| runs at exactly 0.0 % | ~40-50 % | **1 of 8** |
| retuned runs surviving | 0 of 3 | **4 of 5** |
| mean health | 49.3 % | **67.6 %** |

**"Always > 80 %" is NOT achieved — 4 of 8.**

## 3. The one thing that matters if you continue: DELIVERY, not the correction

The estimator is EXACT (measured +6055 vs a true +6055 Hz; -5455 vs -5453) and the gate works (zero
false applies in 6 runs). **Every failure has been in delivering the correction to a running
receiver.** Three attempts:

1. retune in place + `is_synchronized = 0` -> **3/3 runs died at active_s=4**. `pbch_process()` cannot
   reach `stream_status`/`shiftForNextFrame`/the rebase flag.
2. retune + **`nrue_ru_reinit()`** -> **WORKS** (this is F2). It is the only teardown this X410 is
   proven to survive -- stall recovery uses it, 4/4.
3. + seeding `initial_fo` so the re-acquisition accumulates onto the correction -> **INERT**. No
   `CFOTRK seeding` line appeared in any of 8 runs: the branch guarded at `nr-ue.c:553` is not on the
   path the re-init resync takes. Reverted.
   + a re-init cap of 2 -> **HARMFUL**, zeros 1/8 -> 3/8. Reverted.

**STOP PATCHING AND INSTRUMENT.** All three failures were plumbing -- which thread owns the state,
which assignment clobbers the seed, which branch actually executes -- inferred from source reading
and wrong each time. Log every write to `common_vars.freq_offset` and every read of `initial_fo`
across a real re-init before changing anything else.

Known remaining defect: after a correction the re-acquisition measures only the RESIDUAL (-83/-39/
+37 Hz) and `UE_synch` applies THAT as the total via its unconditional `nrue_ru_set_freq()`,
discarding the correction. Costs little when the correction is small; F2_r7 lost a 5.2 kHz
correction this way and decoded 0.0 %.

## 4. DO NOT RE-INVESTIGATE (each killed by measurement)

- `--cont-fo-comp` -- enables the PI loop but DISABLES the acquisition radio retune; **0/2 runs
  synced vs 3/3 without it**. CLAUDE.md's Phase 0 note already said neither FO flag tracks.
- **Acquisition consistency check** (sync twice, compare) -- passed **3/3** bad locks. The mis-lock is
  SYSTEMATIC; both attempts make the same mistake. Removed.
- **A new residual estimator** -- `nr_ue_pbch_freq_offset()` already exists and is correct.
- **RT-core pinning** (`ISAC_UE_RT_CORE`) -- unpinned 3/5 runs >84 %, pinned **0/5**. Cores 2-3 are
  `nohz_full`, which penalises a syscall-heavy thread.
- **Timing/STO** -- `sync_pos_frame = 8832` in ALL 56 runs.
- **AGC/RSRP** -- healthy 47.1 dB vs dead 47.8 dB.
- **`max_pos_acc` as a CRC predictor** -- a co-symptom; runs at 612-653 decode fine at low load. It
  maps to the X410 STALL, not to CRC.

## 5. Open, and NOT the CFO defect

- **The 30-60 % class.** Measured as TWO shapes: degrading over the run (44%->0%, 72%->18%) and
  steady-intermediate (50%->54%, 59%->57%). NOT explained by CFO below ~1600 Hz -- below that the
  relationship is noise (33 Hz -> 69 %, but 820 Hz -> 91 %). Suspects: branch imbalance (branches 1
  and 3 near 0 dB SNR) and load-dependent RT starvation.
- **Grant-rate ceiling.** Reliable at ~160 grants/s, bimodal at ~1500. CFO is part of it; whether
  anything else contributes is unmeasured.
- **~17 dB SNR gap to mcs 25** -- hardware. **Cheapest decisive step: the ch0<->ch3 cable swap**
  (physical, 5 min) -- says whether branch 3's ~13 dB deficit is antenna-side or the X410 channel.
  Feeds both this and the 30-60 % class.

## 6. Method notes that cost real runs

- **>= 5 runs per arm, alternated.** This rig swings 0 %<->88 % between adjacent runs. Three
  conclusions were overturned by the next run today.
- **Never build during a capture.** Check `pgrep -x nr-uesoftmodem` first.
- **`pkill -f` matches your own SSH command line** and kills the session (and the batch). Kill by PID.
- **Bracket and REPORT the offered grant rate per run.** The cell went idle mid-batch twice today;
  a `rate=0/s` run scores 0 % and looks like a receiver failure.
- **After a correction, a fresh acquisition reading NEAR ZERO is the success signal**, not a failure.
  Do not score it against the pre-correction absolute.
