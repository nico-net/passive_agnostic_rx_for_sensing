# Multi-static: world-frame ghost rejection + fast-target scene (2026-07-25)

## 1. World-frame harmonic reject (`isac-core`, opt-in `--harmonic-reject`)

Replaces the earlier absolute `--birth-max-speed-mps` gate, which was **not a ghost discriminator but
a target-class prior**: it only separated ghosts because this scene's targets were slow. A 3rd
harmonic of a 10 m/s walker lands at 30 m/s, indistinguishable by absolute speed from a real 30 m/s
car — so that gate cannot scale.

**Signature exploited (scale-free):** multilateration takes POSITION from the ranges alone, and an
amplitude-gating harmonic ghost carries the *correct* range with k x the range-rate. It therefore
fuses to the **same world position** as its parent with velocity k*v. Reject a track that is
co-located with a slower track (<= `harmonic_colocation_m`) whose speed ratio is a near-integer
2..`harmonic_max_k`, **and** whose heading matches (within `harmonic_max_heading_rad`) — the heading
test spares genuinely crossing targets. Keeps the slower member (the fundamental).

Also added `Fix::rate_rms` + `--birth-max-rate-rms-mps`: RMS of the range-rate residuals against the
fitted world velocity. The pre-existing `birth_max_rms_m` only checked that the *ellipses intersect*;
this checks the pairs *agree on Doppler*. **Only bites with >= 3 pairs** — with two pairs the 2x2
velocity solve is exact so the residual is ~0 by construction. Documented in the field comment.

## 2. Birth-grid bounds bug (REAL BUG, fixed)

`Bounds::around_nodes(measurements, 50.0)` padded the grid search by a flat **50 m** around the nodes.
Any target outside that box is **structurally unfindable**: the grid seed never lands near it, so
Gauss-Newton starts in the wrong basin and the fix is thrown away.

Measured: a 15 m/s target detected in **100% of CPIs by BOTH receivers** produced **zero** world
tracks, purely because it drove outside the pad. Fixed by deriving the pad from the measurements
themselves — a detection at bistatic path R_b lies on an ellipse of semi-major axis R_b/2, so the pad
is now `max(R_b)/2 + 50 m`. After the fix that target yields 24 matched world updates.
Regression test: `finds_a_target_far_outside_the_node_footprint`.

**Consequence:** the search area grows, so use the **release** build (`cargo build --release`) —
the debug build takes minutes per replay on a 600 m-scale scene.

## 3. Fast-target scene (`ue.sensing.traffic.moving.fast.rfsim.conf` + `_fast_rx2.conf`)

Slow target A 7.0 m/s (25 km/h) + fast target C **15.0 m/s (54 km/h, urban car)**, 2.14x A.
Numerically verified at BOTH receivers: no Doppler aliasing, no zero-Doppler-notch dwell, 2nd
harmonics still unaliased (so the reject path is exercised), dR within the tap cap. The 2.14x ratio at
>80 m separation is also a deliberate **false-positive check** — harmonic reject must NOT fire on it.

### Why NOT 50 m/s (asked for; not physically possible here)
- Unambiguous bistatic range-rate is `c/(2*fc*t_slow)` = **+/-48.3 m/s** (t_slow ~ 0.83 ms).
- A target of speed v produces bistatic rate up to **2v** -> 50 m/s needs +/-100 m/s.
- Even at perfect every-slot scheduling the TDD split (7 DL / 10) floors t_slow ~ 0.7 ms -> +/-57 m/s.
- Grid search over thousands of geometries found **zero** valid 50 m/s trajectories inside the tap cap.
- **To actually support it: sub-slot CFR sampling** — emit one slow-time row per OFDM symbol instead
  of per slot (data-aided reconstruction already gives H at every data RE), raising the PRF up to 14x.
  Tap-side change in `nr_isac_pdsch_data_aided_tap`, not a scene change.

### CIR tap cap — the trap that cost a run
`sensing_channel.c` says "~1244 m", but that is for the **40 MHz / 61.44 MHz-fs** config. The cap is
255 **taps**, so it *halves when fs doubles*: at **100 MHz / 273 PRB it is only ~622 m**. A first
version of this scene placed targets at 650-930 m and they were silently dropped (5% detection). The
harness does log `requested N taps exceeds the uint8_t channel_length cap (255); clamping` — grep for
it whenever a target mysteriously fails to detect.

