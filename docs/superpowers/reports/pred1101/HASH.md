# pred1101/hash: USS hash pruning of the dedicated blind PDCCH scan

Base: mpci-int-1031 (a6e8f12e52). Branch pred1101/hash.

## What already existed
The DL dedicated scan already pruned known-RNTI polar tasks with a fitted TS 38.213 10.1 hash (`ussh_*` in
nr_pdcch_blind_monitor_rt.c, 1 audit occasion in 16), but: the logic was static in a 10k-line file (not unit-testable),
a single out-of-set DCI poisoned the geometry-wide fit (nh == 0 until 256 samples rotated), there was no per-RNTI demotion
and no counter.

## Changes
- New `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_prune.{c,h}`: the hash (Y_p,n, candidate positions, n_CI = 0), the fit (N_CCE, p mod 3,
  M_L per AL, up to 4 hypotheses, cover <= 12), admission (`nr_uss_prune_admit/allows`), audit slot (1 in 16, pure function of the
  absolute slot), `nr_uss_prune_note`. All state per cell (`NR_CELL_STATIC`), no env/CLI flag; pruning is ON only once a fit exists
  (>= 24 CRC-accepted DCIs of the geometry, hypotheses consistent with every sample).
- New falsification: a CRC-accepted DCI of a known RNTI outside its hashed set is an `audit_miss`. The fit is refitted with it; if a
  hypothesis survives (M was under-fitted) the fit widens, else the sample is NOT fitted and that RNTI is demoted to full search
  (`rnti_demotions`); it is re-promoted after 2 consecutive in-set DCIs (`rnti_repromotions`). Other RNTIs stay pruned. 8 misses with
  no in-set DCI between them (cell search space changed) reset the geometry's fit (`geom_resets`) and it is re-learned.
- rt.c: `ussh_*` now thin macros over the module; DL task site uses `nr_uss_prune_admit` (counts `pruned`, `audit_tasks`);
  the length-sweep bootstrap site also counts. UL gate (SCAN2 ULSSH) unchanged.
- Counters: log line `SENSING: USS_PRUNE pruned= audit_tasks= audit_miss= rnti_demotions= rnti_repromotions= geom_resets= fits=`
  (summary cadence) and ISAC_METRICS JSON keys `uss_pruned, uss_audit_tasks, uss_audit_miss, uss_rnti_demotions,
  uss_rnti_repromotions, uss_geom_resets, uss_fits`.
- Files: CMakeLists.txt (library source + test), nr_passive_metrics.{h,c}, nr_passive_metrics_json.c.

## Tests
`test_nr_pdcch_uss_prune` (tests/nr_pdcch_uss_prune_test.cc): Y_p,n and position examples against independently computed values,
pruned set contains every true hashed position, audit admits all, audit slot density, per-RNTI demotion and re-promotion, geometry
reset, forced UL sample take-back. Results: see below.

## Not verified
No receiver, replay or OTA run. CRC non-regression on air is by construction (pruning only removes tasks the fit says cannot carry
that RNTI; audit and demotion are the falsifiers) but unmeasured.

## Expected on-air effect [HYPOTHESIS]
Fewer polar tasks per dedicated occasion on cells with many known C-RNTIs (earlier measurement of the same mechanism, PCI 415
2026-10-08: p99 134 ms per scan before the filter), lower scanq drop_full. Read `pruned` vs `audit_tasks` (the pruned fraction),
`audit_miss` (should be ~0 on a stable cell; >0 with `rnti_demotions` means the hash assumption failed for those RNTIs) and
`geom_resets`.

## Risks for other fixes
Touches nr_pdcch_blind_monitor_rt.c (the ussh block ~3030, DL task site ~7165, summary log ~10240) and the metrics struct/JSON.
