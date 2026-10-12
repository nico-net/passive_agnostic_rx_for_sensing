# CORESETALL 2026-10-12: why 508 never verifies a dedicated CORESET, and a bounded geometry enumeration (ISAC_PDCCH_DISC_ENUM)

Branch csd/coreset-all (from csd/otatest eb0fec76ff), worktree rr-csd-cal, build dir build_t26 (CUDA, DGX GB10 aarch64). Labels: [MEASURED, OTA log] = ota_runs
logs read; [MEASURED, replay <capture>] = DGX replay 0.25x, 4 RX, ENVFILE t26z.env + ISAC_CARRIER_FROM_SIB1=1, 12 s capture, n=2..4 runs per arm
(indicative); [OFFLINE VERIFIED] unit tests; [CODE-READ]; [INFERRED]; [NOT RUN]. Cells and captures are never merged.

## 1. Why every 508 candidate is rejected [MEASURED, OTA log: ab_V508a/b/c, ab_U508a/b]

| run | gpu passes | first pass with candidates | dwells (non-interl / interl) | rejected | VERIFIED |
|---|---|---|---|---|---|
| V508a 20:07 | 227 | 125 | 24 (14 / 10) | 24 | 0 |
| V508b 20:09 | 239 | 56 | 16 (11 / 5) | 15 | 0 |
| V508c 20:12 (flag off) | 214 | 199 | 9 (9 / 0) | 8 | 0 |
| U508a 16:41 | 235 | 68 | 10 (7 / 3) | 10 | 0 |
| U508b 16:43 | 232 | 3 | 14 (14 / 0) | 14 | 0 |
Controls on 415 (ab_A1, ab_B2): first candidates at GPU pass 1 and 12, VERIFIED at the first dwell.

Findings, in order of weight:
1. **The nID candidates are noise, so the rejections are correct.** Candidates per run: V508a {51400, 57280, 56419, 61948, 61933}, V508b {63328, 63196, 63040,
   63955, 51400}, V508c {63955, 51400, 31039}, U508a {36098, 59080, 43339, 36566}; `ties=3..6` (415/923 have `ties=2`), structure fields differ per record
   (`grid_rb=33 groups=30 L=0`, `grid_rb=75 groups=18 L=6 R=3 shift=13`, `L=6 R=6 shift=3`: not reproducible between runs), and only 51400 / 63955 recur across
   runs. The geometry hypotheses actually dwelled were therefore never tested against a real nID. The L=6 R=2/3/6 "structure" in the GPU records is not a
   measurement of an interleaver [INFERRED from irreproducibility].
2. **There was no UE-specific traffic to verify against in any of the 5 windows.** DCIs seen per run (>= 3 occurrences): SI-RNTI 0xffff 4573-7710, RA-RNTI 0x63 35-142
   and 0x1d 57-131 (0x1d = 1 + 0 + 14*2 = 29 is also an RA-RNTI), no TC-RNTI, no C-RNTI; `pdcch_accepts_c` 496-756 are CSS0 mismatch accepts. Technique A
   (nID = PCI): COREMAPOCC 6.7 % (= the noise bar 0.55), recurring windows are w3 / w25 / w14 outside CORESET#0 with `rnti=0x0000`; the bootstrapped footprints
   (`rb_offset=23 span_rb=6 bootstrap_rnti=0x3eda`) and the 666 x 3 x 6 six-RB candidates all end in `DCI length unresolved after 500 occasions`. 403 (barred) and 675 show
   the same absence (X403: 0 recurring non-SI RNTIs; W675: SI only). "No dedicated CORESET found" and "no dedicated CORESET traffic" are indistinguishable on these logs.
