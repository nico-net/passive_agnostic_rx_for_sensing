# Fictitious-gNB sensing simulation

A fully synthetic, no-SDR, no-core-network end-to-end test of the NR_UE_ISAC passive sensing
pipeline: a core-less gNB (`nr-softmodem --do-ra`) and this repo's UE (`nr-uesoftmodem --do-ra`)
talk over the rfsimulator TCP loopback, with a synthetic **moving-target channel**
(`openair1/SIMULATION/TOOLS/sensing_channel.{h,c}`) injected on the downlink path. One or more
objects follow configured piecewise-linear trajectories; the resulting time-varying bistatic CIR
(direct path + reflections) is synthesized into the channel every rfsimulator block, so the UE's
**real** RT PHY procedures (SSB sync, MIB/SIB1, RA, CSI-RS reception) see a physically consistent,
controllable scene — not a CFR value injected directly into the ISAC pipeline (that's what
`selftest`/`selftest_targets`/`selftest_los` already do; see `openair1/PHY/NR_UE_ISAC/README.md`
and `docs/NR_UE_ISAC_sync_gap_analysis.md` section 12 for that, separate, offline harness).

## Why `--do-ra`, not `--phy-test`

`--do-ra` runs a real RACH (Msg1-Msg3) with no core network — closer to "behaves like an actual
gNB" than `--phy-test` (which skips RA entirely and treats the UE as pre-connected). Confirmed
live in this checkout: RA completes ("RA procedure succeeded. CFRA: RAR successfully received.").

**Correction (2026-07-22), superseding an earlier assumption made before end-to-end testing**:
`--do-ra` does **not** leave RRC unconnected. `executables/nr-ue.c`'s `init_NR_UE()`
unconditionally calls `init_nsa_message()` (`openair2/RRC/NR_UE/main_ue.c`) whenever
`phy_test || do_ra` is set. That function `fopen()`s `./reconfig.raw` + `./rbconfig.raw` (repo-root
fixtures — `AssertFatal`s if missing, i.e. `--do-ra` cannot run at all without them), UPER-decodes
them as a genuine `NR_RRCReconfiguration_t`, and feeds it through the *same*
`nr_rrc_ue_process_rrcReconfiguration()` a real over-the-air message would use. This populates a
real `mac->sc_info.csi_MeasConfig`, so the UE's **own** RRC-configured CSI-RS path
(`nr_ue_csi_rs_procedures`, gated on `csirs_vars.active`) genuinely activates under `--do-ra` —
confirmed live, capturing a real NZP-CSI-RS occurrence spanning the full 106-PRB carrier
(`nof_re=106`, `comb=12`, i.e. density="one" over the whole BWP).

The original plan (before this was discovered) was to use `csirs_monitor`
(`openair1/PHY/NR_UE_TRANSPORT/nr_csirs_monitor.c` — RRC-independent, config-parsed, cell-common
CSI-RS capture) on the theory that RRC never completes under do-ra. Running that *in addition to*
the (unexpectedly active) own-CSI-RS path fed two differently-scoped CSI-RS occurrences (24 PRB
hand-derived vs. 106 PRB canned-from-`reconfig.raw`) into the same `sources="csi_rs"` fused CFR
grid — an untested combination that corrupted the very first CPI (top detection 365 m against a
true 88.7-164.3 m ground truth). **Fix**: `csirs_monitor` is not used in these configs; the
own-CSI-RS path (real, reconfig.raw-driven) is the sole CSI-RS source. `csirs_monitor` itself
remains correctly implemented and unit-tested — see `CLAUDE.md`'s "CSI-RS UE-agnostic monitor"
note for the live-cell (non-simulated, genuinely-RRC-less) scenario that actually motivated it.

## Which process logs what

The DL (gNB->UE) channel model, and therefore the synthetic sensing channel and its ground-truth
log line (`SENSING_CHANNEL gt: ...`), is applied on the **rfsimulator client side** — i.e. inside
`nr-uesoftmodem`, not `nr-softmodem`. Source-verified: `radio/rfsimulator/simulator.cpp`'s
`legacy_model_name` ternary maps `role==SIMU_ROLE_SERVER (gNB) -> "rfsimu_channel_ue0"` and
`role==SIMU_ROLE_CLIENT (UE) -> "rfsimu_channel_enB0"`, and `set_channeldesc_direction(...,
role==SIMU_ROLE_SERVER)` sets `is_uplink`, so on the UE side `is_uplink=false` and the DL model
(`rfsimu_channel_enB0` — where the sensing channel attaches) is local to the UE process. **Both
ground truth and NR_UE_ISAC's own detections/DetectionReports therefore end up in the UE's log and
output files, not the gNB's** — `[sensing_channel]` and `[sensing]` both live in
`ue.sensing.rfsim.conf`, not the gNB config.

