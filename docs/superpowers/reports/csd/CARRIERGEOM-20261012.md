# CARRIERGEOM 2026-10-12: adopt the SIB1 carrier geometry inside a wider receiver grid (ISAC_CARRIER_FROM_SIB1=1, default off)

Branch csd/carrier-geom (from csd/otatest 67dc42702c). Host: DGX GB10 aarch64. Labels: [MEASURED, OTA log] ab_U508a/b logs,
[CODE-READ], [OFFLINE VERIFIED] unit tests, [NOT RUN] no replay/OTA evidence, [HYPOTHESIS].

## What the U508 logs show [MEASURED, OTA log ab_U508a_1_164111]
- Retune worked: Point A at grid sc 324 (RB 27), carrier end 2268 < 2604, so the carrier is fully inside the 217-PRB grid. ACQ still prints
  `carrier MISMATCH` because `grid_match` demands carrier == grid; that verdict is correct (grid is wider) but then drives nothing useful.
- The main (CSS0) config already uses SIB1 sizes (`1_1 len=49 bwp=162`, `1_0 n_rb_riv=162`). The DISCOVERED dwell/bank did not:
  `nr_pdcch_discovered_snapshot()` hard-coded `bwp_start=0, bwp_size=dci10_n_rb_riv=N_RB_DL(217)`, the cover ran to the grid end
  (`groups=31` = (217-27)/6, i.e. 24 RB past the carrier end at RB 189), and `coreset_rb_offset` was a grid RB.
- GPU candidates: nid 36098 `grid_rb=87 groups=12 L=6 R=6`, nid 43339 `L=6 R=3`, nid 59080 `grid_rb=33 groups=30 L=0` (30 groups = 213 > carrier end 189).
  All dwells: `rejected (no fresh CRC evidence in 4000 occasions)`; JOINT_ANON 165 hits / 2000 items but no recurrence, no `VERIFIED`.

## Where the chain used the grid instead of the carrier [CODE-READ]
| place | before | now (flag on, carrier inside grid) |
|---|---|---|
| dwell/bank BWP start, size | 0, N_RB_DL | carrier first RB (Point A + offsetToCarrier), SIB1 carrierBandwidth |
| DCI 1_0 USS N_RB for RIV, rb base | N_RB_DL, 0 | BWP size, BWP start |
| DCI 1_1 FDRA / PDSCH RIV, BWPSize, RB origin, PDSCH DM-RS CRB0 (`refPoint` -> BWPStart) | BWP 0+217 | BWP carrier start + SIB1 size (PDSCH DM-RS CRB0 exact when offsetToCarrier = 0) |
| CORESET start | grid RB | BWP-relative (`d->rb - bwp_start`); DM-RS ref / 6-RB group alignment already CRB0-based via `nr_pdcch_crb0_grid_rb()` |
| cover width | to grid end | to carrier end; published hulls wider than the carrier are clipped (non-interleaved only) |
| UL SIB1 seed BWP start (CRB) | used as grid RB | + grid RB of CRB 0 |
| NOT changed (owned elsewhere) | | UL layout lock `carrier_rb = N_RB_DL` (rr-csd-ulk); CSI-RS discovery maps (nr_csirs_disc*); PDSCH rate matching/TBS (rr-csd-rmx) |
The DCI length sweep covers `dci_len_min..max`, so wrong FDRA sizing alone does not stop the CRC; it breaks RIV decoding, PDSCH origin and the DM-RS
reference after a CORESET is verified. [CODE-READ]

## CORESET rejections on 508 (c)
- Mapping defects above are fixed by the flag, but they are not shown to be the cause of the missing CRC. [HYPOTHESIS]
- Candidate cause: the CORESET is INTERLEAVED over the whole carrier. Evidence [MEASURED, OTA log]: every GPU record with structure has bundle L=6 and
  R in {2,3,6} (Wz1 `L=6 R=2/3`, U508 `L=6 R=6/3`); 162 PRB = 27 groups divides only by R=3; the old non-interleaved cover can never decode it and a
  window-truncated interleaved range cannot be de-interleaved. Added under the flag: per nID a second cover = whole carrier, L=6, first R in {3,2,6}
  dividing the REG count, shift = PCI (default shiftIndex). Verification by TB CRC as for all dwells. Untested on air [NOT RUN].
