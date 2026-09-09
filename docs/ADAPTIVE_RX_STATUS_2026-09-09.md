
## Target: >=60 % CRC (excluding empty TBs) on DL and UL, all UEs. NOT met. What has been ruled out.

The correct metric already exists and should be the ONLY one quoted:
`health = crc_ok / (crc_ok + seg_fail)`, which excludes empty grants by construction. Its own
code comment records that quoting `crc_ok/try` misled the downlink investigation once. Note
that empty TBs are NOT the explanation for the low uplink figure: `health` has read
0.8-4.2 % throughout, and the best arm measured `zero_tb=0`.

### Ruled out by measurement today
| lever | expectation | measured |
|---|---|---|
| Antenna combining (MRC mode 2 -> 0) | recorded 54-71 % at mode 0 | **3.6 %** (234/(234+6258)); mode 2 was 1-4 %. No lever. |
| Link margin / MCS | low CRC from high MCS | REFUTED: MCS 4 fails 897/897, MCS 6 928/928, from close UEs |
| Segment-index rate-matching defect | gradient 81 %->100 % | REFUTED: `SEGIDXC C>=5` fails uniformly; it was a selection effect |
| Equivalence too fine (oracle-invisible fields) | class count collapses | REFUTED: 101 -> 99 |

The recorded 54-71 % single-branch figure came with an nvar fix that is itself recorded as
having failed to replicate (p=0.07, default OFF), so it should not have been trusted as a
target. Inheriting it cost a run.

