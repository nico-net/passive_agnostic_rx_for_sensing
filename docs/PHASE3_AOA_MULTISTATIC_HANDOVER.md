# Handover: Phase 3 — AoA fusion + multi-static hardware bring-up

Written 2026-07-26 for whoever picks this up next. Read §1 and §2 before touching anything; §5 is the
AoA work, which is the critical path for the planned hardware.

Companion docs: `PHASE2_MOT_MULTIUE_HANDOVER.md` (MOT arc, still accurate for the tracker),
`openairinterface5g/tests/sensing_sim/MULTISTATIC_FAST_TARGET_NOTES.md` (§§1–9, the measured record
including all the negative results — read §7 and §9 before proposing a ghost fix, several obvious ones
are already refuted).

---

## 1. Where this actually stands

Per-receiver detection and tracking work. Multi-static fusion works and is the thing that made the
difference. The remaining weakness is multi-target fusion, not detection.

Best measured numbers (all `tests/sensing_sim`, 100 MHz / 273 PRB, sub-slot sampling on):

| | per-rx track precision | fused world precision | median world err |
|---|---|---|---|
| 2 targets, crossing | 53% | **96%** | **0.2 m** |
| 2 targets, manoeuvring | **71%** | 83% | 2.4 m |
| 2 targets, fast (car) 3 pairs + gate | — | **98%** | **1.1 m** |
| 5 targets | 53% | 30% | 27.9 m |

Settled questions, with evidence, so they are not re-opened:

- **Greedy association is sufficient.** On a genuine transversal crossing (co-range 20% of the run,
  7.7 m/s Doppler separation) there were **zero identity swaps** — `check_identity.py`. Hungarian
  assignment is not warranted until a scene is found where greedy demonstrably swaps.
- **Two Tx–Rx pairs are structurally not enough.** Birth is exactly determined (2 equations, 2
  unknowns, for position and again for velocity), so any detection set yields a zero-residual fix and
  no consistency test is possible — `birth_max_rate_rms_mps` is provably inert there. Three pairs
  over-determine it: 59% → 88% precision, and 98% with the consistency gate enabled.
- **`cfar_per_row` is the single biggest detection-side win** and it holds at 5 targets, not just 2
  (turning it off tripled ghost count and quadrupled world error). Judge it on TRACK precision, not
  raw precision — at matched detection density its raw-precision advantage mostly disappears while its
  track-precision advantage (2×) does not. Reason: ridge detections are kinematically coherent, so
  they confirm false tracks; ordinary false alarms scatter and never confirm.
- **Single-receiver ghost suppression has a floor.** Four mechanisms were implemented, offline-tested,
  and then measured neutral-or-harmful end to end: separable CLEAN, occupancy-aware CLEAN, flicker/TBD
  energy consistency, absolute speed gating. They remain in the tree, off by default. Do not re-try
  them without new information — see NOTES §7.

**Everything above is n=1 or n=3.** On this harness the fused precision metric ranged **8–58% across
identical repetitions** at two pairs. Before any number here goes into a thesis it needs reps. This is
the single highest-integrity task outstanding and it costs only wall time.

---

## 2. Traps that fail silently

Each of these cost real time. None produce an error message.

1. **CIR tap cap.** `dR` must stay under **~622 m at 100 MHz / 273 PRB**. The "~1244 m" in
   `sensing_channel.c` is the 40 MHz figure — the cap is 255 *taps*, so it halves when `fs` doubles.
   A scene placed at 650–930 m simply lost both targets. The harness *does* log
   `requested N taps exceeds the uint8_t channel_length cap (255); clamping` — grep for it whenever a
   target mysteriously fails to detect.
2. **AVX alignment in the data-aided tap.** `nr_modulation()` and the LDPC encoder store through AVX2
   intrinsics; the tap's `coded_bits`/`scrambled`/`mod_syms` are `__thread`, so their alignment
   depended on the TLS block layout and was only *accidentally* correct. Adding `__thread` state to
   that function shifted the layout and produced an immediate GP fault. All four are now explicitly
   `aligned(32)`. If you add thread-local state there and it crashes inside a SIMD routine, this is why.
3. **`vel_max` read from a run's first CPI is meaningless.** `T_slot` is large during startup. This
   produced a false "the scene is nearly aliasing" conclusion. Use the mean (≈1.0 slots, ±82 m/s).
4. **Hand-edited scene configs get reverted.** `make_scenes.py --write` regenerates from the base
   config. Per-scene overrides must go in its `OVERRIDES` dict (there is an assert that each applies).
   A hand-set `max_detections` was silently reverted and the 5-target scene then saturated 16/16
   detections per CPI for an entire batch.
