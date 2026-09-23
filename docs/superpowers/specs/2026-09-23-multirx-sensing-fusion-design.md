# Multi-RX sensing fused into the agnostic passive receiver — design

Date: 2026-09-23. Branch: `feature/multirx-clean-adaptive` (sens6 worktree
`/home/sens/NICOLA/multirx-clean-adaptive`), created from `mygitlab/feature/multirx-clean-detector`
@ `c5b28b1de3`. Merge source: sens6 `adaptive-rx-UL-DL` @ `51f7d3deac` (HEAD `94dd382c6c` + one WIP
commit holding the 41 previously-uncommitted files).

## 1. Goal

Replace the passive receiver's current sensing part (detector + tracker) with the frozen multi-RX
chain from `feature/multirx-clean-detector`, fed by the adaptive receiver's dedicated DL and UL
decodes, on all four X410 channels, with a live 3D UI, per-block debug output, and one launch
command. Build it and validate it over the air.

## 2. Hard constraints

1. **Fully passive.** The receiver never transmits. Nothing in this work adds a TX path.
2. **Fully agnostic.** No value that describes the gNB or the cell is hardcoded, pinned in a conf,
   or passed by the launcher, except the three front-end start values below: no SSB position, PCI, CORESET, DCI length or
   layout, TDA, DM-RS configuration, RNTI, or CFO seed. Every such value is discovered on air.
   Operator inputs are limited to:
   - the RF front-end start values `-r 273 --numerology 1 -C 3450000000` (decided, §9),
   - RX gain,
   - number of RX channels,
   - the **site survey** file (§7), i.e. where the antennas and the gNB physically are. This is
     geometry, not a gNB configuration parameter,
   - X410 addresses, which are verified at preflight, never assumed.
   §9 lists every pin found so far and what replaces it.
3. **Non-interference.** Sensing never feeds anything back into the receive/decode path, never
   blocks it, and does not run on the RT thread. `ENABLE_ISAC_SENSING=OFF` builds exactly today's
   passive receiver.
4. **Compute.** Sensing runs CUDA-first: stages [1], [2] and [4] of the engine on the sens6 RTX
   4060 Ti. The same code runs on CPU automatically when there is no device. Campaign runs set
   `NR_ISAC_REQUIRE_CUDA=1`, so a missing GPU fails at startup instead of silently degrading.

## 3. Physical setup

- One X410 on sens6, four RX channels.
- Each channel's antenna is **cabled to its own surveyed position**, metres to tens of metres
  apart. The four channels are therefore four spatially separate receivers that share one clock
  and LO.
- This is the geometry the frozen chain was validated on: `spatial_receivers[0..3]`, stage 9's
  6-D solve needs at least 3 receivers at distinct positions.
- **A co-located layout is refused by the launcher.** With all antennas in one place the four
  bistatic ellipses coincide and stage 9 is degenerate.

## 4. Merge

A merge (not a rebase) of `adaptive-rx-UL-DL` into the new branch, so both histories survive.
Conflict policy:

| Area | Winner |
|---|---|
| `openair1/PHY/NR_UE_ISAC/**` (engine, tools, tests) | feature branch (the frozen chain) |
| `tests/passive_rx/monitor/` | feature branch, then extended per §11 |
| Receiver: `NR_UE_TRANSPORT/*` (blind PDCCH / discovery / bootstrap, PDSCH/PUSCH passive decode, queues), `MODULATION/`, `executables/`, `radio/`, capture scripts | adaptive |
| CFR producers (glue inside `NR_UE_TRANSPORT`) | adaptive code, rewritten against the feature-branch API |
| `CMakeLists.txt` | both, merged by hand |

Known reconciliation items:
- **API changes.** `nr_isac_aoa_antennas()` becomes `nr_isac_rx_channels()`. UL producers call
  `nr_isac_submit_cfr_multi_session(..., session_id = C-RNTI)`, which the feature branch makes
  mandatory for UL.
- **Per-branch routing.** The feature branch's `pipeline_types.h` / `sensing_engine` lack the
  receiver lineage's `branch_id` / `NR_ISAC_BRANCH_NONE` (P10a). Here every submission is
  antenna-major over all `nof_ant` channels, and channel index = spatial receiver index. P10a's
  per-branch identity is carried by that index and is not reintroduced. The merge must check that
  no adaptive producer still depends on it.
- **Tests.** `tests/python_parity_test.cc` includes `detector.h`, which the feature branch deleted.
  Fix the test, never restore the header.
