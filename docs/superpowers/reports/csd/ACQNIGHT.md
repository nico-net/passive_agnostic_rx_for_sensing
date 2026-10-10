# ACQNIGHT: calibrated PDCCH prescreen -- adaptive dwell effective at low traffic

Host: DGX aarch64 / GB10. Branch `csd/acq-night` (base `csd/otatest` 656c623dac). Date 2026-10-10.
Offline only: no radio, no hardware tools. Every build and replay ran through `heavy.sh`. New behaviour is behind flags, default off.

## 1. The prescreen gates and why ADAPTIVE never gated anything

[CODE-READ] `nr_pdcch_blind_monitor_rt.c`, candidate loop of `run_occasion_one()`. A candidate reaches decoding (and sets
`t_discovered_prescreen_passed` for the dwell) only after these gates:

| gate | config / env | measures | state in our OTA config |
|---|---|---|---|
| energy, absolute | `pdcch_blind_monitor_noise_gates` field 1 `energy_min` | mean(abs I + abs Q) of the equalised candidate REs, receiver units | 0 = off |
| energy, adaptive | field 5 `energy_adapt_factor` | same mean divided by a streaming-median "noise floor" of all candidates; reject below factor x floor (after 200 candidates) | **"0:2:500:0:3.0" parses as energy_min=0, persist_k=2, window=500 ms, min_snr=0, adapt_factor=3.0, so it WAS on in the cfg, but CSS0 autoconf (`nr_pdcch_blind_monitor_autoconf_css0`) zeroes both energy fields and the dedicated-CORESET path deliberately does not restore them (comment at "ENERGY GATE: DELIBERATELY NOT SET"). So on the dwell it is off.** (The first field 0 is not the "enable" bit.) |
| known-RNTI prescreen | `ISAC_PDCCH_KNOWN_PRESCREEN=1`, `_MIN` (0.66) | amplitude-consistency score of the soft bits (below); **skipped on the discovery dwell** (`!t_discovered_token`), it applies to bank passes | on in t26j, but not on the dwell |
| joint prescreen | `nr_pdcch_joint_prescreen` (JOINT_ANON queue) | same score, threshold 2/pi + 3.09 sigma | affects the anonymous-solve queue only |
| shared prescreen | `ISAC_PDCCH_SHARED_PRESCREEN(_MIN)` | DM-RS rank score | off |
| DM-RS coherence | `ISAC_PDCCH_DMRS_GATE=1` | candidate DM-RS correlation vs per-occasion median+MAD | default off (it cost real DCIs, measured 2026-09-23) |

Amplitude-consistency score: `(sum|x|)^2 / (E sum x^2)` over the E = 108 L soft values of a candidate. Gaussian noise: mean 2/pi = 0.637, sigma ~ 0.339/sqrt(E).
Result: on the dwell nothing is gated, every candidate counted as "prescreen-passing" (consistent with the daytime logs "4000 prescreen-passing occasions, 4000 total").

## 2. Calibration (what separates a real PDCCH candidate from an empty CCE)

New diagnostics (flag `ISAC_PDCCH_PRESCREEN_DIAG=1`; disables all rejecting gates, drops nothing, histograms per AL the score, energy/floor ratio and DM-RS correlation of every candidate and of candidates that later pass CRC with a known RNTI; `PSDIAG` lines; analysis `tests/passive_rx/tools/psdiag_roc.py`).
Replays: capture b (`raw_pci415_12s_b`), full ACQ-ROBUST flag set, 0.25x.

**Positives = real DCIs** [MEASURED, capture b, daytime bank passes, 1 run]: 45027 AL2 and 3466 AL4 candidates with CRC OK on a known RNTI.
**Negatives = silent cell** [MEASURED, SIM]: `ISAC_PDCCH_SIM_SPARSE=1000000` replaces the PDCCH OFDM symbol (all 4 antennas) by Gaussian noise at the median-bin level of the real symbol; ~36000 candidates per AL in the dwell. The capture is a busy daytime cell, so real "empty CCE" statistics come only from this synthetic noise (real negatives in the daytime capture are contaminated by DCIs of unknown RNTIs).

Score distribution of real DCIs (AL2): 1/5/25/50/75/95/99 % quantiles 0.53 / 0.60 / 0.71 / 0.81 / 0.91 / 0.99 / 0.995. Silent noise (AL2, 36176 cand): maximum score 0.65 (z of that max = +0.5 sigma); AL4 0.64, AL8 0.615, AL1 0.70.