## 4. Results (two simultaneous receivers, fast scene, 37 fused CPIs)
Per-receiver: **100% detection coverage on BOTH targets**, including the 15 m/s car.
Fused world frame: obj0 25 / obj1 24 matched updates, matched error ~5 m, 26% precision, 33 ids.
`--harmonic-reject` changed little here (188 -> 183 updates): on this capture the dominant residual
ghosts are **spurious ellipse intersections**, not co-located harmonics. The mechanism is implemented
and unit-tested (19/19 isac-core), but it is not the binding constraint on this scene — the wider
birth grid (necessary for the fast target) admits more spurious fixes, which is the next thing to
attack (tighter `birth_max_rms_m`, or requiring 3 pairs for over-determined birth).

## Repro
```
sudo ./_run_mot_variant.sh ue.sensing.traffic.moving.fast.rfsim.conf /tmp/fast_rx1 300   # rx1
sudo ./_run_rx2_sync.sh   _fast_rx2.conf                            /tmp/fast_rx2 300   # rx2, parallel
python3 merge_receivers_simtime.py /tmp/fast_rx1/oaiue_reports.jsonl \
        /tmp/fast_rx2/oaiue_reports.jsonl /tmp/merged.jsonl 150
/home/sens/NICOLA/repos/isac/target/release/isac-track --harmonic-reject \
        --out /tmp/tracks.jsonl replay /tmp/merged.jsonl
python3 score_world_tracks.py /tmp/tracks.jsonl /tmp/fast_rx1 15
```

---

# 5. Sub-slot CFR sampling (2026-07-25) -- lifts the Doppler ambiguity limit

**What it does.** The slow-time sample rate IS the radar PRF and caps the unambiguous bistatic
range-rate at `c/(2*fc*t_slow)`. The data-aided tap was collapsing all 14 OFDM symbols of a slot into
ONE row (t_slow = 0.5 ms -> +/-48 m/s, so anything past ~24 m/s of target speed aliases). Data-aided
reconstruction already gives H = Y/X at every data RE, i.e. at every symbol, so the tap now emits one
row per symbol GROUP: `[sensing] subslot_symbols = N` (0 = off, legacy one-row-per-slot).

**Sparsity + SNR handling (the reason grouping is adaptive, not fixed).** A shorter row integrates
fewer REs, so it is both sparser in frequency (poorer range profile) and lower SNR (~10log10(N_re) of
coherent gain). Emitting such rows is worse than useless -- they inject noise into the slow-time
sequence and feed CFAR. So a group keeps ABSORBING the next symbol until it passes BOTH:
- `subslot_min_re`     -- SPARSITY gate: minimum REs for a row to stand alone.
- `subslot_min_snr_db` -- SNR gate: estimated post-integration SNR, computed as
  `10log10( N_re * (mean|Y|^2 - nvar)/nvar )`. Both terms are in the demodulator's own received-signal
  units, so this is a real dB figure, not a scale-dependent fudge.
A tail group that can never pass is merged backwards instead of emitted.

**Plumbing.** New `nr_isac_submit_cfr_at(slot_idx, slot_frac, ...)` (the old call is this with
frac = 0, so every existing caller is untouched); `sensing_slot_t.slot_frac`; the engine's slow-time
position/`cpi_slot_span` are now FRACTIONAL so several groups from one slot land on distinct rows at
their true times instead of merging.

**Measured (fast scene, 300 s, rx1):**

| config | T_slot | vel_max (median) | CPIs | det coverage | raw precision |
|---|---|---|---|---|---|
| off (1 row/slot) | 1.65 | +/-48 m/s | 37 | 100% / 100% | 28% |
| `subslot_symbols=3`, min_re 600, min_snr 5 dB | 0.45 | **+/-176 m/s** | 151 | 99% / 95% | 18% |
| `subslot_symbols=6`, min_re 1500, min_snr 12 dB | ~0.9 | **+/-89 m/s** | 77 | 100% / 97% | 29% |

So the gates give a real, monotonic knob: **aggressive grouping buys 3.7x unambiguous velocity at a
precision cost; conservative grouping buys 1.8x at NO precision cost** (29% vs the 28% baseline) --
which is the setting to prefer unless a specific scene needs the full range. Both keep detection
coverage. A 50 m/s target (bistatic rate up to 100 m/s) is unambiguous from `subslot_symbols=6`
upward, which was impossible before at any setting.

