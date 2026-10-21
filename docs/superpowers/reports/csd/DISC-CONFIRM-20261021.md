# Why the discovery non-blocking build logs fewer CSIRS_BLIND CONFIRMED lines, 2026-10-21

Labels: [MEASURED] = counted in run.log of the named runs (DGX + X410, daytime, n=1 per arm, never pooled across cells); [CODE-READ]; [SIM] = host-only g++ simulation of nr_gpu_chunk.h (no CUDA); [INFERRED]. Runs: TIM PCI 923 ab_m7TR1_1_073002 (restore ref) vs ab_m7TD1_1_092142 (disc); Vodafone PCI 415 ab_m7VR1_1_074100 (restore ref), ab_m7VC1_1_081200 (cleaned main 3f0d9217fa, same base as disc), ab_m7VD1_1_093301 (disc).

## 1. Verdict

Vodafone: no layout is lost. TIM: undetermined (n=1, 3 vs 5 layouts, ambiguous row-6/11/16 family). Separately, the disc build processes 4.5x fewer slots and its resident-detect stage is about 26x slower per batch; a defect in the chunk adaptation (nr_gpu_chunk.h) is the most likely cause and is fixed in this commit [fix UNVERIFIED on air].

## 2. Vodafone 415 [MEASURED]

| | VR1 (restore) | VC1 (main) | VD1 (disc) |
|---|---|---|---|
| CONFIRMED lines | 1857 | 1862 | 228 |
| distinct strings | 175 | 167 | 52 |
| resource identities (row, ports, density, nID; 24 each) | 24 | 24 | 24 |
| distinct (start RB, nRB) spans | 73 | 82 | 31 |
| identities with more than one span | 19 | 22 | 5 |
| CONFIG_EPOCH (all cause CSIRS_MAP_CHANGE except 1 DEDICATED in VD1) | 147 | 187 | 10 |
| search restarts ("#N" falls back) | 225 | 229 | 29 |
| FEP slots processed (sum of FEP lines) | 472 080 | 462 892 | 102 471 |
| BATCH lines / detect_us per batch | 88 / 0.27 s | 85 / 0.30 s | 12 / 7.75 s |
| final feed_drops / confirm_drops | 13 028 / 45 814 | 44 879 / 63 483 | 399 370 / 125 827 |

- The same 24 identities (nIDs 380, 748, 713, 415, 582, 249; 4 rows of port/density each) are confirmed in all three runs; the modal span is identical for 24 of 24 (52 RB); the modal period/offset pairs match (160/15+16, 160/20+21).
- Layouts the refs confirm and disc does not (46 and 56 at 9-field level) account for 79 and 81 of about 1860 refs lines; each appears at most 8 times (the 24 stable identities appear 39 to 108 times in the refs). They are start/width variants (for example 128:32, 128:40, 128:48, 128:56 for nID 713) of the same identities: extent jitter of the refinement, not other resources. Disc-only variants also exist (5 spans).
- The refs re-confirm because the search is reset about every 8 s: each CSIRS_MAP_CHANGE epoch is not an RX recovery, so epoch_listener (nr_csirs_disc.c) calls nr_csirs_blind_rt_discovery_reset, which forgets all confirmations, removes the monitor exports and resets the ring [CODE-READ]. The next batch re-logs every identity (CONFIRMED #0, #1, ...). 1682 of 1857 ref lines (91 %) repeat a string already logged; disc 176 of 228 (77 %) with 29 restarts.
- The CONFIRMED rate per 10 min in disc follows its epochs: 136 / 7 / 85 lines with 6 / 0 / 4 epochs.

## 3. TIM 923 [MEASURED, n=1 each]

- Ref: 12 lines, 6 strings, 9 epochs, all DEDICATED_CHANGE_SUSPECTED (first 20 s). Disc: 3 lines, 3 strings, 0 epochs. The repeats in the ref come from those 9 resets.
- Both confirm row 1 (period 80, offsets 16/17, symbol 9 and 5). The ref also confirms row 6 symbol 13 (40/8), row 11 and 16 (40/8) and logs MONITOR_ADD for them (16 adds against 5); disc confirms one row 6 symbol 12 period 80 offset 48 and adds only row 6 80/48. Row 16 40/8 is a WINNER 10 times in disc and is never added. In disc 4.5x fewer slots were processed (36 989 vs 136 956 FEP slots), detect_us per batch 7.7 s vs 0.24 s.
- This is the row 6/11/16/17 family at period 40 offset 8 (the same slot lattice with different symbol hypotheses); with n=1 and different symbols found per run, the data cannot say whether disc missed a resource or settled on a different alias. Undetermined.

## 4. Diff review (3f0d9217fa..ecc628467b) [CODE-READ]

- Chunking: normalize, pool, accumulate, lattice_*, segment, fep_unpack/extract, sweep (candidate dimension, existing cand0/nctot), refine (unit range, accumulators in device memory): block offsets only, independent blocks run in the same order; refine unit count (valid slots with abs_slot%period==offset, times antennas, times ports for rows other than 1) matches refine_cpu; a short count only moves work into the last launch. Bit-identical as the report claims; no semantic change found.
- finish_host and energies on the host run the same functions (`__host__ __device__`); the only numerical difference possible is FMA contraction in nvcc device code vs the host compiler (double, last-bit) and not tested here. energies read one or two symbol rows of antenna 0, the same elements the kernel read.
- Polled waits: results are consumed in the same order; only the latency changes.
- No confirm threshold or admission rule changed. The only effect on confirmation counts is throughput (next section).

## 5. Defect found: chunk size pinned at one unit [CODE-READ + SIM, on-air cause INFERRED]

NrGpuChunker starts at per=1 (a static per call site) and grows only when the chunk's whole wall time (launch, poll wake-up, up to 100 us admission, queueing behind the always-busy real-time graphs) is under 500 us; it shrinks above 1500 us. With a wall-time floor of 1000 us or more the chunk never grows from one unit [SIM, 3000 chunks for 3000 units, 3.3 s instead of 7.4 ms in the model; with the old rule at floor 1000 us]. A resident detect of a few thousand blocks then costs seconds [consistent with MEASURED 7.75 s vs 0.27 s per batch, FEP us/slot 196 vs 158, 4.5x fewer slots, feed_drops 9x higher]. Not proven on air: no per-chunk count is logged.

Fix (nr_gpu_chunk.h): the adaptation counts only the time above a measured floor (minimum wall time, raised only by one-unit chunks) as work. Tests: existing GpuChunk test unchanged and passing in a standalone g++ replica; new GpuChunk.FloorOfWallTimeDoesNotPinChunkAtOneUnit (floor 0 to 3000 us: fewer than 40 chunks for 3000 units, run time per chunk bounded). Not compiled with the project build.

## 6. On-air check

Same cell, same hour, 30 min each, Vodafone: ref (3f0d9217fa) and disc with this fix, 2 runs each. Pass: BATCH detect_us per batch within 2x of the ref (about 0.3 s), FEP slots sum within 20 % of the ref, feed_drops of the same order, 24 identities and the same modal spans, and ULSTALL/EVMGRANT gspan criteria of DISC-NONBLOCK-20261021.md still met. TIM: 3 runs each; compare the set of MONITOR_ADD rows, not the number of CONFIRMED lines. Count identities (row, ports, density, nID) and modal spans, never CONFIRMED lines.
