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
