# MPCI-FIX — multi-PCI fixes, 2026-11-02 (branch fix1102/mpci-pbch, from mpci-main 531488c715; not merged, not pushed)
Host DGX Spark + X410 34B2AF1, 4 RX, sensing cfg, MPCI_20261025.env + ISAC_SIB1_CACHE=0. Labels per PROJECT_MEMORY §0.1; numbers are per cell, never pooled.
Commits: 3d5e2371c9 (nID admission), 02e01e8ad2 (lock verify + per-cell recovery), bd0585c111 / bf45fa3835 (offline channelizer tool).

## B. Per-cell recovery — DONE on air
Root cause [MEASURED, OTA 15:45-15:59]: through the wideband (channelizer) path a cell locks an SSB beam whose initial-sync PBCH passes CRC but whose tracking
PBCH never decodes (34/34 such locks across today's OTA, 0 ok from the first census window; every lock that tracked decoded in its first window). The RFSTALL
watchdog waited 8 windows, then a full device re-init restarted EVERY cell; after 5 the receiver exited rc=3 (6 of 6 two-cell runs at ~90 s, both builds).
Fix 02e01e8ad2 (general, no flag): nr_lock_verify.{h,c} (pure, test_nr_lock_verify): a lock with no tracking PBCH for 2 census windows is rejected (LOCK_REJECT),
only that cell re-acquires and the beam is skipped by its next searches (forgotten after a verified lock or 4 empty searches); a PBCH loss with signal present
re-acquires that cell only; the device re-init stays for a collapsed RF stream. Static-audit table updated (2 per-cell statics).
| OTA 300 s [MEASURED] | build | exit | device re-inits | 415 grants (CRC) / >128 PRB | other cell grants (CRC) |
|---|---|---|---|---|---|
| 415+923 x6 (b1B,f1B,b2B,b2Br,f2B,f2Br) | base / base+3d5e | rc=3 ~90 s | 4 each | ~0 | 923 ~0 |
| 415+923 fB1 / fB2 | fix | full | 0 / 0 | 8717 (79.5 %) / 30049 (81.3 %) | 923 8680 (79.3 %) / 0 (1 lock) |
| 403+508 bLW / fLW | base / fix | rc=3 / full | 4 / 0 | 403: 6 / 496 (0 %) | 508: 1 / 9 |
| 415+403 bVL / fVL | base / fix | full / full | 2 / 0 | 70306 (70.7 %) / 45388 (77.6 %) | 403: 6163 (25.2 %) / 10903 (33.2 %) |
Solos on the fix build, 180 s [MEASURED]: 415 74063 (68.2 %), 923 23599 (72.7 %), 403 2183 (18.2 %), 508 3: in this morning's range, 0 rejects.
Not covered: the rejected beam comes back after the memory clears (same beam rejected 3-29x per run), so the cell loses time; the underlying bad lock is C below.

## A. Second-cell CORESET / PBCH — partly
[OFFLINE VERIFIED, control-arm replays] the later-locking cell's GPU nID sweep was shed by pool admission (nid_passes=0) -> fix 3d5e2371c9 (never shed the sweep
of a cell with no published CORESET; test_nr_pdcch_nid_worker_shed). No measured gain in replay (the 4 s replay loop restarts the sweep); on air 923 banked its
CORESET in fB1 (32 passes, 8680 grants). OPEN: 923 PBCH acquisition with two cells (fB2: 1 lock, 57 failed searches); not root-caused.

## C. Wideband path — root cause narrowed, not fixed
[OFFLINE VERIFIED, DGX replay, 11:47 capture, fix build, 100 s, n=3; 415 = c0]
| arm | lock rejects | >128 PRB grants (CRC) | decoded PRBs |
|---|---|---|---|
| X: capture channelized OFFLINE (iq_chan_offline, receiver's filter code), replayed as direct rings | 0/0/0 | ~4k each (86-88 %) | 1.2-1.3 M |
| W: same capture, live GPU channelizer (default) | 35/32/11 | 0/0/763 | 0/0/0.23 M |
| W at publisher speed 0.5 | 18/4/18 | 0/5056/0 | |
| W, live CPU channelizer (NR_GPU_PATH_CHAN=cpu, diagnosis) | 0/0/0 | 891/1368/1498 (5/90/7 %: 18-20 overruns) | |
| W, GPU channelizer + every other selectable GPU path on CPU | 28/19 | 0/0 | |
Established: GPU vs CPU channelizer arithmetic on the real capture <= 1 LSB; a temporary tap of what 415 reads from its ring in the live GPU run equals the offline
output (99.98 % of 61k blocks; 1/4 = the publisher's >>2, same in every arm; 10 zero samples at each 2 s replay loop seam); GPU FEP stale/self-check counters 0.
So the samples are right; the bad locks need the live GPU channelizer running, and are not caused by real-time pressure or by the selectable GPU paths.
Remaining suspects (no switch exists): the GPU channelizer's own stream/lease/green-context interaction with the slot path, and GPU users without a path switch
(PDCCH GPU, CSI-RS discovery, nID sweep). Next (one plan): with the GPU channelizer on, turn those off one at a time (needs a diagnostic build); stop at the
first one whose removal gives 0 rejects in 2 runs, else instrument the receive timing of 415's ring reads vs the X arm.

## OTA test for the operator
Fix build (wt/rr-fix-mpci-pbch, 02e01e8ad2 or later), daytime, 300 s, 2-PCI 415+923 and 415+403 x2 each, + solos 415/923 x1. Look at: rc (expect 130, no rc=3),
"full device re-init" = 0, LOCK_REJECT per cell, per cell >128 PRB CRC and decoded PRBs vs solo, 923 locks / failed searches.