### Currently testing
`--ue-rxgain 40 -> 25`. Basis: the uplink is 20-26 dB STRONGER than the downlink at this
receiver, so a DL-set gain compresses the UL; dropping to 25 previously moved the ceiling
from ~MCS 3 to ~MCS 9. **First observation: at 273 PRB, rxgain 25 does not acquire SIB1** --
the documented tension is real ("the receiver needs the WEAK downlink to sync and the STRONG
uplink to decode; one gain cannot serve both"), and 273 PRB needs more DL gain than the
20 MHz configuration that measurement came from. An intermediate value is the next test.
`RXGAIN`, `MRC` and `UL_BRANCH` are now runner env knobs, and each capture records its arm in
`arm.txt` so a result cannot be attributed to the wrong configuration.

## Scope finding: `--ue-scan-carrier` is NOT band/carrier discovery

It calls `get_scan_ssb_first_sc(fp->dl_CarrierFreq, fp->N_RB_DL, nrue_get_band(UE),
fp->numerology_index, ...)` -- it sweeps GSCN INSIDE an already-specified carrier, band,
bandwidth and numerology. So of the five CLI acquisition inputs it removes exactly one:

- `--ssb` -- **free**, just enable the existing scan.
- `--band`, `-C`, `--numerology`, `-r` -- **a real project**: true discovery means retuning
  across bands at several sample rates and numerologies, i.e. a new acquisition state machine.
  Bandwidth is the worst of them: SIB1 carries the true carrier bandwidth, but a sample rate
  is required BEFORE SIB1 can be decoded, so it needs acquire-narrow-then-retune.

Cheap middle ground worth doing regardless: MIB and SIB1 are already decoded, so the receiver
can CROSS-CHECK the supplied `-r`/`--numerology`/carrier against what the air reports and fail
loudly on mismatch. That is exactly the class of defect that silently broke sync when an
`--ssb` value was carried over from a 106-PRB configuration.

## LLR clipping: measured, and it is a SYMPTOM not a cause

There is an existing diagnostic, `LLRCLIP`, and it looked damning: 57-79 % of LLRs flattened
to +-127 by the int8 pack, with `llr_mean` swinging 13 -> 837 (60x) between grants.

Split by outcome it inverts:

| | mean clipped_at_int8 |
|---|---|
| DECODED | **64.6 %** (n=116) |
| FAILED  | **~35 %** |

Failures clip LESS. Heavy clipping accompanies a STRONG signal (large LLRs -> more clipping
AND more likely to decode); weak signal gives small LLRs, little clipping, and failure. So
the int8 pack is not what is breaking the decode.

## RETRACTION: "MCS 4 fails 897/897, therefore not link margin" is INVALID

That MCS value was read out of a WRONG WIDTH HYPOTHESIS. Those grants are not MCS 4 -- they
are misparsed DCIs pointing at the wrong PRBs, so the receiver equalises the wrong REs and
gets noise. The conclusion was drawn from the hypothesis-contaminated UL stream after
explicitly warning that this stream cannot answer chain questions.

**Standing rule for this work: the ONLY uplink numbers that mean anything come from a
converged or manually-pinned layout. Every field in an exploratory grant -- MCS, PRB, TDA --
is as wrong as the hypothesis that produced it.**

With that removed, the surviving evidence (failed decodes carry WEAKER LLRs) points back at
signal level, which is what the rxgain arm tests. `-31 dBFS` with `clip=0` at rxgain 40 says
there are ~31 dB of unused ADC range.

## A pinned layout expires when the UE population changes — measured

The 36.5 % clean UL figure was real but it was a property of the UE POPULATION it was
learned from, not of the receiver. When the UEs on the cell were swapped, the same pinned
layout gave:

| run | UL accepts | health |
|---|---:|---:|
| pinned, original UEs (0x4xxx) | 185,057 | **36.5 %** |
| pinned, population shifting | 21,890 | 8.9 % |
| pinned, new UE only (0x73fd dominant) | 1,832 | **0 %** |

**Read the ACCEPT count, not the CRC.** A wrong field layout does not merely decode badly --
it stops recognising grants at all, and accepts fell ~100x. That is the cleanest available
signature of "this layout does not belong to this UE", and it is visible long before any
CRC statistic settles.

This is the same principle as the pooled-winner corroboration gate added earlier: equal DCI
length does not prove equal RRC configuration. Here it applies to a HAND-PINNED layout
rather than a pooled one.

### RETRACTED: the UCI sweep-range conclusion

Widening the sweep 64 -> 256 RE was reasoned from "14 sweeps, 1792 valid demuxes, zero
rescues, so the footprint must be out of range". With the layout wrong for the UE, every
grant fails for reasons the UCI inverse cannot address, so zero rescues says nothing about
the range. **The 256 value is UNPROVEN, not validated.** Re-test it only once a layout that
matches the current UE is in place.

### What this makes the right experiment

Not a better pinned number -- a demonstration that the receiver RE-LEARNS an unseen UE's
layout with no hints. That is the actual deliverable (agnosticity + adaptiveness), and it is
worth more than a good CRC from a hand-pinned configuration that expires the next time a UE
re-attaches.

## BEST UL RESULT: 54.9 % health, manual pinned layout, one UE

Manual mode (`full_auto=0`), pinned layout `0:0:0:0:4:2:0:0:0:3:2:0:0:0:0:1`, UCI recovery
active. Per-UE, which is how the target is defined:

| identity | ok / n | health |
|---|---:|---:|
| **0x4cf4** | 48210 / 87795 | **54.9 %** |
| 0x74be | 0 / 253 | 0.0 % (noise) |
| 0x74a9 | 0 / 176 | 0.0 % (noise) |

`zero_tb=0` throughout, so this is clean `health`, not inflated by empty grants.

### The UCI demux is carrying about a quarter of it

| attempts | health | UCI rescue share |
|---:|---:|---:|
| 23,734 | 51.2 % | 22.5 % |
| 52,215 | 51.9 % | 23.0 % |
| 82,235 | 54.2 % | 26.3 % |

Health rises AS the rescue share rises -- the footprint cache learning this cell's UCI
footprints once and reusing them cheaply. Without the demux this run would sit near 40 %.

### Corrections to my own earlier reading

- I attributed the jump from 2.5 % to ~54 % to "the UE population". WRONG: this window has
  ONE UE with volume. The correct statement is that the pinned layout FITS 0x4cf4 and does
  NOT fit 0x4bd1 -- still direct evidence for "equal DCI length, different RRC config", but
  between two single UEs, not a population effect.
- Earlier I read 2.5 % as evidence the LAYOUT was wrong. The more likely reading is that a
  pinned layout matches some UEs and not others, which is exactly why per-UE learning (and
  the pooled-winner corroboration gate) matter.

### Still open

- **A genuine MULTI-UE measurement.** Only one UE produces recoverable PUSCH volume in this
  capture; the other identities are noise. Two UEs on iperf did not yield two decodable
  grant streams, and why is not established.
- **60 % target**: 54.9 % is the closest measured, on one UE, uplink only.
- **The conditioned-rate defect in the width search** (in-search 16.9 % vs pinned 2.5 % for
  the same class) remains unfixed, and it is what decides which layout the search would crown.
