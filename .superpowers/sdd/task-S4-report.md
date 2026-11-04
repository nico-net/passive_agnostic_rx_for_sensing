# Task S4 report: offline sensing scorer
Files: tools/sense/score_sensing.py, tools/sense/test_score_sensing.py, tools/sense/fixtures/{cell0,cell1}.jsonl.
Tests: `python3 -m unittest tools/sense/test_score_sensing.py` -> 21 tests OK (stdlib only; no pytest needed).
Mutation checks: percentile floor->ceil (3 failures), rescue "other cell" predicate inverted (3 failures); both restored.
Evidence label: synthetic fixtures built from the real nr_sense_rs_to_json format; no measured data.

## Brief vs code differences
- Brief says run `python3 -m pytest`; task says unittest; used unittest.
- Real keys: DL `tx` is an int PCI, UL `tx` is the string "rnti:<hex>"; `dir` is upper case; `layer`/`n_layers` appear only when known; `prg_size` null unless latched; `wideband` flag exists in the record (ignored: threshold re-cut from valid_cfr_prbs >= --min-prb).
- `sense_units_enabled/_violations/_disabled_skips` are NOT emitted yet in this branch (planned in the S5 task, plan line ~394); parse_log reads them and returns null/empty when absent.
- Records without a quality block (non-sensing rs, valid_cfr_prbs null/absent) are counted (`rs_without_quality`) and not scored.
## Definitions chosen (brief ambiguous)
- Percentiles linear-interpolated (gap p50 of 0.1,0.4 = 0.25). Same-instant records are one event.
- Link = (dir, tx, layer, n_layers, prg_size) inside a cell (file, pci). prg_size change splits the link.
- Windows: grid from the earliest scored record of the run, 100 ms / 1 s. Rescue fraction = windows covered by exactly one cell / all windows; split = share of those per PCI. Null with one cell.
- valid_cfr_rate = valid_cfr_prbs>0 / produced; wideband_rate also reported. valid PRBs/s over the whole run span, wideband-only and all.
- Occupancy map (JSON only): per link, rows = 1 s, columns = absolute PRB (first_prb + mask bit), count of wideband records.
## Concerns
- Per-cell (PCI) gap/availability are the union over that PCI's links (any wideband CFR), links are never pooled in the per-link rows.
- Window grid assumes one monotonic clock across files (one process); files from different runs/hosts must not be mixed.

## Fix round 1
- I1: `score()` raises ScoreError (CLI exit 2, stderr message) when per-file time ranges neither chain-overlap nor fit in 2x the longest file; `--allow-mixed-clocks` overrides. A combined span over 24 h of 1 s windows is always refused (corrupt-timestamp guard). Tests: refused, override, chained/close accepted, CLI exit codes.
- I2: single streaming pass per file; per-link compact accumulators (array('q') wideband times, array('d') snr/resid/recon, histogram, occupancy counters). Records with t_mono_ns outside [0, 4e17) are malformed. Peak RSS 111 MB (/usr/bin/time -v, 250 MB synthetic file, 527000 records, 2 cells, 23 s; host DGX, synthetic data, file removed). Occupancy rows are now absolute-second aligned (`first_sec_abs` in JSON) because the run-wide t0 is unknown in one pass.
- M2: ssb_beam and port_mask added to the link key (test: two SSB beams are two links).
- M1, M3: documented in the module docstring (unknown layer / null prg_size form their own links; link availability uses the run-wide grid).
- M4: tests for mixed clocks, short mask (vp 128, 8 mask bits), window boundary (99.999999 ms vs 100 ms). Mutation: `n = (tmax-t0)//w + 1` -> `//w` fails 4 tests; chain-overlap check weakened fails 2; restored.
- 28 tests OK.
