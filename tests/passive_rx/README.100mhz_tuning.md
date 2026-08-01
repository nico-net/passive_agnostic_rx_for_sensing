# Making the 100 MHz 2-receiver AoA case actually produce data (2026-07-29)

Record of what was wrong and what was measured, because most of it is counter-intuitive and all of
it was found by measurement rather than reasoning.

## Starting point

The 100 MHz full-passive arms produced almost nothing: `passive_3rx` yielded 1 fused track,
`passive_2rx_aoa` yielded 0. Initial read was "the AoA/chi2 gating is too aggressive". That was
wrong — or rather, it was the last link in a chain of four independent problems.

## 1. iperf3 was burning 25% of the machine

**Measured**: `iperf3 -c ... -u -b 6M` costs **100-102% CPU** — one full core per stream — and this
does not change with `--pacing-timer` (1000 / 20000 / 50000 us all measured at 100%), with a 64 KB
block size, or in TCP mode. It busy-spins. Three active UEs meant three of twelve cores went to
generating 18 Mbit/s, on a box where the rfsimulator federation was the actual bottleneck.

**Fix**: `udp_dl.py` (this directory), an absolute-deadline sleep-paced UDP sender.
**Measured: 1.0% CPU sender + 0.4% receiver for the identical 6.00 Mbit/s, zero loss.** Selected by
`TRAFFIC=udp` (now the default); `TRAFFIC=iperf3` restores the old behaviour.

**Result**: sim-time throughput **2.8% -> 6.7% of real time**, and ~7.6x more CPIs per wall second.

## 2. CPU pinning crashes this OAI build — do not re-enable without fixing upstream

Pinning each softmodem to a disjoint core set with `taskset -c` looked like the obvious next step.
It **crashes**: a netns'd active UE aborts during startup at `common/utils/system.c:273`,
`AssertFatal` on `pthread_setname_np()` returning **ENOENT** — the just-created thread's
`/proc/self/task/<tid>` is already gone, i.e. it exited before being named. A latent startup race
that a constrained CPU set makes very likely. `PIN=0` (the default) attaches all 3 UEs reliably;
`PIN=1` does not. The mechanism is kept in `pin_for()` because the idea is sound, but the fix
belongs in OAI (that assert should tolerate ESRCH/ENOENT), not here.

## 3. The velocity axis is load-dependent when two RS sources are mixed — this is the big one

`vel_max` was swinging **0.7 to 27 m/s CPI-to-CPI**, which makes it impossible to choose a target
speed: the zero-Doppler notch is `zero_doppler_guard * vel_res` and the alias limit is `vel_max`,
and with a 30x spread there is no speed inside both for every CPI.

**Mechanism** (from the `occ[csi=N ...]` counts — note `dmrs=0` is the known cosmetic gap, blind
PDCCH rows are not counted there):
- blind-PDCCH rows arrive **with DL grants** — traffic- and CPU-dependent.
- CSI-RS rows arrive on a **strict 160-slot (80 ms) lattice**.
- When the receiver had CPU, blind-PDCCH dominated (`csi=1` of 32 rows) -> dense grid -> `vel_max`
  ~18-22 m/s. When it starved, CSI-RS filled the grid instead (`csi=3..21`) -> sparse grid ->
  `vel_max` ~0.8. Same config, same binary; only load differed.

**Fix**: use ONE source. With `sources = "csi_rs"` the measured `vel_max` is **exactly 0.500 m/s in
every CPI** (0th and 100th percentile identical). The axis becomes designable.

This matters for OTA too: the same instability will exist on real air, so the source choice must be
deliberate there as well.

## 4. The old scene was undetectable by construction

The inherited scene had one target at 0.1 m/s -> bistatic range rate ~0.17 m/s, which fell **inside
the zero-Doppler notch in 70% of CPIs**. No fusion or gating tuning could have recovered it, and
some of the poor full-passive numbers were measuring that, not the algorithm.

`design_scene.py` now designs and **validates** a scene against the measured CPI geometry, checking
at every trajectory instant and at both receivers: dR clear of the zero-range notch, dR inside the
255-tap CIR span (622 m), `|dR/dt|` clear of the zero-Doppler notch AND under `vel_max`, and bearing
inside the estimator's scan window. It prints per-constraint pass/fail. Run it before trusting any
scene.

