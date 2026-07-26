# Handover: Phase 3 — AoA fusion + multi-static hardware bring-up

Written 2026-07-26 for whoever picks this up next. Read §1 and §2 before touching anything; §5 is the
AoA work, which is the critical path for the planned hardware.

> **STATUS UPDATE 2026-07-26 (same day): §5.1–§5.6 are IMPLEMENTED and offline-tested.** All five
> steps of §5.6's suggested order are done, in that order, and the whole chain has been exercised end
> to end against `isac-track replay`. See **§8** at the bottom for what landed, the measured results,
> and what is still open. §5 is retained unchanged as the design brief it was; §8 records where the
> implementation departed from it and why.

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

# offline tests (64 C++ cases + 36 Rust -- see section 8)
for t in test_occ_clean test_clean_deconv test_multi_target_tracker test_target_tracker \
         test_eca_clutter test_isac_sync test_matrix_complete test_sparse_doppler test_isac_aoa; do
  openairinterface5g/cmake_targets/ran_build/build/$t; done
# test_sensing_channel links into a build SUBDIRECTORY, not build/ itself -- ninja reports
# "up to date" with zero output if you look for it at the top level and conclude it doesn't exist.
openairinterface5g/cmake_targets/ran_build/build/openair1/SIMULATION/TOOLS/test_sensing_channel
(cd repos/isac && cargo test)

# first live AoA run (not yet done -- see section 8.9)
UE_NB_ANT_RX=4 sudo setsid nohup ./_run_crossing_check.sh 270 > /tmp/x.log 2>&1 < /dev/null &

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
handover docs live). A version-controlled copy lives at `openairinterface5g/docs/` -- when you edit
one, copy it to the other, or they will silently drift (this happened once already: the docs/ copy was
committed before section 8 existed).

---

## 8. What was implemented (2026-07-26)

All of §5, in §5.6's order. Everything is **opt-in and inert by default**: with no `rx_array`
configured and no `azimuth_deg` on the wire, every code path below reduces to exactly the previous
behaviour. That property is asserted by tests, not assumed —
`localization::absent_bearings_leave_the_fix_unchanged`,
`tracker::use_bearing_false_ignores_reported_azimuths`,
`sensing_channel_array.unconfigured_array_leaves_antennas_identical`.

**Test count: 64 C++ cases (was 44) + 36 Rust (was 19+1). All pass.**

### 8.1 Simulator array support (§5.5) — `openair1/SIMULATION/TOOLS/sensing_channel.{h,c}`