False-alarm per candidate on silent noise vs threshold z (threshold = 2/pi + z sigma_E), [MEASURED, SIM]:

| AL | cand | z=0 | z=1 | z=2 | z=3.09 | z>=4 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 38186 | 3.0e-3 | 4.5e-4 | 2.6e-5 | 0 | 0 |
| 2 | 36176 | 4.7e-4 | 0 | 0 | 0 | 0 |
| 4 | 34970 | 2.9e-5 | 0 | 0 | 0 | 0 |
| 8 | 34300 | 0 | 0 | 0 | 0 | 0 |

z = 3.09: 0 false passes in 143.6k candidates (95 % upper bound 2.1e-5 per candidate, ~1.3e-3 per occasion of ~64 candidates).

Real-DCI "miss" of the score at z = 3.09 (fraction of CRC-OK candidates whose score is below the threshold): AL2 27 %, AL4 36 %. So the score is NOT a gate that may drop candidates (it would lose a quarter of weak DCIs; that is why a gate in front of the decoder was rejected). Design chosen: **the gate does not drop anything; it only decides whether an occasion counts toward the 4000 rule**, and a CRC-OK DCI always counts. DCI loss is zero by construction (candidates are decoded exactly as before).

Alternatives measured on the same data (AL2, real DCIs vs the daytime non-CRC population, contaminated): energy/floor ratio: miss <= 1 % at 0.8x floor rejects only 53 %; DM-RS correlation likewise (53 % at 1 % miss). The amplitude score is the sharpest (66 % at 1 % miss, ~100 % on pure noise at z >= 1), and needs no absolute scale. The cfg `noise_gates` energy gate is therefore NOT the right knob (and it would also drop candidates before decoding).

## 3. Implementation (`nr_pdcch_blind_monitor_rt.c`)

- `ISAC_PDCCH_DWELL_PRESCREEN=1` (default off): in the discovered-dwell pass a candidate sets `t_discovered_prescreen_passed` only if score >= 2/pi + z*0.339/sqrt(E), z = `ISAC_PDCCH_DWELL_PRESCREEN_Z` (default `NR_PDCCH_DWELL_PS_Z_DEFAULT` = 3.09), or if a DCI passed CRC in the dwell (both DL result loops). Nothing is skipped.
- `ISAC_PDCCH_PRESCREEN_DIAG=1`: diagnostics above (also builds the DM-RS rank grid so the correlation is available).
- `ISAC_PDCCH_SIM_SPARSE=N` (test only): in dedicated passes, silent-slot simulation: only slots with abs_slot % N == 0 carry the real PDCCH symbol.
- Flag off: unchanged (new branches are guarded; the existing 228 `test_nr_pdcch_blind_monitor` tests pass).

## 4. Low-traffic simulation [MEASURED, SIM, capture b, full ACQ-ROBUST flags, no seed, 0.25x]

N = one slot in N keeps its real PDCCH (traffic fraction 1/N); other slots are noise.

| N | traffic | DWELL_PRESCREEN off (= current ACQ-ROBUST) | DWELL_PRESCREEN on |
|---|---:|---|---|
| 300 | 0.33 % | correct nID 10163 cover dwell **rejected** 4000/4000 ("no fresh CRC evidence"); 62922 rejected too; no useful bank | 10163 **VERIFIED -> bank add** (z=4.5 1/1, z=3.09 2/2); no rejection |
| 100 | 1 % | all dwells rejected at 4000/4000 | dwell kept alive, not yet verified in the 12 s capture (too few DCIs) |
| 800, 1500, 5000 | 0.12-0.02 % | rejected at 4000/4000 | kept alive, no verification in the 12 s capture (<= 30 traffic slots) |

So the gate makes the correct geometry survive silence and verify as soon as DCIs arrive (N = 300); below that the capture is too short to show verification, but the dwell is not discarded (a 12 s capture is 24000 slots; the cap is 40000).

