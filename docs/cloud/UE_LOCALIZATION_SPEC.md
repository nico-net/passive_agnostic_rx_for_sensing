# Single-Node UE Localization and UE-Illuminated Sensing — Future-Work Spec

**Status: FUTURE WORK — NOT APPROVED FOR IMPLEMENTATION.**
Nothing here may be implemented until the operator approves it. Current priority remains validating the passive
agnostic receiver (4 channels, 100 MHz, 1 operator). Related unapproved specs: multi-cell reception
(`docs/superpowers/plans/2026-09-27-multi-cell-reception.md`), wideband GPU receiver
(`docs/superpowers/specs/2026-09-27-wideband-multi-operator-gpu-receiver-design.md`), beam/CSI telemetry
(`docs/superpowers/specs/2026-09-27-beam-csi-telemetry-design.md`).

Date: 2026-09-28.

## 1. Goal

With **one X410, 4 channels, one horizontal 4-element λ/2 ULA** and **one gNB at a known position**:
1. localize UEs passively from their uplink transmissions;
2. use the located UEs, together with the gNB, as illuminators for sensing moving objects;
3. estimate object position in 2-D for ground objects (height fixed) and a coarse 3-D position for elevated
   objects (drones).

No network cooperation, no RRC access, no seeding from operator data.

## 2. Inputs available to the passive receiver

- Downlink timing and the gNB position (surveyed or from the public site registry) → absolute DL frame timing at
  the receiver.
- Blind decoding (existing): C-RNTIs, UL grants from DCI 0_1, PUSCH decode (data-aided reference), RAR (absolute
  timing advance at random access / SCG addition), plaintext MAC CEs (relative timing-advance commands).
- ULA measurements: for each ray, the cone angle ψ about the array axis, with u = cos ψ = cos(el)·cos(φ).
- Optional uplink sightings not tied to PUSCH (future, blind): PUCCH (formats 2/3/4, frequent HARQ-ACK on NSA) and
  SRS (wideband, periodic).

## 3. UE localization

- **Timing constraint:** the UE transmits at DL reception minus its timing advance (≈ 2·d(UE,gNB)/c), so the UL
  arrival at the receiver relative to DL timing gives **d(UE→rx) − d(UE→gNB)** — a hyperbola (2-D) or hyperboloid
  (3-D) with foci gNB and receiver.
- **Bearing constraint:** the ULA cone angle; with the UE height assumed (1.5 m handheld), the elevation coupling
  is removed and the cone becomes a bearing ray.
- **Fix:** ray ∩ hyperbola, closed form (same family as the existing ray–ellipse birth seeding).
- **Consistency check:** the absolute timing advance (RAR) accumulated with relative MAC-CE updates gives
  d(UE,gNB) within the TA granularity (~20 m at 30 kHz); used as a sanity/outlier test, not as the primary fix.
- **Height:** not estimated for UEs (poorly conditioned with the receiver, array and UEs near the same plane);
  fixed prior.
- **Tracking:** per-RNTI (per tracking epoch) filter over time; RNTI identity removes data association between UEs.
- **Uncertainty:** each fix exported with covariance from timing resolution (~3 m range difference at 100 MHz,
  better sub-bin), bearing CRB, and TA consistency.

## 4. Sensing with gNB and UEs as illuminators

- **Illuminators:** the gNB (known position, strong) and each located UE (estimated position, weaker, bursty
  uplink). Reference signals: decoded PDSCH (data-aided) for the gNB, decoded PUSCH (data-aided) for UEs; later PUCCH/
  SRS if blind detection exists.
- **Per illuminator:** a bistatic range (ellipse in 2-D, ellipsoid in 3-D) and Doppler per detection; the ULA gives
  the cone angle of the reflection.
- **Ground objects:** height fixed (≈1 m); fix = cone (as bearing) ∩ gNB ellipse; each UE illuminator adds a
  redundant ellipse → ghost rejection via the existing redundancy/χ² gates.
