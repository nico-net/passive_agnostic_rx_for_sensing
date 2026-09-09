# Adaptive UL/DL passive RX — session record 2026-09-09 (later)

Branch/worktree: `adaptive-rx-UL-DL` on sens6. Supersedes nothing; extends
`ADAPTIVE_RX_VALIDATION_2026-09-09.md`. Receiver-only OTA, authorised.

## Headline

DL is autonomous and works. **UL is not, and the reason is NOT what the previous
sessions' architecture assumed.** Everything the UL width/interpretation search
needs is now in place and correct, and it still never sees a single format-0_1
grant, on a cell that the gNB's own log proves is sending them.

## Fixed this session

1. **The tree did not build.** `nr_pusch_passive_decode.c` included
   `nr_passive_uci_demux.h` and called `nr_passive_uci_ack_demux()`; neither exists
   in the tree or as an untracked file. Reverted that hunk, kept the six real
   fixes in the same uncommitted diff. Reasoning: `docs/UL_UCI_DEMUX_PARKED.md`.
2. **UL joint initialisation** (`fe74ce576c`). `nr_pdcch_ul_discovery_grant()`
   refused every grant unless `bwp_size` and `tda_count >= 1` were configured, and
   nothing supplied either in full_auto. Two defects: `tda_count == 0` is the
   TS 38.214 default 16-entry table, a COMPLETE interpretation that
   `nr_pdcch_blind_dci01_size()` already reads as a 4-bit field, so refusing it
   treated a resolved case as unresolved; and SIB1's common initial UL BWP and
   `pusch-TimeDomainAllocationList` were already decoded and published, with only
   `ul_mu` being read off them. Now seeded as a HYPOTHESIS -- the TB CRC stays the
   authority, so a differing dedicated config fails to converge exactly as an
   unseeded search would, and the seed can only add reach.
   **Live: `UL discovery seeded from SIB1: UL-BWP=0+273 TDAs=6`.**
3. **DCI 0_0 recovered off the 1_0 scan** (`1fd296e9b4`). `nr_pdcch_blind_extract_00()`
   had existed since 2026-08-25 with no caller: 0_0 is size-aligned with 1_0, so
   the polar decode already produced its payload and the 1_0 path rejected it at
   the identifier bit. Also fixed a real defect the new test found -- extract_10's
   class-retry loop `memset`s the result between hypotheses and restored only
   rnti/format/mismatches, wiping the decoded payload.
   **Live: `dci00[accepts=2 rejects=1310]` over 480 s.** It works.

## RETRACTED, same session — read this before reusing the 0_0 rationale

The 0_0 commit message argues this cell "schedules with fallback formats", from
the census showing 13,610 format-1_0 accepts and zero 1_1. **That is wrong.**
Two independent measurements refute it:

- `dci00[accepts=2 rejects=1310]`: if the cell were scheduling UL with 0_0, that
  accept count would be in the thousands, not 2.
- The gNB's own log, last 20 MB: **746 `format=1_1` and 147 `format=0_1`, both at
  `al=2`, and no fallback at all.** Payload sizes across the last 200 MB:
  `payload_size=47` x3813 (DL 1_1), `=43` x1005 (UL 0_1), `=45` x11, `=39` x18.

So the receiver's 13,610 "dci10 accepts" are not what the gNB is sending, and the
0_0 work -- while a genuine gap, correctly closed, and live-verified -- is NOT the
reason UL sits at zero. Keep the code; discard the rationale.

## The actual open blocker, stated precisely

Over 480 s the receiver scans ~5.4M format-0_1 candidates and accepts **zero**,
while on the same cell, in the same CORESET, the same UE-specific search space,
the same aggregation level and the same RNTI, it converges Technique D off the
format-1_1 stream and produces CRC-verified PDSCH.

Everything the 0_1 path needs has now been checked against the gNB and is right:

