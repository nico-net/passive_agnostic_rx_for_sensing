# PBCH tracking failure at 100 MHz/273 PRB — root-caused to buffer lifecycle, not PHY (2026-08-05)

## Symptom
On the live n78 cell (PCI 2, 100 MHz/273 PRB, X410, `total-passive-ue`), PBCH/PDCCH decode fails
on every tracking occasion. Initial acquisition (the one-time cell search at process start) decodes
PBCH reliably. This document is the record of the diagnostic chain that root-caused it, branch
`nrsniffer-pdcch-pdsch-migration` (misnamed for this thread of work — the PBCH/tracking
investigation below is unrelated to that branch's original NRSniffer-migration purpose and should
probably be split onto its own branch before further work).

**Do not re-litigate anything below** — each hypothesis was eliminated by a live measurement, not
by inspection alone, and the diagnostic code (still in the tree, gated by bounded static counters
so it's inert after ~40-60 firings and safe to leave in place) is cited at each step.

## Eliminated, in order, with the measurement that killed each hypothesis

1. **Config errors** (wrong `-C`/`--ssb`/gain/USRP address) — ruled out early this session; the
   live command now used throughout (`-C 3414990000 --ssb 165 --ue-rxgain 39
   --ue-nb-ant-rx 4 --usrp-args "type=x4xx,addr=192.168.20.2"`) is independently confirmed correct
   by the SPECDIAG/FRAMESCAN evidence below (acquisition finds a clean 19 dB peak using the exact
   same config-derived `ssb_start_subcarrier`/`first_carrier_offset`).
2. **RF/hardware path** (sample loss, AGC/ADC compression, socket buffers, CPU/thread starvation,
   X410 rate coercion) — all previously eliminated (see this repo's earlier session record); not
   revisited here.
3. **Static residual CFO** — swept ±8 kHz in 500 Hz steps via the pre-existing
   `TSYNC_CFO_SWEEP`/`ISAC_PBCH_CFO_SWEEP=1` shadow path. No hypothesis rescued decode; the peak
   coherence (0.37) sat near 0 Hz with no sharp maximum anywhere in range. **Not the cause.**
4. **SFO / progressive phase drift** — the decisive test was phase-slope *coherence*, not raw
   amplitude (raw amplitude was explicitly the wrong metric here, per the correction that reframed
   this whole sub-investigation: the SSB's 4 symbols carry different signals over different
   occupied-RE patterns, so cross-symbol amplitude isn't comparable, and SFO's signature is a
   *repeatable* phase ramp, not amplitude loss). Measured via `PBCHDIAG` in
   `nr_pbch.c`'s `nr_generate_pbch_llr()` (the shared acquisition/tracking convergence point):
   acquisition shows `slope_coh=0.996-0.998` with a small, **consistent** `implied_dn≈+3` samples
   (a real, correctable delay); tracking shows `slope_coh=0.78-0.90` with `implied_dn` **random
   per-occasion** (−33 to +55 samples) and `resid≈0.9-1.0` (no coherent structure survives removing
   the fitted slope). This is decorrelation, not a timing ramp. **Not SFO.**
5. **Wrong subcarrier/extraction indices** — `SPECDIAG` (same file) logs the exact `start_bin`
   `nr_pbch_extract()` computes and reads, plus a full-spectrum sliding-window search for where the
   real SSB peak actually is. Acquisition: `start_bin=2623`, `best_bin=2623` (**delta=0**),
   62-66 RMS at the SSB band vs 7-7.6 RMS in adjacent guard bands (**≈19 dB peak, cleanly
   isolated**) — this also independently confirms `ssb_sc=165` is correct, since acquisition uses
   the identical config-derived value and finds the peak exactly there. Tracking: `cfg_rms≈7`
   (noise floor), best window found by the search is only 11-12 RMS (≈4 dB above floor — ordinary
   noise statistics) at a **different random bin every occasion**. **Not an indexing bug** —
   acquisition proves the indices are correct; tracking's spectrum simply has no SSB anywhere in it.
6. **Wrong symbol/slot selected within the buffer (SSB time-selection)** — `SSBTIME` (in
   `phy_procedures_nr_ue.c`'s `nr_process_pbch_symbol()`) logs tracking's full time-coordinate state
   against acquisition's own `SYNCDIAG` numbers (`ssbOffset=112124`, `rx_offset=103292`,
   `sync_pos_frame=8832`, `symdur=4384`) and sweeps ±1 slot (29 candidate symbol positions) around
   tracking's chosen window. **The offset arithmetic is correct**: `8832/4384=2.01`→symbol 2,
   matching acquisition's detected position exactly, and tracking's own computed `rx_offset` lands
   on the expected symbol-3 boundary to the sample. The re-basing (`syncInFrame()` in `nr-ue.c`,
   discards `rx_offset` samples to advance the live stream after acquisition) completed cleanly —
   `"Resynchronizing RX by 103292 samples"` with **zero short reads, zero give-up**. The ±1 slot
   sweep was flat noise everywhere (all 29 points ≈7, vs acquisition's ≈66) — but this sweep is
   known-blind on a loaded cell (whole-symbol energy can't see a 240-SC SSB under ~17 dB of
   wideband traffic; the pre-existing `SSBSWEEP` diagnostic shows exactly this flatness on every
   symbol). **Not conclusive on its own** — superseded by #7.
7. **DECISIVE: frame-wide frequency-domain search** — `FRAMESCAN` (same function, fires once per
   process) FFTs all ~280 candidate symbol positions spanning tracking's entire live 2-frame
   `rxdata` buffer and scores each against the SSB's own 240 bins vs. local floor (immune to the
   wideband-traffic blindness that limited #6). Result: **best ratio found anywhere in the entire
   buffer is 1.16× — indistinguishable from noise, zero hits above a 2.0× threshold.** Acquisition's
   equivalent measurement was ≈9× (19 dB).

## The clean boundary this leaves

**Acquisition receives the SSB. Tracking's entire two-frame `rxdata` buffer contains only noise —
not at the wrong offset within it, but nowhere in it at all.** Every PHY-side computation checked
(offset arithmetic, re-basing, extraction indexing, config values) is independently verified
correct by acquisition's own success on the identical formulas. The failure is therefore **upstream
of symbol selection and FFT processing** — in how `ue->common_vars.rxdata` gets filled, owned, or
handed off between the RT producer (the read thread pulling samples off the X410) and the tracking
consumer, not in any PHY/DSP computation on its contents.

## Diagnostic code left in the tree (all inert after their bounded counters exhaust)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c` — `PBCHDIAG` (phase slope/coherence, ~line 381) and
  `SPECDIAG` (spectrum/extraction-index search, appended after it) inside `nr_generate_pbch_llr()`.
- `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c` — `FEPDIAG` (3-stage raw/CP/FFT amplitude, in
  `nr_process_pbch_symbol()`'s tracking branch), `SSBTIME` (time-coordinate + ±1 slot sweep, same
  function), `FRAMESCAN` (frame-wide frequency search, same function, fires once).
- `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` — matching `FEPDIAG`/`SPECDIAG`-style logging in
  `do_time_to_freq()` (acquisition side), tagged `path=acquisition`.
- `openair1/PHY/MODULATION/slot_fep_nr.c` + `modulation_UE.h` — exported `__thread`
  `nr_slot_fep_diag_*` globals so callers can read the exact `rx_offset`/CP-length `nr_slot_fep()`
  used without duplicating its arithmetic.

**Per explicit instruction: stop adding PHY-domain diagnostics.** The bug is in the receive-buffer
lifecycle (producer/consumer synchronization, buffer ownership, or antenna/channel indexing in the
RT read thread), not in anything PHY/DSP computes on the buffer's contents.

## 8. Producer/consumer lag — measured directly, REFUTES the "stale async read" hypothesis
Added `_Atomic long nr_ue_diag_producer_absolute_slot` / `nr_ue_diag_producer_wall_ns` (defined in
`executables/nr-ue.c` near the top of the file, updated immediately after each `nrue_ru_read()` in
the main per-slot read loop at ~line 1062; `extern`-declared and read in
`phy_procedures_nr_ue.c`'s `nr_process_pbch_symbol()`, inside the existing `SSBTIME` log line as
`prod_abs_slot=`/`cons_abs_slot_wrapped=`/`wall_lag_us=`). Rationale for suspecting this in the
first place: `curMsg` dispatch to `UE->dl_actors[slot_rx % num_dl_actors]` (default
`num_dl_actors=4`, confirmed active in every run this session) via `pushNotifiedFIFO` at
`nr-ue.c:1150` copies only `.proc` metadata, NOT the samples — the worker reads
`ue->common_vars.rxdata` **by reference**, while the read loop keeps overwriting that same buffer
on every subsequent slot with no backpressure.

**Measured result (live, same command as below): `prod_abs_slot == cons_abs_slot_wrapped` on
EVERY occasion, exactly, and `wall_lag_us` on the first read of each occasion is 496-735 µs** — well
under one slot duration, a healthy pipeline. **This refutes stale-by-worker-lag as the cause.** Do
not re-investigate async dispatch timing without new evidence.

This leaves a genuine, unresolved contradiction: timing sync is healthy, offset arithmetic is
correct, extraction indexing is correct (all proven against acquisition, §§1-7) — yet `FRAMESCAN`
(§7) found no SSB anywhere in the entire buffer. Two concrete, cheap checks were identified to
resolve this and were NOT yet done when this session ended:

### 8a. Antenna/channel identity check (do this first — cheapest, most likely)
`FRAMESCAN` and `SPECDIAG` both hardcode antenna index `0` (`ue->common_vars.rxdata[0]`,
`rxdataF[0]`). Never verified that tracking's antenna-0 array is the SAME physical RX chain
acquisition's `params->rxdata[0]` scans. A channel/antenna mapping offset between the two paths
would produce exactly "healthy timing, zero signal" — this is the single most likely explanation
left standing.

**Where to look**: how `params->rxdata` (nr_initial_sync.c's acquisition scan buffer) gets
populated vs. how `ue->common_vars.rxdata` (the live ring buffer) gets populated — specifically
whether antenna index ordering/count matches between the two allocation/fill sites, and whether
`fp->nb_antennas_rx` (4, from `--ue-nb-ant-rx 4`) is consistently indexed the same way in both.

**Quick diagnostic to add**: in both `do_time_to_freq()` (nr_initial_sync.c, acquisition) and
`nr_process_pbch_symbol()` (phy_procedures_nr_ue.c, tracking), log the actual pointer value of
antenna-0's buffer (`(void*)params->rxdata[0]` / `(void*)ue->common_vars.rxdata[0]`) and, more
usefully, a cheap content checksum of the FIRST FEW SAMPLES read (before assuming anything is
aligned) from EVERY antenna index 0..3, not just 0 — if antenna 0 is silent but a DIFFERENT index
carries the SSB, that directly proves a channel mapping bug. Reuse the FNV-1a pattern already in
`PBCHDIAG` (nr_pbch.c ~line 384) if a hash is wanted, or just log `rxd[i].r,rxd[i].i` for i=0..7 per
antenna.

**Test command** (same live config used throughout this doc):
```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
sudo -E timeout 90 ./nr-uesoftmodem -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 \
  --ue-rxgain 39 --ue-fo-compensation --ue-nb-ant-rx 4 --passive-rx \
  --usrp-args "type=x4xx,addr=192.168.20.2" \
  --uecap_file /home/sens/NICOLA/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml \
  -O /home/sens/NICOLA/nrue.ota.sensing.conf > /tmp/antenna_check.log 2>&1
echo EXIT=$?
```
Then: `grep "ANTCHECK" /tmp/antenna_check.log` (or whatever tag is chosen) — compare per-antenna
levels on BOTH paths. If any tracking antenna index shows an acquisition-like signal while index 0
is silent, that's the bug, found directly. If ALL 4 tracking antennas are silent, this hypothesis is
also refuted and move to 8b.

**Build note**: rebuild with `ninja nr-uesoftmodem` from the build dir above after editing; both
files are cheap single-TU rebuilds (~10-20s each). X410 needs `sudo pkill -9 -f nr-uesoftmodem` and
a few seconds' wait if a previous run was killed uncleanly — it will refuse to reclaim ("Someone
tried to claim this device again") otherwise, not a segfault, don't mistake one for the other.

### 8b. Cross-validate FRAMESCAN itself (do this if 8a comes back clean)
`FRAMESCAN` (phy_procedures_nr_ue.c, `nr_process_pbch_symbol()`, fires once per process) uses a
UNIFORM symbol stride (`sym_stride = ofdm_symbol_size + nb_prefix_samples`), the same simplification
acquisition's `do_time_to_freq()` uses — NOT tracking's own spec-accurate variable-CP formula. Over
~280 candidate positions this could accumulate drift, though near the expected position
(`k≈rx_offset/sym_stride≈3`) the drift should be negligible (a handful of samples at most). Not
proven correct by an independent test — it has never been run against a buffer KNOWN to contain a
clean SSB (e.g. right after acquisition's own successful capture, before `syncInFrame()` discards
`ssb_offset` samples to re-base the stream) to confirm it actually finds the peak there. If 8a is
inconclusive, add a one-shot call to the same FRAMESCAN logic against acquisition's own scan buffer
(`params->rxdata`, nr_initial_sync.c) right after a confirmed detection, and confirm it reproduces
the SPECDIAG peak (`best_ratio` should come out ≈9x, matching §7's acquisition numbers) before
trusting its "nothing anywhere" result on tracking's buffer as ground truth rather than a bug in the
search itself.

## Diagnostic code currently in the tree (all inert after their bounded counters exhaust, safe to leave)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c` — `PBCHDIAG` (phase slope/coherence, ~line 381) and
  `SPECDIAG` (spectrum/extraction-index search, appended after it) inside `nr_generate_pbch_llr()`.
- `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c` — `FEPDIAG` (3-stage raw/CP/FFT amplitude),
  `SSBTIME` (time-coordinate + ±1 slot sweep + producer/consumer lag), `FRAMESCAN` (frame-wide
  frequency search, fires once) — all inside `nr_process_pbch_symbol()`'s tracking branch.
- `openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c` — matching `FEPDIAG`/`SPECDIAG`-style logging in
  `do_time_to_freq()` (acquisition side), tagged `path=acquisition`.
- `openair1/PHY/MODULATION/slot_fep_nr.c` + `modulation_UE.h` — exported `__thread`
  `nr_slot_fep_diag_*` globals (rx_offset/nb_prefix_samples/nb_prefix_samples0/is_synchronized) so
  callers can read `nr_slot_fep()`'s exact internal values without duplicating its arithmetic.
- `executables/nr-ue.c` — `_Atomic long nr_ue_diag_producer_absolute_slot` /
  `nr_ue_diag_producer_wall_ns`, updated in the main read loop right after each `nrue_ru_read()`.

## Standing test command (used for every measurement in this doc)
```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
sudo -E timeout 90 ./nr-uesoftmodem -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 \
  --ue-rxgain 39 --ue-fo-compensation --ue-nb-ant-rx 4 --passive-rx \
  --usrp-args "type=x4xx,addr=192.168.20.2" \
  --uecap_file /home/sens/NICOLA/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml \
  -O /home/sens/NICOLA/nrue.ota.sensing.conf > /tmp/<name>.log 2>&1
echo EXIT=$?   # 124 = ran the full 90s cleanly (expected/good); anything else, check the log tail
```
Config file `/home/sens/NICOLA/nrue.ota.sensing.conf` carries the blind-PDCCH-monitor and AoA
config block — not required for the PBCH-only diagnostics in this doc but harmless to leave in.
`--ue-scan-carrier` was tried once this session to independently verify `--ssb 165`: it works at
`--ue-nb-ant-rx 1` (batches GSCN scans, doesn't segfault) but stalls the RX stream
(`ERROR_CODE_TIMEOUT`) before completing at 4 antennas — acquisition's `SPECDIAG` output from that
partial run DID confirm `ssb_sc=165`/`delta_bins=0` independently, so this doesn't need repeating
unless the cell's SSB position is suspected to have changed.

## Build command
```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
ninja nr-uesoftmodem
```
Single-TU edits (any one file in this doc's diagnostic list) rebuild in ~10-20s. If it's been a
while since the last build in this tree, expect LDPC header codegen steps (`Generating LDPC ...
header files`) to run first — normal, not a sign of a broader rebuild.

## 9. NIC RECEIVE LOSS FOUND AND FIXED — and §7's decisive result DOES NOT REPRODUCE (2026-08-06)

**Read this before acting on §§1-8.** Every elimination in this document above was measured on a
stream that was losing **61.5 % of its samples**. That invalidates them as eliminations: they were
run against corrupted input. The specific claim that must be retracted is §7's.

### 9a. The transport was broken, measured at the NIC
| counter | value on the 2026-08-05/06 config |
|---|---|
| `rx_missed_errors` (NIC HW, host buffers unavailable) | **53,159,263** |
| UHD `ERROR_CODE_OVERFLOW (Out of sequence error)` | **53,323** in one 100 s run |
| `short read` retries in the UE read loop | **20,732** in one 100 s run |
| samples requested but never delivered | **780,780,240 (61.5 %) = 6.35 s of stream** |

Cause is host-side, not RF and not OAI. The link is 100 GbE and healthy, but it ran at **MTU 1500**
with a 1024-descriptor RX ring. 4 channels x 122.88 Msps x 4 B = **15.73 Gbps**, which at MTU 1500
is **1.34 Mpps**; at MTU 9000 it is 0.22 Mpps. The NIC could not get descriptors fast enough and
dropped 53 M packets.

**Fixes applied on sens3** (`enp101s0f1np1`, the dedicated X410 link; SSH is on `enp23s0f0` so these
are safe to change remotely):
```bash
sudo ip link set dev enp101s0f1np1 mtu 9000          # 1500 -> 9000
sudo ethtool -G enp101s0f1np1 rx 8192 tx 8192        # 1024 -> 8192 (max)
sudo sysctl -w net.core.netdev_max_backlog=250000    # was 1000
sudo sysctl -w net.core.rmem_default=62500000
```
Jumbo path verified end to end with `ping -M do -s 8972 192.168.20.2` (0 % loss). Host pause frames
were already `RX on / TX on`, so the X410-side `ethtool -A` step was not needed.
**These are NOT persistent across reboot — re-apply, or make them permanent, before any capture.**

**Result, measured as a per-run delta (the right test — these counters are cumulative):**
`rx_missed_errors` **unchanged over a full 60 s capture** (1451626362 -> 1463859810 packets,
missed 53159263 -> **53159263, delta = 0**).

| run | short reads | out-of-seq | lost samples | PBCH errors |
|---|---|---|---|---|
| baseline (MTU 1500) | 20,732 | 53,323 | 780,780,240 | 2213 |
| MTU 9000 | 31 | **0** | 1,018,040 | 3579 |
| + ring 8192 (60 s) | 15 | **0** | **394,656** | 2152 |

Sample loss fell by ~1978x. `RXDISCONT` (new, see 9c) went from firing on EVERY consecutive slot to
isolated pairs at widely separated slots.

### 9b. RETRACTED: "tracking's buffer contains no SSB anywhere"
§7 reported `FRAMESCAN` best ratio **1.16x, zero hits above 2.0x**, and §"The clean boundary this
leaves" built the whole buffer-lifecycle conclusion on it. **It does not reproduce.** Measured
2026-08-06 across four runs, including on the still-broken MTU-1500 stream:

| run | tracking `best_ratio` | where |
|---|---|---|
| baseline (broken stream) | **12.90** | 4 consecutive symbol positions, 600608/604992/609376/613760 |
| MTU 9000 | **14.16** | best_off 17536, `delta_vs_ref` 4068 |
| + ring 8192 | **13.61** | best_off 17536, `delta_vs_ref` 4068 |

The SSB is present in the tracking buffer at essentially the same strength acquisition sees, and on
the clean stream it is at the position tracking is looking at (the scan grid is `sym_stride`=4384
coarse, so `delta_vs_ref`=4068 is one grid step, not a real misalignment — do not over-read it).
The four adjacent high-ratio positions in the baseline run are the 4-symbol SSB burst, which is
itself a sanity check that the scan works.

**So "the failure is upstream of symbol selection and FFT processing" is unsupported.** Whatever
produced 1.16x on 2026-08-05 was a property of that run (most likely the 61.5 %-lossy stream), not a
standing property of the tracking path.

### 9c. Cross-validation of FRAMESCAN itself (§8b, now done)
`nr_isac_framescan()` (new, `nr_pbch.c`) is the scan factored out so the IDENTICAL code runs against
acquisition's own buffer — the check §8b asked for, which had never been done. Called from
`nr_initial_sync.c` (opt-in `ISAC_GOLDEN_FRAMESCAN=1`) both BEFORE and AFTER
`compensate_freq_offset()`, and from the tracking site.

This also fixes a real methodological asymmetry: `nr_initial_sync.c:289` mutates its buffer with
`compensate_freq_offset()` BEFORE measuring, so acquisition's published ~19 dB was from a
CFO-CORRECTED buffer while tracking's FRAMESCAN read a raw one. At this cell's measured ~-16 kHz vs
30 kHz SCS that is a **0.54-subcarrier straddle** — worst case for smearing energy out of the band.
Measured, it turns out not to matter much (`acq_raw` 12.81 vs `acq_cfo_corrected` 12.72; a +/-20 kHz
sweep only reaches 13.72), but the two numbers were not comparable as originally published.

### 9d. STILL OPEN: PBCH tracking decode fails on a fully clean stream
This is the honest headline. With `rx_missed_errors` delta 0, 0 out-of-sequence, and the SSB
demonstrably present at 13.6x in the tracking buffer, PBCH still fails: **2152 errors in 60 s, 1
sync, 0 T310.**

So the network defect was real and worth fixing, but it is **not** the PBCH root cause.
**Consequence: every hypothesis eliminated in §§1-7 must be re-tested on the clean stream before it
can be treated as eliminated** — in particular §5 (extraction indices) and §6 (symbol selection),
which were judged on spectra taken from a 61.5 %-lossy buffer.

### 9e. Post-sync radio state — tested, NOT the cause
Hypothesis: the post-acquisition `nrue_ru_set_freq()` retune / `nrue_ru_adjust_rx_gain()` at
`nr-ue.c:240/244` changes hardware state exactly at the acquisition->tracking boundary.
- **AGC is inert with the standing command**: `--agc` is absent (`defintval=0`), so
  `nrue_ru_adjust_rx_gain()` is never called. Confirmed by the absence of any `Rxgain adjusted` line
  in every log. The ~20 dB acquisition-vs-tracking amplitude ratio is therefore **not** a gain step.
- **The retune DOES fire** (`carrier off -15204/-16081/-16125 Hz` -> `Setting USRP TX Freq ...`).
  New opt-in `ISAC_FREEZE_RF_RETUNE=1` skips it (carrying the offset digitally the way `cont_fo_comp`
  does). Measured on the clean stream: **3763 PBCH errors vs 3579 with the retune active — no
  improvement.** Not the cause.
- `ISAC_FREEZE_RF_GAIN=1` also exists for symmetry; inert unless `--agc` is passed.

### 9f. New diagnostic: RF sample-stream continuity (`RXDISCONT`)
`executables/nr-ue.c`, right after `nrue_ru_read()`. §8's producer/consumer lag check proved the
software pipeline keeps up; it could not prove consecutive reads return consecutive RF SAMPLES. This
tests the radio's own timestamp: `expected = prev_ts + prev_consumed`, where `prev_consumed`
accumulates the main read, every short-read retry, AND the end-of-frame `first_symbols` read (all
three consume samples; counting only the first gives a false positive every frame). Bounded to 40
logged lines.
On the broken stream it fired on every consecutive slot with deltas up to +/-45792 samples against a
61440-sample slot. That is the mechanism by which a lossy transport corrupts every downstream time
offset: the retry path refills `rxdata` contiguously in MEMORY with samples that are not contiguous
in TIME.

### 9g. Standing test command — ADD THE ENV GATES
Unchanged from §"Standing test command" except prefix `sudo -E env ISAC_GOLDEN_FRAMESCAN=1` (and
optionally `ISAC_FREEZE_RF_RETUNE=1`). **Note `sudo -E env VAR=...`, not `sudo -E` alone.**
**Do not use `pkill -f nr-uesoftmodem` over SSH** — the pattern matches the SSH command line itself
and kills the session (cost two runs here). Use `sudo pkill -9 -x nr-uesoftmodem`.

### 9h. Next steps
1. **Re-test §§5 and 6 on the clean stream.** They are the two eliminations most likely to change,
   and both were judged on corrupted spectra.
2. Make the NIC tuning persistent (netplan/systemd or a udev rule) so a reboot cannot silently
   reintroduce 61.5 % sample loss and invalidate another week of measurements.
3. The remaining 15-31 short reads / ~0.003 s loss per run are not yet zero. Probably worth one look
   at IRQ affinity / CPU isolation for the RX thread, but they are ~2000x below the level that was
   breaking things and are unlikely to be the PBCH cause.