- **Build.** A full OAI build has never run on the feature branch. Expect compile and link fixes.
  Each gets its own commit.

## 5. Architecture

```
X410 ch0..3 ──► nr-uesoftmodem --passive-rx  (adaptive receiver, unchanged behaviour)
                 │  blind PDCCH → DCI 1_1 / 0_1 → PDSCH / PUSCH decode (decode worker threads)
                 │
                 ├─ CFR producers (§6), per grant, per channel i: Ĥ_i = Y_i / X̂
                 ├─ start/stop gate (§8)
                 ▼
                nr_isac_submit_cfr_multi[_session]   ── non-blocking FIFO, drops are counted
                 ▼
                engine threads (own cores, CUDA stream), writing reports.jsonl:
                 [1] sync / direct path     sync_correction(.cu)
                 [2] families + clutter     causal_clutter_filter, family_processing_cuda
                 [3] CPI formation          sensing_engine, variable_cpi
                 [4] RD map                 clean_detector (+fft), detector_cuda(.cu | _stub)
                 [5] CFAR + CLEAN           clean_detector, adaptive_threshold, adaptive_clutter_map
                 [6] long dwells            sensing_engine (dwell ring)
                 ▼
realtime_chain.py --follow reports.jsonl   (own pinned core, one thread), writing tracks.jsonl:
                 [7] stage 8 families       extended_object_stage2.py
                 [8] UE localiser           ue_localiser_dfs.py
                 [9] stage 9 solve          extended_object_stage3_dlul.py
                 [10] tracker B (of record) stage10_state_tracker.py
                 ▼
monitor.py  (tails reports.jsonl + tracks.jsonl + receiver log) ──► browser over an SSH tunnel
```

- **Transport.** Transport is the JSONL files only; they double as the recording. The ZeroMQ
  report bus is not configured by the launcher. ZeroMQ here was only ever a local report socket,
  unrelated to the radio.
- **Trackers.** The C++ `multistatic_imm_tracker` output is recorded in the reports but not
  displayed. Tracker B is the track of record.
- **No C++ port of stages [7]–[10]** in this work: they are frozen and validated, and already
  isolated in their own process. §13 measures `realtime_chain` per-CPI time OTA. If its p99
  exceeds the CPI period, only the slow stage gets ported, as a separate piece of work.

## 6. CFR producers

One decode per grant yields one X̂. Channel i always uses its own raw `rxdataF[i]`, independent of
how the decoder combined branches. So Ĥ_i[k] = Y_i[k] / X̂[k] for i = 0..3.

| REs | X̂ | Condition |
|---|---|---|
| PDSCH / PUSCH DM-RS | generated DM-RS sequence | always, CRC-independent |
| data, TB CRC OK | re-encoded TB (existing `nr_isac_p[du]sch_data_aided_submit` chain) | CRC OK |
| data, TB CRC failed (or UL UCI-hypothesis grant) | hard decision from the decoder's own LLRs (sign → bit → the same scramble + modulate step the re-encode path uses) | RE kept only if min over its Qm bits of \|LLR\| ≥ τ_Qm |

- **τ_Qm is learned, not chosen** (corrected 2026-09-23; the earlier "median + 3.09·MAD" rule was
  wrong, because it kept only the most confident tail). On every CRC-OK grant both X̂s exist: the
  re-encoded truth and the LLR hard decision. From that grant's REs the receiver accumulates, per
  modulation order Qm, a histogram of min|LLR| split into "hard decision right" and "wrong". τ_Qm
  is the smallest bin edge at which the kept REs' error fraction is < 1 %, over the last N ≥ 1e5
  calibration REs. The 1 % is a quality target, not a cell parameter.
- **Until a Qm has calibration data**, CRC-fail grants of that Qm contribute DM-RS only.
- **A live counter** reports the measured agreement between the masked X̂ and the truth on CRC-OK
  grants, so the rule's actual error rate is always visible.
- **LLR sources:**
  - DL: `nr_pdsch_passive_last_llr()`, which is after descrambling. The hard bits are
    re-scrambled before modulation.
  - UL: `pusch_vars.llr` of the grant's context.
- **Dropped REs** are removed from the occupancy mask. They are never zero-filled or interpolated.
- **Decision-directed X̂ also covers UCI-punctured PUSCH REs.** A hard decision recovers the
  transmitted constellation point whatever it encodes. That lifts today's `uci_ack_re == 0`
  restriction for the masked path, while the re-encode path keeps it.
