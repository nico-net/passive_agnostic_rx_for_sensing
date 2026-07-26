# TASK SPECIFICATION: Refactoring and Fixing DSP Subsystems in openair1/PHY/NR_UE_ISAC/

## CONTEXT & OBJECTIVE
We are updating the Integrated Sensing and Communications (ISAC) DSP pipeline in `openair1/PHY/NR_UE_ISAC/` (`range_doppler.{h,cc}` and `isac_sync.{h,cc}`). 
Recent analysis and live testing (`tests/sensing_sim`) revealed two primary bottlenecks:
1. Phase 1 STO fine-tracking flywheels on ~93% of rows because of a static search window, causing tracking instability.
2. Clutter cancellation and mirror-ghost rejection degrade when dynamic target signals align with dominant static Line-of-Sight (LoS) paths.

To resolve these issues, we are incorporating theoretical insights and processing frameworks from recent ISAC literature (Zhao et al., 2024; Wu et al., 2024; Guo et al., 2025).

Please refactor the implementation in `range_doppler.cc`, `isac_sync.cc`, and `eca_clutter.cc` according to the 4 specific technical tasks detailed below.

---

## TASK 1: Dynamic Search Window Expansion for Phase 1 STO Walker (`isac_sync.cc`)

### Background
The current `WALK_HALFWIN_BINS = ±2` is too narrow to absorb inter-clock drift during real per-row SFO excursions, triggering the fade/SNR gate on ~93% of rows and pushing Phase 1 into continuous dead-reckoning/flywheeling.

### Implementation Requirements
1. Modify `cpi_sto_tracker::process()` to replace the static `WALK_HALFWIN_BINS` with a dynamically computed window size.
2. Calculate the dynamic half-width window before calling `build compact CIR` using:
   $$\text{search\_halfwin\_bins} = \max \left( \text{WALK\_HALFWIN\_BINS},\ \left\lceil \frac{SFO_{\text{ppm\_filtered}} \cdot 10^{-6} \cdot dt_s}{\text{bin\_to\_delay\_s}} \right\rceil + 2 \right)$$
   where $SFO_{\text{ppm\_filtered}}$ is retrieved from `cpi_sfo_tracker::filtered_sfo_ppm()`, $dt_s$ is the elapsed time since the last locked row (`time_s[row] - last_locked_time_s`), and $\text{bin\_to\_delay\_s}$ is the range resolution in delay seconds.
3. Ensure the search interval around `current_center_bin` expands dynamically to prevent premature flywheel triggers while keeping dead-reckoning fallback logic intact.

---

## TASK 2: Directional Clutter Pre-Filtering via Subspace Projection (`eca_clutter.cc`)

### Background
In asynchronous ISAC systems, when the target steering vector $a(\theta_d, \tau_d)$ aligns with the static paths vector $h_s$, the Hybrid Cramér-Rao Bound (HCRB) for CGS estimation diverges sharply (the "Low-Accuracy Zone"). Simple slow-time mean subtraction fails here and leaves strong residual noise.

### Implementation Requirements
1. Update `eca_clutter.{h,cc}` to construct an explicit static paths subspace projection matrix $P_{\text{hs}}$:
   $$P_{\text{hs}} = [h_s^M, h_s^{M'}] \left( [h_s^M, h_s^{M'}]^\dagger [h_s^M, h_s^{M'}] \right)^{-1} [h_s^M, h_s^{M'}]^\dagger$$
   where $h_s^M$ is the slow-time mean CFR across subcarriers and $h_s^{M'} = (1_{M \times 1} \otimes 2\pi f) \odot h_s^M$ represents the frequency/delay gradient vector.
2. In `eca_clutter::process()`, measure the spatial-frequency vector-plane angle $\theta_{\text{space}} = \angle(a(\theta_d, \tau_d), \text{span}\{h_s^M, h_s^{M'}\})$.
3. If $\theta_{\text{space}}$ falls below a configurable threshold `eca_low_accuracy_angle_rad`:
   - Enforce full oblique projection $H_{\text{clean}} = H - P_{\Phi} H P_{\Psi}$.
   - Dynamically scale up the downstream 2D CA-CFAR threshold factor $\alpha$ in `range_doppler.cc` for cells in this angular direction to prevent static-path leakage false alarms.

---

## TASK 3: Joint Multi-Carrier SFO Alignment (`isac_sync.cc`)

### Background
Treating subcarriers independently during SFO tracking fails to capture broadband delay resolution benefits, leaving a performance gap relative to synchronous theoretical limits.

### Implementation Requirements
1. Refactor Phase 3 (`cpi_sfo_tracker::process()`) to perform multi-carrier phase alignment anchored to the dominant static/LoS reference path.
2. Extract the complex phase from the reference static tap $l_{\text{ref}}$ across subcarrier $c$ and snapshot $n$:
   $$\hat{H}[n][c] = H[n][c] \cdot \exp\left( -j \angle \tilde{H}_{\text{ref}}[n][c] \right)$$
3. Perform the SFO line-fitting and exponential correction across the aggregated multi-carrier grid simultaneously rather than applying isolated per-row frequency ramps.

---

## TASK 4: Phase-Consistency Mirror Ghost Rejection (`range_doppler.cc`)

### Background
The current conjugate-image rejection rule assumes physical targets are strictly at lower range bins than their mirror ghosts ($N_r - 1 - r$). This rule breaks down when physical scatterers lie at farther ranges or when non-maximum suppression (NMS) drops the near target.

### Implementation Requirements
1. Refactor `range_doppler::reject_conjugate_images()` to evaluate phase consistency over slow-time snapshots instead of relying on pure range order.
2. For candidate detection pairs at bin $r_1$ and mirror bin $r_2 = N_{\text{range}} - 1 - r_1$, compute the Cross-Antenna / Cross-Subcarrier complex ratio across slow-time index $n$:
   $$\rho[n] = \frac{H[n][c_{\text{target}}]}{H[n][c_{\text{ref}}]}$$
3. Compute the slow-time autocorrelation $R_{\rho}[\Delta] = \mathbb{E}\{\rho[n] \rho^*[n-\Delta]\}$.
4. Retain the candidate whose phase evolution trajectory is continuous and causal (corresponding to real physical Doppler shift). Drop the candidate exhibiting unphysical anti-causal phase jumps (the real-valued residual ghost), regardless of whether it resides in a lower or higher range bin.

---

## CONSTRAINTS & CODING STANDARDS
- **Zero-Allocation Path**: Do not introduce heap allocations inside per-CPI processing functions (`process()`). Pre-allocate all workspace buffers in initialization/resize steps.
- **Backwards Compatibility**: All new features must default to standard behavior unless explicitly toggled or tuned via `[sensing]` configuration parameters.
- **Testing**: Maintain and expand unit tests in `tests/isac_sync_test.cc`. Verify that `tests/sensing_sim` runs without regression and that Phase 1 flywheeling rates drop significantly below 93%.