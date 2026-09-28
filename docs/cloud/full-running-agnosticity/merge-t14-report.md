# Merge report: adaptive-rx-UL-DL into sdd/agn-sweep (T9 list-aware probes + T14 k0 probe)

**Status: DONE.** Merge commit `fab71c036d` on `sdd/agn-sweep` (sens6:`/home/sens/NICOLA/agn-wt/sweep`).
Parents: `e15c536687` (sweep: T6 + T14 fix round) and `8779ee2845` (adaptive-rx-UL-DL: T1-9, 15, 16).
Merge base: `d7603075b9`.

## Conflicts
There was only one conflicted file, `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c`, with two hunks. The other 25 changed files merged cleanly.

### Hunk 1: static helpers before `nr_pdsch_passive_queue_thread` (~line 446)
- **HEAD (T14):** adds `dmrs_oracle_measure()` (the oracle measurement lifted into a function, with the lower-median fix and the corrected comment) and the `g_k0_probe*` counters.
- **Theirs (T9):** adds `probe_span()`.
- **Resolution:** kept both, T14 first and then T9. They are independent additions at the same insertion point.

### Hunk 2: DM-RS oracle body (~line 746)
- **HEAD (T14):** the inline measurement was replaced by a call to `dmrs_oracle_measure(..., rb0, nrb, ...)`, with `rb0/nrb = first_rb/num_rbs`. The k0 probe that follows reuses the same `rb0/nrb` for every slot+k.
- **Theirs (T9):** kept the inline measurement but switched `rb0/nrb` to `oracle_rb0/oracle_nrb` from `probe_span()`. The gate `oracle_nrb >= 4` auto-merged outside the hunk.
- **Resolution:** `const int rb0 = oracle_rb0, nrb = oracle_nrb; double prof[14] = {0}, med = 1.0;`, followed by T14's `dmrs_oracle_measure()` call.
  - T9's inline copy was dropped: the function already contains exactly the same code, plus T14's lower-median fix.
  - Because the k0 probe reads the same `rb0/nrb`, a PRB-list/PRG grant is probed on its largest real segment in the DCI slot and in every slot+k. This is the shared-code point the brief flagged, and neither behaviour had to be given up.
  - I added one clause to the k0-probe comment saying that `rb0/nrb` come from `probe_span()`.

## Auto-merged pieces I checked in the result
- **T9:**
  - enqueue normalisation (`nr_pdsch_passive_alloc_normalise`, refuses invalid lists);
  - the slot-share union excludes list/PRG grants (`CONTIG_JOB`);
  - the rank probe, CDM probe and DM-RS-ID accumulator use `probe_span()` with the `pr_nrb > 0` guard;
  - includes `nr_pdsch_prb_set.h`, `nr_pdsch_passive_decode.h` and `<limits.h>`;
  - the thread-local `gidx` in `nr_pdsch_passive_decode.c` (not in conflict).
- **T14/T6:**
  - the Qm-oracle observe still runs after `nr_pdsch_config_sweep_feedback` (~line 1064);
  - the k0 probe is intact: bounded wait until producer ≥ target, `K = min(spf−2, 32)`, all hits OR'd into `hits`, every k≥2 layer added via `add_k0`, retention checks before and after the FEP, `ISAC_PDSCH_K0_PROBE` knob, 1-in-8 throttle;
  - the median comment fix is inside `dmrs_oracle_measure()`.
- **Remaining `first_rb`/`num_rbs` reads in the queue file:** every one is a site that T9's table marks "no change": the union itself for contiguous members, the width sort, the ORACLE_GATE log, the GPU self-check (GPU refuses list grants), the xOverhead census, the big/small census, and the narrow-grant budget.

## Verification
- `pgrep -x nr-uesoftmodem` → `none` before building.
- `cmake .` inside `$B` → rc 0.
- `lane-make.sh sweep nr-uesoftmodem test_nr_pdsch_config_sweep test_nr_pdsch_prb_set test_nr_pdsch_qm_oracle test_nr_pdcch_al1_map test_nr_csirs_blind_search` → `EXIT=0`.
  - No warnings in `nr_pdsch_passive_queue.c`.
  - The warnings printed are all in other files, and `dmrs_first` is the pre-existing one that T9 noted.
- `ctest`: **5/5 pass**:

| Test | Result |
|---|---|
| `test_nr_pdsch_config_sweep` | pass, 36.2 s |
| `test_nr_pdsch_qm_oracle` | pass |
| `test_nr_csirs_blind_search` | pass |
| `test_nr_pdsch_prb_set` | pass |
| `test_nr_pdcch_al1_map` | pass |

## Concerns
- No test exercises the combined list-grant + k0-probe path, and nothing produces PRB lists yet (T10-12). The combination is checked by reading the code, not by running it. The same holds for both parents.
