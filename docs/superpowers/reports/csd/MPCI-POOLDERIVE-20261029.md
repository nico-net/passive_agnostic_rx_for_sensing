# MPCI-POOLDERIVE 2026-10-29 (branch mpci/c-poolderive, off mpci/int-1029c 0947bfaeb3)

Evidence labels: [CODE] read in the diff, [TEST] unit test on the DGX (aarch64), [NOT RUN] needs the OTA.

## 1. Pool admission and promotion derived from the running receiver
Before: per-cell demand 1.9 x max(1, grid ratio), wait target 6.2 %, "6 tokens per active cell" before DISC/HOUSE age promotion (constants of one OTA cell pair).

Now [CODE]:
- Pool (common/utils/nr_core_pool.c): per cell, decayed token-time of the essential classes (SCAN, DECODE, RETRY, UL) and of the sheddable ones (DISC, HOUSE) apart,
  accumulated at every lease release; half-life 2 s, normalised by the observed window; reported as measured after 0.5 s of observation
  (`nr_pool_cell_demand`). `nr_pool_essential_wait` = fraction of SCAN/DECODE/RETRY leases that waited since the pool exists (-1 under 100 leases).
- (a) Promotion of DISC/HOUSE (multi-cell or CHAN reserve only): allowed iff tokens (N - CHAN reserve) minus the SUM of the measured essential demand of the active cells >= 1 token.
  An active cell not measured yet blocks promotion (sheddable work does not jump the queue). The N=1 pool without CHAN reserve is untouched
  (the default config's "N < 6 turns DISC/HOUSE promotion off" of nr_pool_cfg_default_n stays: it is the N=1 behaviour).
- (b) Admission (common/utils/nr_cell_admit.c): `nr_host_caps_t.pool_meas_*` (filled before every run-time add by `mpci_pool_measure` in nr-uesoftmodem.c through the new
  `nr_cell_life_ops_t.pool_measure` hook) give tokens per unit of n_rb x n_rx x scs/30 of the running cells; each cell of the candidate set is charged unit x its own grid
  (no clamp at an anchor). The first/primary cell is never refused. With nothing running/measured only the structural floor (2 tokens per cell after the CHAN reserve) applies.
- (c) Wait target = max(measured essential wait fraction of the running cells, Erlang C of the running state on the measured load); the cell is admitted iff the Erlang C wait
  of the set with it stays <= that target. No 6.2 %, no 1.9, no 217x4 anchor remain (NR_ADMIT_POOL_DEMAND_REF / NR_ADMIT_POOL_WAIT_TARGET deleted).
- Startup plan (before any lease): floor only, so the first inventory is no longer cut by the old constants; run-time adds are the measured check.
- N=1: no MPCI_ADMIT_POOL, behaviour unchanged [CODE].

## 2. CB0 token bucket per cell
nr_td_cb0_wire.c: `g_sched` is NR_CELL_STATIC/NR_CELL_REF; each cell's cpu_pct = process cap (30) / active cells, active = nr_cell_count() minus cells quiesced by the A12 hook
(registered with the existing "cellfeat" hook in nr-uesoftmodem.c, only at >= 2 cells); recomputed at every use (so also on add). N=1: share = 30, identical.

## Tests [TEST]
test_nr_cell_admit 27/27, test_nr_core_pool_budget 13/13 (new: measured demand from hold time with the virtual clock, promotion follows measured demand), test_nr_cell_life 10, test_nr_cell_mgr 14,
test_nr_core_pool, test_nr_core_layout 41 pass. cb0: Cb0Sched.CpuShareIsPerCellAndSplitsTheProcessCap + Cb0Wire.TwoCellsHaveOwnBucketsAndHalfTheCapEach pass (full test_nr_td_cb0_wire passes; nr-uesoftmodem links).
static_audit --write/--check rc 0, 0 UNCLASSIFIED. sens6 frozen gate clean.

## OTA lines to check [NOT RUN]
- `MPCI_ADMIT_POOL` (N>=2 only) now carries `meas_cells= meas_load= unit= meas_wait_pct= model_now_pct=`; at the live add of a second cell expect meas_cells=1, meas_load ~ the CELL_CPU
  essential tokens of the running cell, `unit` = meas_load / (n_rb*n_rx*scs/30), target_pct = max(meas_wait_pct, model_now_pct). A refusal prints `reason=admission:pool`.
- The startup MPCI_ADMIT_POOL has meas_cells=0 unit=0 load=0.00 (floor only).
- `SENSING: TD_CB0 wiring on: ... cpu_pct=15 (process cap 30 / 2 active cells)` for each cell at N=2; 30 at N=1.
- CORE_POOL promoted[DISC/HOUSE] at N>=2 should be 0 unless free tokens exceed the summed CELL_CPU essential tokens by >= 1.