`check_detection.py` closes the loop after a capture: per-CPI, ground-truth-referenced (paired on
`utc_ns`, never on the drifting `t=`), it reports what fraction of CPIs actually contain a detection
at the target's true range AND velocity, plus bearing error. **This is the check to run before
committing hours to a capture.**

## 5. CFAR pfa was never rescaled for the wider band

False alarms scale with the CELL COUNT, `nof_range * nof_doppler` = 3276*32 ~ **105k at 273 PRB** vs
~41k at 106 PRB. The inherited `cfar_pfa = 1e-4` therefore yields ~10 expected false alarms per CPI
at 100 MHz, filling most of `max_detections = 16` before a real target gets a slot. Set to `1e-5`
(~1 expected FA/CPI, the same order the 106 PRB runs had).

**Honest caveat**: lowering pfa 10x did NOT reduce the observed detection count (454 -> 502 per 34
CPIs). So the dominant clutter was never thermal false alarms — see below.

## 6. Blind-PDCCH false accepts inject broadband clutter

With `sources` including `pdsch_dmrs_blind`, detections spread across the whole range axis:
median **2486 m**, p90 **4920 m**, while the targets sit at 62-287 m. Removing the source collapsed
that to median **300 m**, p90 **769 m**.

**Mechanism**: a falsely-accepted RNTI reconstructs the WRONG reference X, so `H = Y/X` is
high-power garbage spread over every range bin — structured energy, not noise, which is why the pfa
change did not touch it. The `energy_min` gate that would suppress these is currently **disabled
(0)** by explicit decision (it should become adaptive rather than a hand-tuned constant), so nothing
is filtering them.

**Consequence to be explicit about**: the blind-PDCCH source is live-verified working (exact RNTI
matches against the gNB log) but at 100 MHz its false-accept clutter currently swamps the range
axis. Clean fusion data needs `sources = "csi_rs"` until `energy_min` is made adaptive. That is a
real, still-open limitation, not a tuning preference.

## Resulting configuration

`ue.passive{,2}.aoa.100mhz.conf`, selected by `CONF_TAG=.aoa`:
- `sources = "csi_rs"` — deterministic 80 ms slow-time lattice, `vel_max` 0.500 m/s exactly
- `cfar_pfa = 1e-5` — rescaled for the 273-PRB cell count
- two patrolling targets designed for that velocity window (`|dR/dt|` 0.20-0.29 m/s, dR 77-400 m),
  moving in opposite directions so they separate in Doppler where they cross in range
- 4-element lambda/2 ULA on both receivers (the ULA, not the 2x2 UPA — see README.upa.md for why
  the ULA is the right pre-OTA choice: 3x better bearing CRB, and the mirror ambiguity is a
  non-issue when the scene sector is known)

Cost of the deterministic grid: CPI is 2.56 s of SIMULATED time, and sim time runs at ~3-7% of real
time, so expect roughly **1 CPI per 40-100 s of wall clock per receiver**. Budget accordingly — a
few hundred CPIs is a multi-hour capture. That is the honest throughput of 100 MHz + 3 active UEs +
2 four-antenna receivers on 12 cores.

## 7. Adaptive energy gate — and what it revealed about blind-PDCCH (2026-07-29)

`energy_min` was an ABSOLUTE threshold in receiver-dependent units, so it could not be carried
between bandwidths or gain settings and had simply been disabled (0). Replaced with
`energy_adapt_factor` (optional 5th field of `pdcch_blind_monitor_noise_gates`, so 4-field configs
still parse): threshold = `factor * running estimate of the noise-floor candidate energy`.
Dimensionless, therefore portable, therefore safe to ship a default for (2.0) in a way the absolute
value never was.

The estimator tracks the **median**, not the mean, via a frugal streaming update (O(1) time and
state, no allocation -- the RT path allows nothing else) with a step RELATIVE to the current
estimate, making it scale-free. Unit-verified: 0.3-4.2% error, and identical accuracy across a
250,000x gain range. The median matters and is not a detail -- at 25% cell load the true median is
11.7 while the mean is 262, so a mean-based floor would sit 22x too high and progressively suppress
the very grants the gate exists to find.

**LIVE-VERIFIED working**: `held[energy=300694]` where it was previously 0, `efloor` converging to a
stable 0.69, and detection clutter collapsing from median 2486 m to 886 m.