5. **Background runs die with the session.** A 9-run batch was killed mid-rep by session teardown
   (no error, output just stops). Launch with `sudo setsid nohup … &`.
6. **Doppler ambiguity.** Unambiguous bistatic rate is `c/(2·fc·t_slow)`, and a target of speed *v*
   produces a rate up to **2v**. One row per slot gave ±48 m/s, so nothing above ~24 m/s was
   observable — which is why 50 m/s was impossible before sub-slot sampling. Now ±82–89 m/s measured.

---

## 3. What was added this session

All opt-in, all defaulting to previous behaviour. OAI branch `isac-ota-sync`, isac branch
`phase1-central-node`.

**`cfar_per_row`** — mirrors `cfar_per_column` along the Doppler axis, training within a range ROW so
a strong scatterer's slow-time ridge gates itself out. Found by *rendering the RVM* (see
`make_tracking_gif2.py`) after four more elaborate mechanisms had been aimed at the wrong ghost family.

**`subbin_interp`** — 3-point parabolic peak fit in range and Doppler, removing the ±half-bin
(±1.5 m) quantisation error. Fitted in **dB**, not linear power: a windowed mainlobe is much closer to
a parabola in log domain, which is why the linear version in `isac_sync.cc` needed an empirical scale
correction.

**Sub-slot CFR sampling** (`subslot_symbols`) — one slow-time row per symbol GROUP instead of per slot,
raising the PRF. Grouping is adaptive: a group absorbs symbols until it passes a sparsity floor
(`subslot_min_re`) AND an SNR floor (`subslot_min_snr_db`), and a tail that cannot qualify is merged
backwards. 6 symbols/row with strict gates buys ±89 m/s at *no* precision cost; 3 symbols/row buys
±176 m/s and trades precision.

**`gating_reject`** (A/B complete, NOTES §10) — measures
the scheduling-replica offsets instead of assuming integer multiples. `harmonic_reject` assumes
replicas at `k·v`, true only for a strictly periodic gate; real traffic has `T_slot` wandering 1.0–3.7
slots. This transforms the **slow-time row-energy envelope** at the Doppler axis length, so its peak
bins *are* the Doppler offsets, then rejects a detection sitting at one of those offsets from a
clearly stronger same-range detection. Row energy (not the occupancy mask) is deliberate: it is the
physical modulating quantity, it is defined on exactly the rows being transformed, and it needs
nothing plumbed from the engine. **`gating_snr_margin` is load-bearing** — with several real targets
crowded in Doppler, a real target can sit a gating offset from another, so only a distinctly stronger
neighbour may veto.
A/B result (3 reps, 5-target scene): per-receiver track precision 57.3 ± 1.2 % vs 50.7 ± 4.9 % — higher
in all three reps individually — false detections down ~15%, fused track ids 20.3 vs 29.0. Fused world
precision is NOT resolvable at n=3 (ON 37.0 ± 1.0, OFF 42.3 ± **22.4**, driven by one 68% outlier), and
the predicted cost is real: obj3, the weakest target, loses coverage 45.3% vs 52.3%. `gating_snr_margin`
= 3.0 dB is still a guess; raise it if weak targets matter.

**isac-core**: birth-grid bounds fix (the pad was a flat 50 m around the nodes, making any target
outside structurally unfindable — a target detected in 100% of CPIs by both receivers produced ZERO
world tracks); `Fix::rate_rms` + `birth_max_rate_rms_mps`; scale-free world-frame `harmonic_reject`.
Use the **release** build: the corrected (larger) search area makes debug replays minutes-slow.

