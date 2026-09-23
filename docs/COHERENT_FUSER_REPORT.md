# Coherent Fuser + 3D Tracker — Presentation Report

> **DRAFT, 2026-09-24.** Written from the binding spec
> (`docs/superpowers/specs/2026-09-23-coherent-fuser-tracker-design.md`), the SDD ledger, the merged
> code on `feature/multirx-clean-adaptive` at `50feed7da2`, and the task reports.
> Tasks 1, 2, 3, 4, 6, 7 and 11 are merged, reviewed and unit-tested. Tasks 5 (focusing and detection),
> 8 (UL), 9 (pipeline and engine switch) and 10 (CUDA) were still being implemented when this was
> written. Their sections are marked **(design; implementation in progress)** and describe the plan
> briefs; the finaliser checks them against the merged code.
> `<!-- FILL: ... -->` marks a measured number or a merged-code detail that does not exist yet.
>
> Legend: **[impl]** = merged, reviewed and unit-tested; **[design]** = design; implementation in progress;
> **[not measured]** = no measurement exists yet, on any setup.

<!-- FILL: final branch head commit, and the list of merged commits for Tasks 5, 8, 9, 10 and the final review -->

---

## 1. Executive summary

**What it does.** The receiver is a single USRP X410 with four RX channels on four surveyed antennas:
a 5 m or 10 m square, with antenna heights alternating 0.5 m / 3.5 m in a checkerboard. It listens
passively to a lab 5G NR gNB (n78, 3.45 GHz, 273 PRB, 30 kHz SCS) and never transmits.
For every PDSCH grant it can blindly decode, it rebuilds the transmitted symbols $\hat X$ and measures
the channel $\hat H_i = Y_i/\hat X$ on each of the four antennas. Every 75 ms coherent processing
interval (CPI), the **coherent fuser** treats the four channels as **one sparse, phase-coherent
array**. The four channels share one device, one reference clock and one sample clock. Per CPI it:

- synchronises the four channels to their own measured direct path;
- calibrates the per-channel phase online;
- focuses energy on a 3D voxel grid over the surveillance volume (about 30 m × 30 m × 30 m);
- detects targets with a threshold derived from a declared false-object rate;
- refines positions interferometrically when the calibration is coherent.

A **3D tracker** then builds tracks (position, velocity, $1\sigma$, existence probability), with no
object-type labels. An **autofocus** stage refines the surveyed antenna positions from confirmed
tracks. Every CPI's detections, tracks and coherence diagnostics go to append-only JSONL files and a
live monitor page.

**Design rule (the "golden rule").** There are no tuned constants.
- Every resolution, grid spacing, Doppler span, threshold and noise level is either **derived** from
  the CPI's own allocation and row times, or **estimated** online from the data.
- The only **declared** inputs are:
  - the tape survey and its accuracy `coherent_survey_sigma_m`;
  - the surveillance volume;
  - `maximum_target_speed_mps`;
  - `false_object_intensity_per_s`;
  - conventional statistical quantiles: $\chi^2$ 99 % gates, and SPRT $\alpha = 1\,\%$, $\beta = 10\,\%$.

**What is measured and what is not.**

| Claim | Status |
|---|---|
| Allocation-aware axes, LOS detection, common row sync, range–Doppler (CPU) | **[impl]** unit-tested. LOS error ≤ 0.111 bin over 200 random-hop seeds × 3 CPIs × 4 channels (2400 checks), with a wall stronger than the LOS, and on comb-2 rows |
| Per-channel phase calibrator, predictive fade, coherence gain $G$, $\rho$ | **[impl]** unit-tested (stable, scrambled, random-walk, missing-LOS cases) |
| 3D tracker (CV Kalman, Hungarian GNN, SPRT, online $P_D$) | **[impl]** unit-tested (3/3 targets confirmed, clutter never confirmed, Hungarian = brute force on 3000 trials) |
| Autofocus of antenna positions | **[impl]** unit-tested. Converges from a ±3 cm declared survey; **declines to act** from a 10 cm (tape-grade) survey |
| Monitor coherent page, launcher coherent mode | **[impl]** API-level tested on a fixture. **Not yet painted in a real browser against real producer output** |
| Envelope focusing, derived CFAR, NMS/harmonic merge, coherent refinement | **[design]** Task 5 <!-- FILL: status + test numbers --> |
| UL-illuminated path | **[design]** Task 8, built and **disabled** (`coherent_ul_enable = 0`) <!-- FILL --> |
| End-to-end offline chain (synthetic scene → replay → JSONL) | **[design]** Task 9 <!-- FILL: chain test result --> |
| GPU path, CPU/GPU parity, real-time per-CPI budget | **[design]** Task 10 <!-- FILL: parity + p50/p95 --> |
| **OTA phase coherence of the four X410 channels** | **[not measured]** |
| **Whether a tape survey is good enough for coherent refinement** | **[not measured]**. Physics says it needs about ±1 cm (§4, §7a) |
| **Autofocus convergence on real targets** | **[not measured]** |
| Detection/track accuracy on real people, cars, drones | **[not measured]** |

**The two most important honest caveats for the presentation:**

1. **The survey decides coherence.**
   - **The envelope stage** needs the survey only to ≪ 1.5 m. It is robust to tape errors.
   - **Coherent refinement** needs about ±1 cm, because the phase error is $k\,|\Delta u|\,\delta$ with
     $k = 72.3$ rad/m.
   - **Autofocus** can only fix sub-cm-to-cm errors. Its capture range is $\pi/(3|a|) \approx$ 7 mm – 1.4 cm.
   - A 10 cm tape survey therefore gives envelope-grade positions (about 0.76 m voxels), and autofocus
     reports `af_corr_m = 0`.
2. **Slow targets.** At a 75 ms CPI, one Doppler bin is 1.16 m/s of bistatic path-length rate, and the
   zero-Doppler notch is ±2.32 m/s. A pedestrian whose bistatic path length changes by less than about
   2.3 m/s is **notched out by construction**. This covers most tangential walking, and the synthetic
   scene's person (path rate ≈ 1.0 m/s) (§4.2).

---

## 2. End-to-end pipeline

### 2.1 Block diagram

```
 lab gNB (PCI 2, n78 3.45 GHz, 273 PRB, 30 kHz)  ── air ──►  4 antennas ── 4 identical cables ──► X410 (4 RX)
                                                                                                  │  (one ref clock,
                                                                                                  │   one sample clock)
═══════════════════════════ RECEIVER  (nr-uesoftmodem --passive-rx, CPU, RT threads) ════════════╪════════════
  SSB acquire (--ssb 150, --initial-fo -15000) → PBCH/MIB → FEP per antenna (rxdataF[a])          ▼
  blind PDCCH scan (CORESET1 45 grp, SS sym 0 every slot, DCI 1_1 = 47 b, manual template)
      │ DCI accepted (CRC-recovered RNTI)
      ├──► DM-RS rows : Ĥ_i[k] = Y_i[k] / X_dmrs[k]    (1st DM-RS symbol, comb-2, 4 antennas)   src PDSCH_DMRS_BLIND
      └──► PDSCH decode (consumer threads) → CRC OK → re-encode (LDPC, rate-match, scramble, QAM)
             → X̂[k,l] on every data RE → Ĥ_i[k,l] = Y_i[k,l] / X̂[k,l]   (comb-1, 4 antennas)      src PDSCH_DATA
                     │  nr_isac_submit_cfr_multi(slot, src, Ĥ[4][RE], k, l)
═════════════════════╪═════════════ SENSING ENGINE (in-process, cores 11-13) ═════════════════════════════════
  FlowGate (DL-only: gate_require_ul = 0) → CPI accumulation, fixed 75 ms (spatial mode)
      │  CfrWindow: antenna-major [4][row][subcarrier], raw phase, observed mask, row times
      ▼  coherent_enable = 1 → CoherentPipeline (worker thread, depth-2 queue, never drops)   [design T9]
  C1  upload once per CPI (pinned host → GPU)                                          GPU   [design T10]
  C2  derive_axes · find_los · estimate_row_sync  (per channel, common CFO/SFO)       CPU   [impl T3]
  C3  per-channel range profiles + static removal + NUDFT  →  RD[4][n_range][n_dopp]  GPU (CPU ref) [impl T3]
  C4  Calibrator: per-channel phase Kalman, predictive fade, G, ρ                     CPU   [impl T4]
  C5.1 envelope E(x,v) over voxel grid                                                GPU (CPU ref) [design T5]
  C6  per-Doppler median CFAR → NMS / harmonic merge / z<0 (host)                     GPU+CPU [design T5/T10]
  C5.2 coherent refinement per detection, ρ-blend                                     GPU (CPU ref) [design T5]
  (UL: TDOA-localise UE, focus with tx = UE — built, OFF)                             CPU   [design T8]
      ▼
  CoherentTracker: 3D CV Kalman + Hungarian GNN + SPRT existence, online P_D          CPU   [impl T6]
  Autofocus: RLS antenna-position refinement from confirmed-track detections          CPU   [impl T7]
      ▼
  JSONL (append-only, unique per process): coherent_reports.* · coherent_tracks.* · coherence.*
      ▼
  monitor.py --coherent-dir  →  /coherent page (3D scene, top-view, 4 RD maps, coherence panel, health)  [impl T11]
```

### 2.2 Data per CPI (lab cell: 273 PRB, 30 kHz, 75 ms, default 30 m cube)

These numbers are **derived from the formulas in §3**, not measured. Measured sizes and timings go in §5.

| Object | Size | Where |
|---|---|---|
| CFR window | $4 \times R \times 3276$ complex float = $R \times 105$ kB. $R$ = rows (decoded DL grants, sub-slot groups if enabled); $R = 100 \Rightarrow$ 10.5 MB, plus a 0.33 MB mask | host → GPU once (C1) |
| Range FFT length | $N_{fft} = 4096$ (next power of 2 ≥ 3276 subcarriers) | — |
| Range bins kept | $n_{range} = \lceil \max\Delta\tau / \Delta\tau_{step} \rceil + 4 \approx 41$ for the synthetic geometry (path-length bin 2.44 m) | — |
| Doppler bins | $n_{dopp} = 150$ (±1000 Hz at 13.33 Hz steps); 145 tested (the notch removes 5) | — |
| RD cube | $4 \times 41 \times 150 \times 8$ B ≈ 0.2 MB | stays resident on GPU |
| Envelope | 145 Doppler × 64 000 voxels (40³ at 0.763 m) × 4 B = **37 MB, 9.3 M cells, 4 channel interpolations each** | GPU (CPU reference is not real-time at this size) |
| Candidates / detections | a few to a few hundred × ~100 B | GPU → host |
| Refinement grid per detection | $(2n+1)^3$ with $n = \lceil 4 D \cdot \text{step}/(\lambda R) \rceil$: **R = 15 m → 0.33 M points; 5 m → 8.1 M; 2 m → 124 M** | GPU (CPU reference) |
| Images for the monitor | top-view 40×40 + 4 RD maps (41×150), dB, every `rvm_period_s` = 0.5 s ≈ 150 kB of JSON | host |

**GPU vs CPU** (Task 10 design):
- CPU: LOS, row sync and calibration. They are $O(\text{rows}\cdot\text{subcarriers})$ and return small results.
- GPU: row-spectrum build, cuFFT, crop/normalise/static removal, the NUDFT, the envelope, the
  per-Doppler median (CUB segmented radix sort), threshold compaction, and the refinement argmax.
  Everything runs on one CUDA stream with persistent device buffers.
- Host: NMS, harmonic merge, the $z<0$ test, `finish_refine`, the tracker and autofocus.
- **Only detections, calibration, per-detection channel terms and rate-limited images come back to
  the host.**

---

## 3. Every block and its mathematics

Symbols used throughout:
- $c$ = speed of light; $f_c$ = carrier (3.45 GHz); $\lambda = c/f_c$ = 8.690 cm;
  $k = 2\pi/\lambda$ = 72.31 rad/m; $\Delta f$ = subcarrier spacing (30 kHz).
- $i \in \{0,1,2,3\}$ = RX channel; $r$ = slow-time row; $t_r$ = row time; $k$ as an index = subcarrier
  (the context disambiguates it from the wavenumber).
- Positions: $\mathbf{tx}$ = gNB antenna; $\mathbf{rx}_i$ = antenna $i$; $\mathbf{x}$ = voxel;
  $\mathbf u_{a\to b} = (\mathbf b - \mathbf a)/|\mathbf b - \mathbf a|$.

### 3.1 Receiver: blind PDCCH → DCI → PDSCH → $\hat X$ → per-antenna $\hat H$ (existing, unchanged)

**Purpose.** Obtain the transmitted symbols $X$ of grants addressed to other UEs, without attaching,
and with them a channel estimate at every observed RE on every antenna.

**DCI.** The receiver scans the dedicated CORESET/search space every slot. All values come from the
manual template (§6.3): DCI 1_1 = 47 bits, 0_1 = 43 bits. A candidate is accepted when the CRC-derived
RNTI is plausible and the noise gates pass. The DCI gives the PRBs, TDA, MCS and DM-RS layout.

**DM-RS rows (`pdsch_dmrs_blind`).**
- On the first DM-RS symbol $l_0$, at the type-1 comb positions $k = k_0 + \delta + 2m$ of layer-0 port $p$:
  $$\hat H_i[k] = \frac{Y_i[k,l_0]}{X_{\mathrm{DMRS}}[k]} = \frac{Y_i[k,l_0]\,X^*_{\mathrm{DMRS}}[k]}{|X_{\mathrm{DMRS}}[k]|^2}$$
  - $X_{\mathrm{DMRS}}$ = the Gold-sequence pilot for (scrambling ID, $n_{SCID}$, slot, symbol).
  - $Y_i$ = FFT output of antenna $i$.
