# X410 RX imbalance — BLOCKED EXPERIMENTS (need the radio)

Written 2026-09-08. Steps 1-2 (config audit + instrumentation) are DONE and in the tree:
`RFCHAN` / `RFGAIN` / `RFPOW` in `radio/USRP/usrp_lib.cpp` and `executables/nr-ue.c`.
Both `nr-uesoftmodem` and `oai_usrpdevif` were rebuilt (the driver is a dlopen'd plugin --
building only the executable leaves the OLD driver running; this has cost real time before).

## Why these are blocked
Everything below needs the X410 streaming. Nothing here can be answered from digital sample
levels alone: ANTPOW is mean(|I|+|Q|) normalised to the strongest branch, i.e. an amplitude
RATIO, and a quiet antenna, a gain that never landed and a wrong RX port all produce the same
ratio. RFPOW (absolute, mean(I^2+Q^2), dBFS, with clipping counts) was added for exactly this.

## Step 3 - controlled gain-response test
Purpose: establish the mapping between the software channel index and the physical channel, and
whether a requested gain actually lands.

1. All four channels streaming, steady state. Record `RFCHAN` + `RFPOW` + `RFGAIN`.
2. Change ONLY channel 1's requested gain by +5 dB, inside its supported range. Hold, then restore.
   Capture before / during / after windows in ONE continuous run, discarding settling transients.
   - Which physical channel's RFPOW moves?  (mapping)
   - Do any other channels move?            (crosstalk / shared gain stage)
   - Does RFGAIN readback equal the request? (silent clamp; note the per-channel clamp fix
     landed 2026-09-08 -- before it, channels 1-3 were applied with NO range check at all)
3. Repeat per channel only as far as needed to resolve the mapping.
4. Then run the SAME physical channel 1 ALONE, identical settings.
   TRAP: when building a single-channel streamer, do not let channel 1 silently become
   physical channel 0. Verify via `RFCHAN` subdev/port, not by array position.

Expected discriminations:
- level follows the requested channel -> mapping OK, imbalance is antenna/RF-side
- level follows a DIFFERENT channel   -> index/streamer-order mapping defect
- readback != request                 -> gain never landed; the trim conclusion is void
- ch1 alone still low                 -> not caused by simultaneous activation of the pair

## Step 4 - minimal UHD-only receiver
Same physical ports, frequency, sample rate, bandwidth, gain profile, gains, sample format.
Stable source, unchanged antenna geometry; a common controlled RF input (splitter) is far
better than over-the-air if one is available, because it removes the antenna as a variable.
- imbalance persists outside OAI  -> UHD / configuration / hardware
- imbalance disappears            -> OAI configuration or sample processing
- imbalance varies per init       -> diff the FULL readback (RFCHAN) across initialisations

Never run two applications against the same X410 at once.

## Standing constraints
- Do NOT compensate with arbitrary trims, normalise the imbalance away, or touch firmware
  before a reproducible baseline exists.
- The 2026-09-07 baseline ([0, -12.4, -0.6, -9.1] dB) is ONE 150 s capture and the shape moved
  between runs, so it is not yet a characterisation.
- "Deficit follows the physical channel when antennas are swapped" was NOT established by this
  session; no antenna swap was performed here.
