# gNB crash on sens4, 2026-09-25: `std::bad_optional_access` after "gNB started"

Read-only investigation. Nothing on sens4 was started, stopped or edited.

## Verdict

**Culprit: `cell_cfg.pdcch.dedicated.al_cqi_offset: -3`. Confidence: high (~85 %).** It triggers a latent
upstream bug in the PDCCH aggregation-level calculator. Any negative offset can crash it, not just -3.

**Minimal fix:** delete the `al_cqi_offset: -3` line, or set it to `0`. Keep everything else.

## What changed (live `/home/sens/gnb.yaml` vs baseline)

The live file does not match the suggestion list in the brief. Actual edits:

| Option | Baseline | Live |
|---|---|---|
| `pdcch.dedicated.coreset1_rb_start` / `coreset1_l_crb` / `coreset1_duration` | (auto) | 12 / 48 / 2 |
| `pdcch.dedicated.ss2_n_candidates` | (auto) | **`[2, 2, 2, 1, 1]`** (mixed ALs, *not* AL1-only) |
| `pdcch.dedicated.al_cqi_offset` | (0) | **-3** |
| `pdsch.max_rank` | 1 | 4 |
| `pdsch.interleaving_bundle_size` | (non-interleaved) | 2 |
| `pdsch.dmrs_additional_position` | (default) | 1 |
| `csi.csi_rs_period` | (default) | 40 |
| `log.mac_level` | - | debug |

## The code path (OCUDU `/home/sens/OCUDU`, commit 01ed93a307)

The only local modifications in the tree are two added comment lines in `csi_helper.cpp` and
`ue_channel_state_manager.h`, so they have no effect. The binary was built 2026-08-27.

1. `lib/scheduler/ue_context/ue_link_adaptation_controller.cpp:76-91`, `get_effective_cqi()`:
   `eff_cqi = wideband_cqi + dl_olla->offset_db()`, clamped to **[1, 15]** as a float. The OLLA offset is
   fractional after any ACK/NACK (`olla_cqi_inc` default 0.001, see `scheduler_expert_config.h:179`).
2. `lib/scheduler/ue_context/ue_cell.cpp:387`, `get_aggregation_level()`:
   `cqi = clamp(cqi + pdcch_al_cqi_offset, 0, 15)`. With the offset at -3, an effective CQI of 3.001
   becomes **0.001**.
3. `lib/scheduler/support/pdcch_aggregation_level_calculator.cpp:62-64`, `map_cqi_to_aggregation_level()`:
   the guard is `if (cqi > 0.0F)`, and 0.001 passes it. It then calls `get_mcs_config(cqi, ...)`.
4. The same file, `:18-21`, `get_mcs_config()`: `cqi_lb = std::floor(cqi)` = **0**, then
   `map_cqi_to_mcs(0, table).value()`.
5. `lib/scheduler/support/mcs_calculator.cpp:95-97`, `map_cqi_to_mcs()`: `if (cqi == 0 ...) return std::nullopt;`.
   So `.value()` on an empty optional throws `std::bad_optional_access` ("bad optional access"). Nothing
   catches it, so the process terminates. The message matches the user's terminal exactly.

**The crash window.** With offset `k < 0`, the crash happens whenever the effective CQI falls in
`(-k, -k+1)`. For `k = -3` that is `(3, 4)`. The default `initial_cqi = 3` (`scheduler_expert_config.h:144`)
plus the first positive OLLA step lands there by construction, so the crash is deterministic for the
first UE. Because the effective CQI is clamped to ≥ 1, *every* negative offset has a reachable window.
Positive offsets are safe: `floor` stays ≥ 1 and the result is clamped to 15, which is within
`CQI_TABLE_SIZE = 16`.

**Why RACH and ConRes succeeded first.** With the effective CQI at exactly 3.0, `3 - 3 = 0.0` fails the
`> 0` guard and takes the safe fallback loop. The fallback scheduler never sets `olla_mcs`
(`ue_fallback_scheduler.cpp` has no reference to it), so the ConRes ACK does not move OLLA. The first
fractional OLLA step comes from the first ACKed dedicated-SS PDSCH (`ue_cell_grid_allocator.cpp:420` sets
`olla_mcs`; `ue_cell.cpp:114` feeds the ACK back). The very next AL computation then throws. That
computation can be `ue_cell.cpp:199/273/345` or `ue_cell_grid_allocator.cpp:586`, all of which pass
`get_effective_cqi()`.