- (b) Carrier NOT contained (starts below or ends above the grid): geometry reported invalid, legacy behaviour; the fix is the existing restart path:
  `retune_args.py RUN.LOG` on the `ISAC_ACQ_RETUNE` run (OTA chain: relaunch with its `C= R= SSB=`). No in-process resize (sample rate and buffers are sized at launch).

## Code
`nr_pdcch_blind_monitor.c`: `nr_carrier_from_sib1_enabled()`, pure `nr_carrier_geom(point_a_sc, carrier_start_sc, carrier_n_rb, grid_n_rb)`, `nr_carrier_geom_now()`,
snapshot changes, interleaved cover queue entries, `nr_pdcch_discovered_note_pci()`; `nr_passive_acq_state.[ch]`: verdict gains `carrier_start_subcarrier`, `carrier_n_rb`;
`nr_pdcch_blind_monitor_rt.c`: PCI hand-off, UL seed offset; the flag implies `ISAC_PDCCH_POINTA_REF` in `nr_pdcch_crb0_grid_rb()`.
`tests/passive_rx/timacq_replay.sh`: `RB=` variable (default 217).
Unit tests (`nr_pdcch_blind_monitor_test.cc`): CarrierGeom.* (508 / identity / offsetToCarrier / not contained / misaligned), DiscoveredCarrier.* (dwell BWP 27+162, riv 162,
interleaved cover R=3 shift 508, clipped published line, flag-off legacy).

## Results
[OFFLINE VERIFIED] `test_nr_pdcch_blind_monitor` 256/256 (new: CarrierGeom x2, DiscoveredCarrier x3). Build dir `build_gpu` (DGX, OAI_USRP=OFF), via heavy.sh.
[MEASURED, replay 0.25x, t26z, 4 RX, 12 s, one run per arm, arms not averaged; indicative only] `ENVFILE=ota/t26z.env`, build 67dc42702c + this diff:
| cell / capture | flag | carrier verdict | bank adds | PDSCHQ crc_ok |
|---|---|---|---|---|
| PCI 415 raw_pci415_12s_b (-C 3680010000 --ssb 1171) | off (first build) | CONFIRMED | 3 | 1361 (12.7 %) |
| same | on (final build) | CONFIRMED | 3 | 1706 (18.2 %) |
| PCI 923 raw_pci923_tim_12s_tile3 (-C 3759990000 --ssb 233) | off | CONFIRMED from SIB1 | 1 | 241 (3.8 %) |
| same | on | CONFIRMED from SIB1 | 1 | 259 (3.6 %) |
Grid == carrier on both cells, so the flag is an identity there: same dwell geometry (`grid_rb=0 groups=36`, bwp 0+217), no extra interleaved dwell. A first build
queued the interleaved cover unconditionally: 415 then gave 1 bank add instead of 3 (extra 4000-occasion dwells delay the real ones); fixed by skipping it
when carrier == grid (final build above). Run-to-run variance of crc_ok is large; no regression is claimed beyond "same verified CORESETs".
[NOT RUN / INCONCLUSIVE] Containment test (415 capture with a 273-PRB grid, `RB=273 SSB=1507`, off and on): initial sync never decoded the PBCH
(`NR_PBCH_DECODE polar decoding wrong`, 245 PDSCH jobs only), so the --ssb mapping for a 273 grid is wrong in my argument choice (1171+336 assumed). Not investigated.
[NOT RUN] 508: no capture contains the retuned carrier (survey c3550 ends at 3611 MHz); nothing measured on 508 after this change.

## OTA spec lines (TAG:ENV:CFG:FREQ:SSB:FO), orchestrator to run; env delta = t26z + `ISAC_CARRIER_FROM_SIB1=1`
```
V508a:t26z+ISAC_CARRIER_FROM_SIB1=1:<cfg of ab_U508a>:3590190000:517:-4842     # -r 217 unchanged
V508b:t26z+ISAC_CARRIER_FROM_SIB1=1:<cfg of ab_U508a>:3590190000:517:-4816
V508c:t26z:<cfg of ab_U508a>:3590190000:517:-4842                                # control, flag off
```
Expect in log: `discovered CORESET .. dwell start .. groups=27` (not 31), an interleaved dwell `groups=27 L=6 R=3 shift=508`, `VERIFIED -> bank add`, DCI 1_1 length lock, PDSCH CRC.
If the interleaved dwell fails too the shift guess may be wrong: next hypotheses shift sweep, R=2/6 with other group counts. Controls: 415 and 923 retuned runs must be unchanged (flag is an identity when carrier == grid).