**Harness**: 3 simultaneous receivers (separate netns sets AND separate working dirs — the softmodem
writes stats to relative paths that collide), `make_scenes.py` (designs + verifies + emits),
`merge_receivers_n.py` (N-receiver alignment, ground truth parsed from each receiver's own log),
`check_identity.py`, `score_world_tracks.py`.

---

## 4. Why fusion does not remove all ghosts

Two families survive it *by construction*. Worth understanding before optimising anything.

**Chance coincidences, which scale badly.** Fusion must consider every cross-receiver combination, and
that count is multiplicative — measured: 2 targets → **505** candidate triples/CPI; 5 targets →
**1774**. Only ~2 or ~5 are real. Each remaining triple has a small chance of intersecting within
`birth_max_rms_m` (8 m) by accident, and with 1774 draws per CPI "small" still yields several ghost
births. This is exactly why 2 targets fuse at 96% and 5 targets at 30%: density rose 1.5×,
combinations rose 3.5×. **Fusion's ghost rejection is statistical, not absolute.**

**Range-correct ghosts triangulate correctly.** A scheduling-harmonic ghost carries the target's
*true* range — only the rate is wrong. Multilateration takes position from **ranges alone**, so one
harmonic ghost per receiver, all of the same real target, intersects at the target's genuine position
with a k× velocity. Fusion has nothing to object to. That is why world-frame `harmonic_reject` exists.

**Consequence:** the lever is **birth selectivity**, not better geometry — tighten `birth_max_rms_m`,
require all pairs (not ≥2) to support a birth, tighten the CPI pairing tolerance. Each cuts the
chance-coincidence rate without touching real targets. This is the main open algorithmic item.

---

## 5. AoA work — the critical path

Target hardware: **2× USRP X410** (multi-channel, phase-coherent → bearing capable) + **1× B210**
(single-channel → range/Doppler only). This is a good scheme: the X410s give bearing so each can
localise alone (ellipse ∩ bearing ray), and the B210 supplies the third independent *range* ellipse,
which is what produced 59% → 98% in simulation.

**Set expectations correctly: AoA does not fix ghosts.** The ridge/harmonic ghosts share the real
target's wavefront, so they share its bearing. AoA buys *accuracy* and single-receiver
observability; the third pair buys *ghost rejection*. Do not sell AoA as a ghost fix.

### STATUS as of 2026-07-26: the central-node half is LARGELY BUILT

Re-read the code before planning; this section was written as a to-do list and much of it has since
landed. What exists now (35 isac-core tests pass, `cargo test`):

- `geometry.rs`: `wrap_angle()`, `TxRxPair::bearing_meas()`, bearing-aware residuals. Tests cover
  wrapping over ±π, ENU convention, ray∩ellipse, and that an inconsistent bearing does NOT yield the
  true position.
- `localization.rs`: `BirthMeas::bearing: Option<f64>`, `Fix::bearing_rms_rad` / `bearings_used`,
  `multilaterate_min_pairs()`. Bearings enter the cost and the Gauss-Newton normal equations as
  **arc length `r·Δθ`**, which is unit-consistent with the metre residuals and needs no weighting knob.
  `absent_bearings_leave_the_fix_unchanged` pins the no-array path as byte-identical.
- `tracker.rs`: per-DETECTION measurement dimension (2-D or 3-D), per-detection noise with
  `TrackerParams` as fallback, `use_bearing`, `birth_max_bearing_rms_deg`,
  `birth_allow_single_pair_bearing`. Tests include
  `bearings_reject_a_cross_target_chance_coincidence` — i.e. §4's combinatorics gate is implemented.
- `report.rs`: `azimuth_deg` **and** `azimuth_std_deg`, both `Option`.
- `isac-track`: `--no-bearing` (the A/B switch), `--azimuth-std-deg`,
  `--birth-max-bearing-rms-deg`, `--birth-single-pair-bearing`.
- UE side: `isac_aoa.{h,cc}` (array parsing + estimator), `nr_isac_submit_cfr_ant()` with
  antenna-major CFR, `nr_isac_aoa_antennas()`, and `detection_report` now emits per-receiver
  `range_std_m`/`rate_std_mps` derived from that run's own resolution (bin/√12, or ~0.3 bin when
  `subbin_interp` is on) rather than a hand-picked constant.

**Two things to know before continuing.** `isac_aoa.cc` was missing from `NR_UE_ISAC_SRC` and the
binary failed to link (`undefined reference to parse_rx_array`); fixed in commit `3deb64fc2f` — if you
add another AoA translation unit, register it. And there is **no `test_isac_aoa` target yet**: the
UE-side estimator is the one part of this with no offline coverage, which is where I would put effort
first.

### 5.1 What remains

1. **Offline test for the UE-side estimator.** Nothing covers `isac_aoa.cc`. Synthesise an array
   response for a known bearing and assert recovery, including the ambiguity boundary (see §5.4).
   Add a `test_isac_aoa` target next to the others in `CMakeLists.txt`.
2. **Simulator array support** — still absent, and still the blocker for validating any of this before
   hardware. `sensing_channel.c` synthesises a SINGLE antenna. It needs array geometry and a per-element
   phase `exp(−j·2π/λ·(d_i·û_target))`. Until this exists the UE-side estimator cannot be exercised
   end to end, only unit-tested. Extend `make_scenes.py`'s verifier with an AoA check (bearing
   separation, ambiguity margin vs element spacing) rather than bypassing it — that verifier has
   already caught bugs in all three existing scenes.