Wrong geometries [MEASURED + CODE-READ]: under silence a wrong geometry is rejected only by the hard cap `NR_PDCCH_DWELL_ADAPTIVE_CAP` = 40000 occasions (unit test `AdaptiveNoPrescreenSurvivesUntilCap`: alive at 39999, rejected at 40000); where it sees energy but no CRC it is rejected at 4000 prescreen-passing occasions (unit test `AdaptivePrescreenWithoutCrcRejectsAt4000`). **Cost found in the daytime replays**: with the gate on, a wrong nID (9020, which the control run rejects at 4000/4000) scores like silence (no QPSK structure) and therefore stays alive up to the cap: no "rejected" line in any of the 8 gate-on runs versus 1-2 per control run. The queue then waits up to 40000 occasions (about 20 s of signal at 2 kslot/s) per wrong geometry instead of 4000. Accepted trade-off (a wrong nID is indistinguishable from a silent correct one); the cap is the lever if too slow.

## 5. Daytime no-regression (flags = ACQ-ROBUST set + `ISAC_PDCCH_DWELL_PRESCREEN=1`, 4+ runs each) [MEASURED]

Capture b, bank add: first CORESET (nid 10163) verified at the same absolute frame as the control (rx frames 721/722/776/721 vs control 721/722/723), i.e. the ACQ-ROBUST 1.1 s figure is unchanged; unique SOLVED RNTIs, gate on (8 runs): 36, 24, 35, **0**, 32, 25, 22, 20; control without gate (8 runs): 32, 21, 33, 26, 33, 26, 35, 28. Gate >= 15 solves: **7/8 on, 8/8 control**. The 0-solve run (fb_4) verified both CORESETs at the same time as the others (PDCCH acquisition identical) but the PDSCH solver produced no candidates; the gate only touches the dwell counter and cannot affect the PDSCH path, so this is attributed to PDSCH-solver variance [INFERRED, not proven; 1 of 8 vs 0 of 8 is not statistically separable]. Flag it for the DECODE-RES owner.
Old capture (20261008), 4 runs: main RNTIs 8f5a / b876 / e01e / 13ae decoded in **4/4 runs**, bank add at rx frame 219-235 (CORESET 62922 first), no "rejected" lines.

## 6. Exact OTA env

`ota/t26j.env` (the file already has `ISAC_PDCCH_DISC_COVER=1 ISAC_PDCCH_RBOFFSET_FIX=1`) plus:

```
ISAC_PDCCH_DWELL_ADAPTIVE=1 ISAC_PDCCH_NO_FAKE_FOOTPRINT=1 ISAC_PDCCH_NID_GPU_NODROP=1 ISAC_PDCCH_DWELL_PRESCREEN=1
```

(`t26k.env` = t26j + the first three; add the last.) No cfg change is needed: the gate is env-only, `pdcch_blind_monitor_noise_gates` stays as is (do not "fix" it, the energy gate is the wrong tool). Optional: `ISAC_PDCCH_DWELL_PRESCREEN_Z=3.09` (default). Look for: `rejected (no fresh CRC evidence: N prescreen-passing occasions, M total, ISAC_PDCCH_DWELL_ADAPTIVE)` with N << M at night (alive until M = 40000), and `discovered CORESET ... VERIFIED -> bank add` on the correct nID. Diagnostics, if wanted on a live run: `ISAC_PDCCH_PRESCREEN_DIAG=1` (drops no candidate but disables the other gates; do not leave on).

## 7. Limits / follow-ups

- The silent-cell negatives are synthetic Gaussian noise; real night interference may score higher. The z = 3.09 margin is large for AL >= 2 (noise maximum z ~ +0.5 sigma) but AL1 has 3e-3 passes at z = 0; z = 3.09 gave 0.
- The 12 s captures cannot show verification below ~1/300 traffic; the cap behaviour is covered by unit tests.
- No unit test for the new static helpers (threshold function is trivial); replay evidence above.
- DEFERRED: none on GPU (CPU/GPU discovery path as in the replays).

## Self-check

New helpers all called: `dwell_prescreen_enabled`, `dwell_prescreen_z`, `dwell_prescreen_threshold` (candidate loop), `prescreen_diag_enabled`, `sim_sparse_period`, `sim_sparse_blank_symbol` (symbol loop), `psd_note` / `psd_note_ok` (candidate loop and DL result loops). All flags read. Constants used: `NR_PDCCH_DWELL_PS_Z_DEFAULT`, `PSD_BINS`, `PSD_NMET`. Flag-off hunks unchanged; existing unit tests 228/228.