- **Grants with more than 1 layer** contribute DM-RS only.
- **Empty grants** (gNB PDSCH with no MAC PDU, ~30 % of grants) get no special case: they carry a real
  codeword, so a CRC-passing one yields an exact re-encoded X̂ like any other.
- **Where the gates change:**
  - DL: `nr_pdsch_passive_queue.c:~916` and `nr_pdcch_blind_monitor_rt.c:~6008` currently submit
    only on `CRC_OK`. The masked path is added beside them.
  - UL: `nr_pusch_passive_decode.c:~1298`.
- **Counters per source:** rows submitted, REs kept vs dropped by τ, CRC-OK vs masked.
- **Emission.** Per-antenna DM-RS producers already exist (`NR_ISAC_SRC_PDSCH_DMRS_BLIND` in the
  blind RT path, and PUSCH DM-RS in `nr_pusch_passive_decode.c`) and are kept. They are restricted
  to dedicated grants (§8). SSB and CSI-RS sources are not enabled by the launcher.

## 7. Site survey file (single source of geometry)

`survey.json` contains:
- frame: ENU metres, **origin at the X410** (east, north, up); every position below is relative to it,
- gNB ENU position (its antenna, relative to the X410),
- the ENU position of each of the four RX antennas (the far ends of the cables, not the X410),
- the channel → antenna mapping,
- optional `max_range_m` (scene's physical maximum bistatic range for the localiser; default = the
  report's own `range_max_m`).

`rx_array_calibration` is NOT used: the engine refuses it in spatial-receiver mode (`nr_isac.cc`
disables sensing if both are set).

The launcher generates the engine's `spatial_rx_positions` / `tx_pos_*` conf lines **and**
`realtime_chain`'s `--geometry` from this one file. Two hand-kept copies disagreeing would be a
silent-garbage failure.

The launcher refuses to start if:
- any two antennas are closer than 1 m (the co-located guard, §3),
- the file is missing,
- the channel count differs from `--ue-nb-ant-rx`.

Cable delay differences between channels: each spatial receiver's differential range is referenced
to **its own** measured direct path (engine stage [1]), so a per-channel fixed delay cancels. This
must be verified in the merged engine (§13 step 3), not assumed.

## 8. Start/stop gate

- **Flow.** A C-RNTI (`rnti_class == NR_BLIND_RNTI_CLASS_C`) that has produced at least one CFR
  from a decoded DL grant **and** at least one from a decoded UL grant within the last 2 s. DM-RS
  counts, so this is CRC-independent: "usable dedicated parts" means a grant we actually extracted
  a channel from.
- **Open** while at least one flow exists. Before that, and while closed, every CFR submission is
  dropped at the submit boundary (counted as `gate_closed`).
- **Admission.** Only grants of RNTIs that are currently flows are submitted.
- **Close** when no flow remains (the 2 s window empties).
  - A watchdog thread in `nr_isac.cc` detects the close, logs it, and asks the engine to discard
    its partial CPI through the same path spatial mode already uses for incomplete windows.
  - The discard is counted separately (`gate_discarded_rows`), so `discarded_pending_rows` keeps
    meaning "loss".
  - Tracker state is left to the chain's own retirement logic.
- **Re-opens** automatically when a flow reappears.
- **Scope.** Only dedicated grants of confirmed flows are ever submitted; CORESET#0/SIB1/RA
  traffic never is.
- **Implementation.** One small state object in the receiver, read by the producers. It is fed
  from the existing bootstrap/USS-tracker confirmation points; the exact hook is located during
  implementation. It logs `SENSING_GATE open|close reason=... rnti=...`.

## 9. Agnosticity audit — pins found and their replacement

| Where | Pin | Replacement |
|---|---|---|
| `tools/realtime_chain.py` | `RANGE_RES_M = 3.05`, `RATE_RES_MPS = 1.14` (273 PRB only) | read from each report's own axis fields |
| `tools/ue_localiser_dfs.py` | `max_range_m = 312.28`, `samples_per_bin = 4096/3276` | derive from the report's carrier (FFT size / nof_prb·12) |
| `tools/realtime_chain.py` | `--ul-sessions` default `[1, 2]` (Sionna ids) | sessions created on first sight of a new C-RNTI |
| `tools/realtime_chain.py` | `max_range_m = 624.57` default | derive from the report |
| `captures/run_arm.sh` | `CARRIER=3450000000`, `SSB=150`, `SCAN=0`, `PRB=273`, `INITFO`, `XENV` rebase ratio | not used; the new launcher passes band + scan only |
| `captures/run_arm.sh` | `MGMT`/`DATA`/`NIC` defaults are stale vs 2026-09-16 | discovered/verified at preflight |
| `captures/run_arm.sh` | `REPO` defaults to another tree | the launcher always uses this worktree's binary and checks it |
| `captures/agnostic_ota.conf` | `pdcch_blind_monitor_pdsch = "2:1:0:1:16:2:64:6"` and other positional strings | each field audited; any cell-describing value removed or derived |

**Decided 2026-09-23 (user):** the RF front-end start values are fixed operator inputs:
`-r 273 --numerology 1 -C 3450000000`. They configure the SDR front end and are the ONLY
cell-related values the launcher passes. The SSB position is NOT pinned: it is found by
`--ue-scan-carrier`. Everything above the front end (PCI, CORESETs, DCI layouts, TDA, DM-RS,
RNTIs, CFO) stays discovered on air.

## 10. Per-channel CFO / SFO / STO

- **Per-channel CFO (continuous).** This is the existing adaptive BRANCHFO loop, not new code:
  - estimator: `nr_pdsch_passive_decode.c:~2155`, the per-branch DM-RS phase slope differential
    to branch 0, as an integrating loop,
  - correction: applied in the FEP via `nr_ue_set_branch_fo_hz()` → `slot_fep_nr.c:268`, so it
    covers UL samples too,
  - together with `usrp_set_rx_freq_all()`, which retunes all channels, not channel 0 only.
- **Changes needed:**
  - Default ON when NANT > 1 (today opt-in `ISAC_RX_BRANCH_FO=1`).
  - Fix the shared accumulator `s_fo_corr[]`, which is updated from several decode threads
    without synchronisation.
- **Common STO/SFO and per-row LOS phase** are handled per CPI by engine stage [1], after the
  per-chain calibration.
- **Status: never validated on air.** Pass criterion (§13): `BRANCHFO d_vs_br0` converges to
  |·| < 20 Hz on channels 1–3.

## 11. Debug, UI

**Per-block debug.** One switch, `NR_ISAC_DEBUG_DIR=<dir>`, generalises the existing
`NR_ISAC_CLUTTER_DUMP_DIR`. It writes, per CPI:

| Block | Output |
|---|---|
| input | `cfr_rows.bin`: every admitted submission (slot, frac, source, session, carrier, per-antenna Ĥ, k/l masks, noise var) |
| [1] | STO / SFO / LOS phase per row per receiver, plus BRANCHFO per channel |
| [2] | CFR before/after clutter (existing `dump_window`), family stats |
| [3] | CPI plan: rows, span, closure reason, loss counters |
| [4]/[5] | likelihood map per view (DL, UL per RNTI), CLEAN components, CFAR decisions |
| [6] | dwell components |
| gate | open/close events |
| [7]–[10] | `realtime_chain.py --debug-dir`: per-CPI JSONL of families, localiser, stage-9 fits, tracker B states |

Two debug tools:
- `isac_replay` (small new executable) feeds `cfr_rows.bin` back through the same engine, so any
  OTA run replays offline and deterministically without a radio.
- `tools/show_block.py` plots any block's dump for one CPI.

**Monitor** (extends `tests/passive_rx/monitor`):
- **Tracking:** 3D Plotly `scatter3d` in ENU with free rotate, zoom and pan. Shows the gNB, the 4
  antennas, localised UEs, and tracker B tracks with trails and height. Updates on every new
  `tracks.jsonl` line. Plotly is vendored locally (no CDN).
- **DL:** live RD map from `dl_rvm_blob` (the likelihood combined over all 4 channels), with
  detections overlaid.
- **UL:** one live RD map per C-RNTI session. Tiles appear and disappear with sessions.
- **Pipeline:** gate state, per-stage counters and latency, FIFO drops, the CUDA/CPU backend
  actually in use, BRANCHFO, and `realtime_chain` lag.

## 12. Launcher: `tests/passive_rx/run_sensing.sh`

One command on sens6, e.g. `run_sensing.sh --band 78 --rxg 43 --survey survey.json --dur 600`.

1. **Preflight.** Refuses to start on any failure:
   - no `nr-uesoftmodem` already running and no build running,
   - X410 answers `ping` and `uhd_find_devices`,
   - NIC MTU / ring / backlog at the known-good values (re-applied, then re-checked),
   - binary built from this worktree with sensing ON, which is proven by grepping a literal from
     the binary,
   - survey checks (§7).
2. **Radio wait.** Waits 30 s for the radio. If MPM was restarted within the last few minutes,
   waits until 180 s after the restart instead.
3. **Start.** The receiver, with `--passive-rx`, band scan, `--ue-nb-ant-rx 4`, and a conf
   generated from the template + survey. Then `realtime_chain.py` (`taskset`) and `monitor.py`.
   Cores are split so receiver RT/decode, engine and the Python tail never share a core.
4. **Supervision.** Catches both a process death and a live process that has gone silent (no
   receiver log progress, or no reports while the gate is open).
5. **Shutdown.** Ctrl-C or `--dur` stops all three cleanly. It then writes `run_verdict.json`:
   VALID/VOID, loss counters, gate time open, CUDA backend, CFO-mislock flag.

## 13. Validation

1. **Build on sens6, both configurations:**
   - sensing ON + CUDA: `-DENABLE_ISAC_SENSING=ON -DENABLE_CHANNEL_SIM_CUDA=ON
     -DCMAKE_CUDA_HOST_COMPILER=g++-13`,
   - sensing OFF.
   - Then run the unit tests of both lineages, including the CUDA parity / benchmark tests.
2. **Sensing-OFF equivalence:** decode counters on a lab capture match the pre-merge
   `adaptive-rx-UL-DL` binary.
3. **Offline:**
   - `isac_replay` of a recorded OTA `cfr_rows.bin` reproduces that run's `reports.jsonl`,
   - a synthetic per-channel delay offset injected into the replay is shown to cancel (§7).
4. **OTA on the lab cell**, 4 channels on separated antennas, two UEs with iperf:
   - **Step 0, before any sensing claim:** 4-channel decode works at all. Known risk: the
     2026-09-15 finding of 0 CORESET#0 hits at 4 RX, and the retune fix still unvalidated.
   - Gate opens only with a DL+UL flow and closes within 2 s of traffic stopping.
   - BRANCHFO converges (§10).
   - Rows arrive per channel. Loss counters (`dropped_cpis`, `discarded_pending_rows`, FIFO drops)
     are 0 when the gate is open.
   - The CUDA backend is active.
   - `realtime_chain` p99 per-CPI time is below the CPI period.
   - Tracks appear in the 3D view.
5. **Non-interference:** decode rate with sensing ON vs OFF, **≥ 5 runs per arm**, alternating
   arms, each scored by `run_verdict.json`. Acceptance: no statistically significant drop.
6. **Honest limit.** OTA without ground truth validates plumbing, timing and non-interference, not
   track accuracy. Accuracy numbers come only from replay against the frozen Sionna captures.

## 14. Risks

- **4-channel X410 decode** may not work on air (the 2026-09-15 finding). Step 0 of §13.4 tests
  this first. If it fails, sensing validation stops there and is reported as blocked.
- **CPU budget.** At 4 RX the passive decode was CPU-bound (68 % of grants dropped, 2026-09-15).
  Adding decision-directed CFR and the engine costs more CPU. Core split and the §13.5 A/B measure
  it.
- **Frozen chain on real data.** It was validated on Sionna only. Its per-class numbers
  (e.g. person 80.6 % / 100 %) do not transfer to OTA and are not quoted as OTA results.

## 15. Out of scope

A C++ port of stages [7]–[10] (conditional on §13.4), the micro-Doppler classifier, a co-located
array / AoA chain, and any TX path.

## 16. Decisions after review (2026-09-23)

- **No absolute UE position work.**
  - The 3D tracks come from the DL leg: surveyed gNB plus 4 separated receivers.
  - UL is shown as per-UE range-Doppler maps and detections, relative to each UE's own direct
    path.
  - UE localisation uses the TDOA across the 4 channels (shared FFT window and clock).
  - TA decoding (RAR / TA MAC CE -> localiser offset prior) and the UL window-advance sidecar are
    NOT built.
  - Watch in OTA validation: if UE positions are 24-40 m off and the stage-9 UL attachment
    degrades tracks, disable that attachment by flag rather than building TA support.
- **The 4-channel CORESET re-validation (plan Task 17 Step 1) is deferred** until the lab gNB is
  available again.