3. **Measure what bearing is worth.** `--no-bearing` exists precisely for this: same report stream,
   bearing consumed or ignored. Do it on the 5-target scene, where §4 predicts the largest gain, and
   with **≥10 reps** — the fused metric ranged 8–58% across identical reps, and the `gating_reject`
   A/B (NOTES §10) showed a 3-rep sample giving a 22-point standard deviation on exactly this metric.
4. **Decide the single-pair-birth policy.** `birth_allow_single_pair_bearing` defaults off, correctly:
   a ridge/harmonic ghost shares its parent's wavefront and therefore its bearing, so one pair plus a
   bearing is *weaker* for ghost rejection than two pairs. It buys coverage, not precision. Keep it off
   unless coverage is the binding constraint, and never present it as ghost rejection.
5. **Direct-path self-calibration** (§5.4 item 3) — not started, and still the cheapest route to array
   calibration.

### 5.6 Suggested order (revised)

1. **Offline test for `isac_aoa.cc`** — the only piece with no coverage at all.
2. **Simulator array support** — the blocker for any end-to-end validation before hardware.
3. **`--no-bearing` A/B on the 5-target scene, ≥10 reps** — measure what bearing actually buys.
   Anything less than ~10 reps cannot resolve this metric; see NOTES §10.
4. **Direct-path self-calibration**, then MUSIC over the 4-channel manifold.

The isac-core bearing model, per-detection noise, and bearing-seeded birth are already done — verify
by reading the tests listed in STATUS rather than re-implementing.

---

## 6. Other open items

- **GPSDO on the B210** and **OTA sync validation (Phase 6b)** — the 7-step procedure is written up in
  `runbook.md` §8.9 and has never been run. Hardware prerequisite for multi-static OTA.
- **First OTA capture through `csirs_monitor`** — parse + occurrence unit-tested offline only.
- **obj3 in the 5-target scene** detects in only ~52% of CPIs. Eliminated by measurement: the
  detection cap (raising it fixed *track* coverage 37→62% but left detection at exactly 52%),
  `cfar_per_row` (52% either way), the clutter notch (21 bins clear), and aliasing (mean vel_max ±82
  vs rates 7.5–20.6). It has the lowest bistatic rate and lowest matched SNR of the five despite the
  simulator applying no path loss. Local competition from neighbours' sidelobe structure is plausible
  but **not demonstrated** — do not assert it without a measurement.
- **`gating_reject`**: done, NOTES §10. Open sub-item: `gating_snr_margin` (3.0 dB) is a guess,
  and it measurably costs the weakest target ~7 points of coverage. Calibrate it.

## 7. Commands

```bash
# build
ninja -C openairinterface5g/cmake_targets/ran_build/build nr-uesoftmodem
(cd repos/isac && cargo build --release)          # release matters, see §3

# offline tests (44 C++ cases + 19 Rust)
for t in test_occ_clean test_clean_deconv test_multi_target_tracker test_target_tracker \
         test_eca_clutter test_isac_sync test_matrix_complete test_sparse_doppler; do
  openairinterface5g/cmake_targets/ran_build/build/$t; done
(cd repos/isac && cargo test)

# scenes: verify, then emit
cd openairinterface5g/tests/sensing_sim
python3 make_scenes.py            # verify only
python3 make_scenes.py --write    # emit _scene_<name>_rx{1,2,3}.conf

# 3 receivers simultaneously (ALWAYS detached, see trap 5)
sudo setsid nohup ./_run_crossing_check.sh 270 > /tmp/x.log 2>&1 < /dev/null &

# fuse + score
python3 merge_receivers_n.py /tmp/m.jsonl 150 \
  "$D1/oaiue_reports.jsonl@$D1/logs/ue.log" \
  "$D2/oaiue_reports.jsonl@$D2/logs/ue.log" \
  "$D3/oaiue_reports.jsonl@$D3/logs/ue.log"
repos/isac/target/release/isac-track --out /tmp/t.jsonl --birth-max-rate-rms-mps 4.0 replay /tmp/m.jsonl
python3 score_world_tracks.py /tmp/t.jsonl $D1 15     # world frame
python3 score_run.py $D1                              # bistatic, per receiver
python3 check_identity.py $D1                         # track-identity continuity
python3 make_tracking_gif2.py $D1 /tmp/rvm.gif 700 2  # render the RVM — this is how ghosts get diagnosed
```

**Note this file is at `/home/sens/NICOLA/`, which is NOT a git repository** (matching where the other
handover docs live). Copy it into `openairinterface5g/docs/` if you want it version-controlled.
