
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
