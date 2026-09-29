# SA fully-agnostic dedicated-CORESET discovery stall — root cause

Machine: sensnuc3, local. Tree: `/home/sens/NICOLA/rfsim-local`. No code edits made; diagnostic
run only (`B0_diag`, `CONF_TAG=.agn NUM_RX=1 NUM_UE=1 RX1_NANT=4 ISAC_DISCOVER_DIAG=1`, killed
early via SIGINT->SIGTERM->SIGKILL on the harness script once the mechanism was confirmed;
softmodem children died cleanly on SIGTERM, no leftover processes, NGAP untouched).

## Root cause (two compounding gates, both in-code)

**Gate 1 — structural ordering bug.** `nr_pdcch_blind_monitor_process_body()`
(`openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2821-2822`):

```c
if (nr_pdcch_coreset_bank_count() == 0)
  return;
```

fires on every DL slot while autodiscover is on and `nr_pdcch_coreset_bank_count()==0` — which is
the state for this entire capture. This `return` exits the whole per-slot handler *before* it ever
reaches `nr_pdcch_blind_monitor_run_occasion()` (called later, at rt.c:2913, past the
`ss_monitoring_slot_periodicity` occasion gate). But `run_occasion()` (rt.c:2954) already contains
the cheap fallback for exactly this state — its `n==0` branch (rt.c:2977-2983) tries
`nr_pdcch_blind_monitor_coreset0_uss_cfg()` first, with the comment "A USS may legally reference
CORESET#0. It is the cheapest exact geometry available OTA, so search it before spending the
occasion on unknown footprints" (rt.c:2964-2965). That fallback needs only SIB1
(`nr_pdcch_blind_monitor.c:132-166`, gated solely on `g_css0_cfg_valid` + the SIB1 DL-BWP prior,
both available since `SIB1_DECODED`, confirmed early in every run) — it is a RAR/Msg2/Msg4/C-RNTI-
on-CSS0 "anchor" path, exactly the escape hatch memory `dedicated-coreset-search-anchor-
circularity` identifies. It never gets to run: Gate 1 returns first, every slot, for the whole
capture. This inverts the code's own stated intent ("search it before spending the occasion on
unknown footprints" — it is never searched at all while undiscovered).

**Gate 2 — Technique A's own convergence gate stalls on this traffic profile, confirmed live.**
`nr_pdcch_blind_monitor_autodiscover_step()` requires `MIN_ORACLE_DWELLS=8` recurrent dwells
(`nr_pdcch_blind_monitor.c:1748`) before committing a footprint, and each dwell additionally needs
the per-window "background" (median hit count across all ~17 candidate windows on this 106 PRB
carrier) to reach `ISAC_DISCOVER_MIN_BG` (default 3, `nr_pdcch_blind_monitor.c:1628-1642`) before
any decision is made — regardless of how significant the true signal already is. Live evidence
(`ISAC_DISCOVER_DIAG=1`, `B0_diag/ue_rx1.log`): the true dedicated CORESET is found almost
immediately and repeatedly (`DISCOVERDIAG ... top_rb=29 top_corr=0.946/0.963`, later also
`top_rb=5 corr=0.908`), and its accumulated hit count climbs steadily (`DISCOVERGATE`:
calls=5000 hits=42→calls=45000 hits=385, top window 16→138) — real signal, not noise. But `floor`
(derived from the background) stays frozen at 9 (⇒ background median ≈2) from calls=20000 through
calls=45000, `lit`/`needed` frozen at 5/150 the whole time, and the min-bg gate returns `false`
unconditionally while `bg < 3` (line 1641) — so **zero dwells ever complete** (`grep -c COREMAPTOP
ue_rx1.log` = 0 in both this diagnostic run and the earlier documented 900s run) even though
`total_hits` already vastly exceeds `obs_hits_needed`. On this scene (1 active UE, 3 Mbps UDP,
AL2-only grants) most of the 17 windows never accumulate even 3 correlator hits, so the median
never becomes "estimable" within any bounded time short of the `AUTODISCOVER_MAX_OBS_CALLS=400000`
reset ceiling (which discards the dwell without counting it, so `s_lt_ndwell` never advances
either).

**Net effect**: `s_dedicated_found` (`nr_pdcch_blind_monitor.c:478`, flipped only at line 1942)
never becomes true, so `nr_pdcch_blind_monitor_autodiscover_done()` stays false, so Gate 1 fires
every slot for the whole run, so the CSS0-USS/RAR-anchor path that could bootstrap the bank via a
much cheaper, already-known geometry never executes — matching every symptom in
`rfsim-results-phaseB.md`: `rb_offset=auto:-1 dci_length=0 class_mask=0x0` frozen forever,
`coreset_ok=0` forever, zero `Technique D ARMED`, zero `accepts=`, zero `pdsch_decode[`.

## Harness note (compounds it, does not solely cause it)

`run_passive_rx.sh` starts the passive receiver only *after* `wait_for "RA procedure succeeded"`
on the active UE (comment at line ~317: "Started AFTER the active UE is connected, so the
CSI-RS/PDCCH they sense is already on the air"). So the RA-RNTI→TC-RNTI→C-RNTI anchor chain is
structurally unavailable in this harness by design (RA always happens before the receiver is up).
This is moot given Gate 1 above (the CSS0-USS pass is unreachable regardless of RA timing), but it
means even fixing Gate 1 alone would only unlock CSS0-referenced C-RNTI grants during the run, not
a fresh RA — a second, independent reason the harness under-serves this discovery path and would
need `NUM_UE>1` with staggered attach, or a UE that re-attaches, to exercise the RA anchor at all.

## Minimal fix (not applied)

Do not gate the CSS0-USS attempt behind `nr_pdcch_coreset_bank_count()==0 → return`. Either (a)
call `nr_pdcch_blind_monitor_run_occasion()` (or inline its existing `n==0` branch) on every
on-occasion slot regardless of `autodiscover_done()`/bank state — it already handles `n==0`
correctly — or (b) narrow the early return at rt.c:2821-2822 so it only skips the *discovery-walk*
consequences of Technique A, not the entire rest of `process_body()` including the on-occasion
gate and `run_occasion()` call further down. Either change lets the already-implemented,
already-cheap CORESET#0-USS/RAR-anchor pass run from `SIB1_DECODED` onward, independent of whether
Technique A's blind full-band search ever converges. Separately, Gate 2's min-background-estimable
check (nr_pdcch_blind_monitor.c:1628-1642) should probably admit a decision when a single window's
hit count is already overwhelmingly significant against the *other windows actually observed* (not
against a not-yet-estimable global median) — but that is a DSP-tuning change, not attempted here.

Report path: `/home/sens/NICOLA/docs/superpowers/sdd/full-running-agnosticity/sa-discovery-stall.md`