| input | receiver | gNB log | agree |
|---|---|---|---|
| DCI 0_1 payload size | 43 (locked automatically) | `payload_size=43` | yes |
| target RNTI | 0x4656 (bootstrapped) | `rnti=0x4656`, 5306 sightings | yes |
| aggregation level | candidate list (1_1 works on it) | `dci_aggregation_level=1` = AL2 | yes |
| CORESET | 1 symbol, full 273, non-interleaved | `symb=[0..1)`, 45 ones, NON INTERLEAVED `reg_bundle_sz=6` | yes |
| DM-RS / data scrambling | PCI 2 | `nid_pdcch_data=2 nid_pdcch_dmrs=2` | yes |
| UL BWP | 0+273 (SIB1) | `bwp=[0..273)` | yes |

Active UEs on the cell: 0x4656 (5306), 0x464f (133), 0x4690 (125).

`dci01[accepts=...]` is counted AFTER the discovery controller, so it could not
distinguish "the polar decode never recovered the RNTI" from "it did, and the
width search has not converged". New counters
`ulscan[sched= crc_hit= disc=]` split exactly that. Result below.

## Method notes worth keeping

- The X410 100 GbE data link had **no IPv4 address** again (not reboot-persistent,
  as always). Every capture would have run over the 1 GbE mgmt path and been
  invalid. Re-check `ip -br -4 addr show enp129s0f0np0` before every session.
- All three captures VALID: exit 124 as designed, NIC `rx_missed_errors` delta 0,
  zero RXDISCONT/RFSTALL.
- sens4's clock is ~29 min behind sens6. Do not compare timestamps across them.

## ROOT CAUSE FOUND — it was an array bound, not the radio

Capture `adaptive_ul_dl_mrc2.AFIAsQ` (480 s, VALID) with the new counters:

```
dci01[accepts=0 rejects=1887904]
ulscan[sched=1887904 crc_hit=19094 disc=19094]
UL raw sample rnti=0x4656 len=43 payload=0x48604222a1 sample=1/8 ... sample=8/8
UL discovery width refused: raw=400 classes/error=-2
```

Read in order:

1. **The polar decode works.** 19,094 real format-0_1 DCIs for rnti 0x4656 at length 43
   were recovered and handed to the discovery controller. `crc_hit == disc`, so not one
   was lost between decode and controller.
2. **The SIB1 seed works.** The controller collected all 8 raw samples it needs.
3. **`error=-2` is `NR_HYP_SWEEP_CLASS_OVERFLOW`.** With SIB1's real 6-entry
   `pusch-TimeDomainAllocationList` (a 3-bit TDA field) at DCI length 43, the generator
   emits **400** admissible width vectors, and against those 8 real payloads they do NOT
   collapse below `NR_HYP_SWEEP_MAX_CLASSES = 64`. The search refuses, and `refused` is
   sticky for the rest of the run.

So autonomous UL was never a decode, length, RNTI, aggregation-level or DCI-format problem.
`dci01[accepts=]` being counted after the controller is what hid it: it reported the same
zero whether the polar decode failed or the search declined to start.

**Fix**: `NR_HYP_SWEEP_MAX_CLASSES` 64 -> 512, sized from the measurement above and with it
recorded in the header. This is NOT the "raise the caps to manufacture convergence" that
`ADAPTIVE_RX_VALIDATION_2026-09-09.md` warns against: `MIN_TRIALS`, `WIN_RATIO` and
`MIN_RATE` are untouched, so nothing converges on weaker evidence than before. It only lets
the search RUN instead of declining to start.

## The next wall, predicted before spending the capture

`nr_hyp_sweep_feed()` requires EVERY class to reach `NR_HYP_SWEEP_MIN_TRIALS = 300` before
any winner is declared, and those trials are PUSCH transport-block CRCs. So convergence
costs `MIN_TRIALS * n_classes` decoded uplink TBs -- 512 x 300 = 153,600, roughly an hour
at this cell's ~40 UL grants/s -- **and it needs a non-zero pass rate.**

The gNB runs `pusch: max_ue_mcs: 25` and the UL DCIs carry `mcs=25`. This receiver has
previously measured **0/58,829 at MCS 25** and **76 % with PUSCH pinned to MCS 10**
(`[[passive-pusch-decode-works-mcs-limited]]`). If the pass rate is zero the search
correctly never converges, because there is genuinely no evidence to converge on.

`pusch_book[parked=0]` and `pusch_passive[try=0]` in every capture so far: with the
controller refusing, **the passive UL decode chain has not executed once on this branch.**
Measure its real rate before proposing any gNB change.