**Latent bug this exposed (important).** `nr_modulation()` and the LDPC encoder store through AVX2
intrinsics that fault on a misaligned buffer, and the tap's `coded_bits` / `scrambled` / `mod_syms`
are `static __thread` -- so their alignment depended on the TLS block layout, i.e. on every other
`__thread` object in the file. It was ACCIDENTALLY correct. Adding the sub-slot bookkeeping arrays
shifted the layout and produced an immediate GP fault (`SIGSEGV` in `nr_modulation`'s
`out128[i] = ...` store) on the first PDSCH after RA. All four buffers are now explicitly
`__attribute__((aligned(32)))`. If you add `__thread` state to that function and it starts crashing
in a SIMD routine, this is why.

---

# 6. Per-range-row CFAR (`cfar_per_row`, 2026-07-25) -- the ghost fix that finally worked

**Diagnosis came from the RVM animation, not from theory.** Rendering the sub-slot run frame by frame
(`make_tracking_gif2.py`, scene-agnostic: it parses ground truth and the per-CPI axis scaling straight
out of `ue.log`) showed the surviving ghosts are **not scattered false alarms**. Every one of them sits
on a bright **horizontal RIDGE at a real target's own range**, spanning the whole velocity axis: one
scatterer's energy smeared along slow-time. They are the target's own signal mis-attributed to wrong
velocities -- which is exactly why every threshold knob failed on them (lowering
`cfar_target_fa_per_cpi` raised RAW precision but left TRACK precision flat: the ridge is signal, and a
pfa knob only starves noise).

**The fix is the mirror of a mechanism we already had.** `cfar_per_column` trains along RANGE, so it
rejects the *vertical* same-velocity pedestal. A ridge runs perpendicular to that and passes straight
through. `cfar_per_row` trains along DOPPLER (same range row, guard band excluded), which makes the
ridge its own noise floor so only a peak genuinely standing above it survives. Complementary -- enable
both to bracket a strong target in each axis. Implementation mirrors the column test exactly in
`range_doppler.cc`'s `cfar()`, and the reported SNR now uses whichever of the three noise references
(2-D box / column / row) is the strongest, so a ridge survivor is never over-credited.

**Measured -- same binary, same scene, same 300 s, only the flag differs:**

| | `cfar_per_row=0` | `cfar_per_row=1` |
|---|---|---|
| raw detections | 509 (358 false) | **302 (153 false)** |
| raw precision | 30% | **49%** |
| track precision | 25% | **54%** |
| obj0 / obj1 detection coverage | 97% / 99% | 95% / 96% |
| obj0 / obj1 TRACK coverage | 68% / 100% | **99% / 99%** |

**Track precision more than doubled (25% -> 54%) and false detections dropped 58%, while track
coverage went UP, not down** (obj0 68% -> 99%: with the ridge suppressed the tracker stops being
dragged off the real target by co-range ghosts). This is the single largest precision gain of the whole
2026-07-25 investigation, and it cost one CFAR test -- after four more elaborate mechanisms
(separable CLEAN, occupancy-aware CLEAN, flicker TBD, absolute speed gating) each failed or came out
neutral. **Recommend enabling it by default alongside `cfar_per_column`.**

The lesson worth keeping: the win came from *looking at the data* (rendering the RVM) rather than from
reasoning about which algorithm ought to help. Every mechanism tried before that was aimed at a ghost
family that was not the dominant one.

---

# 7. Fusion A/B for `cfar_per_row` (4 reps x 2 configs, 2026-07-25) -- per-receiver win does NOT transfer

Prediction going in: since spurious ellipse intersections scale with the product of the two receivers'
false-detection counts, halving false detections per receiver should cut spurious births ~4x.
**That prediction was wrong.** Measured (4 paired reps, both receivers simultaneous, 270 s each):

| | per-rx1 precision | WORLD precision | world track ids | dets/CPI/rx |
|---|---|---|---|---|
| `cfar_per_row=1` | **46.8 ± 2.2 %** | 30.2 ± **20.8** % | **14.2 ± 1.9** | **3.8** |
| `cfar_per_row=0` | 28.0 ± 0.8 % | 36.0 ± 9.8 % | 46.0 ± 5.5 | 7.5 |

What is solid:
- **Per-receiver, `cfar_per_row` is unambiguous**: 46.8 ± 2.2 vs 28.0 ± 0.8, non-overlapping, tight.
  Keep it on. Section 6's single-run result reproduces.
