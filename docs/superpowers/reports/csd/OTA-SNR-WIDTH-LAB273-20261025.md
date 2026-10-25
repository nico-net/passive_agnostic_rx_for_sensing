# Wide vs narrow grants on the sens4 lab cell: SNR, clipping, rank (2026-10-25, interim)

[OTA MEASURED sens6 X410 4 RX, sens4 OCUDU lab cell PCI 2, 273 PRB, two phones; gNB SCHED log = validation only.]
Decoder and discovery code are unchanged since `76a8b89392`. Instrumentation is `32b9ad5e71` (ADC census).

## 1. gNB rank / MCS arms (one run each)

The caps were applied via per-run copies of the gNB yml on sens4, in `/home/sens/NICOLA/gnbLogs/armcfg/gnb_{L1,L1M,L2}.yml`. Each copy
changes only `pdsch.max_rank`, an added `pusch.max_rank`, and `max_ue_mcs` (L1M). The live yml's sha256 `1e3123cc…` was unchanged.
The `ri=` values in the gNB log confirm the caps took effect.

| arm | gNB DL rank used | PDSCH CRC phone A / B | TBs OK | layout solve at | PUSCH (rx try / ok) |
|---|---|---|---|---|---|
| B0 (rank 4, MCS ≤ 25) | ri=4 | 0.052 / 0.058 | 541 / 584 | 22 % of the run | 13067 / 705 |
| L1 (rank 1) | ri=1 | **0.712 / 0.843** | 18066 / 20480 | 9 % | 0 / 0 |
| L1M (rank 1, MCS ≤ 16) | ri=1, MCS 16 | **0.521 / 0.836** | 51875 / 47295 | 2 % | 0 / 0 |
| L2 (rank 2) | ri=2 | – (layout never solved) | 0 | never | 0 / 0 |
| B0 rep 2 | ri=4 | – (layout never solved) | 0 | never | 0 / 0 |

- PUSCH `try=0` in the capped arms: the UL layout never locks, which matches the Q4 note on uniform UL grants.
- Confound in all arms: 36–56 % PDCCH scan-queue drops whenever PDSCH decoding runs at volume. This is CPU contention, and it cuts DCI recall
  to ~0.6–0.7 (0.98 in runs without a layout solve).

## 2. DL: SNR vs MCS requirement, by width

- Measured SNR = EVMGRANT post-equalisation data SNR per layer.
- Requirement = Shannon per layer + 1.5 dB (`nr_snr_gate.h`; MCS 25 qam256 = 22.3 dB).

| arm | width | n | data SNR | margin | CRC |
|---|---|---|---|---|---|
| L1 | 1–4 RB | 37950 | 25.1 | +2.8 | 0.89 |
| L1 | 5–32 | 4798 | 27.1 | +4.8 | 0.93 |
| L1 | >128 | 3760 | 20.0 | −2.3 | 0.00 |
| L1M | 1–4 | 75602 | 20.1 | +6.2 | 0.99 |
| L1M | 5–32 | 23742 | 21.0 | +7.1 | 0.99 |
| L1M | 33–128 | 2459 | 17.0 | +3.1 | 0.29 |
| L1M | >128 | 30015 | 15.3 | +1.4 | 0.01 |
| B0 (rank 4) | 1–4 | 13636 | 23.6 | +1.4 | 0.08 |
| B0 | >128 | 138 | 5.3 | −16.9 | 0.00 |

- At the same +2 dB margin, rank 4 passes 0.29 against 0.91 at rank 1: the per-layer SNR estimate is optimistic at 4 layers.

**Frequency selectivity alone does not explain the wide-grant loss.**
- The per-16-RB DM-RS power across the carrier spans −5.4 … +4.2 dB relative to RB 0 (05:53 diagnostic run).
- With a flat noise floor and the narrow-grant SNR at RB 0, a full-band rank-1 grant predicts a capacity-MIESM effective SNR of:
  - 23.3 dB at MCS 25, margin +1.0 dB;
  - 17.6 dB at MCS 16, margin +3.7 dB, i.e. CRC ≈ 1 on the narrow-grant curve.
- Measured: the wide L1M grants have 15.3 dB data SNR and CRC 0.01.
- That leaves **~5 dB of width-dependent loss beyond the channel.**
- Not yet confirmed with true per-RB post-eq SNR: no replay capture exists on sens6, and the offline MIESM tool (`linkbudget.py`) is ready
  for the next capture.

## 3. UL: not SNR-limited, wide grants fail

B0 run 08:53, `UL_PUSCH_CRC` lines:

| width | n | DM-RS SNR | CRC |
|---|---|---|---|
| ≤ 8 RB | 966 | 40.9 dB | 0.72 |
| 9–64 RB | 22 | 37.0 dB | 0.55 |
| > 64 RB | 11703 | 34.2 dB | **0.000** |

- Wide PUSCH is single-layer, and its TBS equals the gNB's (188576 bits at 273 RB, MCS 25).
- With the correct data scrambling ID (782 grants), 0–1 of ~20 segments decode, at 26 dB DM-RS SNR against 16.8 dB needed.
- 10.1k wide grants were also tried with wrong data-scrambling hypotheses.

## 4. ADC clipping (ADCCLASS, full scale 8191 = the driver's shift; slot class from the receiver's TDD table)

| | mean | at full scale | above 0.9 FS |
|---|---|---|---|
| DL slots | −13.4 … −16.1 dBFS | 0.05 – 0.26 % | 0.19 – 0.73 % |
| UL slots | −25 … −37 dBFS | 0 | ~0 |
| mixed slots | ≈ −16.5 dBFS | ~0.05 % | — |

All four channels behave alike. The highest DL clip fraction (L1M, 0.26 %) did not stop the narrow grants, which pass 0.99 there.

## Reading so far

- Narrow grants decode in both directions: DL at rank 1, UL at any rank.
- **Wide grants fail in both directions** with SNR that would suffice. The width-dependent ~5 dB DL loss and the wide UL segment failures at
  26–35 dB point at an impairment that grows with the occupied bandwidth, not at the link budget:
  - DL: ADC clipping (full-band load and PAPR) is the leading candidate.
  - UL: clipping is not observed in UL slots, so a width-dependent receiver impairment is open (e.g. UL frequency/timing handling across
    the band).
- Planned tests (blocked since 09:32: the X410 NIC lost its IPv4; the radio is stopped by request):
  - RX-gain steps 49/42/36/30. If clipping limits, the wide-grant SNR rises while the narrow-grant SNR falls.
  - One rank-1 replay capture for the per-RB MIESM and the guard-bin noise floor.