- Why comb-2: DM-RS type 1 with one CDM group occupies every second subcarrier. A comb-2 row has a
  bit-identical delay replica at $N_{fft}/2$ (§3.5 folds it).
- This needs only a correct DCI, not a correct decode.

**Data-aided rows (`pdsch_data`).**
- After LDPC decode and a CRC pass, the transport block is re-encoded exactly as the gNB does:
  segmentation, LDPC, rate matching, scrambling with the gNB Gold sequence, QAM (qam256 here). That
  gives $\hat X[k,l]$ on every data RE.
- Then:
  $$\hat H_i[k,l] = Y_i[k,l]/\hat X[k,l], \quad \forall (k,l) \in \text{data REs}$$
- Why: the rows are comb-1 over the whole allocation. They are measured per RE, not interpolated,
  which is about 9× the REs of DM-RS alone. This gives full-band range resolution $c/B$ and
  unambiguous delay out to $1/\Delta f$.
- The CRC gate is what makes $\hat X$ trustworthy: a wrong $\hat X$ produces random-phase $\hat H$,
  not a weak one.

**Per-antenna submission.** `nr_isac_submit_cfr_multi(slot, src, Ĥ[a][RE], k, l, nof_ant)`.
- $\hat X$ comes from one decode, and $\hat H$ is formed on all 4 antennas from the consumer's own FFT.
- **Raw phase is kept**: no per-antenna equalisation or combining. The coherent fuser needs the
  channels' relative phase intact.
- Note: `SENSECOMB` in `run_arm.sh` sets `ISAC_SENSE_COMB`, which nothing in this tree reads (grep).
  So it does not combine the coherent path's antennas.

**Failure behaviour.**
- Wrong DCI layout → zero accepts → no rows.
- CRC failure → no data-aided row, but the DM-RS row may still exist.
- CFO mis-lock → both fail. `run_arm.sh`'s validity verdict flags it.

### 3.2 Flow gate — DL-only mode (existing)

- `FlowGate(window = 2.0 s, require_ul)`. A C-RNTI is a *flow* while it produced
  - `require_ul = 1`: a DL CFR **and** a UL CFR within the last 2 s;
  - `gate_require_ul = 0`: a DL CFR alone.
- The gate is open while at least one flow exists, and CFR is admitted only for RNTIs that are flows.
- Why DL-only here: the user chose DL illumination only. With a UL requirement, a UE whose PUSCH the
  receiver cannot decode would keep the gate shut.
- Log line: `SENSING_GATE open rnti=0x....`. `run_sensing.sh`'s verdict requires it.

### 3.3 CPI formation (existing, spatial mode)

- With `spatial_rx_positions` configured, the engine fixes one immutable coherent interval,
  `SPATIAL_CPI_DURATION_S = 0.075` s. The environment variable `NR_ISAC_SPATIAL_CPI_S` can override it
  for a whole run; it is never data-driven.
- Rows are keyed by (absolute slot, sub-slot fraction). The window is antenna-major
  $H[a][r][k]$, with an `observed` mask and `row_time_slots`.
- The coherent path requires `spatial_rx_positions` (4 antennas), otherwise it disables itself with an
  error. That is why it inherits the 75 ms interval.

### 3.4 C2a — `derive_axes`: every axis from this CPI's own allocation [impl]

Rejection. The CPI is rejected, with a reason string, when:
- it has fewer than 3 rows, or fewer than 3 non-empty rows;
- it does not have exactly 4 antennas;
- row times are non-monotonic (ledger ruling; guards a known past out-of-order CFR bug);
- all rows share one timestamp;
- the median allocation is narrower than 2 subcarriers.

| Quantity | Formula | Why derived this way |
|---|---|---|
| Effective bandwidth | $B_{eff} = \operatorname{median}_r\,(k_{hi,r} - k_{lo,r} + 1)\,\Delta f$ | Resolution is set by the band *actually observed* per row; the median is robust to a few narrow or wide grants. Never a 273-PRB constant |
| FFT length | $N_{fft} = 2^{\lceil \log_2 N_{sc} \rceil}$ | zero-padding to the grid width keeps the centred subcarrier index exact |
| Delay step | $\Delta\tau = 1/(N_{fft}\,\Delta f)$ (8.14 ns = 2.44 m of path) | the IFFT bin spacing of that FFT |
| Range crop | $n_{range} = \min\!\big(N_{fft}/2,\ \lceil \max_{\mathbf x \in \text{corners},\,i} \Delta\tau_i(\mathbf x)/\Delta\tau \rceil + 4\big)$ | $\Delta\tau_i(\mathbf x)$ (§3.9) is a sum of norms, hence **convex**, so its maximum over a box is at a corner: 8 corners × 4 channels are exact. The +4-bin margin is a literal, recorded in the ledger as a deferred review item |
| Row times | $t_r = (s_r - s_0)\,T_{slot}$ | actual timestamps, not an assumed uniform grid |
| Row spacing | $\overline{\Delta t} = \operatorname{median}_r (t_r - t_{r-1})$ over increasing pairs | the dominant sampling interval of this CPI's grants |
| CPI duration | $T_{cpi} = t_{R-1} + \overline{\Delta t}$ | $R$ samples each "occupying" one interval |
| Doppler step | $\delta f_D = 1/T_{cpi}$ (13.33 Hz at 75 ms) | Rayleigh resolution of a $T_{cpi}$ record |
| Doppler span | $\pm 1/(2\overline{\Delta t})$ (±1000 Hz at 0.5 ms) | Nyquist of the dominant row interval |
| $n_{dopp}$ | $2\lceil (1/(2\overline{\Delta t}))/\delta f_D \rceil$ | covers the span at the resolution step |
| Tested bins | $2\,\delta f_D < |f| \le \min\big(1/(2\overline{\Delta t}),\ 2 v_{max}/\lambda\big)$ | $\lvert \tfrac{d}{dt}(\lvert\mathbf x-\mathbf{tx}\rvert + \lvert\mathbf x-\mathbf{rx}\rvert)\rvert \le 2\lvert\mathbf v\rvert$, so no real target at $\lvert\mathbf v\rvert \le v_{max}$ can have a higher Doppler (1151 Hz at 50 m/s) |
| Zero-Doppler notch | $\lvert f\rvert \le 2\,\delta f_D$ (`notch_half_bins = 2`) | the slow-time Hann window's **mainlobe half-width** is exactly 2 bins (first null at $2/T$). Residual static and slow clutter lives there. A window property, not a tuning |

**Failure behaviour.** An invalid axis gives a skipped CPI with `skipped_reason`. It is written to all
three JSONL streams, so it is never silent.

### 3.5 C2b — `find_los`: earliest significant path, not the strongest [impl]

**Purpose.** Measure each channel's direct-path (gNB→antenna) delay $\hat\tau_{LOS,i}$ and its complex
tap. A wall reflection of the gNB can be stronger than the LOS, and must not become the delay or phase
reference.

**1. Non-coherent row combining.** Per-row centred IFFT profile $p_{r}[n]$ (Hann over the row's
observed span, amplitude normalised by the window sum, see §3.7):
$$P[n] = \frac1R \sum_r |p_r[n]|^2$$
- Why power: before row sync, the common CFO rotates each row, so a coherent mean cancels the LOS it is
  looking for. Measured: at 23 Hz CFO over 60 rows, the coherent mean put the LOS 20–28 bins off.

**2. Noise level and threshold.**
- In a noise-only bin, $P[n]$ is the mean of $R$ exponentials, so $P \sim \Gamma(R, \mu/R)$. Its median
  is $\mu\,Q^{-1}(R,\tfrac12)/R$. Hence:
  $$\hat\mu = \operatorname{median}_n P[n] \cdot \frac{R}{Q^{-1}(R,\tfrac12)}, \qquad \theta = \hat\mu\,\frac{Q^{-1}(R, P_{fa,LOS})}{R}$$
  - $Q^{-1}$ = inverse regularised upper incomplete gamma.
  - It is computed in the log domain, because the linear form underflows at $x \approx 745$. Tested
    against scipy to $10^{-9}$ relative.
  - The median is robust because paths occupy few bins.
- $P_{fa,LOS} = \min(0.5,\ 1/(N_{fft}\cdot 4))$: at most one expected false LOS pick per CPI over all
  bins and channels [design T9].

**3. Derived sidelobe guard (no factor).**
- Kernel = the rows' mean point-spread $\overline{|\mathrm{psf}|^2}$, computed from their **own observed
  masks**. This makes it valid for comb, gapped and hopping rows, where the Hann PSL bounds nothing.