## Files

- `gnb.sensing.rfsim.conf` — core-less gNB (do-ra), band 78, 106 PRB, PCI 2, `do_CSIRS=1`. Derived
  from this repo's own `ci-scripts/conf_files/gnb.band78.106prb.rfsim.phytest-dora.conf`.
- `ue.sensing.rfsim.conf` — UE side: `[rfsimulator]` (client, `options=(chanmod)`), `[channelmod]`
  (base AWGN model named `rfsimu_channel_enB0`, low noise), `[sensing_channel]` (the synthetic
  moving-target channel — geometry, objects, fractional-delay kernel), `[sensing]` (NR_UE_ISAC,
  own-CSI-RS path only — see the correction above).
- `ue.sensing.static.rfsim.conf` — same as above but with a single, zero-velocity object, and its
  own `out_path`/`report_path` (`/tmp/sensing_sim_static*`). Use this first: it isolates the
  tap-synthesis/detection math from CPI-duration-vs-target-dynamics effects (see "Known
  limitation" below).
- `run_sim.sh [duration_s] [out_dir] [ue_conf_name]` — launches both processes, waits, tears down,
  prints a summary (RA outcome, last few ground-truth lines, last few CPI summaries).
  `ue_conf_name` defaults to `ue.sensing.rfsim.conf`; pass `ue.sensing.static.rfsim.conf` for the
  static baseline. Must be run with CWD containing `reconfig.raw`/`rbconfig.raw` (repo root).
- `compare_ground_truth.py` — parses the UE log's ground-truth and DetectionReport JSON-lines,
  reports the range/velocity error of the nearest detection per ground-truth sample.
- `ue.sensing.traffic.rfsim.conf` / `run_sim_traffic.sh` — DL-traffic-injection variant (`--noS1`
  + network namespaces + `ping`, PDSCH-fused sensing). See "DL traffic injection" below.

## Running

```bash
cd /home/sens/NICOLA/openairinterface5g   # reconfig.raw/rbconfig.raw must be in CWD
cmake --build cmake_targets/ran_build/build --target nr-softmodem nr-uesoftmodem -- -j$(nproc)
./tests/sensing_sim/run_sim.sh 320 /tmp/sensing_sim_static ue.sensing.static.rfsim.conf
python3 tests/sensing_sim/compare_ground_truth.py /tmp/sensing_sim_static/ue.log
```

No `sudo` needed for the rfsimulator path in this environment (confirmed: only a "no SYS_NICE
capability" performance warning, not a hard requirement). Expect ~300-600 s wall-clock for one CPI
to close in this sandbox (PHY DSP runs ~5-8x slower than real-time here; the own-CSI-RS occurrence
period works out to 160 slots = 80 ms, so 256 CPI rows need ~20 s of *simulated* on-air time).

## Scenario in the default configs

TX (illuminator) at ENU (0,0), RX (UE) at ENU (100,0) -> `R_los = 100 m`. One object moves from
(50,80) to (150,80) over 20 s (constant velocity along x). Bistatic differential range sweeps
~88.7 m -> ~164.3 m over the run. Compare the `range_rate` the ground-truth log reports against the
detected `vel_mps` in `<out_path>_detections.csv` / the DetectionReport JSON-lines.

The static variant (`ue.sensing.static.rfsim.conf`) places one fixed object at (120,60) ->
`dR = 97.41 m`, `range_rate = 0.000 m/s`, held for the whole run.

## DL traffic injection (`run_sim_traffic.sh`, 2026-07-22)

The original comb-12-CSI-RS-only scenario above showed a spurious detection at a fixed range that
didn't track the injected object. The initial theory (a `range_doppler.cc` comb/de-aliasing bug)
turned out to be **wrong** — disproved by actually injecting real DL traffic and testing with a
denser fused reference. Two new files enable this:

- `ue.sensing.traffic.rfsim.conf` — same static-object scenario, `sources` fusion instead of
  CSI-RS-only, `--noS1` TUN addressing baked in.
- `run_sim_traffic.sh [duration_s] [out_dir]` — launches gNB+UE with `--noS1` (in addition to
  `--do-ra --rfsim`) so each gets a real IP tunnel (`oaitun_enb1`/10.0.1.1,
  `oaitun_ue1`/10.0.1.2 — **not** `oaitun_gnb1`, despite what some docs/searches suggest), then
  drives a `ping` across them so PDSCH actually gets scheduled. **Requires sudo and two Linux
  network namespaces + a veth pair**: per `doc/runmodem-nrue.md`'s explicit warning, `--noS1`
  doesn't work with both interfaces on one host/namespace (the kernel can't tell which local TUN a
  packet is "for"), so each process runs in its own netns (`oai_isac_gnb`/`oai_isac_ue`), joined by
  a veth pair (192.168.100.1/.2) that also carries the rfsimulator TCP loopback.

Confirmed live: `ping` gets real replies, and CPI summaries show real `pdsch_data` occurrences
(`occ[csi=... dmrs=0 data=200+]`) — DL traffic is genuinely flowing and being captured.

## What was actually wrong (superseding the original "comb-12 aliasing" theory)

Systematically root-caused via the raw `_rvm_N.f32` rasters (not just the CPI summary line),
across CSI-RS-only, PDSCH-fused, wider-guard, and boosted-target-reflectivity runs:

1. **Not aliasing.** With dense (comb-1) PDSCH data dominating occupancy — where comb-related
   range aliasing is structurally impossible — the *same* spurious near-fixed-range detection
   persisted. This ruled out the original `range_doppler.cc` de-aliasing-taper theory entirely.
2. **Real cause #1 — LOS leakage skirt wider than the guard.** The raw range profile showed a
   single, smooth, slowly-decaying skirt around the (correctly notched) LOS position, not fully
   flat again until ~50+ bins (~200 m) out — far wider than `zero_range_guard = 3`. This matches
   the *exact same lesson* already recorded in `CLAUDE.md` from real OTA testing (B210 LOS/CFO
   artifact, fixed by widening `zero_range_guard` 2→9). Fix: widen `zero_range_guard` (15 here;
   still not fully sufficient — see below).
3. **Real cause #2 — CA-CFAR self-masking.** Once the target's true signal was made unmistakably
   large (test-only 20x reflectivity boost), the raw range-Doppler map's global maximum (by single
   cell, not row-sum) *did* land correctly at the target's true range in every CPI — the channel
   injection and CFR pipeline are correct. But the target's own response spans ~9 range bins
   (comparable to `cfar_guard = 4`), so CA-CFAR's local-noise estimate around the target's own peak
   was contaminated by the target's own skirt (a known CA-CFAR failure mode for wide/smeared
   targets), suppressing its apparent SNR below isolated, weaker artifacts sitting in genuinely
   quiet neighborhoods. Fix: widen `cfar_guard` (16) to fully contain the target's own spread —
   this alone made the true target win as the CFAR top-1 pick in some (not yet all) CPIs.