- **Fragmentation improves 3.2x** (14 vs 46 track ids for 2 targets), and matched position error is
  usually better (1.2 / 2.4 / 12.9 / 2.9 m vs 4.3 / 2.2 / 6.3 / 1.2 m).

What is NOT true:
- **World-frame precision does not improve** -- nominally lower and, more importantly, **wildly
  unstable** (8 % to 58 % across identical reps, SD 21 vs 10). Rep 3 essentially failed to fuse obj0
  at all (1 matched update, 131 m median error).

**Root cause, measured not guessed:** `cfar_per_row` is a second gate stacked on the existing pfa
budget, so it does not just remove ridge ghosts -- it halves the TOTAL detection density feeding
fusion (**3.8 vs 7.5 detections per receiver per fused CPI**, paired-CPI count essentially unchanged
at 58 vs 63). Track birth needs coincident detections from BOTH receivers in the same fused CPI; at
~3.8/CPI with 2 targets the birth step is starved, so births become sporadic and a single bad pairing
produces a persistent ghost that then dominates a small update count. **The bottleneck has moved from
detection quality to birth robustness, and the failure mode is detection STARVATION, not excess.**

That reverses the fix direction from what section 6 suggested. Do NOT tighten birth further. Instead:
1. **Restore detection density while keeping the ridge suppressed** -- raise `cfar_target_fa_per_cpi`
   to compensate for the second gate, targeting ~7-8 detections/CPI/rx with `cfar_per_row=1`. Cheapest
   test, and it directly addresses the measured cause.
2. **Harden the fused lifecycle** -- more supporting CPIs before a world track confirms, so a fragile
   mis-birth cannot immediately confirm and dominate.
3. **A 3rd receiver** -- more pairs means both more coincidences AND over-determined birth (which also
   finally activates `birth_max_rate_rms_mps`, inert at 2 pairs).

Methodological note: the single-run section-6 result was per-receiver and reproduced perfectly. The
*fusion* claim was the one that needed 4 reps -- a single fused run would have read anywhere from 8 %
to 58 % and "confirmed" whatever was expected.

---

# 8. Density restoration (single run, 2026-07-25): the knob works, and it re-frames what `cfar_per_row` buys

Section 7 measured that `cfar_per_row=1` starves fusion (3.8 vs 7.5 detections/CPI/rx). Fix tried:
keep `cfar_per_row=1` but re-open the CFAR budget (`cfar_target_fa_per_cpi` 4 -> 16, adaptive loop
OFF so the budget is actually honoured -- with the loop ON its [2,60] band never bound and density
stayed pinned).

**Density restored, exactly as intended: 7.0 detections/CPI/rx** (was 3.8; `per_row=0` reference 7.5).
The knob does what it says.

The more interesting result is what happens to precision at MATCHED density:

| config | dets/CPI/rx | raw precision | TRACK precision |
|---|---|---|---|
| `per_row=0`, fa=4 | 6.8 | 30 % | 25 % |
| `per_row=1`, fa=4 (starved) | 3.8 | 47 % | 54 % |
| `per_row=1`, fa=16 | 7.0 | 25 % | **48 %** |

**Read this carefully, it corrects section 6.** Most of `cfar_per_row`'s apparent *raw*-precision gain
was a density/threshold effect: re-open the budget and raw precision falls back to the `per_row=0`
level (25 % vs 30 %). But **TRACK precision does NOT fall back -- it stays at 48 % vs 25 %, i.e. still
~2x, at matched detection density.** That is a genuine discrimination gain, and the mechanism explains
why: ridge detections are *kinematically coherent* (co-range, spread across velocity), so they readily
confirm false TRACKS, whereas ordinary false alarms scatter and never confirm. Removing ridges
therefore helps the tracker far more than it helps raw counts. Judge `cfar_per_row` on track
precision, not raw precision.

Fused world frame this run: 33 % precision, 19 ids, 5.7 m matched error -- squarely mid-band for a
metric that ranged 8-58 % across identical reps in section 7, so **a single run says nothing
conclusive about fusion here**; density recovery is the only claim this run supports (that one is a
direct measurement, not a noisy ratio). Confirming whether restored density actually stabilises fusion
needs the multi-rep batch.

Recommended config going forward: `cfar_per_row=1` WITH a re-opened budget (fa ~16, adaptive loop off
or its min_det raised), since it keeps the track-level discrimination while feeding fusion enough
detections to birth from.
