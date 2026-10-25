# MPCI cell inventory v1 (`cells.json`, `schema: 1`)

Status: D1 deliverable (2026-10-25), for review by the blind front end agent. Reader: `common/utils/nr_cell_inventory.{h,c}` and `nr_cell_mgr.{h,c}`
(branch `mpci/c-d1`). Writer: the front end (`tests/passive_rx/autofront/*`, not changed by D1). Evidence labels: `[CODE-READ]` = read from the current
code of `fastscan.py`, `autofront.py`, `live_chain.sh` at `mpci-main` @ `e23fdddc89`.

## 1. Purpose and rules

The front end discovers cells from a wideband capture; the receiver serves them. This file is the only interface. No gNB log or config is ever an input.
The receiver never trusts a field it can verify: PCI and MIB are confirmed by its own sync; a wrong `pci` costs a failed sync, never a wrong decode.

- File name `cells.json` in the run directory (the name `live_chain.sh` already writes: `OUT/cells.json`). No environment variable and no option selects it:
  the path is an argument of `nr_cell_inventory_watch_init`; `nr_cell_inventory_default_path(run_dir)` returns `<run_dir>/cells.json`. A8 passes the receiver's run directory.
  Today there is NO receiver-side convention (the receiver is started from the triples printed by the tools), so D1 takes the path as a function argument.
- Writing: write `cells.json.tmp` in the same directory, `fsync` (optional), `rename()` over `cells.json`. The reader re-reads when `(mtime, size, inode)` change.
  A file that is truncated, not valid JSON, or violates this spec is rejected as a whole and the previous inventory stays in force.
- The writer rewrites the whole file (a snapshot of all known cells), not a delta. Unknown keys are ignored by the reader (additive evolution inside v1);
  a change of meaning or a removed required key needs `schema: 2`.
- JSON strictly per RFC 8259, UTF-8, at most 1 MiB, at most 64 cells, nesting depth at most 8.

## 2. Document

```json
{
  "schema": 1,
  "window": {"centre_hz": 3600000000, "rate_msps": 122.88, "n_rx": 4},
  "cells": [ { ...cell... } ]
}
```

`window` describes the capture window the cells are received in (one radio claim): `centre_hz` (number, Hz, 1e6..1e11), `rate_msps` (number > 0),
`n_rx` (integer 1..64). All three required. Every cell inherits `n_rx` and `window.centre_hz` (the receiver needs the channelizer unless a cell sits at the window centre).

### Cell (all keys required)

| key | type | meaning |
|---|---|---|
| `id` | string, 1..31 bytes | stable key of the cell across rewrites; unique in the file. Suggested `"pci<N>"`; for a PCI that is re-detected on another GSCN use a new id. |
| `centre_hz` | number | the receiver `-C` for this cell (Hz). Final tuning, including pass-2 / geometry refinement if the front end has it. |
| `gscn` | integer 0..100000 | global synchronisation channel number (0 unknown) |
| `ssb_hz` | number | SSB centre frequency in Hz (0 unknown): `3000000000 + (gscn - 7499) * 1440000` in n77/n78 |
| `ssb_index` | integer -1..63 | SSB beam index `i_ssb` (-1 unknown) |
| `pci` | integer -1..1007 | physical cell id (-1 unknown) |
| `cfo_hz` | number | carrier offset seed (receiver `--initial-fo`) |
| `n_rb_hint` | integer 1..275 or `null` | grid width in PRB if the front end knows it (SIB1 `sib1_nrb`, or the carrier-edge `tune_geom.size`); `null` = unknown (the admission model then sizes the largest grid of the SCS) |
| `scs_khz` | 0, 15, 30, 60, 120 | subcarrier spacing (0 unknown) |
| `sss_gap` | number | SSS detection ratio of the front end (informational, 0 unknown) |
| `dmrs_ratio` | number | PBCH-DMRS detection ratio (informational, 0 unknown) |
| `first_seen_ns` | integer >= 0 | front-end clock of the first detection (informational) |
| `state` | `"new"`, `"acquire"`, `"maintain"`, `"lost"` | lifecycle as the front end sees it |

## 3. Semantics (what the cell manager does)

| writer action | receiver reaction |
|---|---|
| new `id` appears | **add**: activate a free pre-sized slot (admission order MAINTAIN, ACQUIRE, NEW; file order inside a class). Beyond `N_max`: **refuse** with the admission reason (`cores`, `ram`, `gpu_mem`, `channelizer`, `max`), logged once, not an error. |
| same `id`, `centre_hz`, `ssb_hz` or `cfo_hz` changed | **update**: a retune event for that slot's channelizer entry. Other keys are stored, no event. |
| `state: "lost"`, or `id` missing from the file | **retire** after a hysteresis (policy default 10 s of continuous lost/absent, configurable by the caller); reappearing before it cancels the retire. The slot is freed and can be given to a refused cell. |
| two entries with equal `centre_hz` and `ssb_hz` | **co-channel cells** (WindTre 508 + PCI 4, Vodafone 415 + PCI 380): two ids, two slots, one SSB. The receiver verifies which PCI it syncs on. |