- **Elevated objects (drones):** free height; needs ≥ 3 equations: cone + gNB ellipsoid + ≥ 1 UE ellipsoid.
  The elevated gNB provides vertical sensitivity; accuracy improves with drone height and with more illuminators;
  poor below ~10 m. Tracking over CPIs smooths z.
- **Model selection:** each track is evaluated under both hypotheses — ground (z fixed, 2-D) and aerial (free z,
  3-D) — and the hypothesis with consistent residuals and trajectory over time is kept; ambiguous tracks are
  exported as unresolved.
- **Error propagation:** UE position covariance is propagated into each UE ellipsoid; illuminators with large
  position uncertainty are down-weighted.

## 5. What is reused

- Blind decoding chain (PDCCH, PDSCH/PUSCH decode, RAR anchor, MAC-CE parsing).
- Sensing pipeline (CFR, range-Doppler, CFAR, detection reports) and the AoA module (`isac_aoa`, beamscan/
  interferometry, direct-path self-calibration of per-channel phase and array orientation using the known gNB
  direction).
- Fusion/tracker (`repos/isac`): ray–ellipse closed-form birth, whitened Gauss–Newton refinement, redundancy and
  χ² gates, UKF tracking — extended to UE illuminators with position uncertainty.

## 6. New components (when approved)

- UL timing extraction per decoded PUSCH (arrival vs DL frame timing, sub-sample), TA bookkeeping (RAR absolute +
  MAC-CE relative) per tracking epoch.
- UE localizer (ray ∩ hyperbola, covariance, per-epoch tracker).
- UE-illuminated CFR path (bistatic geometry with a moving, uncertain transmitter; irregular uplink slow-time
  sampling handling).
- Tracker extension: illuminator position covariance; 2-D/3-D hypothesis selection per track.
- Optional later: blind PUCCH/SRS detection for more UE sightings (important on NSA, where NR uplink data can be
  sparse but NR PUCCH HARQ-ACK is frequent).

## 7. Known limits

- One receiver: zero geometric redundancy with a single illuminator; multipath/NLOS biases cannot be detected
  without extra illuminators.
- ULA elevation ambiguity: only the cone angle is measured; elevation for objects comes from geometry, not the
  array. Reliable drone altitude needs an L-shaped or 2×2 UPA arrangement of the same 4 channels (alternative
  configuration, same hardware).
- UE transmit power (~23 dBm) limits UE-illuminated range; uplink scheduling is bursty.
- UE timing errors and TA quantization bound UE localization accuracy.
- NSA: absolute TA only at SCG addition (RAR rare); relative TA accumulation drifts across gaps.

## 8. Ethics and legal

Passive localization of third-party UEs processes personal location data (GDPR). Any study requires ethics
approval and data minimization: pseudonymized RNTIs, no identities stored, retention limits. Validation should use
the team's own UEs at known positions.

## 9. Validation plan (when approved)

1. **Simulation:** OCUDU ZMQ bed with srsUE(s) at configured positions (broker applies per-UE delays/levels to
   emulate geometry); truth = configured positions.
2. **OTA, controlled:** own phones at GNSS-surveyed positions in an open area (e.g. Piazza Leonardo da Vinci),
   receiver and gNB positions known.
3. **Metrics:** UE localization error (median, p90) vs distance and geometry; TA-consistency outlier rate; object
   localization error for ground objects (z fixed) and for a drone with logged GNSS altitude (x, y, z errors);
   ghost-track rate with 1 vs N illuminators; 2-D/3-D model-selection accuracy.
4. Thresholds pre-registered before the acceptance runs; calibration data kept separate from the acceptance set.

## 10. Open questions

- Achievable UL timing accuracy on real UEs (UE timing error, TA quantization).
- Minimum number/geometry of UE illuminators for usable drone height.
- Multipath robustness in an urban park; benefit of adding the 2-D array arrangement.
- Interaction with multi-cell reception (more gNB illuminators) and with PUCCH/SRS blind detection.