3. **Hypotheses actually tried vs the TS 38.211 7.3.2 space.** Tried: duration 1 only (t26z sets `ISAC_COREMAP_IDSWEEP_DUR=1`, the GPU stage reports `dur=1`
   always), symbol 0, bundle 0 (non-interleaved) on the CRB-0 grid, one whole-carrier interleaved cover: L=6, R = the first of 3,2,6 dividing the REG count (R=3 for 27
   groups), shiftIndex = PCI, plus the GPU hulls clipped to the carrier. Not tried: durations 2 and 3, bundle 2 (and 3 at duration 3), R = 2/6 for group counts they
   divide, shiftIndex 0 or any value other than PCI, CORESET smaller than the BWP (interleaved: the group count changes the interleaver), nID = PCI (pdcch-DMRS-ScramblingID
   absent, the commonest configuration; CORESET#0 is masked from the nID sweep and Technique A only tests six-RB windows).
4. **What is ruled out.** DCI size (the dwell sweeps `dci_len_min..max`, so a 162-PRB RIV width cannot reject; CARRIERGEOM fixed the sizing anyway); Point A / DM-RS reference
   (CRB-0 based, carrier verified, `groups=27` after the fix); CCE-to-REG mapping for interleaved CORESET#0 (the same demap decodes SI-RNTI at 98 % CRC); aggregation levels
   and search-space hashing (anonymous AL cover, USS hash only after an RNTI is known). None of these is tested by a dwell whose nID is noise.
5. **Pre-existing cost of any dwell** [MEASURED, replay raw_pci923_tim_12s_tile3, OFF arm]: C-RNTI 0x4e32 is decoded 477 / 641 times per 3000 slots before a dwell starts
   and 45 / 23 / 10 afterwards (slots 18000-27000); `pdcch_accepts` 2844 vs 831-841 when a dwell is active for most of the run (ENUM extras queued behind a verified
   CORESET), PDSCH crc_ok 16-187 vs 231-269. An active dwell starves the verified CORESET. Owner of that path was not touched (PDCCH rt / decode); needs its own task.

## 2. Change (default off): ISAC_PDCCH_DISC_ENUM=1 (+ ISAC_PDCCH_DISC_COVER=1, as in t26z)
`nr_pdcch_blind_monitor.c` (`discovered_cover_queue`, `nr_pdcch_discovered_snapshot`), `nr_pdcch_blind_monitor_rt.c` (PCI hand-off now precedes the poll).
- Extras appended AFTER the covers and published lines, per nID (first `ISAC_PDCCH_DISC_ENUM_NIDS`=4 GPU nIDs, then nID = PCI), at most `ISAC_PDCCH_DISC_ENUM_MAX`=24 per nID:
  A durations 1-3 not yet covered, non-interleaved; B bundle 6, R 3/2/6, size {whole carrier, largest even, largest multiple of 6} (group counts the R divides), shift = PCI;
  C shift 0; D bundle 2; E other durations interleaved. Combinations the carrier width cannot satisfy are skipped at dwell start; dwells already run are not repeated
  (legacy interleaved cover included); legacy interleaved cover still skipped when grid == carrier, the extras are not.
- Statistical control unchanged: each extra is a dwell verified by TB-CRC-recovered RNTI recurrence with a fresh payload (4000 occasions). Bound: <= 5 x 24 dwells (~2.5 s each).
- Extras stop as soon as any CORESET is VERIFIED (finding 5): enumeration only runs while nothing is banked, so 415/923 are unaffected (see 3).
- `disc_coreset_t` gains `ex`; queue 128 -> 256 entries; cover marker `ng <= 0` (0 whole carrier, -1 / -2 size codes).
[OFFLINE VERIFIED] test_nr_pdcch_blind_monitor 262/262 (new DiscoveredEnum x4: durations/interleaver/shift/bundle/PCI enumeration, cap and flag off, 162-PRB sizes
and no duplicate of the legacy cover, extras end after a bank add).

## 3. Regression [MEASURED, replay, t26z + ISAC_CARRIER_FROM_SIB1=1, build = this diff]
| capture | arm | runs | first bank slot (abs_slot at VERIFIED) | bank adds | PDSCHQ crc_ok / decoded |
|---|---|---|---|---|---|
| raw_pci415_12s_b (-C 3680010000 --ssb 1171, FO 6900) | OFF | 2 | 11980 / 14853 | 1 / 3 | 1304/5072, 1745/10084 |
| same | ENUM final | 2 | 15275 / 14822 | 2 / 3 | 1839/10333, 1590/9655 |
| raw_pci923_tim_12s_tile3 (-C 3759990000 --ssb 233, FO -4800) | OFF | 4 | 13820 x4 | 1 | 269, 231, 238, 247 of 4363-5036 |
| same | ENUM final | 3 | 13782 / 13820 / 13782 | 1 | 282/4942, 271/4519, 590/10686 (bimodal, as OFF history) |
| same | ENUM earlier builds (extras still ran after the bank add; 4 + 4 runs) | 8 | 13782-13862 | 1 | 16 x4, 85, 182, 186, 187 (rejected design, finding 5) |
Same time-to-bank and same banked CORESETs; no regression claimed beyond that (n = 2..4). survey c3610g8 (1 s tiled x8, `--ssb 1171`, -C 3570570000): syncs, PCI 508 from the SIB1
cache, no crash, no GPU passes in 1 s (ENUM has nothing to do): [MEASURED, replay survey c3610g8]. 508 itself: [NOT RUN] no capture contains an active 508 UE.
Offline traffic probe c3610 (rx0, 508 band 3562-3618 MHz): 32 % of 0.5 ms blocks are >= 10 dB above the floor; cannot separate PDSCH from CSI-RS, SSB and the co-channel PCI 4 [MEASURED, offline].

## 4. Cells without servingCellConfigCommon (403) or SIB1 (PCI 4): NOT implemented
The nID sweep needs the CRB-0 index of the grid (PDCCH DM-RS sequence index, TS 38.211 7.4.1.3.1; also the 6-RB group phase). It comes only from SIB1 offsetToPointA + k_SSB;
the MIB gives k_SSB and the CORESET#0 offset relative to the SSB, which fixes Point A modulo one RB only [INFERRED]. A MIB-only hypothesis therefore needs offsetToPointA
swept (even values in 15 kHz-RB units, ~0..140, ~70 hypotheses, prior: SSB within ~10 MHz of the carrier edge as on 508/403), each with a full GPU nID pass series (3..199 passes
here) and a significance gate against the `ties>=3` noise of finding 1; it cannot be validated on any existing capture. The only existing SIB1-free path is
`ISAC_ACQ_CARRIER_FROM_RS` (a CRB-indexed CSI-RS confirms Point A = grid start), which needs the grid started at Point A. Proposed next step: an offline Point-A scan for nID = PCI
on a capture with an active UE before touching the live gate.

## 5. OTA spec lines (TAG:ENV:CFG:FREQ:SSB:FO), orchestrator to run; env delta = t26z + ISAC_CARRIER_FROM_SIB1=1 + ISAC_PDCCH_DISC_ENUM=1
```
E508a:t26z+ISAC_CARRIER_FROM_SIB1=1+ISAC_PDCCH_DISC_ENUM=1:<cfg of ab_U508a>:3590190000:517:-4842   # run >= 400 s; nID candidates arrive at GPU pass 3..199
E508b:t26z+ISAC_CARRIER_FROM_SIB1=1+ISAC_PDCCH_DISC_ENUM=1:<cfg of ab_U508a>:3590190000:517:-4816
E4a:t26y_pci4+ISAC_CARRIER_FROM_SIB1=1+ISAC_PDCCH_DISC_ENUM=1:<cfg>:3590190000:517:-4842             # PCI 4 (ISAC_TARGET_PCI=4); gate stays closed without SIB1 (section 4)
E403a:t26z+ISAC_PDCCH_DISC_ENUM=1:<cfg of ab_X403>:3625290000:1171:-2400                            # control: expect SI/TC only, no dwell
E675a:t26z+ISAC_PDCCH_DISC_ENUM=1:<cfg of ab_W675a>:3426570000:1171:-2400                          # weak; expect no change
```
Precondition for a meaningful 508 verdict: a connected UE with traffic on the 508 operator during the window (all 5 logs had none; a null result without one proves nothing).
Expected log on success: `... is the ISAC_PDCCH_DISC_ENUM hypothesis dwell`, `VERIFIED -> bank add`, then C-RNTI recurrence and PDSCH CRC. Optional knobs:
`ISAC_PDCCH_DISC_ENUM_NIDS` (4), `ISAC_PDCCH_DISC_ENUM_MAX` (24). Controls 415 / 923 must show unchanged time-to-bank (flag is inert once a CORESET is banked).