- Mainlobe half-width $h_m$ = the kernel's first null.
- Peaks are accepted strongest-first. A peak at $q$ is a *path* only if it exceeds the leakage of the
  already-accepted paths plus noise, summed **as amplitudes**:
  $$\sqrt{P[q]} > \sqrt\theta + \sum_{p\,\in\,\text{paths}} \Big(\sqrt{P_p\,L(q-n_p)} + \sqrt{E^{xs}_p\,L_{any}(q-n_p)}\Big)$$
  - $L(d)$ = kernel maximum over $d \pm 1$ bin (the path's sub-bin position unknown), zero inside the
    mainlobe.
  - $E^{xs}_p$ = the mainlobe's excess energy over a single-path kernel (paths merged into it).
  - $L_{any}$ = the worst leakage over positions inside the mainlobe.
- Why amplitudes: static paths and the noise sample add coherently, so a power sum is not a bound.
- Why the excess term: a merged target widens the mainlobe and lifts its sidelobes. The sweep went from
  65/200 seeds without the term to 200/200 with it and the row-sync fix below.

**4. LOS selection.**
- The earliest accepted path within $n_{range}$ bins *before* the strongest.
- Delays are folded into $[-\text{span}/2, \text{span}/2)$ with $\text{span} = N_{fft}/\Delta k_{min}$, so
  a comb-2 row's bit-identical replica at $N_{fft}/2$ counts once.
- Coarse sub-bin: parabolic interpolation on $\sqrt P$.

**5. Coherent refinement.**
- After row sync (§3.6), a coherent row mean at ±2 bins around the coarse LOS, then a parabolic fit.
- This gives the full union-band resolution, and averages out a moving path near the LOS (it biased the
  power peak by up to 0.27 bin).
- Output: $\hat\tau_{LOS,i}$, and the phase-aligned coherent tap.

**Complexity.** About $3 \times R \times 4$ IFFTs of $N_{fft}$ plus one kernel pass. **Failure:** no
path above $\theta$ gives `found[i] = false`, and the calibrator predicts (§3.8).

### 3.6 C2c — `estimate_row_sync`: common CFO/SFO, linear over the CPI [impl]

The four channels share one reference and sample clock, so CFO and SFO are **common**. They are
estimated once from all channels and applied identically.

**1. Per-row, per-channel LOS tap and slope.**
$$\mathrm{tap}_{r,i} = \frac{1}{|K_r|}\sum_{k \in K_r} Y_i[r,k]\,e^{+j2\pi f_k \hat\tau_{LOS,i}}, \qquad \mathrm{slope}_r = \sum_{\text{adjacent } k,k'} z_{k'} z_k^*$$
- $f_k$ = baseband frequency of subcarrier $k$.
- Adjacent means consecutive observed subcarriers at the row's own finest comb $\Delta k_r$.

**2. Residual reference delay, per channel, where the rows combine best.**
$$\widehat{\mathrm{err}}_i = \arg\max_{\mathrm{err},\,f} \Big|\sum_r \mathrm{tap}_{r,i}\, e^{+j2\pi \bar f_r\,\mathrm{err}}\, e^{-j2\pi f t_r}\Big|$$
- $\bar f_r$ = the row's mean observed baseband frequency.
- $\mathrm{err} \in \pm 1/B_{eff}$, stepped $1/(8\max|\bar f_r|)$ so the hop phase error stays ≤ $\pi/4$.
- $f \in \pm 1/(2\overline{\Delta t})$, stepped $1/(2T_{cpi})$.
- Why: with a hopping allocation, a 1-bin reference error adds a hop-dependent phase of up to ±2 rad per
  row, and the CFO fit collapses. Measured: 3.9 Hz returned for a true 23 Hz.

**3. Channel alignment against the strongest channel.**
- $a_i = \big(\sum_r \mathrm{tap}_{r,i}\,\mathrm{tap}^*_{r,\mathrm{ref}}\big)^* / |\cdot|$.
- The common row rotation cancels in the product, so this stays defined when a CFO spins the plain mean
  to zero.

**4. Phase line.**
- $\phi_r = \operatorname{unwrap}\arg \sum_i \mathrm{tap}_{r,i}\,a_i$ (weights = LOS amplitudes), then a
  least-squares line in $t_r$.
- Why a line and not per-row values: CFO is a linear phase ramp. Per-row sync would absorb part of any
  target within about 1 range cell of the LOS (measured −2.4 dB and −1.2 dB of target).
- Ponytail limit: oscillator phase noise *inside* a CPI is not followed.

**5. Delay drift.**
- $\delta_r = -\arg(\mathrm{slope}_r)/(2\pi\,\Delta k_r\,\Delta f)$, then a least-squares line in $t_r$,
  **minus its mean**.
- Why drift only: the fit's intercept is the power-weighted centroid of all paths, not the LOS. A static
  wall moved it 1.2 bins. The absolute delay stays referenced to `find_los`.

**Cost.** $O(n_{err}\cdot n_f\cdot R)$ per channel. For narrow grants with wide hopping, $n_{err}$ is
large: about 136 × 300 × 150 × 4 ≈ 25 M complex MACs. That is a **CPU budget risk** (§4.2).
The $e^{-j2\pi f t_r}$ table is hoisted out of the err and channel loops (bit-identical, Task 4).

### 3.7 C3 — range profile, static clutter removal, NUDFT [impl CPU; GPU design T10]

**1. Per-subcarrier static removal first.** With the de-ramp
$$z_{i,r,k} = Y_i[r,k]\,e^{\,j2\pi f_k(\hat\tau_{LOS,i}+\delta_r) - j\phi_r}$$
the static component on each subcarrier is $S_{i,k} = \operatorname{mean}_{r:\,k\in K_r} z_{i,r,k}$, and it
is subtracted.
- Why per subcarrier, not per range bin: when grants hop, LOS leakage into bin $m \ne 0$ carries a
  row-dependent phase $2\pi q_c(r) m/N_{fft}$, so it is not static in the range domain. It *is* static
  on a fixed subcarrier. A per-bin mean left a +350 Hz artifact that beat the target on every channel.
- The LOS tap for calibration, $\mathrm{los\_tap}_i$ = the Hann-weighted row mean at bin 0, is taken
  **before** removal.

**2. Range profile (centred IFFT, Hann over the observed band, per-row normalisation).**
$$p_{i,r}[n] = \frac{N_{fft}}{\sum_{k\in K_r} w_{r,k}}\;\mathrm{IFFT}_{N_{fft}}\Big\{w_{r,k}\,(z_{i,r,k} - S_{i,k})\Big\}[n], \qquad w_{r,k} = \mathrm{Hann}\Big(\tfrac{k-k_{lo,r}}{k_{hi,r}-k_{lo,r}}\Big)$$
- The index is centred ($q = k - N_{sc}/2$).
- Normalising by the row's own window sum makes every allocation width give the path amplitude at its
  peak. Wide and narrow grants then weigh equally in slow time.
- Bin $m$ is the excess delay $m\,\Delta\tau$ relative to the channel's own LOS. Only bins $0..n_{range}-1$
  are kept.

**3. Doppler: non-uniform DFT on the actual row times, Hann in slow time.**
$$\mathrm{RD}_i[m,d] = \frac{\sum_r h_r\, p_{i,r}[m]\, e^{-j2\pi f_d t_r}}{\sum_r h_r}, \quad f_d = f_0 + d\,\delta f_D, \quad h_r = \mathrm{Hann}(t_r/t_{R-1})$$
- Why an NUDFT: grants are irregular and TDD removes UL slots, so an FFT would assume a grid that does
  not exist.

**4. Noise per channel.**
$$\hat N_i = \operatorname{median}_{m,\,d\in\text{tested}} |\mathrm{RD}_i[m,d]|^2 / \ln 2$$
- The median of an exponential is $\mu\ln 2$, so this gives the mean noise power.

**Complexity.** $4R$ IFFTs + $4\,n_{range}\,n_{dopp}\,R$ complex MACs (NUDFT).

### 3.8 C4 — Calibrator: per-channel phase Kalman, predictive fade, coherence diagnostic [impl]

**Measurement.** Channel 0 is the phase reference (absolute phase is unobservable and irrelevant to
$|\sum|^2$). For $i \ge 1$:
$$z_i = \frac{\mathrm{tap}_i\,\mathrm{tap}_0^*}{|\mathrm{tap}_i|\,|\mathrm{tap}_0|}, \qquad r_i = \frac{1}{2\,\mathrm{SNR}_i} + \frac{1}{2\,\mathrm{SNR}_0}$$
- Why this $r$: at high SNR, the phase of a complex-Gaussian-perturbed phasor has variance
  $1/(2\,\mathrm{SNR})$. The ratio of two independent taps adds the two variances.

**State and update.** A unit phasor per channel, $s_i \in S^1$, with variance $p_i$.
- **Seeding.** The first valid measurement seeds $s_i = z_i$, $p_i = r_i$ directly. The prior is not a
  prediction, so its "innovation" is not evidence.
- **Predict:** $p^- = p + \max(0,\hat q)$.
- **Update:**
  $$\nu = \arg(z_i s_i^*), \qquad K = \frac{p^-}{p^- + r}, \qquad s_i \leftarrow s_i\,e^{jK\nu}, \qquad p \leftarrow (1-K)\,p^-$$
- **Process noise by covariance matching** (Mehra form, unclipped accumulator):
  $$\hat q_n = \frac1n \sum_{j=1}^{n}\Big(\nu_j^2 - (p_{j-1} + r_j)\Big)$$
  - It is subtracted against the posterior **before** this CPI's $q$ is added. Subtracting $p^-$ feeds
    $\hat q$ back into itself and biases it low: measured 8× CRB on a static channel before the fix.
  - It is clipped to ≥ 0 **only at use**, because $E[\max(0,X)] \ne \max(0,E[X])$.
- **No LOS this CPI:** predict only ($p \leftarrow p^-$). Variance never shrinks without data.

**Predictive fade (combining weight).**
$$c_i = \exp\!\big(-\tfrac12\,(p_i + \max(0,\hat q_i))\big)$$
For a Gaussian phase error $\varepsilon \sim \mathcal N(0,\sigma^2)$, $|E[e^{j\varepsilon}]| = e^{-\sigma^2/2}$.
So $c_i$ is the **expected coherent-amplitude factor** for the *next* CPI. A channel whose phase is
unpredictable between CPIs fades out, even when its own per-CPI posterior looks confident (ruling:
predictive variance, supersedes R3). Measured: scrambled channels $c_i$ = 0.16–0.27; stable ≈ 1.000.

**Coherence gain and ρ** (non-tautological: scored against the **previous** posterior, before
updating). Over the channels found now and already seeded ($n$ of them):
$$G = \frac{\big|\sum_{i} z_i\,s_i^{*}\big|^2}{n} \in [0, n], \qquad \rho = \operatorname{clip}_{[0,1]}\frac{G-1}{n-1}$$
- Random phases give $E|\sum|^2 = n$, so $G \approx 1$.
- Perfectly predicted phases give $G = n = 4$.
- $\rho$ maps that range linearly onto $[0,1]$.
- **$G \to 4$, $\rho \to 1$ is "the array is coherent and its calibration predicts the next CPI".**

**Diagnostics also emitted.**
- $\text{jitter}_i = \sqrt{\overline{\nu^2}}$ (running innovation RMS).
- $\text{bound}_i = \sqrt{r_i} = 1/\sqrt{2\,\mathrm{SNR}}$-type bound.
- Caveat (§4.2): the SNR fed is the **per-row** LOS SNR, while the tap is an $R$-row coherent mean, so
  the true bound is about $\sqrt R$ smaller. A healthy channel can read *below* the bound.

**Known properties (measured, Task 4).**
- $\hat q$ recovered within 2× at $r \ll q$.
- The wrapped-innovation $\hat q$ is biased low when $r$ is comparable to $q$ (ratio 0.3–0.5 at SNR 200).
- $\hat q$ is a cumulative mean, so after a phase hop its effect decays as $1/n$.

### 3.9 Excess bistatic delay and why LOS referencing cancels STO and cable delay

For voxel $\mathbf x$ seen on channel $i$:
$$\Delta\tau_i(\mathbf x) = \frac{|\mathbf x - \mathbf{tx}| + |\mathbf x - \mathbf{rx}_i| - |\mathbf{tx} - \mathbf{rx}_i|}{c}$$

The delay the receiver measures on channel $i$ for any path is
$$\tau^{meas}_{i} = \tau^{geo}_{i} + \tau_{STO} + \tau_{cable,i} + \tau_{chain,i}$$
- $\tau_{STO}$ = the common timing offset of the passive receiver vs the gNB (unknown, drifting).
- $\tau_{cable,i}$, $\tau_{chain,i}$ = per-channel cable and front-end group delays.

Subtracting the channel's own measured LOS delay:
$$\tau^{meas}_{target,i} - \tau^{meas}_{LOS,i} = \tau^{geo}_{target,i} - \tau^{geo}_{LOS,i} = \Delta\tau_i(\mathbf x)$$
- **Every unknown offset cancels exactly** (common or per-channel), because it appears in both terms.
- The survey enters only through **distance differences** $|\mathbf x-\mathbf{rx}_i| - |\mathbf{tx}-\mathbf{rx}_i|$.
- The baseband phase ramp cancels too. The carrier phase $e^{-j2\pi f_c(\tau_{LOS,i}+\tau_{cable,i})}$
  and the channel's LO phase remain as a per-channel constant, which is exactly what the calibrator
  estimates (ledger R2: after LOS referencing the tap keeps the carrier term $e^{-j2\pi f_c d_i}$, and
  the calibration correctly absorbs it).

### 3.10 C5.1 — Envelope statistic and its Γ(4,1) null [design T5]

**Grid.** Voxel step $\Delta x = c/(4B_{eff})$ (0.763 m at full band), over the declared volume.
- Why: a path-length resolution cell $c/B$ maps to at least $c/(2B)$ in space, because
  $|\nabla_{\mathbf x}(|\mathbf x-\mathbf{tx}|+|\mathbf x-\mathbf{rx}|)| = 2\cos(\beta/2) \le 2$.
- Sampling at half of that is Nyquist on the mainlobe.
- It follows each CPI's own bandwidth.

**Statistic** (non-coherent, robust to survey error):
$$E(\mathbf x, d) = \sum_{i=0}^{3} \frac{\big|\mathrm{RD}_i\big(\Delta\tau_i(\mathbf x)/\Delta\tau,\ d\big)\big|^2}{\hat N_i}$$
- Linear complex interpolation in the range bin; out-of-axis bins are skipped, never read out of bounds.

**Null distribution.**
- Under noise, each $\mathrm{RD}_i$ cell is circular complex Gaussian, so $|\mathrm{RD}_i|^2/\hat N_i \sim \mathrm{Exp}(1)$.
- The four receive chains have independent thermal noise, so $E \sim \Gamma(4,1)$.
- Why non-coherent here: it needs the survey only to within a fraction of the 1.5 m spatial cell. This
  is the stage that finds targets whatever the phase calibration and survey quality are.

### 3.11 C6 — Detection: derived CFAR, NMS, harmonic merge, $z<0$ [design T5]

**False-alarm probability per test.**
$$P_{fa} = \frac{\lambda_{FA}\,T_{cpi}}{N_{vox}\cdot N_{dopp,tested}}$$
- $\lambda_{FA}$ = `false_object_intensity_per_s` (declared).
- Then $E[\#\text{false detections}/\text{s}] = P_{fa} N_{vox} N_{dopp}/T_{cpi} = \lambda_{FA}$.
- Lab numbers: $0.075/(64\,000 \cdot 145) = 8.1\times 10^{-9}$.

**Threshold per Doppler bin** (robust noise level from the bin's own voxel population):
$$\theta_d = Q^{-1}_{\Gamma(4,1)}(P_{fa}) \cdot \frac{\operatorname{median}_{\mathbf x} E(\mathbf x,d)}{Q^{-1}_{\Gamma(4,1)}(\tfrac12)}$$
- Lab numbers: $26.83/3.672$, i.e. **8.6 dB above the median**. That corresponds to a per-channel
  RD-cell SNR of about $(26.83-4)/4 = 5.7$ (7.6 dB).
- Why per Doppler bin: the noise-plus-leakage floor changes with Doppler (irregular sampling sidelobes,
  residual clutter). Targets occupy a negligible fraction of voxels, so the median is the floor.

**Candidates, strongest first. A candidate is dropped when:**
- $z<0$: the **ground-bounce mirror**. By the image method, the path tx → target → ground → rx equals
  the path to the target's mirror at $z=-h$, and it focuses there. This requires $z=0$ to be the real
  reflecting ground (§7a).
- it lies within the **NMS radius** $2\Delta x = c/(2B)$ (one spatial range cell) of a kept detection
  and within one Doppler bin of it;
- it is a **harmonic merge**: within the NMS radius, with $|f - k f_{kept}| \le \delta f_D$ for an
  integer $k \ge 2$. A scheduling/sampling harmonic of a stronger target is dropped.

**Per-detection outputs.**
- $\widehat{\mathrm{SNR}} = \max(10^{-6}, (E\cdot Q^{-1}(4,\frac12)/\mathrm{med} - 4)/4)$, per-channel equivalent.
- Range rate $\dot r = -\lambda f_d$ (bistatic path-length rate).
- $\sigma_{pos} = \max\big(\frac{c/B}{\sqrt{2\,\mathrm{SNR}}},\ \frac{\Delta x}{\sqrt{12}}\big)$ and
  $\sigma_{\dot r} = \max\big(\frac{\lambda\,\delta f_D}{\sqrt{2\,\mathrm{SNR}}},\ \frac{\lambda\,\delta f_D}{\sqrt{12}}\big)$.
  - Resolution cell over $\sqrt{2\,\mathrm{SNR}}$ is the standard high-SNR scaling of a peak-location
    estimator.
  - The floor is uniform quantisation.

<!-- FILL: Task 5 implementation deviations from this design (as merged), and the regular-hop envelope-rejection test result carried from Task 3 -->

### 3.12 C5.2 — Coherent refinement and the ρ-blend [design T5]

**Per-channel coherent term** at a fine-grid point $\mathbf x$ (calibration and noise-normalised,
carrier-corrected):
$$t_i(\mathbf x) = c_i\,e^{-j\hat\phi_i}\,\frac{\mathrm{RD}_i\big(\Delta\tau_i(\mathbf x)/\Delta\tau,\ d\big)}{\sqrt{\hat N_i}}\;e^{+j2\pi f_c\,\Delta\tau_i(\mathbf x)}, \qquad C(\mathbf x) = \Big|\sum_i t_i(\mathbf x)\Big|^2$$
- The factor $e^{+j2\pi f_c\Delta\tau_i}$ undoes the carrier phase of the *excess* path; the calibration
  $\hat\phi_i$ undoes the LOS carrier term and the LO phase (§3.9).

**Fine grid.**
- Fringe width $w = \lambda R/(2D)$. $R$ = distance from the array centroid (floored at $\Delta x$);
  $D$ = maximum baseline (14.14 m for a 10 m square).
- Step $w/2$ (Nyquist on the fringe pattern). Extent ± one envelope step.
- Examples: $w$ = 3.1 cm at 10 m, 4.6 cm at 15 m, 7.7 cm at 25 m.
- Output: argmax $\mathbf x_{coh}$ and the terms $t_i(\mathbf x_{coh})$ (autofocus input).

**Blend.**
$$\mathbf x = \rho\,\mathbf x_{coh} + (1-\rho)\,\mathbf x_{env}, \qquad \sigma = \max\!\Big(\frac{\rho\,w + (1-\rho)\,c/B}{\sqrt{2\,\mathrm{SNR}}},\ \frac{w/2}{\sqrt{12}}\Big)$$
- Why ρ: the interferometric position is only as good as the phase calibration. $\rho$ (§3.8) moves
  smoothly from the envelope (incoherent) to the fringe-limited (coherent) estimate.
- **Honest limit** (§4.2): $\rho$ certifies the *calibration*, not the *survey*.

### 3.13 Tracker [impl T6]

**State and model.**
- State $\mathbf s = [x\ y\ z\ v_x\ v_y\ v_z]^T$, constant velocity.
- Per axis, $F = \begin{bmatrix}1 & \Delta t\\ 0 & 1\end{bmatrix}$ and
  $Q = q\begin{bmatrix}\Delta t^3/3 & \Delta t^2/2\\ \Delta t^2/2 & \Delta t\end{bmatrix}$
  (white-acceleration PSD $q$).
- $\Delta t$ = air-time difference between CPIs.

**Process noise.**
- Start $q_0 = (v_{max}/1\,\mathrm s)^2$ (declared bound, stage-10 convention): 2500 m²/s³ at 50 m/s.
- Then, per track, plain covariance matching (ruling, no clamp):
  $$q = q_0\cdot\overline{\mathrm{NIS}}/3$$
  - 3 = $E[\chi^2_3]$ at correct sizing.

**Position update** (linear, Joseph form, then symmetrised):
$$S = H P H^T + R,\quad K = P H^T S^{-1},\quad P \leftarrow (I-KH)P(I-KH)^T + KRK^T$$
- $R = \operatorname{diag}(\sigma_{pos}^2)$.
- Why Joseph: it is positive semi-definite under round-off by construction, so no diagonal repair.

**Range-rate update** (scalar, Joseph form):
$$h = \big[0,0,0,\ (\mathbf u_{tx\to x} + \mathbf u_{c\to x})^T\big]$$
- $c$ = array centroid.
- Innovation $\chi^2_1$ 99 % (6.63) outlier gate: a bad rate is ignored, and the position update is kept.

**Association.**
- Cost = $\mathrm{NIS} + \ln|S|$ when $\mathrm{NIS} \le \chi^2_3(0.99) = 11.345$, else forbidden.
- Solved with the rectangular Hungarian algorithm, $O(T^2 D)$.
- Forbidden entries use a data-derived big-M $2(1+\sum|\text{finite costs}|)$ instead of $10^{18}$.
  $\mathrm{ulp}(10^{18}) \approx 128$ corrupted the optimum in 4131/200 000 trials. Verified against
  brute force on 3000 trials.

**Existence (SPRT on the log-likelihood ratio).**
- Hit: $\ell \mathrel{+}= \ln\big(P_D\,\mathcal N(\nu;0,S)/\lambda_c\big)$. Miss: $\ell \mathrel{+}= \ln(1-P_D)$.
- Clutter density $\lambda_c = \lambda_{FA} T_{cpi}/V$ = $2.8\times10^{-6}$ m⁻³ for a 30 m cube.
- Confirm at $\ln\frac{1-\beta}{\alpha} = \ln 90 = 4.50$; delete at $\ln\frac{\beta}{1-\alpha} = -2.29$.
- Reported existence probability $p_e = 1/(1+e^{-\ell})$.

**Online $P_D$.** $\hat P_D = (h+1)/(h+m+2)$ over confirmed tracks (Laplace prior).

**Births.** Every unassociated detection starts a track: velocity 0 with variance $q_0$ per axis,
position variance $\sigma_{pos}^2$.

**Consequence of the numbers.** With $\sigma_{pos} \approx 0.3$ m, one associated hit adds about
$\ln(0.9 \times 0.84 / 2.8\times10^{-6}) \approx 12.5$. A track is therefore confirmed on its **first
association** (second detection). A newborn's first miss ($-2.3$) deletes it.

### 3.14 Autofocus — online antenna-position refinement [impl T7]

**Phase-sensitivity model.**
- The per-channel phase residual at the focused voxel, to first order in the antenna error
  $\boldsymbol\delta_i$:
  $$e_i \approx \mathbf a_i\cdot\boldsymbol\delta_i, \qquad \mathbf a_i = -k\,(\mathbf u_t - \mathbf u_x), \quad \mathbf u_t = \mathbf u_{rx_i\to tx},\ \mathbf u_x = \mathbf u_{rx_i\to x}$$
  - $\nabla_{\mathbf{rx}}\big(|\mathbf x-\mathbf{rx}| - |\mathbf{tx}-\mathbf{rx}|\big) = -\mathbf u_x + \mathbf u_t$.
  - The sign is derived, and confirmed numerically; flipping it makes every test regress.
- **The component of $\boldsymbol\delta_i$ along which $\mathbf u_t = \mathbf u_x$ is invisible**: this is
  the LOS-direction cancellation of §3.9.

**Estimator** (ruling: the reviewer's derived design; innovation form, joint centred 12-dim state):
- State $\mathbf x \in \mathbb R^{12}$ = the four $\boldsymbol\delta_i$.
- Prior information $J_0 = I/\sigma_{survey}^2$, $\mathbf b_0 = 0$.
- Per detection associated to a **confirmed**, DL-illuminated track, relinearise at the current estimate
  $\hat{\mathbf x}$:
  - **Admission**, per channel: $3\sqrt{\mathbf a_i^T P_i \mathbf a_i + 1/(2\,\mathrm{SNR})} < \pi$.
    $P_i$ = the channel's 3×3 block of $J^{-1}$, and $z = 3$ is conventional. This is the capture-range
    test: a residual that may wrap past $\pi$ is not a linear measurement. At least 2 channels must be
    admitted.
  - **Reference and innovation.** $\mathrm{ref} = \sum_{j} \frac{t_j}{|t_j|}e^{-j(\mathbf a_j\cdot\hat{\mathbf x}_j - \mathbf a_j\cdot\mathbf x_{f,j})}$
    and $\nu_i = \arg\big(t_i e^{-j(\mathbf a_i\cdot\hat{\mathbf x}_i - \mathbf a_i\cdot\mathbf x_{f,i})}\,\mathrm{ref}^*\big)$.
    - $\mathbf x_{f,i}$ = the correction already baked into the focus geometry (the focus-geometry
      contract, ruling: `add(d, focus_geometry)`). It cancels exactly in the algebra.
  - **Centred row.** $\mathbf y_i = [\,\mathbf a_i \text{ in block } i\,] - \operatorname{mean}_{j\in\text{adm}}[\,\mathbf a_j \text{ in block } j\,]$.
    Centring removes the unknown common phase absorbed by `ref`.
  - **Information update:** $J \mathrel{+}= \mathbf y_i\mathbf y_i^T/\mathrm{var}$ and
    $\mathbf b \mathrel{+}= \mathbf y_i(\nu_i + \mathbf a_i\cdot\hat{\mathbf x}_i)/\mathrm{var}$, with
    $\mathrm{var} = 1/(2\,\mathrm{SNR})$. Solved by Cholesky; SPD by construction.
- **Apply test**, per channel: $\hat{\mathbf x}_i^T P_{ii}^{-1}\hat{\mathbf x}_i > \chi^2_3(0.99) = 11.345$,
  with the **marginal** $P_{ii}$ (ruling). It is harder to pass than $J_{ii}$, i.e. conservative.
  Otherwise the channel keeps its surveyed position.

**Capture range (the key number).** $|\boldsymbol\delta_i| < \pi/(z|\mathbf a_i|)$ with
$|\mathbf a_i| = k|\mathbf u_t - \mathbf u_x| \in [0, 2k]$:
- **1.45 cm** for $|\mathbf u_t-\mathbf u_x| = 1$;
- **7.2 mm** at the worst geometry ($2k = 144.6$ rad/m).

**Measured (Task 7, synthetic, 4000 detections).**

| Case | Result |
|---|---|
| σ = 3 cm, seeded errors ≈ 2.2–2.7 cm | sum error 0.103 → 0.027 m; ch3 correctly **not applied** (fringe-ambiguous) |
| σ = 3 cm, errors ≈ 4.5–5.4 cm | 0.206 → 0.0008 m over 4 channels; median ratio over 8 seeds 0.228 |
| **σ = 0.1 m (tape), errors ≈ 5 cm** | **no channel admitted, geometry unchanged: safe decline** |

Ruling (spec-level): a coarse delay/envelope-domain stage that would pull a tape-grade survey into
capture range is **future work, not built**.

### 3.15 UL path — UE-illuminated focusing (built, disabled) [design T8]

**Purpose.** Use a UE's PUSCH as a second illuminator, for UL session windows.

**UE localisation by TDOA.**
- `find_los` on the UL window gives the UE's direct-path arrival per channel, $\hat\tau_i$.
- Grid search over the envelope grid:
  $$J(\mathbf x) = \sum_{i=1}^{3}\frac{\big((\hat\tau_i - \hat\tau_0) - (|\mathbf x-\mathbf{rx}_i| - |\mathbf x-\mathbf{rx}_0|)/c\big)^2}{2\sigma_\tau^2}, \qquad \sigma_\tau = \Delta\tau/\sqrt{12}$$
  - Differences cancel the UE's unknown transmit time and timing advance.
- $\sigma_{UE}$ = RMS distance of the voxels inside the $\chi^2$ +1 contour, floored at $\Delta x/\sqrt{12}$.

**Focusing.** `ue_geometry()` sets $\mathbf{tx} = \hat{\mathbf x}_{UE}$ and reuses C3–C6 unchanged:
$\Delta\tau_i(\mathbf x)$ with the UE as transmitter is exactly the UL excess delay, after each channel
is referenced to its own UE direct path. Detections carry `ill` = UL session id, and $\sigma_{UE}$ is
added to their position $\sigma$.

**Why it is OFF** (`coherent_ul_enable = 0`):
1. The user asked for DL only for now.
2. Physics — TX-side unknowns:
   - The UE position is itself an estimate, and its error enters every $\Delta\tau_i$.
   - The UE's direct path must be the earliest arrival on all 4 channels; a body-blocked or NLOS UE
     breaks the TDOA.
   - UL rows are sparse (UL slots only, grant-driven), so the Doppler span and resolution are poor.
   - The passive PUSCH decode chain is margin-limited.

<!-- FILL: Task 8 as-merged deviations and its unit-test numbers -->

### 3.16 Real time, threading, failure handling, logs [design T9/T10]

- **Queue.** One `CoherentPipeline` worker thread with a **depth-2 queue** (double buffering).
  `submit()` **waits** when the queue is full and counts `queue_waits`. It never drops.
- **Overrun.** A CPI whose total processing time exceeds its own $T_{cpi}$ is counted as an overrun.
- **Timing.** Per-stage timers per CPI: sync, rd, env, detect, refine, track, total. They are CUDA
  events on the GPU path.
- **Failures:**
  - No LOS on a channel → the calibrator predicts (σ grows, ρ falls) → envelope-only.
  - A decohered channel → its $c_i$ fades.
  - No CUDA → CPU reference, flagged `"gpu": false`.
  - `NR_ISAC_REQUIRE_CUDA=1` (set by `run_sensing.sh`) → fails closed: CPIs skipped with reason
    `"cuda required but unavailable"`.
  - An exception inside a CPI is logged (`LOG_E`) and the worker continues.
- **Logs.** `coherent_reports.<UTC>_<pid>_<n>.jsonl`, `coherent_tracks.*`, `coherence.*`, in the
  directory of `report_path`.
  - Opened with `fopen(..., "a")` and flushed per line. The name is unique per process and per sink.
    **Nothing truncates.**
  - `run_sensing.sh` renames the previous try's `chain.out`/`monitor.out`/`*.jsonl` to `.tryN` on a
    retry.

**JSONL schemas.** The monitor and the analysis snippet (§7d) rely on these exactly:

- `coherent_reports`:
  ```
  {cpi, t, t_cpi_s, b_eff_hz, range_res_m, grid_step_m, n_voxels, n_dopp_tested, rows, gpu,
   timing_ms{sync,rd,env,detect,refine,track,total}, overrun,
   stats{processed,skipped,overruns,queue_waits}, skipped_reason,
   detections[{p,s,rr,rr_s,snr,fd,ill}], topview|null, rd|null}
  ```
  - `rd[].db` is **range-major** `[m*nd+d]`.
  - A skipped CPI writes only `{cpi, t, skipped_reason}`.
- `coherent_tracks`: `{cpi, t, tracks[{id, p, v, s, pe, hits, age, confirmed}]}`
- `coherence`: `{cpi, t, phase[4], phase_var[4], jitter[4], bound[4], snr[4], los_found[4], G, rho, af_corr_m[4]}`
  - A skipped CPI writes `{cpi, t, skipped: true}`.

<!-- FILL: Task 9/10 as-merged deviations (engine switch location, CUDA kernels, pfa_los, image cadence) -->

---

## 4. Strengths, weaknesses and risks

### 4.1 Strengths

1. **One device, one clock.** The four channels share the X410's reference and sample clock.
   - Inter-receiver time sync disappears. It was the bottleneck of the earlier multi-receiver phases
     (free-running receivers, SFO, and CPI pairing across hosts).
   - CFO and SFO are **common** and are estimated once from all four LOS taps (§3.6).
2. **LOS referencing cancels every unknown delay exactly**: STO, cable lengths, front-end group delay
   (§3.9). The survey enters only through distance differences.
3. **Graceful degradation, envelope first.** The envelope needs the geometry only to ≪ 1.5 m and works
   whatever the phase state. Coherent refinement is blended in only as far as $\rho$ allows.
4. **No tuned constants.** Axes, grid, CFAR, gates and noise levels are derived per CPI from the actual
   allocation and row times, so the same code runs on a 4-PRB grant or a 273-PRB grant, 30 kHz or not.
   Every derivation is documented with its reason (§3).
5. **Robust LOS.** Earliest-not-strongest, with a sidelobe guard derived from the rows' own point-spread.
   It handles a gNB wall reflection stronger than the LOS, comb-2 rows and hopping grants: 200/200
   seeds, max error 0.111 bin.
6. **Hopping-aware clutter removal**: per subcarrier, not per range bin (§3.7).
7. **Irregular slow time is handled exactly** by an NUDFT on the actual row timestamps.
8. **Self-weighting calibration.**
   - The predictive fade drops an unpredictable channel automatically.
   - A missing LOS means predict-only, with no NaN.
   - $G$/$\rho$ are non-tautological (scored against the previous posterior), so they are an honest
     live coherence meter.
9. **Tracker numerics are sound**: Joseph form everywhere, correct big-M Hungarian (brute-force
   verified), SPRT with conventional α/β, online $P_D$, full 3D (drones).
10. **Autofocus is safe.** It *declines* rather than diverges outside its capture range, and never makes
    an antenna worse (tested).
11. **Operational hygiene.**
    - Logs are append-only and unique per process; retries are renamed.
    - The launcher refuses to start on a running build, a wrong governor, or an unreachable X410.
    - A validity verdict is written per run.
12. **DL-only, no cooperation.** No UE is controlled and nothing is transmitted. The UL path exists for
    later.

### 4.2 Weaknesses and risks (with the physics)

| # | Risk | Physics / numbers | What it does | Mitigation / status |
|---|---|---|---|---|
| W1 | **Tape survey vs λ = 8.69 cm** | Coherent phase error on channel $i$ = $k\,(\mathbf u_t-\mathbf u_x)\cdot\boldsymbol\delta_i$, $k$ = 72.3 rad/m. ≤ π/4 needs $\lvert\boldsymbol\delta\rvert \lesssim \lambda/8 \approx$ **1.1 cm** at $\lvert\Delta\mathbf u\rvert = 1$. A 10 cm error gives ≈ 7 rad | Coherent refinement is useless with a tape-grade survey. The envelope is unaffected (needs ≪ 1.5 m) | §7a asks for ±1 cm; declare the honest σ |
| W2 | **Autofocus capture range** | $\pi/(3\lvert\mathbf a\rvert)$ = 7 mm – 1.45 cm. With the prior only, admission needs $3k\lvert\Delta\mathbf u\rvert\sigma < \pi$, i.e. $\sigma \lesssim$ 1.4 cm | From σ = 0.1 m it **never acts**: `af_corr_m = 0` (by design, ledger) | Coarse delay/envelope stage = future work |
| W3 | **ρ certifies calibration, not geometry** | $\rho$ is computed from LOS taps, which the calibrator phase-aligns whatever the survey error. With a 5–10 cm survey, $\rho \to 1$ while the target-direction phases are random | The coherent argmax lands on an arbitrary fringe within ±1 envelope step (±0.76 m) **and reports fringe-scale σ** (cm). The tracker over-trusts it; NIS inflates; $q$ adapts up | Suggested (not built): blend by the detection's own coherence $C(\mathbf x_{coh})/\big(\sum_i\lvert t_i\rvert\big)^2$ instead of the LOS $\rho$. <!-- FILL: confirm/deny against the Task 9 chain test --> |
| W4 | **Sparse-array fringe ambiguity** | 4 elements over a 14 m aperture: fringe 3–8 cm, with many near-equal grating lobes inside one 0.76 m envelope cell | Noise or small phase errors pick the wrong lobe | ρ-blend (W3 caveat); tighter survey; autofocus when in range |
| W5 | **gNB position error** | $\partial\Delta\tau_i/\partial\mathbf{tx} = (\mathbf u_{x\to tx} - \mathbf u_{rx_i\to tx})/c$. The first term is common to all channels and cancels in $\lvert\sum\rvert^2$; the per-channel part scales with the angle the array subtends at the gNB (≈ 0.35 rad at 40 m) | 10 cm gNB error → about 2.5 rad of differential phase | Survey the gNB antenna to cm too; a far gNB helps. Autofocus does not refine $\mathbf{tx}$ |
| W6 | **X410 per-retune channel phase** | Each retune / re-init / MPM restart re-draws each channel's LO/NCO phase | A step in $\hat\phi_i$: the predictive fade drops the channel for a few CPIs; $\hat q$ is a cumulative mean, so the hop's effect decays as $1/n$ (ruling) | Expected. Look for step-then-flat phase traces (§7e). CFO-trim delivery (`CFOAPPLY`) is off by default in `run_arm.sh` |
| W7 | **Per-channel CFO after a retune** | A past driver bug retuned UHD channel 0 only: branches 1–3 sat ≈ 600 Hz off (measured 2026-09-15). That is **not common-mode**, so the row sync cannot remove it; 600 Hz × 75 ms = 45 cycles | Coherence is destroyed: $G \approx 1$, and each channel's Doppler shifts by ≈ 45 bins | Fixed in `usrp_set_rx_freq_all()` (present in this tree) **only if `liboai_usrpdevif.so` is rebuilt** (stale-plugin trap). **Not validated on air** |
| W8 | **LO phase-noise commonality** | The design assumes the channels' phase noise is common. Each channel's synthesiser locks to the common reference, so slow phase is shared up to a constant; fast phase noise may be partly independent | Residual per-channel jitter above the bound | Measure on the static run: `jitter` vs `bound` (with the W16 caveat) |
| W9 | **Regular-hop waveform ghost, per channel** | A regular PRB-stride hop makes the band centre a sawtooth in time (serrodyne). A path appears as a ridge shifted in delay *and* Doppler; measured 0.94 of the peak at 1.6 bins / +176 Hz | A per-channel RD ghost that no linear processing removes | The ridge falls at a *different voxel per channel*, so the 4-channel envelope should reject it. Required test in Task 5 <!-- FILL: result --> |
| W10 | **Comb-2 DM-RS replica** | DM-RS-only rows are comb-2 → a full replica at $N_{fft}/2$ = $1/(2\Delta f)$ = 16.7 µs ≈ 5 km of path | Irrelevant inside the volume ($n_{range} \le N_{fft}/2$); for the LOS it is folded | Handled (§3.5); tested with a −360 m STO |
| W11 | **Doppler resolution vs pedestrians** | One bin = $\lambda/T_{cpi}$ = **1.16 m/s** of path rate; the notch is ±2 bins = **±2.32 m/s** (Hann mainlobe) | A walker's path rate is $\mathbf v\cdot(\mathbf u_{tx\to x}+\mathbf u_{rx\to x})$, between 0 (tangential) and 2.8 m/s (along the bisector). **Most walkers are notched out.** The synthetic person (≈ −1.0 m/s path rate) is inside the notch | Walk along the gNB–array bisector for the test (§7e). `NR_ISAC_SPATIAL_CPI_S` = 0.15/0.3 s halves/quarters the notch, but **untested with the coherent path** and adds range migration. <!-- FILL: Task 9 chain-test outcome for the person --> |
| W12 | **Near-field Doppler diversity across channels** | Channel $i$ sees $\dot r_i = \mathbf v\cdot(\mathbf u_{tx\to x}+\mathbf u_{rx_i\to x})$, so $\dot r_i - \dot r_j = \mathbf v\cdot(\mathbf u_{rx_i\to x}-\mathbf u_{rx_j\to x})$, of order $\lvert\mathbf v\rvert\,D/R$. In the synthetic scene: drone 3.0 bins of spread, car 0.5, person 0.2 | The envelope and refinement sum the 4 channels **at the same Doppler bin**. When the spread is > 1 bin, $E$ loses up to 6 dB, and one target can give several detections at one voxel with different Doppler (duplicate births) | Upgrade path: a per-voxel velocity hypothesis, or a per-channel Doppler window. Not built |
| W13 | **Rotor micro-Doppler aliasing** | Blade-tip path rates ≫ $v_{max}$ exceed both the tested band ($2v_{max}/\lambda$ = 1151 Hz) and the alias span $\pm1/(2\overline{\Delta t})$ (±1000 Hz → ±43 m/s of *radial* speed at 0.5 ms) | Rotor energy folds into the band. It raises the per-Doppler median (desensitising) and gives sidebands at the drone voxel spaced by the blade-flash rate. These are **not integer harmonics**, so the merge rule does not remove them | Expect extra tentative tracks at the drone. Micro-Doppler classification is out of scope |
| W14 | **Grant-driven slow time** | The span is $\pm1/(2\overline{\Delta t})$, so the maximum unambiguous path rate is $\lambda/(2\overline{\Delta t})$: 87 m/s at a grant every DL slot, **7.2 m/s at a grant every 6 ms**. TDD gating with duty $D$ and period $T_p$ gives Doppler replicas at $n/T_p$ with relative amplitude $\mathrm{sinc}(nD)$: **−8.7 dB at ±200 Hz** for 7-of-10 DL slots, $T_p$ = 5 ms | Low traffic → cars/drones alias. TDD replicas at the same voxel ±$1/T_p$ are **not** caught by the NMS (±1 bin) or the harmonic merge ($k f_d$) → possible duplicate tracks | Keep steady DL traffic (§7b). Suggested (not built): a sidelobe-aware Doppler NMS from the CPI's own slow-time point-spread, the slow-time analogue of §3.5's range guard. <!-- FILL: measured in the chain test (the generator uses a 7-of-10 pattern) --> |
| W15 | **Multipath exceptions** | (a) ground-bounce mirror → $z<0$ rejection, correct only if $z=0$ is the real ground; low targets' mirrors are only $2h$ apart. (b) target–wall double bounce → focuses at the wall-mirror of the target, a real-looking ghost if inside the volume. (c) blocked LOS on a channel → the gNB wall reflection becomes the "LOS"; that channel's delay reference is off by the extra path, so every $\Delta\tau_i$ is biased | (a) handled. (b) ghost track. (c) misfocus plus calibration jumps | (a) survey heights from the ground. (b) keep reflecting walls outside the volume. (c) site each antenna with a clear LOS to the gNB (§7a) |
| W16 | **Calibrator SNR scale** | `find_los` passes the **per-row** power-mean SNR, but the calibrated tap is an $R$-row coherent mean (≈ $R\times$ higher SNR). So $r_i$ is over-estimated by ≈ $R$ | Conservative: slower convergence. $\hat q$ biased toward 0, so drift smaller than the per-row phase noise is invisible. The jitter panel can read **below** `bound` on a healthy channel | Code-grounded finding, not yet discussed in review. Candidate fix: pass the coherent-tap SNR |
| W17 | **Static-clutter removal costs slow targets** | Subtracting the per-subcarrier slow-time mean removes ≈ 1/(rows per subcarrier) of a moving target (5–7 % measured). A stationary person is removed entirely | Standing targets are invisible (a passive radar property) | By design |
| W18 | **Linear row sync** | CFO/SFO are modelled as linear over 75 ms; oscillator phase noise inside the CPI is not tracked | Widens Doppler peaks | Accepted (ruling). Upgrade: per-row residual tracking with the target Doppler protected |
| W19 | **Tracker LLR is unbounded above; pe saturates** | A confirmed track's $\ell$ grows about 12 per hit; each miss costs only $\ln(1-P_D)$ ≈ −2.3 | After 100 hits, a track **coasts for ≈ 500 CPIs (≈ 40 s)** after the target leaves, with $p_e \approx 1$ | Standard fix: cap $\ell$ (e.g. at the confirm threshold plus a window) or use a windowed SPRT. Not built |
| W20 | **Adaptive $q$ is a lifetime mean** | $q = q_0\,\overline{\mathrm{NIS}}/3$ over the whole track | A manoeuvre late in a long track moves $q$ only slowly (1/n); a manoeuvring target can grow $q$ without bound (ruling) | Accepted |
| W21 | **Rate update uses the array centroid** | $h$ uses $\mathbf u_{c\to x}$ instead of the detecting channel's $\mathbf u_{rx_i\to x}$ | A small model error of the order of W12 | Accepted |
| W22 | **CFAR null is approximate** | Voxels are oversampled ($c/(4B)$) and correlated, and interpolation changes the per-cell law | True $P_{fa}$ is likely *below* design, i.e. conservative | Measure the false-object rate on a static scene |
| W23 | **Compute margins** | Envelope: 9.3 M cells × 4 interpolations per CPI (the CPU is not real time at this size). Refinement cost $\propto R^{-3}$: **124 M points for a target 2 m from the centroid**. CPU row-sync search ≈ 25 M MACs for narrow hopping grants | Overruns, backlog, `queue_waits` | GPU path (Task 10). Watch `timing_ms` and `overrun` in the health strip. <!-- FILL: measured p50/p95 per stage, CPU and GPU --> |
| W24 | **Manual template is cell-specific** | DCI 1_1 = 47 b, 0_1 = 43 b, TDA/DM-RS/MCS table were read from the lab gNB's log on 2026-09-23 | A gNB restart with a changed config gives zero accepts (silent) | Re-check the RRC/FAPI log after any gNB restart (§6.3) |
| W25 | **UL TX-side unknowns** | UE position, NLOS UE, sparse UL rows | The UL path would bias and ghost | OFF by default |

---

## 5. Measured results

### 5.1 Unit tests (as reported by each task implementer; re-run on the final tree by the finaliser)

| Test | What it proves | Result |
|---|---|---|
| `test_nr_isac_coherent_report` | volume parsing; two sinks in the same second get distinct paths; append never truncates; $\Delta\tau$ = 0 on the baseline | PASS |
| `make_coherent_scene.py` self-test | record layout, `resid_delay_s`, DL-only cadence (> 100 rows, > 5 allocation widths), LOS-referenced peak within 20 ns | PASS; a 6 s scene in 2.80 s |
| `test_nr_isac_coherent_core` | axes, LOS, row sync, RD; contiguous, **wall stronger than LOS**, and **comb-2 with −360 m STO** CPIs; non-monotonic rejection; gamma quantile vs scipy | PASS. **200/200 hop seeds, 2400 LOS checks, max 0.111 bin, mean 0.027 bin** (tol 0.25); comb-2 ≤ 0.06 bin; $Q^{-1}$ matches scipy to 1e-9 (shape 700/2000 at p = 1e-4) |
| `test_nr_isac_coherent_calibration` | phase recovery (3σ-consistent), variance shrinks, $q$ recovery, scrambled → fade, missing-LOS predict | PASS. Scrambled $c_i$ 0.16–0.27, mean ρ (last 20) **0.103**; stable $c_i$ ≈ 1.000; $\hat q/q$ 0.84–1.14 (10 seeds, SNR 2000) |
| `test_nr_isac_coherent_tracker` | Hungarian vs brute force (3000 trials); assoc remap after deletion; 3 targets confirmed; clutter never confirmed; $P_D \in (0.6,1)$; PD covariance (6×6 Cholesky) every step with rate updates | PASS; 400-detection association burst 0.005 ms |
| `test_nr_isac_coherent_autofocus` | see the §3.14 table; sign test; focus-geometry contract | PASS (σ = 0.1 → declines, no regression) |
| `test_coherent_view.py`, `test_monitor.py` | the monitor follows the newest file; RD range-major transpose | PASS, 7/7 |
| `test_nr_isac_coherent_focus` [T5] | 3 targets within a resolution cell; mirror rejected; harmonic merged; derived grid and $P_{fa}$ | <!-- FILL --> |
| `test_nr_isac_coherent_ul` [T8] | UE localised within 2 resolution cells | <!-- FILL --> |
| `test_nr_isac_coherent_cuda_parity` [T10] | RD rel. err < 1e-3; same detections, Δsnr < 1 %; refine within fringe/2 | <!-- FILL: parity numbers --> |

Command for all of them (main worktree, sensing build):
```bash
ssh sens6 'cd /home/sens/NICOLA/multirx-clean-adaptive/cmake_targets/ran_build/build_sense && \
  ninja NR_UE_ISAC $(ninja -t targets all | grep -o "^test_nr_isac_coherent[a-z_]*" | sort -u) && \
  ctest -R test_nr_isac_coherent --output-on-failure'
```
<!-- FILL: ctest output on the final tree -->

### 5.2 Offline end-to-end (synthetic scene → `isac_replay` lockstep) [T9]

**Scene** (`make_coherent_scene.py`, 6 s):
- 10 m checkerboard square; gNB at (35, 20, 6).
- Random per-channel phases, sub-ns cable residuals, 37 Hz common CFO, a 7-of-10 DL pattern with 60 %
  grant probability and random widths 4–273 PRB, 20 dB SNR.
- A gNB wall reflection *stronger than the LOS on ch2*.
- Targets: a person (4, 18, 1), −1.2 m/s y; a car (−12, 5, 0.8), 8 m/s x; a drone (8, 8, 15) with a ground bounce.
- The survey is perturbed as a tape would be.

```bash
ssh sens6 'cd /home/sens/NICOLA/multirx-clean-adaptive && \
  BUILD=$PWD/cmake_targets/ran_build/build_sense bash openair1/PHY/NR_UE_ISAC/tools/test_coherent_chain.sh'
```

| Metric | Value |
|---|---|
| CPIs processed / skipped | <!-- FILL --> |
| Phase recovery error, ch1–3 (vs expected LOS-tap phase, ruling R2) | <!-- FILL --> |
| Final $G$ / $\rho$ | <!-- FILL --> |
| Confirmed tracks / targets; per-target position error vs range resolution | <!-- FILL --> |
| Below-ground tracks; ghost tracks | <!-- FILL --> |
| Per-stage CPU timing p50/p95 (ms) vs $T_{cpi}$ | <!-- FILL --> |
| Non-regression (`test_offline_chain.sh`, `coherent_enable = 0`) | <!-- FILL --> |

### 5.3 Real time (GPU, `isac_replay_rt --realtime`, cores 11–13) [T10]

| Metric | Value |
|---|---|
| Per-stage GPU timing p50 / p95 (sync, rd, env, detect, refine, track, total) | <!-- FILL --> |
| total p95 vs median $T_{cpi}$ | <!-- FILL --> |
| overruns / queue_waits / CPI count equal to lockstep | <!-- FILL --> |
| Host→device bytes per CPI | <!-- FILL --> |

### 5.4 OTA

**None yet.** §7 is the procedure that produces them.

---

## 6. Hardware and measurement needs

### 6.1 RF

- **One USRP X410**, 4 RX channels (`--ue-nb-ant-rx 4`). One device = one reference clock and one
  sample clock for all four channels (the premise of the design).
- **Addresses:** data plane `192.168.20.2` (host NIC `enp2s0f1np1`); management `192.168.1.140`
  (pass `--mgmt`; `uhd_find_devices` reports the data port as `mgmt_addr`).
- **Four identical antennas**: same model, same orientation, same polarisation as the gNB antenna
  (normally vertical).
- **Four identical coax cables**: same type, length and batch. Strictly, LOS referencing and the
  calibrator absorb any constant cable delay or phase. Identical cables keep **temperature-driven
  phase drift common-mode**: an unequal cable's phase drifts by degrees per kelvin at 3.45 GHz. They
  also keep the channels' SNR equal.
- **Cable length.** With the X410 at the square's centre, each cable must reach
  $\sqrt2\cdot s/2 + 3.5$ m plus slack: about 11 m for a 10 m square, about 8 m for a 5 m square. If
  the cables are shorter, place the X410 elsewhere and shift `coherent_volume_m` to match (§7a).
- **Masts/tripods** for 0.5 m and 3.5 m phase-centre heights. They must be rigid: a mast swaying by
  1 cm is 0.7 rad of phase (§4.2 W1).

### 6.2 Host (sens6)

| Need | Value / check |
|---|---|
| CPU | 14 cores. Core map in the template header: RT PHY pool 0,1,4,5,6,7; RF RX thread 2; UL decode 3; DL decode consumers 6–10; NIC IRQs 8–13; **sensing 11,12,13**; Python monitor 4 |
| Governor | **`performance`** on every core. `run_sensing.sh` aborts otherwise: `sudo cpupower frequency-set -g performance` |
| GPU | RTX 4060 Ti, 8 GB. `NR_ISAC_REQUIRE_CUDA=1` fails closed if CUDA is not warm |
| Build | `build_sense` (sensing ON + CUDA) containing Tasks 1–10, **plus `ninja oai_usrpdevif`** (the dlopen'd UHD plugin carries the per-channel retune fix, W7). **Never build during a capture** (the launcher refuses) |
| Link | 10 GbE to the X410 with the ring/MTU tuning that `run_arm.sh`'s link preflight asserts |

### 6.3 The lab gNB configuration the manual template assumes

These values were read from the gNB's own RRC/FAPI log on 2026-09-23. They are
`tests/passive_rx/ota/sensing_ota_manual.conf.template` plus `run_arm.sh`.

| Parameter | Value |
|---|---|
| Cell | PCI 2; band n78; `dl_arfcn` 630000 = **3450.00 MHz** carrier; 273 PRB; 30 kHz SCS |
| SSB | `dl_ssb_arfcn` 627264 = 3408.96 MHz; point A 3400.86 MHz → **`--ssb 150`** (ssb_start_subcarrier) |
| CORESET1 / SS | 45 CCE groups, duration 1, non-interleaved, nid 2; SS symbol 0, every slot |
| DL DCI 1_1 | **47 bits**. Field widths: bwp 0, vrb 0, bundling 0, ratematch 0, zp 0, tb2 0, harq 4, dai 2, k1 3, antports 4, tci 0, srs 2, cbg 0 |
| UL DCI 0_1 | **43 bits** |
| PDSCH TDA | no dedicated list → SIB1 common, 2 entries: (S1, L13), (S1, L7) → 1-bit field |
| DL DM-RS | type 1, `additionalPosition` pos2 → symbols **2, 7, 11**, maxLength 1 |
| MCS table | **qam256** |
| UL | BWP 0+273, SIB1 common TDA (6 entries, 3 bits), DM-RS type 1 pos2, qam64, scrambling IDs 2 |
| X410 oscillator | about **−15 kHz** against this gNB, just beyond the ±15 kHz PSS search → **`--initial-fo -15000`** (`INITIALFO=-15000`) |

**Traffic.** Rows exist only where the gNB schedules PDSCH, so **a UE must be attached with steady DL
traffic** for the whole capture. The Doppler span is set by the grant spacing (W14):
- a grant every DL slot → ±87 m/s of path rate;
- a grant every 6 ms → only ±7 m/s.

Use a constant-rate DL flow, e.g. `iperf3 -u -R` at a fixed rate, and read `rows` per CPI in the
reports.

### 6.4 Survey requirements and what accuracy buys

| Declared 1σ survey accuracy | Envelope | Coherent refinement | Autofocus |
|---|---|---|---|
| ≤ 1 cm | full | usable immediately (phase error ≲ 0.7 rad) | admits typical geometries → can refine to mm |
| 1–2 cm | full | partial (0.7–1.5 rad) | admits only low-$\lvert\Delta\mathbf u\rvert$ detections |
| 5–10 cm (tape, careless) | full | **useless**; ρ-blend risk W3 | **declines** (`af_corr_m = 0`) |
| ≫ 1 m | misfocus | — | — |

- The **gNB antenna** needs the same care (W5).
- **Heights must be measured from the ground plane** that reflects ($z<0$ rejection, W15a).
- The declared value goes into `coherent_survey_sigma_m`. It must be **honest**: over-claiming
  accuracy lets autofocus admit wrapped residuals.

---

## 7. Tomorrow's procedure

### 7a. Measuring-tape survey, step by step

**Tools:**
- a **steel** tape ≥ 30 m and a 5 m tape (not fibreglass: it stretches by mm under tension);
- a plumb bob; a spirit level; chalk line or string; marking tape;
- a 3-4-5 (or 6-8-10) right-angle template;
- a helper.

**Frame** (right-handed, metres, $+z$ up):
1. **Origin, horizontal.** Pick a reference mark on the X410 chassis (e.g. the RF0 front-panel
   connector). Drop the plumb bob and mark the floor point **O** beneath it.
2. **Origin, vertical.** $z = 0$ is the **ground plane at O**, not the X410's height. The detector
   rejects $z<0$ as the ground-bounce mirror, and the default volume starts at $z=0$. If the ground is
   not level across the site, measure every height from the ground directly beneath that point, and
   note any slope.
3. **$+x$ axis.** Stretch a string from O parallel to one edge of the antenna square, to a stake at
   least 12 m away. Mark it "+x".
4. **$+y$ axis.** At O, lay the right angle: 3 m along +x, 4 m toward the intended +y, a 5 m diagonal
   (use 6-8-10 for more precision). $+y$ is **counter-clockwise from $+x$ seen from above**. Stretch a
   second string along it.
5. **$+z$** is up.

**Layout.**
- Put the X410 at the centre of the square (cables permitting), so the default
  `coherent_volume_m = "-15:15:-15:15:0:30"` is centred.
- For a 10 m square, target positions:
  - **ch0** (−5, −5, 0.5)
  - **ch1** (5, −5, 3.5)
  - **ch2** (−5, 5, 3.5)
  - **ch3** (5, 5, 0.5)
- That is a checkerboard: diagonal pairs at the same height, adjacent pairs 3 m apart vertically. The
  3 m vertical baseline is what gives elevation, for drones. For a 5 m square use ±2.5.
- `ch<i>` = X410 RX channel $i$ = `--ue-nb-ant-rx` order. Label each cable at both ends with its channel
  number. Verify the mapping on the static run (§7e step 1).
- Give **every antenna a clear line of sight to the gNB antenna** (W15c), and keep large reflecting
  walls out of the volume (W15b).

**What to measure, for each antenna $i$ and for the gNB antenna.**
1. **Phase centre = the feed point.**
   - dipole: the gap between the arms;
   - monopole: the base of the radiator at the ground plane;
   - patch/panel: the centre of the radiating face.
   Mark it with tape.
2. **Plumb it down** and mark the floor point $F_i$.
3. **$x_i$**: along the +x string to the foot of the perpendicular from $F_i$. Check the perpendicular
   with the 3-4-5.
4. **$y_i$**: the perpendicular offset from the string to $F_i$. Sign by the side.
5. **$z_i$**: vertical tape from $F_i$ to the phase centre, held plumb (spirit level on the tape).
   Target 0.500 m and 3.500 m; record what you *measured*, not the target.
6. **All measurements are either on the floor (horizontal) or vertical.** A tape stretched in the air
   over 10–14 m sags by cm; that is the whole error budget.

**Redundant checks.** Tape these **horizontal** floor distances between plumb marks:
- all **6 pairs** of the four antennas: 4 sides and **2 diagonals**;
- the gNB plumb mark to at least **two** antennas.

The coordinates predict every one of them. A single wrong coordinate shows up as residuals on **the
three distances that involve that point and on none of the others**, which identifies the bad point.
Unequal diagonals mean the square is not square, or a coordinate is wrong.

**Target accuracy: ±1 cm per coordinate (accept ≤ 2 cm).** Why:
- It keeps the coherent phase error $k|\Delta\mathbf u|\delta$ ≲ 0.7 rad (W1).
- It puts the survey inside autofocus's capture range, $3k|\Delta\mathbf u|\sigma < \pi$ →
  $\sigma \lesssim$ 1.4 cm at $|\Delta\mathbf u| = 1$ (W2).
- At 5–10 cm the system still works, but envelope-only.

**Write `tests/passive_rx/ota/survey.json`.** Schema = what `tests/passive_rx/survey.py` reads:
- `gnb_m`: [x, y, z];
- `rx_antennas_m.ch0..ch3`: [x, y, z];
- `max_range_m` > 0 (required).

`survey.py` rejects antennas closer than 1 m to each other or to the gNB. Extra keys are ignored, so
keep the check distances in the same file:

```json
{"frame": "right-handed, metres; origin = floor point under the X410 reference mark; +z up; z = 0 is the ground plane",
 "gnb_m": [30.00, 12.00, 2.50],
 "rx_antennas_m": {"ch0": [-5.00, -5.00, 0.50], "ch1": [5.00, -5.00, 3.50], "ch2": [-5.00, 5.00, 3.50], "ch3": [5.00, 5.00, 0.50]},
 "max_range_m": 120,
 "check_distances_m": {"h:ch0-ch1": 10.00, "h:ch1-ch3": 10.00, "h:ch3-ch2": 10.01, "h:ch2-ch0": 10.00,
                       "h:ch0-ch3": 14.14, "h:ch1-ch2": 14.15, "h:gnb-ch0": 38.91, "h:gnb-ch3": 25.96},
 "survey_notes": "steel tape, horizontal distances on the floor between plumb marks, heights by vertical tape"}
```

- The numbers above are illustrative. Replace every one with a measurement.
- `h:` = horizontal (floor) distance; a key without `h:` is a 3D distance.
- `max_range_m` is required by `survey.py` and used by the legacy stages. Use ≥ the largest bistatic
  excess path over the volume (≈ 120 m covers a 30 m cube with a gNB ≤ 40 m away).

**Consistency check.** Run on sens6; it prints each residual and the value to declare:
```bash
python3 - tests/passive_rx/ota/survey.json <<'EOF'
import json, math, sys
s = json.load(open(sys.argv[1]))
P = {"gnb": s["gnb_m"], **s["rx_antennas_m"]}
res = []
for key, taped in s.get("check_distances_m", {}).items():
    horiz = key.startswith("h:"); a, b = (key[2:] if horiz else key).split("-")
    model = math.dist(P[a][:2], P[b][:2]) if horiz else math.dist(P[a], P[b])
    res.append(taped - model)
    print(f"{key:12s} taped {taped:8.3f}  from coords {model:8.3f}  residual {100*(taped-model):+6.1f} cm"
          + ("   <-- RE-MEASURE" if abs(taped - model) > 0.02 else ""))
if res:
    rms = math.sqrt(sum(r * r for r in res) / len(res))
    print(f"RMS residual {100*rms:.1f} cm over {len(res)} checks -> declare coherent_survey_sigma_m >= {max(0.01, rms):.3f}")
EOF
```

**Declare the accuracy and set the volume.** Both are template keys, not survey keys. Make a
run-specific copy of the manual template:
```bash
cd /home/sens/NICOLA/multirx-clean-adaptive
cp tests/passive_rx/ota/sensing_ota_manual.conf.template tests/passive_rx/ota/sensing_ota_manual.today.conf.template
sed -i 's/^\(\s*coherent_survey_sigma_m\s*=\s*\).*;/\10.015;/' tests/passive_rx/ota/sensing_ota_manual.today.conf.template        # your RMS
sed -i 's/^\(\s*coherent_volume_m\s*=\s*\).*;/\1"-15:15:-15:15:0:30";/' tests/passive_rx/ota/sensing_ota_manual.today.conf.template  # same frame as survey.json
python3 tests/passive_rx/survey.py tests/passive_rx/ota/survey.json --geometry /tmp/g.json \
  --apply tests/passive_rx/ota/sensing_ota_manual.today.conf.template /tmp/ue.conf --report-path /tmp/x/reports.jsonl \
  && grep -nE 'spatial_rx_positions|tx_pos_|coherent_' /tmp/ue.conf     # dry-run: shows exactly what the receiver will get
```

### 7b. Capture

Before **every** launch:
```bash
ssh sens6
test -e /home/sens/NICOLA/.capture_active && echo LOCKED; pgrep -x nr-uesoftmodem   # both must print nothing
sudo -v                                                                              # the launcher uses sudo
sudo cpupower frequency-set -g performance
# The X410 refuses the next session after every receiver exit (observed 2026-09-23): restart MPM and SETTLE.
sleep 60                                                                             # after a stopped/killed capture
ssh -o BatchMode=yes root@192.168.1.140 'systemctl restart usrp-hwd'
sleep 180                                                                            # measured: 40 s is too short (PBCH never decodes)
ping -c1 -W2 192.168.20.2 && timeout 20 uhd_find_devices --args "type=x4xx,addr=192.168.20.2" | grep -q x4xx && echo X410 OK
```
- Alternative: prefix the launch with `FORCE_HWD=1`. `run_arm.sh` then restarts `usrp-hwd` itself and
  sleeps `SETTLE_S` (180 s).
- The manual sequence above lets you *see* that the device came back.

**Fully manual coherent run.** One command. It runs the preflight, a 30 s radio wait, the receiver with
the in-process engine, the monitor, supervision, and a verdict:
```bash
cd /home/sens/NICOLA/multirx-clean-adaptive
TEMPLATE=$PWD/tests/passive_rx/ota/sensing_ota_manual.today.conf.template \
SCAN=0 SSB=150 INITIALFO=-15000 NANT=4 MRC=0 \
  bash tests/passive_rx/run_sensing.sh --survey tests/passive_rx/ota/survey.json \
       --dur 300 --rxg 43 --tries 1 --mgmt 192.168.1.140 \
       --run-dir /home/sens/NICOLA/sensing_runs/$(date +%Y%m%d_%H%M%S)_static
```

| Setting | Why |
|---|---|
| `TEMPLATE=` | the manual (non-agnostic) template with the coherent block. The launcher's default is the *agnostic* template |
| `SCAN=0 SSB=150` | pins the SSB. `run_sensing.sh` defaults to `SCAN=1` (blind scan, which has a known crash risk at 4 antennas / 273 PRB) |
| **`INITIALFO=-15000`** | **must be explicit**: `run_sensing.sh` passes `INITIALFO=${INITIALFO:-0}`, which *overrides* `run_arm.sh`'s −15000 default. At 0 the receiver starts outside the PSS search range |
| `NANT=4` | all four channels (4 is also the default) |
| `MRC=0` | the `run_arm.sh` default: decode on branch 0 only (4-branch MRC gave 0 % CRC before the retune fix; unvalidated since). It affects only *decoding* $\hat X$; the CFR is formed on all 4 antennas regardless |
| `--rxg 43` | the launcher default gain |
| `--dur` | seconds per run |
| `--tries 1` | no automatic retries; do the MPM restart above between runs instead |
| `--run-dir` | one directory per scene, so runs never mix |
| `--debug` | optional; writes `NR_ISAC_DEBUG_DIR` |

`run_sensing.sh`'s options are exactly: `--dur --tries --rxg --survey --debug --port --mgmt --run-dir`.
Anything else exits with "unknown arg". Its environment overrides are `TEMPLATE SCAN SSB INITIALFO NANT`
and `BUILD SENSE_CPUS CHAIN_CPU MON_CPU DISC_CPUS`. Every other variable (`MRC`, `FORCE_HWD`,
`SETTLE_S`, …) is inherited by `run_arm.sh`.

**In coherent mode the launcher:**
- detects `coherent_enable = 1` in the applied conf and logs `coherent mode: 1`;
- does **not** start `realtime_chain.py`;
- points the monitor at the run directory.

**Outputs in the run directory:**
- `coherent_reports.*.jsonl`, `coherent_tracks.*.jsonl`, `coherence.*.jsonl`;
- `ue.conf`, `geometry.json`, `launcher.log`, `run_arm.out`;
- `capture/` → the receiver's `run.log`;
- `run_verdict.json`.

**Stop early** with Ctrl-C (the trap stops everything and writes the verdict). Then do the 60 s + MPM
restart + 180 s sequence before the next launch.

### 7c. UI

From the laptop:
```bash
ssh -L 8080:localhost:8080 sens6
```
Then open **http://localhost:8080/coherent**. `/`, `/receiver` and `/sensing` still exist; the
launcher also prints this hint in `launcher.log`.

The coherent page shows:
- **3D scene**: antennas, gNB, volume box, detections sized by SNR (hover for values), track trails and
  velocity arrows coloured by existence;
- **top-view envelope heatmap**;
- **the 4 per-channel RD maps**;
- **coherence panel**: ∠g_i, jitter vs bound, $G$/$\rho$ over time;
- **health strip**: CPI time vs $T_{cpi}$, GPU/CPU, overruns, queue waits.

### 7d. Analysis of a run directory

Quick verdict and gate:
```bash
RUN=$(ls -1dt /home/sens/NICOLA/sensing_runs/*/ | head -1)   # newest run; or set it to the scene's --run-dir
cat $RUN/run_verdict.json                       # gate_opened, cpis, cfo_mislock, arm_verdict (VALID?), valid
grep -m3 'SENSING_GATE open\|coherent fuser enabled\|coherent pipeline writing' $RUN/capture/run.log
```

Per-CPI timing p50/p95 vs $T_{cpi}$, coherence $G$/$\rho$ trace, detections per CPI and confirmed
tracks. Ready to run on sens6; it reads every retry file of each stem:
```bash
python3 - "$RUN" <<'EOF'
import collections, glob, json, math, sys
import numpy as np
run = sys.argv[1]
def load(stem):                                   # all files of a stem (retries included), in name order
    out = []
    for fp in sorted(glob.glob(f"{run}/{stem}.*.jsonl")):
        for line in open(fp, errors="replace"):
            try: out.append(json.loads(line))
            except json.JSONDecodeError: pass     # a line still being written
    return out
reps, trk, coh = load("coherent_reports"), load("coherent_tracks"), load("coherence")
ok = [r for r in reps if r.get("skipped_reason") is None]
print(f"CPIs: {len(reps)} reported, {len(ok)} processed, skipped reasons: "
      f"{dict(collections.Counter(r['skipped_reason'] for r in reps if r.get('skipped_reason')))}")
if ok:
    tcpi = 1e3 * np.median([r["t_cpi_s"] for r in ok])
    print(f"timing (ms)  t_cpi median {tcpi:.1f}   gpu={sorted({r.get('gpu') for r in ok})}")
    for k in ("sync", "rd", "env", "detect", "refine", "track", "total"):
        v = np.array([r["timing_ms"][k] for r in ok if r["timing_ms"].get(k) is not None])
        if v.size: print(f"  {k:7s} p50 {np.percentile(v,50):8.2f}  p95 {np.percentile(v,95):8.2f}  max {v.max():8.2f}")
    print(f"  last stats {ok[-1].get('stats')}   overrun CPIs {sum(bool(r.get('overrun')) for r in ok)}")
    print(f"  B_eff median {np.median([r['b_eff_hz'] for r in ok])/1e6:.2f} MHz, range_res {np.median([r['range_res_m'] for r in ok]):.2f} m, "
          f"rows/CPI median {np.median([r['rows'] for r in ok]):.0f}")
    nd = np.array([len(r["detections"]) for r in ok])
    print(f"detections/CPI: mean {nd.mean():.2f}  max {nd.max()}  zero-CPIs {int((nd==0).sum())}")
c = [x for x in coh if not x.get("skipped")]
if c:
    G = np.array([x["G"] for x in c]); rho = np.array([x["rho"] for x in c])
    print(f"coherence: G p10/p50/p90 {np.percentile(G,10):.2f}/{np.median(G):.2f}/{np.percentile(G,90):.2f}   "
          f"rho p10/p50/p90 {np.percentile(rho,10):.2f}/{np.median(rho):.2f}/{np.percentile(rho,90):.2f}")
    ph = np.array([x["phase"] for x in c[-100:]])
    cstd = [math.degrees(math.sqrt(-2 * math.log(max(1e-12, abs(np.mean(np.exp(1j * ph[:, i]))))))) for i in range(4)]
    print("  per-ch circular phase std, last 100 CPIs (deg):", [round(v, 1) for v in cstd])
    print("  LOS found fraction per ch:", [round(float(np.mean([x["los_found"][i] for x in c])), 3) for i in range(4)],
          " LOS SNR median per ch (dB):", [round(10 * math.log10(max(1e-12, np.median([x["snr"][i] for x in c]))), 1) for i in range(4)])
    print("  af_corr_m (last):", c[-1].get("af_corr_m"))
    step = max(1, len(c) // 20)
    print("  trace (cpi  G  rho  phase_deg[0..3]):")
    for x in c[::step]:
        print(f"    {x['cpi']:6d} {x['G']:5.2f} {x['rho']:5.2f}  " + " ".join(f"{math.degrees(p):7.1f}" for p in x["phase"]))
if trk:
    conf = [[t for t in r["tracks"] if t.get("confirmed")] for r in trk]
    ids = {t["id"] for r in conf for t in r}
    life = collections.Counter(t["id"] for r in conf for t in r)
    print(f"tracks: confirmed/CPI mean {np.mean([len(x) for x in conf]):.2f} max {max(len(x) for x in conf)}; "
          f"distinct confirmed ids {len(ids)}; longest (id, CPIs) {life.most_common(3)}")
    if conf[-1]:
        print("  last CPI confirmed:", [(t["id"], [round(v, 2) for v in t["p"]], [round(v, 2) for v in t["v"]], round(t["pe"], 3)) for t in conf[-1]])
EOF
```
It was verified against the Task 11 monitor fixture (`/tmp/coh_fixture` on sens6).

**Ground truth for the moving scenes.**
- Mark waypoints on the ground and survey them with the same tape frame.
- Note UTC times: the host clock is NTP-synced, and `t` in the JSONL is the CPI's air time.
- Compare `p` of the confirmed tracks against the waypoints. Score with a tolerance of one range cell
  ($c/B$ ≈ 3 m) for envelope-grade positions, and of the fringe width only if $\rho$ ≈ 1 **and** the
  survey was ≤ 1 cm.

### 7e. OTA test order and what "good" looks like

Run in this order. Do not move on until the current step looks right.

**1. Static coherence** (empty volume, 5 min).

What good looks like on the coherence panel:
- `los_found` = true on all 4 channels in nearly every CPI.
- LOS SNR within a few dB across channels.
- **$G \to 4$** (p50 > 3.5).
- **$\rho \to 1$** (p50 > 0.8).
- **Phase traces flat**: circular phase std over 100 CPIs of a few degrees; only slow drift.
- `jitter` at or below `bound`. It can be below, see W16.
- Few detections per CPI: about 1/s over the whole volume, at `false_object_intensity_per_s = 1`.
- No overruns; `queue_waits` not growing.

**Channel-map check.** Briefly cover (not disconnect) one antenna at a time. That channel's `snr` must
drop. This confirms `ch<i>` in the survey = X410 channel $i$.

If it is not good:

| Symptom | Likely cause | Action |
|---|---|---|
| `los_found` false on a channel / its SNR ≫ lower | cable, connector, antenna, mapping; or the per-antenna CFR is not reaching the engine | check the connector; confirm `NANT=4`; check the per-branch power in `run.log` |
| $G$ ≈ 1–2 with phases jumping every CPI | per-channel CFO (W7); or a CFO mis-lock | check that `liboai_usrpdevif.so` was rebuilt from this tree; check the `cfo_mislock` verdict; restart the X410 per §7b and relaunch |
| Phase **steps** at discrete times, flat in between | X410 retune/re-init (W6) | expected. The channel fades for a few CPIs and recovers; correlate with re-init lines in `run.log` |
| Slow drift of one channel | cable temperature (sun/shade) | acceptable while $G$ > 3.5 |
| LOS delay jumping between CPIs on a channel | blocked LOS, so the wall reflection is taken as LOS (W15c) | re-site that antenna for a clear view of the gNB |
| No CPIs, or `skipped_reason` "fewer than 3 rows" | no DL traffic, or the gate is closed | check that the UE is attached and the DL flow is running, and `SENSING_GATE open` in `run.log` |
| Overruns | compute (W23) | check `"gpu": true` and the per-stage timing |

**2. Walking person.**
- Walk **along the gNB–array bisector**, toward and away, briskly (≥ 1.5 m/s). This gives a path rate
  above the ±2.3 m/s notch.
- Then walk tangentially: **expect it to disappear**. That is the physics of W11, not a failure.
- Good: one confirmed track following the waypoints within about one range cell, $p_e$ → 1, no
  duplicate tracks.
- If the track appears only when walking radially: expected.
- If there are duplicate tracks at the same place: check W12/W14. Look for the same voxel at two
  Dopplers in `detections`.

**3. Car.**
- Drive at 10–20 km/h, ≥ 15 m from the array, through the volume.
- Good: one track with the correct speed.
- Duplicates at one position mean near-field Doppler diversity (W12) or TDD replicas (W14). Note the
  Doppler separation: if it is ≈ $1/T_p$ it is TDD; if it grows as the car nears the array it is W12.
- Low `rows` per CPI with a fast car means aliasing (W14): raise the DL traffic.

**4. Drone.**
- Hover, then fly slow lines at 5–15 m altitude.
- Good: $z > 0$ with no mirror track, and the altitude roughly right. Elevation accuracy is weaker than
  horizontal: 3 m vertical against 10–14 m horizontal baseline.
- Expected: extra short-lived tentative tracks at the drone from rotor micro-Doppler (W13).

**Throughout:** keep `af_corr_m` in view. It stays 0 unless the survey is ≤ about 1.4 cm (W2); non-zero
values mean autofocus judged a correction statistically significant.

---

## 8. Rulings (ledger, verbatim, grouped by task)

**Pre-flight / process**
- R1 Ruling: implementers run in PARALLEL (separate worktrees/branches) for non-interfering tasks, overriding the skill's "never parallel" default — the user explicitly asked for it — cost if wrong: merge conflicts in CMakeLists.txt, resolved by the controller at merge.
- R2 Ruling: Task 9's chain test must compare the calibration phase against the EXPECTED LOS-tap phase, not the raw channel phase: expected_i = wrap((φ_i − φ_0) − 2π·fc·((|rx_i−gnb| − |rx_0−gnb|)/c + (resid_i − resid_0))). Task 2's generator therefore also writes "resid_delay_s":[4] into truth.json — because after LOS referencing the tap keeps the carrier term e^{−j2π fc d_i} (the calibration correctly absorbs it; the plan's assertion forgot it) — cost if wrong: a false test failure/pass on phase recovery.
- R3 Ruling: Task 4's scrambled-phase test drops the "coh_factor < 0.5" assertion (a per-CPI phase hop is legitimately recalibrated within the CPI, so the posterior variance is small and c_i ≈ 1 is correct); it keeps rho < 0.35 (use the mean rho over the last 20 updates if the single last value is noisy) and asserts coh_factor finite in [0,1] — because ρ (predicted-phase coherence) is the right gate for between-CPI hops — cost if wrong: coherent refinement is used on a CPI whose phases hopped, but ρ then blends it back to the envelope anyway. *(Superseded for coh_factor by the Task 4 predictive-fade ruling below.)*

**Task 1**
- Ruling: T1 commit carries "Co-Authored-By: Claude Sonnet 5" instead of the Opus 5.5 line — accepted, no amend (amending is forbidden and attribution is not functional) — cost if wrong: one commit with a different trailer.

**Task 3 (axes, LOS, row sync, RD)**
- Ruling: accept Task 3's test hop change (regular 37-PRB stride creates a waveform range-Doppler ambiguity no linear processing removes; the test must exercise the code) — cost if wrong: the regular-hop case is untested in T3; carried into T5 as a required envelope-rejection test.
- Ruling: accept the Hann peak-sidelobe guard in find_los (a window property, -31.5 dB, not a tuned value) — cost if wrong: a true earlier path >28.5 dB below the strongest is taken as a sidelobe. *(Later replaced in fix round 1 by the fully derived kernel-based guard, §3.5.)*
- Ruling: accept linear-in-time row sync (declared model: CFO+SFO are linear over a 75 ms CPI; oscillator phase noise within a CPI is not followed) — cost if wrong: residual intra-CPI phase noise widens Doppler peaks.
- Ruling: derive_axes rejects non-monotonic row_time_slots with reason "non-monotonic row times" — reviewer ⚠️ + known past out-of-order CFR bug — cost if wrong: an out-of-order CPI is skipped instead of processed.
- Ruling: Task 3 fix commit 544d516234 has a truncated message without the Co-Authored-By trailer — accepted, no amend (amend forbidden) — cost if wrong: one commit message incomplete.

**Task 4 (calibrator)**
- Ruling: Task 4 uses Mehra-style unclipped cumulative matching nu²−(p_prev+r), clipped only at use; first valid measurement seeds the state — derived, fixes a measured 8×-CRB floor — cost if wrong: none measured.
- Ruling: Task 4 coh_factor = exp(−(p+q)/2) (predictive variance), per spec §2 — supersedes R3; a channel with unpredictable phase fades — cost if wrong: after an X410 retune phase hop a channel's weight dips for a few CPIs (q̂ decays as 1/n).

**Task 6 (tracker)**
- Ruling: Task 6 assoc-after-erase bug is fixed despite being in the plan's code — spec needs correct association for autofocus — cost if wrong: none.
- Ruling: Task 6 adaptive q = q0·mean_NIS/3 (plain covariance matching), no clamp — golden rule — cost if wrong: q can grow unboundedly on a manoeuvring target (that is the intended adaptation).
- Ruling: Task 6 range-rate update uses Joseph form — spec says Joseph + symmetrise — cost if wrong: none.
- Ruling: Task 6 gradient normalized(x−tx)+normalized(x−rx) stands; spec notation means unit vectors toward the target — cost if wrong: sign of rate innovation (would show in the new rate test).

**Task 7 (autofocus)**
- Ruling: Task 7 adopts the reviewer's derived estimator (innovation form, joint centred 3·kCh state, admission z·sqrt(aᵀPa+1/(2SNR))<π with z=3, per-antenna χ²₃ 1 % apply test) — golden rule + non-divergence — cost if wrong: autofocus inactive more often.
- Ruling (spec-level): autofocus is phase-only with a mm–cm capture range; from a tape-grade survey (σ=0.1 m) it safely declines to act. A coarse delay/envelope stage is future work, NOT built tonight — cost if wrong: tomorrow's OTA runs will likely show af_corr_m = 0 unless the survey is ~1-2 cm; the survey procedure will ask for careful ±1-2 cm tape work.
- Ruling: Task 7 add() takes the focus geometry (add(d, focus_geometry)) — the pipeline focuses with the refined geometry — cost if wrong: none (Task 9 dispatch must call add(d, geo)).
- Ruling: Task 7 ≥50 % bar applies at 4000 detections; 400 detections assert non-regression only — autofocus is a slow long-run estimator — cost if wrong: slower visible convergence OTA.
- Ruling: Task 7 apply test uses the marginal (P_ii)^-1 instead of J_ii — conservative significance test — cost if wrong: corrections applied later.

**Tasks 5, 8, 9, 10 and the final review**
<!-- FILL: every Ruling: line added to the ledger for Tasks 5, 8, 9, 10 and the final whole-branch review, verbatim with cost-if-wrong -->

---

## Appendix A — Derived numbers for the lab cell (for the slides)

| Quantity | Formula | Value |
|---|---|---|
| Wavelength / wavenumber | $c/f_c$, $2\pi/\lambda$ | 8.690 cm / 72.31 rad/m |
| Full bandwidth | 273 × 12 × 30 kHz | 98.28 MHz |
| Path-length resolution | $c/B$ | 3.05 m |
| Envelope voxel | $c/(4B)$ | 0.763 m → 40³ = 64 000 voxels in a 30 m cube |
| Range bin (path length) | $c/(N_{fft}\Delta f)$, $N_{fft}$ = 4096 | 2.44 m |
| Doppler bin (75 ms) | $1/T_{cpi}$; path rate $\lambda/T_{cpi}$ | 13.33 Hz; 1.16 m/s |
| Zero-Doppler notch | ±2 bins | ±26.7 Hz; ±2.32 m/s |
| Doppler span (grant every 0.5 ms) | $\pm1/(2\overline{\Delta t})$; path rate $\lambda/(2\overline{\Delta t})$ | ±1000 Hz; ±86.9 m/s |
| Tested Doppler band ($v_{max}$ = 50 m/s) | $2v_{max}/\lambda$ | 1151 Hz → 145 of 150 bins tested |
| $P_{fa}$ per test ($\lambda_{FA}$ = 1/s) | $\lambda_{FA}T/(N_{vox}N_d)$ | 8.1 × 10⁻⁹ |
| Envelope threshold | $Q^{-1}_{\Gamma(4,1)}(P_{fa})$ / median | 26.83 / 3.672 = 8.6 dB |
| Fringe width (D = 14.14 m) | $\lambda R/(2D)$ | 3.1 / 4.6 / 7.7 cm at R = 10 / 15 / 25 m |
| Autofocus capture range (z = 3) | $\pi/(3k\lvert\Delta\mathbf u\rvert)$ | 1.45 cm ($\lvert\Delta\mathbf u\rvert$ = 1); 7.2 mm (= 2) |
| SPRT thresholds (α = 1 %, β = 10 %) | $\ln\frac{1-\beta}{\alpha}$, $\ln\frac{\beta}{1-\alpha}$ | +4.50 / −2.29 |
| Gates | $\chi^2_3(0.99)$, $\chi^2_1(0.99)$ | 11.345, 6.635 |
| Clutter density (30 m cube) | $\lambda_{FA}T/V$ | 2.8 × 10⁻⁶ m⁻³ |
| Tracker start PSD | $(v_{max}/1\,\mathrm s)^2$ | 2500 m²/s³ |
