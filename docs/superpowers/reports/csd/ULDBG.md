# UL-DBG — physics-first UL census, capture raw_pci415_12s_b (PCI 415, 217 PRB, 4 RX)

2026-10-09; `csd/ul-dbg`; DGX GB10 aarch64. Offline only: `tools/uldbg/census.py` (memmap streaming, run through
`heavy.sh`). DCIs from replay `replays/replay_uldbg_b1_001409` with `ISAC_UL_DBG_LOG=1` (4170 ULDBG_DCI, 1076 ULDBG_TS).

## Results

- **[OFFLINE VERIFIED, TDD]** Frame pattern DDDDDDDSUU x2: slot 7/17 has 6 DL symbols then guard; UL slots 8,9,18,19.
  DL RE power ~100-111 dB (arb.), UL slots 65-71 dB, guard symbols 65-71 dB: **the UL slots sit at the guard/noise
  floor**; DL is ~40-45 dB above it.
- **[OFFLINE VERIFIED, RX hardware]** RX2 is ~23 dB below RX0/1/3 on DL (87 vs 109-113 dB) and flat at 64.2 dB in UL
  and guard: RX2 chain is dead/disconnected in this capture. Effective UL array = 3 RX. Check the X410 RX2 cable/antenna.
- **[OFFLINE VERIFIED, positive control]** DCI 0_1 -> PUSCH mapping (FDRA N=216 start 1, TDRA row0 k2=2 S0 L14,
  frame offset abs-cap = 487 from ULDBG_TS) predicts the PRBs/slot of 4155 grants. Coherent DM-RS (ID 415, comb 0,
  syms 2+11, delay search) at the predicted location vs the same PRBs 2 frames later (control):
  MRC SNR > 0 dB: 74 grants vs 10 control; > 3 dB: 27 vs 3; > 10 dB: 7 vs 0. Residual delay tau 12-44 samples on the
  strongest. **The mapping is physically confirmed** on ~64 grants (~1.5 %), all 3-PRB, MCS 17-27, mostly the last
  quarter of the capture (frames > 1040).
- **[OFFLINE VERIFIED, waveform-agnostic]** RE energy (4 RX, 14 symbols) on predicted PRBs vs control: median
  +0.03 dB over 4155 grants; tails symmetric (>+3 dB 64, <-3 dB 68). L>=10 PRB (527 grants) and MCS<=4 (326) show
  no excess (median -0.00 / +0.01 dB; DM-RS >0 dB 0/527 and 1/326). Typical PUSCH arrives well below 0 dB per RE.

## Link-budget verdict

**[OFFLINE VERIFIED, this capture/position]** First PUSCH CRC is physically out of reach here. Low-MCS/wide grants
(the decodable kind, MCS0 needs about -5 dB post-MRC) are not detectable even coherently; the only detectable PUSCH
are 3-PRB grants at MCS 17-27 whose best DM-RS SNR (16.4 dB MRC, one grant, MCS 23; next 12.3 dB at MCS 21) is below
the ~18-22 dB those MCS need. Caveat [INFERRED]: UEs using a non-415 DM-RS ID or DFT-s-OFDM DM-RS would be missed by
the coherent test, but the energy test is ID/waveform agnostic and also shows nothing. Needed: a position with
>= 15-20 dB more UL path gain (closer to UEs / higher-gain UL antenna) and a working RX2.
