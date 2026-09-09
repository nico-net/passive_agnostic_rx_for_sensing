# UL UCI-on-PUSCH demux — PARKED 2026-09-09

## What was found

The branch tip carried an uncommitted, non-compiling rewrite of the UCI-on-PUSCH
rescue path in `nr_pusch_passive_decode.c`. It `#include`d
`nr_passive_uci_demux.h` and called `nr_passive_uci_ack_demux()`; **neither the
header nor the implementation exists anywhere in the tree** (`git status` shows no
untracked files). `nr-uesoftmodem` therefore did not build:

```
nr_pusch_passive_decode.c:2:10: fatal error: nr_passive_uci_demux.h: No such file or directory
```

The rewrite was reverted to the committed reservation-only search. The other
uncommitted fixes in the same file were **kept** (they are unrelated and correct):
`N_RB_UL` no longer aliased to `N_RB_DL`, numerology/CP taken from `frame_parms`
instead of hardcoded mu=1, the `lenWithCrc()` bits/bytes unit bug in
`passive_ul_unav_res()`, the signed FEP window offset, the final transport-block
CRC check in `hp_crc_failed()`, and the DM-RS scrambling identity kept separate
from the physical PCI.

## Why it was parked rather than finished

1. **It is the wrong target.** Measured on `captures/mcs10_145223`
   (see `[[passive-ul-csi-on-pusch-is-the-residual]]`), the UL residual failure is
   **CSI Part 1, 9 bits, on ~12.5 % of grants — 7216 of 13786 failures, exactly**.
   HARQ-ACK at `O_ACK <= 2` *punctures*, leaves `G` unchanged, and already rides
   out at 97.8 %. A HARQ-ACK RE search addresses the case that already works.
2. **Its search is unaffordable on this rig.** The automatic branch swept
   `1024 / Qm` candidates — 256 full LDPC re-decodes per failing grant. Passive UL
   decode already runs **in-line on the RT thread at ~1065 us/grant against a
   500 us slot budget** (`[[ul-decode-inline-on-rt-thread]]`). 256x that is not a
   tuning question.
3. **CSI needs no search at all.** `O_CSI1` is fixed by the CSI report config, not
   DAI-ambiguous, and `nr_pdcch_blind_monitor.c` already parses `out->csi_request`
   — `nr_pusch_passive_decode.c` simply never reads it. Only the beta offset is
   unknown to a passive receiver, which is a handful of candidates, not 256.

## What the real fix looks like, when it is picked up

- CSI Part 1 is **always rate-matched**, never punctured
  (`G_ulsch = G - E_CSI1 - E_CSI2`, unconditional in `calc_rate_match_info_uci`),
  so the LLRs must be **compacted** at the CSI RE positions — truncating `G`
  without removing the interleaved CSI LLRs cannot work, and that is precisely
  what the reverted code was trying to fix.
- Drive it from the parsed `csi_request` plus a small beta-offset candidate list,
  with the TB CRC as oracle. Bound the trials to single digits.
- Do it off the RT thread, or behind the existing deferred-job path.

Nothing here is a claim about the OTA rate. Re-measure before quoting one.
