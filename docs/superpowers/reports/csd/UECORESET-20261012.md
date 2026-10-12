# UECORESET 2026-10-12: why no dedicated (UE-specific) CORESET on WindTre 508 / Iliad 403 / PCI 4

Labels: [MEASURED, OTA log] = ota_runs logs read (DGX 2026-10-12, t26x/y/z); [INFERRED] = reasoning; [NOT RUN] = blocked.
Branch csd/ue-coreset. No receiver code changed, no build, no replay: `pgrep -x nr-uesoftmodem` returned PIDs 3228228/3228248
(/opt/oai-nr-ue rfsim, 38 min old) so the no-build rule applied; and the only 508 capture (survey c3550, centre 3550 MHz, 122.88 MS/s,
passband ~3489-3611 MHz) does not contain the carrier above 3611 MHz, so it cannot test a retuned grid anyway.

## Three different causes (not one)

### 1. PCI 508 (WindTre): the receiver grid does not contain the carrier [MEASURED, OTA log; cause INFERRED]
- `ab_P508/X508/Wz1`: `ACQ carrier MISMATCH: SIB1 says 162 PRB mu=1 (Point A at grid subcarrier 972 ... carrier end 2916) but PHY started with 217 PRB,
  grid end 2604`. Carrier = grid RB 81..243, grid = RB 0..216: the top 26 RB (9.4 MHz, 16 % of the carrier) are never received.
  Point A 3560.85 MHz, carrier 3560.85-3619.17 MHz, receiver window 3531.7-3609.8 MHz (`-C 3570570000`, 78 MHz).
  offsetToPointA 32 and k_SSB 2 reproduce Point A exactly: 3570.24 - 120*30k - (6*32+1)*30k = 3560.85 MHz.
- All 8+ discovery candidates in P508/Wz1 are `groups=22 L=0 R=0` (= (217-81)/6, the most that fits). A BWP of 162 PRB is 27 groups. The
  GPU stage also reported the real structure once: Wz1 pass 124 `nid=46930 grid_rb=135 groups=12 L=6 R=2` and `grid_rb=153 groups=9 L=6 R=3`,
  i.e. an INTERLEAVED CORESET (bundle 6). An interleaver depends on the total REG count, so a window-truncated CORESET cannot be de-interleaved.
- Consequence in the logs: `discovered CORESET ... rejected (no fresh CRC evidence in 4000 occasions)` (P508 x2, Wz1 x8), `GRANTDROP ...
  autodiscover-unverified` for RA/TC RNTIs. The only success, Wz1 `nid=46930 VERIFIED -> bank add offset=81 span=132 len=48`, came after 124 GPU passes
  and only via JOINT_ANON hits at AL1 CCE 19-21 (non-interleaved low CCEs survive truncation); no C-RNTI traffic followed.
- Second defect on the same cells: `KSSB_GRID k_ssb=2 ssb_sc=1171 ... off the CRB grid by 6 subcarriers` with ISAC_KSSB_ALIGN not active in these runs
  (no "retunes" suffix in P508/X508/Wz1), so every RB (and 6-RB group) of the grid is half an RB off the cell's.
- The receiver already computes the fix (`ISAC_ACQ_RETUNE`), but only `tests/passive_rx/captures/run_arm.sh` (ADAPT) acts on it; the OTA chain (ota.sh) does not.
  Its `centre_hz` is the carrier centre; with an odd-RB grid (217) around an even carrier (162) that centre is again half an RB off the CRB grid.
- Fix (general, from SIB1 only): `tests/passive_rx/survey/retune_args.py RUN.LOG` prints `-C -r --ssb` that put the SIB1 carrier fully inside an
  RB-aligned grid, centred on it (whole RBs below Point A, SSB re-expressed on the CRB grid). Output on the three 508 logs:
  `C=3590190000 R=217 SSB=517` (grid 3551.13-3629.25 MHz, carrier 3560.85-3619.17 MHz). Tool is a log parser: no receiver change, nothing to regress on 415/923.
  Same rule as TIMACQ (TIM 923 retune to the SIB1 carrier made the dedicated CORESET verify in < 1 s) [MEASURED, TIMACQ].

### 2. PCI 403 (Iliad): nothing to discover [MEASURED + INFERRED]
- `X403/Iz1`: `barred=1`, `SIB1 has no servingCellConfigCommon (NSA-style SIB1)`, SIB1 decodes 31x, SI-RNTI 0xffff PDSCH CRC 98 %.
  No carrier geometry exists, `carrier_verified` stays 0, and `nr_pdcch_coreset_map_idsweep_want()` returns -1 (`snap.carrier_verified == 0`):
  0 `PDCCH_NID_GPU pass` lines in both runs. DCI classes: SI 1334-2314, TC 131-151, C 355 (held: mismatch 293-384, persist 282-308 = not real C-RNTIs).
  A barred NSA-anchor-style cell carries no SA UEs, so there is likely no dedicated CORESET [INFERRED]. If discovery is wanted anyway the gate needs a
  Point-A hypothesis (pa mod 6 RB unknown without SIB1) - NOT implemented (needs build + replay).

### 3. PCI 4: SIB1 never decoded, same gate [MEASURED]
- `P4`: only 1 SI-RNTI DCI, RA 92, TC 31, C 48 (class C held: mismatch 26); `acq_sib1_decodes=0`; `carrier=unchecked`; 0 NID passes (gate above).
  In the 508 runs the gate was open only because the PCI-508 SIB1 CACHE gave a carrier (and then MISMATCH, value -1, which is != 0).
  Cause for PCI 4 is therefore no SIB1 (weak SI traffic, not a CORESET search problem). Unresolved here. If PCI 4 shares site/carrier with 508 the same
  retuned grid applies [INFERRED], and the 508 SIB1 cache is keyed by PCI so it does not help PCI 4.

## Not found / not done
- Dwell length: rejections happened on window-truncated candidates; sparse traffic is not the cause on 508 (RA/TC DCIs are plentiful) [MEASURED].
- Receiver-side auto-retune (in-process) not built: the sample grid is sized at launch (see nr_passive_acq_state.c comment); relaunch by the supervisor is the existing design.
- No replay/regression run [NOT RUN]; script-only change.

## OTA spec lines (TAG:ENV:CFG:FREQ:SSB:FO). `-r 217` unchanged. FO scaled by 3590.19/3570.57 (-4816 -> -4842); keep -4816 as an alternative.
```
U508a:t26z:<cfg of ab_P508>:3590190000:517:-4842     # retuned grid, ISAC_KSSB_ALIGN stays on (shift 0, no retune expected)
U508b:t26z:<cfg of ab_P508>:3590190000:517:-4816
U4a:t26y_pci4:<cfg>:3590190000:517:-4842              # PCI 4, assumes 508's carrier; ISAC_TARGET_PCI=4
```
Expected in the log: no `carrier MISMATCH` (`carrier CONFIRMED from SIB1`), `KSSB_GRID` absent, discovery candidates with groups up to 27,
`VERIFIED -> bank add`, DCI 1_1 length lock. If MISMATCH persists, read the new RETUNE line and run `retune_args.py` again.
Control (optional): 415 `-C 3680010000 --ssb 1171 FO 6900` must still give `carrier CONFIRMED`.
Iliad 403: no retune possible (no SIB1 carrier); expect SI/TC only.