4. **Real cause #3 — comb-12 CSI-RS rows' own periodic mirroring.** Even with the above fixes, a
   smaller recurring artifact remained at a *fixed* range (~365 m) but with **randomly varying
   velocity from one CPI to the next** — the signature of noise rather than a real (necessarily
   zero-Doppler, for this static scenario) target. This is consistent with the row-native comb-12
   CSI-RS occurrences (still ~15% of rows even with PDSCH flowing) re-injecting a copy of the same
   LOS-skirt phenomenon, reflected within their own `nof_range/comb`-bin valid window. Dropping
   `csi_rs` from `sources` (keeping only `pdsch_dmrs,pdsch_data`, both dense enough to stand alone
   once real traffic is flowing) **eliminated this artifact completely** — confirmed by an
   `occ[csi=0 ... data=256]` run showing no trace of it.

## The "mirror" ghost: root-caused + fixed (2026-07-22)

With `csi_rs` dropped (PDSCH-only), a distinct artifact appeared: a high-SNR detection near
`nof_range - target_bin` (~4930 m, the mirror of a ~90 m target). Root-caused by inspecting the raw
RVM across static and moving runs:

- **It is a conjugate image.** The range IFFT of a CFR that carries any real-valued
  (conjugate-symmetric) component produces energy at both bin `r` and bin `nof_range-1-r`. The
  real-valued component here is the near-zero-Doppler residual a **static / slow** scatterer leaves
  after clutter (slow-time mean) removal: its phase `exp(-j2πR/λ)` is constant across slow-time, so
  what survives mean-subtraction is essentially real. Confirmed decisively by comparing a **static**
  object (mirror is *co-equal* with the true peak) against a **moving** object (mirror drops to near
  noise — the target's genuine Doppler phase progression breaks the conjugate symmetry). The mirror
  sits at `nof_range-1-r` in **range** but pinned near **zero Doppler**, independent of the true
  target's Doppler — exactly the fingerprint of a real-valued-residual image, not a copy of the
  moving return.

- **Fix: conjugate-image rejection** (`range_doppler.cc`, post-NMS; `[sensing] conj_image_reject`,
  default on). A physical bistatic target has **small** differential range (`dR ≥ 0`, near bins) and
  its image lands in the far/upper half near `range_max`; so for any detection pair at each other's
  range-mirror (`nof_range-1-r`, within `conj_image_guard` bins) the **lower-range** member is kept
  and the upper/far one dropped. Range — not SNR — is the discriminator on purpose: the image sits in
  the quiet far-range region and CFAR gives it a *higher* SNR than the real target buried by the LOS
  skirt, so "keep the stronger" fails (verified: it kept the ghost). This cleanly removes the mirror
  when the pair is isolated (verified: clean CPIs collapse to 3-4 detections at the true near-range
  bin). It is defeated only when the near-range partner is itself notched/NMS-suppressed, orphaning
  the far image — a residual limitation, not a wrong result.

## Fusion weighting: inverse-variance (2026-07-22)

CFR fusion previously merged same-slot same-subcarrier collisions by **last-write-wins** (arbitrary).
Now `sensing_engine::accumulate_cpi` combines them by **inverse-variance weighting**
`ĥ = Σ(ĥ_i/σ²_i) / Σ(1/σ²_i)`, so the lower-noise estimate dominates. Each RT tap passes its own noise
estimate through the new `nr_isac_submit_cfr(..., float noise_var)` parameter — CSI-RS its
`noise_power` (`csi_rx.c`), PDSCH its `nvar` (`phy_procedures_nr_ue.c`); an unknown/zero value falls
back to unit (equal) weight, preserving old behaviour. The merge is an incremental running weighted
mean (order-independent). Exercised live with a 3-source config (`occ[csi=… data=…]`, no regression).

## Fixed: true data-aided PDSCH reconstruction (2026-07-22)

The comb-106 (period-12-subcarrier) ripple above was traced to `pdsch_data` merely *resampling* the
DM-RS-**interpolated** channel estimate at comb-1 — the interpolator's own per-PRB seam, not a real
channel feature. Fixed by implementing genuine data-aided reconstruction, per 38.212/38.211: after a
DL-SCH transport block passes CRC (`harq->decodeResult`), the confirmed-correct payload is re-encoded
through the real chain — LDPC encode + rate-match (`nrLDPC_TB_encoding_parameters_t` via
`ue->nrLDPC_coding_interface.nrLDPC_coding_encoder`, the **same** interface/shared-library call the
UE's own PUSCH TX path already uses) + scrambling (`nr_codeword_scrambling`, the same Gold-sequence
XOR the gNB TX side uses) + modulation (`nr_modulation`, already common to both TX/RX) — to
reconstruct the *actually-transmitted* symbol X at every data RE, then Ĥ[k] = Y[k]/X[k] is computed
fresh with no interpolation at all. New function: `nr_isac_pdsch_data_aided_tap()` in
`phy_procedures_nr_ue.c`, hooked in `pdsch_processing()` right after `nr_ue_dlsch_procedures()`
(post-decode) — a genuinely new, correctness-critical hook point; the pre-existing tap this replaces
ran *before* decode and could never have checked CRC. No gNB-only code was duplicated: every
primitive used (`nr_segmentation`, `nr_get_G`/`nr_get_E`, the LDPC encoder interface,
`nr_codeword_scrambling`, `nr_modulation`) was already linked into `nr-uesoftmodem` for its own PUSCH
TX chain (`nr_ulsch_coding.c` was the exact template mirrored here for the DL direction). Scope
(silently no-ops outside these — common do-ra/rfsim conditions, not a correctness risk): CRC must
have passed, single layer, no PTRS, no CSI-RS rate-matching overlap.

**Verified live** (moving-target traffic run, `sources="pdsch_data"`): the comb-106 forest is
**completely gone** from the raw RVM — replaced by smooth, physically-sensible energy clusters. The
true target's raw energy now lands squarely on ground truth in the majority of CPIs (e.g. one CPI's
top-8 bins 25-31 ≈ 98-122 m against a ground truth of 106-112 m at that point in the run). A **bonus,
predicted-by-analogy finding**: with `pdsch_dmrs` also in the source set, its comb-2 rows reintroduced
the *same class* of artifact at their own de-aliasing boundary (`nof_range/2` = bin 636, since
comb=2) — confirmed, then confirmed gone when `pdsch_dmrs` was dropped too (same fix pattern as
dropping `csi_rs` earlier: a comb-N row's own taper boundary leaks unless a denser source stands
alone). **Recommended sources setting is now simply `sources="pdsch_data"`** when real DL traffic is
flowing.