**And that is how it disproved the case for blind-PDCCH here.** With false accepts removed,
`cfr_submits` fell from ~1000-2000 to **119**. The genuine grant rate is that low; the rest was
noise. A sparse slow-time grid takes far longer to fill a CPI, so `vel_max` collapsed to 0.1-0.4 m/s
and only 3 CPIs completed in 900 s (versus ~30 before, and 9 for csi_rs).

So the row density that made blind-PDCCH look attractive -- `vel_max` 14-27 m/s, plenty of CPIs --
was **substantially manufactured by false accepts**. Properly gated it delivers a LOWER PRF and ~10x
fewer CPIs than csi_rs. This is structural, not a tuning problem: lowering `energy_adapt_factor`
would only re-admit the noise that produced the illusion.

**Consequence**: `sources = "csi_rs"` is the right choice for this deployment, and that is now a
MEASURED conclusion rather than the untested assumption it was in section 6. The blind-PDCCH decode
path itself remains live-verified correct (exact RNTI matches against the gNB log) -- it is the
sensing DUTY CYCLE that is inadequate here, not the decoder. On a busier cell, with genuinely more
DL grants, the balance could change; re-measure `cfr_submits` before assuming either way.

## 8. Blind-PDCCH, properly measured (2026-07-29) — supersedes section 7's verdict

Section 7 concluded blind-PDCCH's grant rate was "structurally too sparse". **That was measured on a
nearly-idle cell and is withdrawn.**

**The harness was not delivering the traffic it reported.** `udp_dl.py`'s RECEIVERS were dying at
startup on `bind()` -> EADDRNOTAVAIL (the UE address is not on `oaitun_ue1` yet when the receiver is
launched one second after the UE reports its IP). The tracebacks went to `udp_server_*.log`, which
nothing read, while the SENDERS all reported a clean 6.00 Mbit/s -- so the failure was invisible.
The gNB's own counters showed the truth: `dlsch_rounds` of 1523 / 56 / 59, i.e. two of three UEs
were idle and the cell was carrying about one UE's worth of load.

Fixed: the receiver retries the bind for 60 s and falls back to `0.0.0.0`; and `run_passive_rx.sh`
now prints per-UE `dlsch_rounds` 20 s into every run with a loud warning if the cell is idle. A 20 s
check is nothing against a multi-hour capture, and this class of failure had already invalidated
one set of conclusions.

**With traffic actually flowing** (dlsch_rounds 4489/6191/6036): `cfr_submits` 119 -> **1957**, CPIs
3 -> **61 per 1800 s**. So the grant rate was indeed traffic-limited, not structural.

**A second error of mine, in the opposite direction from section 4**: the blind scene was designed
against `vel_max` 13.8-27 m/s, but that figure came from runs where FALSE accepts arrived every slot
and inflated the row density. Real grants land ~46 ms apart, giving `vel_max` **0.7 m/s**. Targets
at 5.4-11.5 m/s were therefore outside the Doppler window -- the same mistake as the original 0.17
m/s scene, just overshooting instead of undershooting. Always size a scene against the velocity axis
the CLEAN configuration produces, never one measured with the noise still in.

**The honest head-to-head**, same loaded cell, same adaptive gate, same DSP options, each source
given a scene matched to its OWN measured velocity window:

| | csi_rs | pdsch_dmrs_blind |
|---|---|---|
| CPIs / 1800 s | ~18 | **64** |
| target detection | **56-78 %** | 8-14 % |
| bearing error, median | **0.05-0.26 deg** | 2.5-45 deg |
| bearing p90 | <= 0.51 deg | 38-49 deg |

Blind-PDCCH works -- it detects both targets. It wins on CPI count and loses decisively on quality.
**Likely cause of the bearing collapse**: every blind-PDCCH row comes from a different grant with a
different PRB allocation, so the frequency support changes from row to row, whereas CSI-RS always
occupies the same comb. Inconsistent support across a CPI degrades coherent integration, and AoA --
which lives on precise inter-element phase -- degrades hardest. A 45 deg bearing is useless to
fusion, which is the entire reason the array exists. (Not yet confirmed by direct experiment; it is
the mechanism most consistent with high CPI count but poor phase coherence.)

**Conclusion: `sources = "csi_rs"` for AoA/fusion work** -- now on a like-for-like measurement rather
than section 7's artifact-driven reasoning. Blind-PDCCH remains valuable where bearings are not
needed, and its decoder is unaffected by any of this (exact RNTI matches against the gNB log).