Pass-2 refinement (`k_SSB` -> `--ssb`, `-C`) is applied by the receiver from its own log (`nr_cell_mgr_pass2_from_log`): the lines `MIB fields sfn=<n> k_ssb=<k>` and
`Initial sync successful, PCI: <p>` (the lines `live_chain.sh pass2` reads); `--ssb = 1164 + k/2`, `-C = ssb_hz + (1182 - ssb) * 30 kHz`. A cell whose `n_rb_hint` is
non-null is considered already refined by the front end and its `centre_hz` stays authoritative.

## 4. Mapping from today's front-end output `[CODE-READ]`

Today `fastscan.py` (`main`, last lines) and `autofront.py discover` write `cells.json` as a **bare JSON list** (`json.dump(cells, ...)`), one object per detection,
with exactly these keys:

| key written today | by | notes |
|---|---|---|
| `gscn`, `ssb_mhz`, `pci`, `metric_db`, `sss_ratio`, `dmrs_ratio`, `i_ssb`, `scs_khz`, `cfo_hz`, `n_occ`, `capture`, `capture_centre_mhz` | both | `ssb_mhz = round(3000 + N*1.44, 2)` |
| `n_cells`, `dt_samples_7p68` | `fastscan.py` | |
| `cfo_hyp_hz` | `autofront.py discover` | |
| `tune_pass1` = `{C_hz, ssb, initial_fo, k_ssb, k_ssb_odd, pass_}` | `fastscan.py`, `autofront.py args` | pass 1: `ssb = 1171`, `C_hz = gscn_hz + (1182 - 1171) * 30 kHz` |
| `k_ssb`, `sib1_nrb`, `offset_to_point_a`, `mcc`, `mnc`, `tune_pass2` | `autofront.py refine` (`live_chain.sh pass2`, in place) | from a receiver log |
| `tune_geom` = `{C_hz, ssb, size, initial_fo, ...}` | `autofront.py geom` (`live_chain.sh geom`, in place) | carrier edges when SIB1 has no carrier info |

There is no `schema`, no `window`, no `id`, no `state`, no `first_seen_ns`, no `n_rb_hint`, no `ssb_hz` and no per-cell receiver window in the file today.

| v1 key | from today's key | rule |
|---|---|---|
| `schema` | - | constant `1` (new) |
| `window.centre_hz`, `window.rate_msps`, `window.n_rx` | - (not in the file) | the live capture/streaming window; for offline discovery `capture_centre_mhz * 1e6`, 122.88 (`SAMPLES` of `survey_capture.sh`), 4 |
| `id` | `pci` | `"pci" + pci` (new; a PCI seen on two GSCNs needs two ids) |
| `centre_hz` | `tune_geom.C_hz`, else `tune_pass2.C_hz`, else `tune_pass1.C_hz` | the most refined `-C` available |
| `gscn` | `gscn` | as is |
| `ssb_hz` | `ssb_mhz` | `round(ssb_mhz * 1e6)` (equals `3000000000 + (gscn - 7499) * 1440000`) |
| `ssb_index` | `i_ssb` | as is |
| `pci` | `pci` | as is |
| `cfo_hz` | `cfo_hz` (pass-1 value), or `tune_*.initial_fo` of the chosen tuning | as is |
| `n_rb_hint` | `sib1_nrb`, else `tune_geom.size`, else `null` | |
| `scs_khz` | `scs_khz` | as is |
| `sss_gap` | `sss_ratio` | renamed |
| `dmrs_ratio` | `dmrs_ratio` | as is |
| `first_seen_ns` | - | new: front-end clock of the first detection |
| `state` | - | new: `"new"` / `"acquire"` on first sight, `"maintain"` once the receiver reported sync, `"lost"` when it disappears |

The existing keys not listed (`metric_db`, `n_occ`, `n_cells`, `dt_samples_7p68`, `capture`, `capture_centre_mhz`, `cfo_hyp_hz`, `tune_*`, `k_ssb`, `sib1_nrb`, ...) may stay in
the file: unknown keys are ignored. The old consumers (`autofront.py args|triple|refine|specs|geom`) read a bare list, so a v1 writer either writes both files
(`cells.json` v1 for the receiver, the old list under another name) or the old tools are ported; this is the front-end agent's decision. The reader rejects a bare list.

## 5. Fixtures (tests, receiver-side measurements only)

`tests/passive_rx/mpci/inventory/`: `pass1_20261012.json` (415, 923, 508, PCI 4, 403, 675 pass-1 triples of AUTOFRONT-20261012 / FRONTLIVE-20261013 [MEASURED, DGX offline]),
`live_tuned_20261023.json` (final live tunings of PROJECT_MEMORY §25 block 2026-10-23/24 item 4 [MEASURED, DGX OTA] plus the co-channel PCI 4 and PCI 380 [INFERRED tunings]),
`pass2_pci415_excerpt.txt`, `pass2_pci923_excerpt.txt` (log lines in the format of the receiver, values from the AUTOFRONT table: 415 k_ssb=14 -> `--ssb 1171`, 923 k_ssb=10 -> `--ssb 1169`, `-C 3731910000`; SYNTHETIC excerpts, not captured logs).
The `window` of the fixtures is a fixture value and does not span all cells (the real cells sit in several windows).