Also added: a new `[sensing]` config path is unnecessary — no new config surface, this is a drop-in
quality improvement to the existing `pdsch_data` source name/semantics (previously misleadingly named
per `CLAUDE.md`'s own caveat; now the name matches the implementation).

## New: sync-compensation logging (2026-07-22)

Each CPI now logs one consolidated line showing all four tracked corrections (previously computed
into the JSON report but never surfaced as a readable log): `SENSING: sync CPI #N STO[...] CFO[...]
SFO[...] LOS[...]`. Verified live: STO fractional-bin residuals stayed small (±0.002 to ±0.1 bins),
CFO residuals single-digit Hz, SFO sub-0.01 ppm, all marked `corrected=yes` — sane, physically
plausible values, not NaN/garbage. See `sensing_engine.cc`'s `process_cpi()` for the exact fields
(mapped from the existing `isac_sync.h` Phase 1-4 tracker outputs: STO=Phase 1 sub-sample delay
drift, CFO=Phase 2 residual carrier offset, SFO=Phase 3 sample-clock error, LOS=Phase 4 closed-loop
delay+frequency bias).

## Practical takeaways for future runs

- `zero_range_guard`/`cfar_guard` (15/16) and the 20x-boosted test target in the traffic configs were
  chosen to make diagnosis tractable, **not** validated at realistic (target ≪ LOS) reflectivity.
- **Prefer `sources="pdsch_data"` alone** when real DL traffic is flowing — it's now genuine
  data-aided reconstruction, denser and cleaner than fusing in `pdsch_dmrs` or `csi_rs` (both
  comb-limited sources reintroduce their own de-aliasing-boundary leakage; see above).
- `conj_image_reject` (default on) removes the conjugate mirror ghost; set it `0` to see the raw
  images. It still gets occasionally defeated when the near-range (physical) member of a mirror pair
  is itself notched/NMS-suppressed, orphaning the far image — unchanged, pre-existing limitation, now
  also observed with the data-aided source (lower overall signal levels made this more visible, not
  worse in absolute terms).
- Inverse-variance fusion weighting is automatic (no config) — matters most when combining sources of
  genuinely different quality (e.g. `pdsch_data` + `csi_rs`), less so for `pdsch_data` alone.

## Regression / A-B knobs

- `[sensing] sync_correction = 0` — reproduces pre-Phase-1 (uncorrected) STO/CFO/SFO behaviour for
  an A/B comparison, per `ota_sync_passive_ue.md`.
- `[sensing_channel] frac_delay_taps = 0` — nearest-bin (not windowed-sinc) target placement, to
  demonstrate the range-walk snapping artifact the fractional-delay kernel avoids (see
  `sensing_channel.c`'s file header).
- `[sensing_channel] enable = 0` — disables the synthetic channel entirely (plain AWGN loopback);
  confirms the feature is strictly opt-in and the rest of the do-ra/rfsim path is unaffected.