New `sensing_channel_set_rx_array(traj, "x,y;x,y;...", boresight_deg)`, wired to
`[sensing_channel] rx_array` / `rx_array_boresight_deg`. Each path's tap is steered per rx element by
`exp(+j·2π·(d_i·û)/λ)`, `û` pointing **from the receiver towards the scatterer**; `ch[]` is indexed
`[aarx + aatx*nb_rx]` (OAI's convention, per `multipath_channel.c`).

- **Narrowband, phase-only, stated so it isn't re-derived**: the across-aperture delay spread
  (~4 cm / c = 0.13 ns) is two orders of magnitude below one sample at any rate this simulator runs
  at (8.1 ns at 122.88 MHz), so an element offset changes the tap PHASE only, never its delay `tau`.
  All elements share one fractional-delay kernel.
- **The LOS tap is steered too**, by the illuminator's bearing. That is not incidental — it is the
  entire basis of the direct-path self-calibration in §8.5, and it has its own test.
- `sensing_channel_make()`'s signature is unchanged (the array is a separate setter), so the existing
  unit test and every existing scene are untouched.
- The ground-truth log line now carries `azimuth=<deg>`, so an AoA run can be scored straight off it.

### 8.2 Scene verifier (§5.5) — `tests/sensing_sim/make_scenes.py`

`RX_ARRAYS` models the planned fleet: rx1/rx2 get 4-element λ/2 ULAs (X410 stand-ins), **rx3 gets
none** (B210). `verify_aoa()` hard-fails on element spacing above λ/2 (which makes an estimator
confidently *wrong*, not merely noisy) and warns on endfire dwell and sub-beamwidth bearing gaps.
`_apply_array()` writes the SAME element list into **both** `[sensing_channel]` (what the air carries)
and `[sensing]` (what the estimator assumes) — those two disagreeing is a silent-garbage failure of
exactly the `csirs_monitor` `scramb_id` class, so they are always emitted together from one source.

Re-running `--write` changed the rx3 configs **not at all** (verified by diff) and added only the
array lines to rx1/rx2.

**It immediately found things.** On the existing scenes: rx2's obj0 sits within 15° of endfire for
100% of the crossing run (weak bearing accuracy there), and in the 5-target scene most target pairs
are co-bearing to within a degree at rx1 — i.e. **AoA will not separate them**, which is worth knowing
before it is proposed as the 5-target fix. Two scenes also sweep across ±180°, so the existing scene
set already exercises the wrap case §5.2 warned about.

### 8.3 isac-core bearing model (§5.2) — `geometry.rs`, `ukf.rs`, `tracker.rs`, `report.rs`

- `TxRxPair::bearing_meas(p) = atan2(p.y−rx.y, p.x−rx.x)`, ENU radians CCW from east.
- **Angle wrapping written first, as instructed.** `wrap_angle()` + `meas_residual()` +
  `BEARING_IDX`, and *every* difference of two measurement vectors goes through them — including the
  sigma-point scatter that forms `S` and `Pxz`, not just the final innovation (an unwrapped sigma
  point straddling ±180° inflates `S` by ~(2π)² and silently disables the bearing update). The
  sigma-point mean unwraps onto the centre point's branch before averaging.
- Measurement dimension is **per DETECTION, not per pair**: an array receiver still fails AoA on a
  weak cell, and such a detection must stay usable as a plain 2-D range/rate measurement rather than
  being dropped from a receiver that is otherwise contributing. `Ukf` moved to `DVector`/`DMatrix`
  (2- or 3-element; the allocation is negligible next to the birth grid search).
- Per-pair, per-dimension noise: `DetectionReport.range_std_m` / `rate_std_mps` (optional, declared by
  the receiver) and `Detection.azimuth_std_deg` (per detection, since it is SNR- and
  geometry-dependent), with `TrackerParams` values as fallback.
- New `TrackerParams`: `use_bearing` (master A/B switch), `azimuth_std_deg`,
  `birth_max_bearing_rms_deg`, `birth_allow_single_pair_bearing`. All exposed on `isac-track`
  (`--no-bearing`, `--azimuth-std-deg`, `--birth-max-bearing-rms-deg`, `--birth-single-pair-bearing`).

**A pre-existing test had to be corrected, and it is worth knowing why.** `ukf::
converges_to_a_static_target_from_two_pairs` started from `(100,100)`, which sits *exactly* on the
mirror-symmetry axis of its own two-pair geometry; `(140,80)` is an exact second solution with
identical residuals on both pairs (verified numerically). Which basin it fell into was decided by
round-off, and the `DVector` refactor duly flipped it. The start point is now off-axis, with a comment
saying not to move it back. **This was a latent bad test, not a regression.**

### 8.4 Bearing-seeded birth (§5.3) — `localization.rs`

- `TxRxPair::localize_with_bearing()` — ray ∩ ellipse in **closed form**; the `t²` terms cancel:
  `t = (R_b² − |a|²) / (2(R_b + a·û))` with `a = R − T`. One pair localises exactly.
- `multilaterate()` seeds from every bearing-carrying measurement's ray∩ellipse point and takes the
  best; the grid search runs only when no bearing is available. That removes the grid-pad fragility
  that caused the 2026-07-25 bounds bug **structurally** for array receivers (regression test:
  a target at (1200, −2300) with a deliberately coarse 500 m grid is still found to <5 m).
- Bearings enter `refine_position()` as **arc-length** residuals `r·Δθ` with Jacobian `r·∂θ/∂p =
  (−sinθ, cosθ)`, so metres and radians are never mixed and no weighting knob is needed.
- `Fix` gains `bearing_rms_rad` / `bearings_used`. `rms` stays **range-only** on purpose: every
  existing `birth_max_rms_m` value is tuned against it, and folding an angular term in would silently
  retune every scene.
- `multilaterate_min_pairs(..., 1)` allows single-pair birth; `birth_allow_single_pair_bearing`
  defaults **off**. Reason recorded in the code: §1's measured result is that even two pairs are
  structurally too few for ghost rejection, and one pair plus a bearing is weaker still, because a
  ridge/harmonic ghost shares its parent's wavefront and therefore its bearing. It buys coverage, not
  ghost rejection.

**Measured while writing the tests, and it changes the story slightly:** a cross-receiver
mis-pairing is rejected at the **range** gate, not at `birth_max_bearing_rms_deg`. Because the
bearing residual enters the refinement, a contradictory bearing drags the fix off the ellipse
intersection and the existing `birth_max_rms_m` throws it out. The explicit bearing gate remains as
belt-and-braces for geometries where the refinement can find a compromise point that still satisfies
every range.

### 8.5 UE-side AoA (§5.4) — new `openair1/PHY/NR_UE_ISAC/isac_aoa.{h,cc}`

Per-antenna CFR now flows from both taps: the data-aided PDSCH tap
(`SCHED_NR_UE/phy_procedures_nr_ue.c`, inner loop over `rxdataF[a]` against the one reconstructed `X`)
and the CSI-RS tap (`PHY/NR_UE_TRANSPORT/csi_rx.c`, strided over the `[ant][port][sc]` LS array). New
C API `nr_isac_submit_cfr_multi(..., nof_ant, ant_stride_re, ...)`; the two existing entry points are
that with `nof_ant = 1`, so nothing else changed. `nr_isac_aoa_antennas()` tells the taps how many
antennas to extract.

**AoA is estimated per DETECTION CELL, after CFAR** (as §5.4 recommends) — a cell's angle of arrival
is only meaningful where there is a detection, so this is orders of magnitude cheaper than per-RE.

Three design decisions worth not re-litigating:

1. **A separate, deliberately plain transform, on its own raw per-antenna grid.** The main
   `range_doppler` chain carries data-dependent machinery (whitening, ECA, matrix completion, CLEAN,
   sparse Doppler) which would weight each antenna's grid *differently* and perturb the exact quantity
   being measured. The AoA path instead does slow-time mean subtraction → occupied-only range
   projection → non-uniform slow-time projection at the true row times, identically for every antenna.
   The argument is one line: at a target's cell `H_a = s_a·H_0`, and a linear operator applied
   identically to all antennas factors `s_a` straight through. **That also means the window choice is
   irrelevant to the bearing** (a common real weighting cancels in `z_a·conj(z_0)`), so no effort is
   spent matching the main path's window.
2. **The sync corrections (Phases 1–3) are deliberately NOT applied** to the AoA grid: they are
   common-mode receiver impairments, so they multiply every antenna's row by the same factor and
   cancel in the phase difference. What they *can* do is move a peak by a fraction of a bin relative
   to the corrected grid the detections came from — hence a ±`aoa_cell_search_bins` re-peak around
   each detection rather than trusting the cell index blindly.
3. **Estimators**: `beamscan` (default, any element count, Bartlett over the manifold with a
   parabolic-in-dB peak refinement — same log-domain reasoning as `subbin_interp`), `interferometry`
   (exact 2-element closed form, auto-selected at 2 elements), `music` (≥3 elements; builds the
   covariance from the detection cell's neighbour cells as snapshots, because ONE cell gives a rank-1
   covariance MUSIC cannot use — falls back to beamscan rather than dropping the detection).

**Two ambiguities are surfaced, not hidden.** Element spacing > λ/2 is flagged at parse time. And a
**linear array physically cannot separate θ from its mirror about the array axis** — the phase depends
only on `d·û`. The scan is therefore restricted to the half-plane the array faces
(`aoa_broadside_deg`, default = the array normal) and `mirror_ambiguous` is set. Only a genuinely 2-D
array removes this; `parse_rx_array()` detects collinearity at λ/8 scale and scans the full circle
when the array is 2-D.

`azimuth_std_deg` is a **CRB**, computed from the array geometry and the detection SNR
(`σ = 1/sqrt(2·SNR·Σ(g_a−ḡ)²)`, `g_a = (2π/λ)(d_a·û⊥)`), and reported as such. It correctly blows up
towards endfire, which is asserted.

Below `aoa_min_snr_db` the azimuth is **omitted from the JSON entirely** (not null, not zero) — an
absent azimuth degrades cleanly to a 2-D measurement at the central node, a wrong one corrupts the fix.

`detection_report.cc` also now declares `range_std_m` / `rate_std_mps`, derived from **this run's own**
resolution rather than hand-picked: `bin/√12` for a quantised peak, ~0.3 bin with `subbin_interp`.

### 8.6 Direct-path self-calibration (§5.4 item 3)

`aoa_estimator::calibrate_from_los()`, gated on `[sensing] aoa_selfcal`. The illuminator is surveyed,
so the LOS tap sits at a known bearing at zero Doppler and the configured `nominal_los_range_m`;
whatever inter-element phase is left over after removing the expected steering is the receiver's own
per-channel error. Averaged across CPIs, applied before any bearing is read.

**Ordering constraint, easy to get wrong**: clutter removal subtracts the per-subcarrier slow-time
mean, which *is* the direct path. Calibration therefore runs on the RAW grid, before `process()`.

Only phase is corrected, not gain — an amplitude taper does not bias a phase-only estimator, and
inverting a badly-conditioned channel's gain would amplify its noise.

**Measured**: with per-channel phase errors of {0, 0.9, −1.7, 2.4} rad injected, the raw bearing error
exceeds 5° and calibration brings it under 2°.

### 8.7 End-to-end wire check (not a source read)

Two receivers' `DetectionReport` JSON was generated through the **real** C++ serialiser with bearings
populated, and fed to the release `isac-track replay`:

| | tracks emitted | median world error |
|---|---|---|
| geometrically consistent bearings, gate on | 10 | **0.02 m** |
| same data, `--no-bearing` (pre-AoA A/B) | 10 | 0.03 m |
| one receiver's bearing wrong by 45° | **0** | — |

The schema extension is confirmed safe by actually running the deserialiser, not by reading
`report.rs`. **Note the accuracy gain here is marginal (0.02 vs 0.03 m) and should not be quoted as a
result**: this is a noiseless, well-conditioned two-pair scene where range alone already nails it. The
meaningful demonstrations are the *rejection* row and §8.3/§8.4's single-pair observability tests.

### 8.8 Config surface added

`[sensing_channel]`: `rx_array`, `rx_array_boresight_deg`.
`[sensing]`: `aoa_enable`, `rx_array`, `rx_array_boresight_deg`, `aoa_estimator`,
`aoa_scan_step_deg`, `aoa_min_snr_db`, `aoa_cell_search_bins`, `aoa_broadside_deg`, `aoa_selfcal`.
`isac-track`: `--no-bearing`, `--azimuth-std-deg`, `--birth-max-bearing-rms-deg`,
`--birth-single-pair-bearing`.
`run_sim_traffic_iperf.sh`: `UE_NB_ANT_RX` env var (opt-in; raising it changes the rfsim channel's
`nb_rx` and the CFR extraction cost, which would invalidate every single-antenna baseline).

A **loud one-shot warning** fires if AoA is configured but every CFR submission carries one antenna
(i.e. `--ue-nb-ant-rx` was forgotten) — otherwise every bearing would simply be absent with nothing in
the logs to say why, which is precisely trap-class §2.

### 8.9 Still open

- **No live `tests/sensing_sim` AoA run has been done.** Everything above is offline-tested. The
  scenes now carry the geometry and the runner has the switch, so the next step is
  `UE_NB_ANT_RX=4 sudo setsid nohup ./_run_crossing_check.sh …` and scoring reported azimuth against
  the `SENSING_CHANNEL gt: … azimuth=` log lines. **Budget for it being slower**: 4× the CFR
  extraction and 4× the AoA transform per CPI.
- **§1's reps problem is untouched and remains the highest-integrity outstanding task.** Nothing here
  changes that the headline numbers are n=1–n=3 with an 8–58% spread across identical repetitions.
- **AoA on the 5-target scene looks unpromising and §8.2 says why**: most target pairs are co-bearing
  to within a degree at rx1. The §5.3 claim that bearing prunes the chance-coincidence combinatorics
  is implemented and unit-tested, but that scene is not the case where it will show.
- **Mirror ambiguity is reported but not consumed.** `mirror_ambiguous` never reaches the wire; a
  target in a linear array's back half-plane is reported mirrored with no flag at the central node.
  Fixing it properly means a 2-D array, not a protocol field.
- **Nothing OTA.** The B210 is single-channel, so the UE-side AoA path cannot run on the current
  hardware at all — it needs the X410s. §6's GPSDO and Phase 6b items are unaffected and still open.