## Log evidence (`/home/sens/NICOLA/gnbLogs/gnb.log`, run at 20:00:05, `mac_level: debug`)

- Many UEs got as far as Msg3, but Msg3 failed (PHY `failed to reserve buffer`, 4 retx). That is not the crash.
- Then `tc-rnti=0x460e` completed Msg3, "MAC UE Creation finished successfully", and ConRes + RRCSetup
  (310 B SRB0) was ACKed at 76.5. After that came an SR, a PUCCH F2 CSI (`csi-1_bits=11`) reported
  `invalid`, and the file ends mid-line at 78.8 (`prb=[4..5)`). The async logger lost its tail on
  `abort()`, so the true crash slot is some time after 78.8. The crash is consistent with the first UE
  leaving fallback. It is not consistent with a cell-configuration build error, since the cell ran
  cleanly for 30 s before any UE arrived.
- No core dumps are available (`coredumpctl list` returned nothing).

## Other edited options: checked, not the cause

- `ss2_n_candidates [2,2,2,1,1]` with CORESET#1 of 48 RB × 2 symbols gives 96 REGs = 16 CCEs. AL16 × 1
  fits exactly, and the start RB (12) and length (48) are both multiples of 6. The config validator
  (`du_high_config_validator.cpp:202-214`) checks the range. There is no optional access on this path
  beyond the `report_fatal_error` fallback, which would print a different message.
- `max_rank: 4`: rank 4 has run on this gNB before (the 2026-09-16 rank-4 OTA work). Unrelated side note:
  the one CSI report logged was decoded as `invalid`. Watch that once the gNB is up, because it can pin
  the CQI at its initial value 3. It does not cause the crash.
- `interleaving_bundle_size: 2`, `dmrs_additional_position: 1`, `csi_rs_period: 40` (40 is a multiple of
  the 10-slot TDD period): no optional dereference reachable from these in the UE path above.

## Recommended config (keeps all the intended realism)

```yaml
  pdcch:
    dedicated:
      coreset1_rb_start: 12
      coreset1_l_crb: 48
      coreset1_duration: 2
      ss2_n_candidates: [2, 2, 2, 1, 1]
      # al_cqi_offset: -3      # REMOVED: any negative value crashes (floor(cqi) == 0 -> map_cqi_to_mcs nullopt)
```

**For AL1 testing, which is the priority:** use `ss2_n_candidates: [4, 0, 0, 0, 0]`, which pins every UE
DCI to AL1. If you keep the mixed list, a *positive* offset such as `al_cqi_offset: 3` pushes selection
toward AL1: once the effective modulation is above QPSK, the calculator returns the lowest AL that fits
the DCI. An AL1-only list does **not** make a negative offset safe, because `get_mcs_config()` runs before
the candidate check. Note that 69 bits (45 + 24 CRC) fits within the 108 bits of one CCE.

**If higher ALs are wanted** (which is what the -3 was for), do not use the CQI offset. Put candidates only
at the high ALs instead, e.g. `[0, 0, 2, 1, 1]`.

## Fallback if it still crashes after removing the offset

Bisect by reverting one option per start. Revert them from the baseline downward, in this order:

1. `al_cqi_offset` (already removed by the fix above)
2. `max_rank: 4` → `1`
3. `csi_rs_period: 40` → delete
4. `interleaving_bundle_size`, then `dmrs_additional_position`
5. `coreset1_*` (keep `ss2_n_candidates`)

Get a backtrace in one shot (the user runs this; it drives the RU):

```bash
sudo gdb -batch -ex 'catch throw std::bad_optional_access' -ex run -ex bt \
  --args /home/sens/OCUDU/build/apps/gnb/gnb -c /home/sens/gnb.yaml
```

`catch throw` stops at the throw site, before the stack unwinds. With the offset present, the expected
frames are `get_mcs_config` ← `map_cqi_to_aggregation_level` ← `ue_cell::get_aggregation_level`. The
plain `-ex run -ex bt` form also works, because an uncaught exception aborts before unwinding.

## Upstream bug (for the record)

`pdcch_aggregation_level_calculator.cpp:62`: the guard `cqi > 0.0F` should be `cqi >= 1.0F`, or
`get_mcs_config` should clamp `cqi_lb` to at least 1. The CLI accepts `al_cqi_offset` in [-15, 15]
(`du_high_config_cli11_schema.cpp:236`), so the validator lets through every value that crashes.
