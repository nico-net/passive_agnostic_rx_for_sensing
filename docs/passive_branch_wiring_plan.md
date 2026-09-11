# Passive-RX branch wiring plan (P05)

Plan: `adaptive_RX_pipeline.md` (`adaptive-rx-sensing`, `merge/adaptive-sensing`, inspected at
`419f18bf42c2aa732d790ee62f3dcb6beeb64a9e`). Task: P05 (Stage 1, `adaptive_RX_pipeline.md` sec
3.1/3.2/4). **This document does not wire anything.** `executables/nr-ue.c` and `nr-ue-ru.c`
remain dirty with another session's uncommitted work (see the P05 brief's "Controller scope
ruling"), so this is a read-only map from the real read loop, as it stands today, onto the new
`nr_rx_branch_sync_t` (this task) and P03's `nr_rx_branch_t` — exactly what the eventual wiring
step needs to move, and where to put it.

Method: read `executables/nr-ue.c`'s `UE_thread()` (the single, persistent RX+decode loop) and
`executables/nr-ue-ru.c` directly against the running tree (commit above), starting from
`docs/passive_branch_globals_audit.md`'s (P03) classification of the same file's globals, then
re-confirming every statement cited below by reading its actual surrounding code — not by copying
the audit's line numbers unchecked, since the audit covers *globals* and this plan covers
*statements in the per-slot loop*, a different (overlapping) pass over the same file. Every
file:line citation below was read directly in this session. Re-grep before acting on this against
a later commit — `UE_thread()` has visibly been under active edit by other sessions (see the
brief's list of dirty files) since P03's audit commit.

## Classification key

Same two buckets the plan (sec 3.1) draws: **AcquisitionOwner** (the one thing that owns the
radio — `nrue_ru_read`, retune, device restart, the common sample counter/RF continuity) and
**ReceiverBranch** (per-branch synchronization/CFO/timing state). A third column notes whether the
field lives in `PHY_VARS_NR_UE` today (so a branch-per-context design needs one `PHY_VARS_NR_UE`
instance per branch to carry it) or is `UE_thread()`-local (so a branch-per-context design can
carry it in a per-branch struct without touching `PHY_VARS_NR_UE` layout at all).

---

## AcquisitionOwner — statements that must stay common, one instance for the whole receiver

| Statement | File:line | Lives in `PHY_VARS_NR_UE`? | Why AcquisitionOwner |
|---|---|---|---|
| `nrue_ru_read(UE, ..., fp->nb_antennas_rx)` — the actual radio read, called 4 times per loop body (initial-symbol read, main per-slot read, next-frame first-symbol read, post-rebase resync read) | `nr-ue.c:945, 1243, 1399, 1529` | N/A (function call; consumes `UE->rf_map.card` indirectly via `nrue_ru_read()` itself, defined `nr-ue-ru.c:498`) | The literal hardware read `nrue_ru_read()`/`nrue_ru_reinit()` names verbatim per the plan (sec 3.1). One physical channel set is read once per slot for ALL branches; a branch never issues its own read. |
| `dev->trx_read_func(dev, ...)` inside `nrue_ru_read()`, indexed by `openair0_dev[UE->rf_map.card]` | `nr-ue-ru.c:498-500` | N/A — indexed via `UE->rf_map.card` (`PHY_VARS_NR_UE.rf_map`, `defs_nr_UE.h:285`) into file-scope `openair0_dev[]`/`openair0_cfg_g[]` (`nr-ue-ru.c:93-94`, both (H) in the P03 audit) | The literal UHD device handle. `rf_map` is the one field that couples a reused per-branch `PHY_VARS_NR_UE` back to hardware — see the audit's cross-cutting observation 4. |
| `rx_continuity` (the DETECTION half only): `nr_rx_continuity_check(&rx_continuity, rx_timestamp)` — compares the actual sample timestamp against the expected next timestamp | `nr-ue.c:1007` (decl), `1465-1466` (check), `1547-1548` (commit) | No — `UE_thread()`-local | Detects a genuine UHD-timestamp gap: an AcquisitionOwner-level event (the audit's own words: "a genuine UHD-timestamp gap — an AcquisitionOwner-level event"), matching what P03's `nr_rx_branch_set_rf_discontinuity()` is for. Today this is ONE detector for the one physical read; a per-branch design still needs only one of these per PHYSICAL channel set (not one per logical branch), since all branches share the same underlying sample stream from the acquisition side. |
| `s_rxts_prev_consumed`, `s_rxts_discont_total` — the discontinuity accounting fed by the check above | `nr-ue.c:1461-1462, 1483, 1490, 1547` | No — `UE_thread()`-local `static` | Same reasoning as `rx_continuity`: counts a hardware-stream event, not a branch's own reaction to it. |
| `nr_ue_rebase_epoch`, `nr_ue_pending_rebase_delta`, `nr_ue_pending_rebase_valid` — the coarse-timing-rebase mechanism and its own epoch counter | `nr-ue.c:842-846` (decl), `1311-1324` (apply) | No — file-scope `_Atomic` globals | This codebase's own pre-existing analogue of the plan's "RF continuity epoch" (P03 audit, line 38). `syncInFrame()` (`nr-ue.c:1317`) discards samples through the acquisition-owned discard path — moving the stream origin is explicitly AcquisitionOwner's job, never a branch's. |
| `nr_ue_cfo_resync_request` / `nr_ue_cfo_resync_hz` (request flag + Hz value) and their consumption: `nrue_ru_set_freq(UE, ul_carrier, dl_carrier, nr_ue_cfo_resync_hz)`, `UE->common_vars.freq_offset = nr_ue_cfo_resync_hz`, `nrue_ru_reinit()` | decl `phy_procedures_nr_ue.c:65-66` (extern'd `nr-ue.c:278-279`); consumption block `nr-ue.c:1998-2029` (retune at 2013-2014, reinit at 2023) | `common_vars.freq_offset` is in `PHY_VARS_NR_UE` (`defs_nr_UE.h:183`, `int32_t freq_offset` inside the `common_vars` sub-struct); the request/value pair themselves are not | **This is the literal hardware frequency move P05's rule ("No branch-local correction may move the common hardware frequency") is written against.** `nrue_ru_set_freq()` retunes the shared RF chain; `nrue_ru_reinit()` tears down and reopens the device. `nr_rx_branch_sync_apply_cfo()` (this task) is a digital-only correction and must never be wired to trigger this path — see that function's header comment. |
| `s_reinits`, `s_reinit_cap` gating `nrue_ru_reinit()` from the RFSTALL watchdog | `nr-ue.c:1947-1948` (decl, per P03 audit), call site inside the watchdog block around `nr-ue.c:1913-1984` | No — `UE_thread()`-local `static` | Same device-restart trigger as above, reached via a different caller (stall detection instead of CFO trim). |
| `openair0_cfg_g[MAX_CARDS]`, `openair0_dev[MAX_CARDS]`, `nrue_rus`, `nrue_ru_count` | `nr-ue-ru.c:88-94` | N/A (file-scope in `nr-ue-ru.c`) | The literal UHD device config/handles and per-RU (physical-channel) config, indexed by `ru_id`/`rf_map.card`, never by logical branch. |

## ReceiverBranch — statements that must become one instance per branch

| Statement | File:line | New `nr_rx_branch_sync_t` field | Lives in `PHY_VARS_NR_UE`? | Notes |
|---|---|---|---|---|
| `shiftForNextFrame` — the per-loop incremental frame-boundary timing correction, applied to `readBlockSize` and to `UE->timing_advance_ntn` | decl `nr-ue.c:1046`; recomputed at `1236`, `1386`, `1654-1656`; consumed at `1379-1387` (`iq_shift_to_apply`), `1450` (`readBlockSize`) | `shift_for_next_frame` | No — `UE_thread()`-local `int` | Directly named in the P05 brief. Becomes per-branch once each branch's own timing loop can diverge from the others'. |
| `UE->max_pos_acc` — the timing-offset accumulated-error integrator that `shiftForNextFrame` is derived from (`shiftForNextFrame = -round(UE->max_pos_acc * time_sync_I)`), and `UE->max_pos_iir` (the IIR filter feeding it) | `defs_nr_UE.h:381-382` (decl); read/written throughout `nr-ue.c` e.g. `1027, 1116, 1163, 1236, 1319, 1386, 1497, 1690, 1770, 1919, 1981, 2031` | `timing_offset_samples` (the brief's struct does not carry a separate IIR-filter field; the wiring step will need to decide whether `max_pos_iir` becomes a second per-branch field alongside `timing_offset_samples` or is folded into it — flagged here, not decided by this task, since it is a real design choice the wiring step owns, not a "read the code and report" fact) | **Yes** — `PHY_VARS_NR_UE.max_pos_acc`/`max_pos_iir` | The clearest single instance of "reuse `PHY_VARS_NR_UE` where appropriate needs one instance per branch": this integrator is read and written from a dozen+ call sites across the loop, all assuming a single `UE`. |
| `UE->is_synchronized` — branch sync flag; the REACTION side (see the AcquisitionOwner table for the detection side) | `defs_nr_UE.h:297` (decl); set/cleared at `nr-ue.c:1015, 1052, 1066, 1089(via stream_status), 1111, 1494-1496, 1979, 2029, 2077-2078` (representative, not exhaustive — over a dozen sites) | `synchronized` | **Yes** — `PHY_VARS_NR_UE.is_synchronized` | **The one symbol the P03 audit flags as genuinely split across both buckets** (audit line 53): its DETECTION trigger (`rx_continuity`'s timestamp check) is AcquisitionOwner-level, but its own value and every one of its dozen+ reaction sites (clearing `max_pos_acc`/`shiftForNextFrame`, resetting `decoded_frame_rx`, invalidating the rebase flag) are branch-owned digital state. `nr_rx_branch_sync_t.synchronized` is the branch-owned half; the wiring step's job is to make the AcquisitionOwner's discontinuity detection *notify* each branch (so each branch clears its OWN `synchronized`) rather than writing one shared `UE->is_synchronized` directly. |
| CFO estimation/compensation fields: `UE->common_vars.freq_offset` (currently-compensated DL frequency offset, DIGITAL compensation applied to received samples — distinct from the hardware retune value `nr_ue_cfo_resync_hz` in the AcquisitionOwner table above), `UE->freq_off_acc` (accumulated DL frequency error for the PI controller), `UE->initial_fo`, `UE->cont_fo_comp` | `defs_nr_UE.h:183` (`common_vars.freq_offset`), `386-387` (`freq_offset`/`freq_off_acc` — note `defs_nr_UE.h` has TWO distinct `freq_offset` fields, one in `common_vars` at line 183 and one top-level at line 386; the wiring step must confirm which one is live on the digital-compensation path vs. which mirrors the hardware-applied value before choosing what `cfo_hz`/`cfo_accum_hz` replace) | `cfo_hz` (latest correction), `cfo_accum_hz` (running total) | **Yes** — both `freq_offset` fields and `freq_off_acc` are in `PHY_VARS_NR_UE` | **Caveat, stated plainly rather than glossed over**: `UE_thread()`'s per-slot loop body does not itself read/write `common_vars.freq_offset`/`freq_off_acc` in the statements read for this plan (the loop's own CFO-adjacent writes are the retune-request consumption at `2013-2014`, which is AcquisitionOwner's, not a per-slot digital-compensation statement). The digital CFO *estimation/compensation* loop lives downstream in PBCH/PDCCH processing (`phy_procedures_nr_ue.c`, outside this task's read — see "Not read for this plan" below), not in `UE_thread()`'s own per-slot statements. Listed here because the brief names "CFO compensation fields" explicitly and P03's audit independently flags `nr_pdsch_passive_decode.c:299`'s `g_sfo_ppm_ema` as the same class of per-branch sync state; the exact read/write sites belong to a wiring pass over `phy_procedures_nr_ue.c`, out of this task's scope (`nr-ue.c`/`nr-ue-ru.c` only, per the brief). |
| `decoded_frame_rx`, `trashed_frames` — frame-tracking state reset alongside `is_synchronized` on every loss-of-sync path | decl `nr-ue.c:1040` (same statement as `absolute_slot`'s decl above); reset at `1501-1502` (RXDISCONT path), `1985-1986` (watchdog path), `2035-2036` (CFOTRK path) | Not directly represented by a listed brief field; closest existing field is `last_absolute_slot`/`frame_wraps` (slot-continuity bookkeeping), though `decoded_frame_rx` is SFN-domain, not absolute-slot-domain — flagged as a possible additional field the wiring step may need, not assumed to already be covered. | No — `UE_thread()`-local | Reset together with `is_synchronized` at all three loss-of-sync sites; a branch that reacquires independently needs its own `decoded_frame_rx`/`trashed_frames`, not a shared one. |
| `stream_status` (`STREAM_STATUS_UNSYNC`/`SYNCING`/`SYNCED`) | decl `nr-ue.c:1006`; read/written throughout, e.g. `1089, 1144-1145, 1496, 1980, 2030, 2069, 2077-2078` | Closest to `synchronized`, but is a 3-state enum vs. the brief's `uint8_t synchronized` 2-state flag — the wiring step must decide whether `STREAM_STATUS_SYNCING` collapses into `synchronized=0` or needs a field this task does not provide. Flagged, not resolved. | No — `UE_thread()`-local `enum` | Same branch-local lifecycle class as `is_synchronized`; kept as a separate row because it is a genuinely different variable with different states, not a duplicate mention of the row above. |
| `absolute_slot` itself, and `slot_nr = absolute_slot % nb_slot_frame` | decl `nr-ue.c:1040` (`int absolute_slot = 0, ...`; `nb_slot_frame` decl `1039`), increment `1297`, per-slot use throughout (e.g. `1304-1305`) | `last_absolute_slot` via `nr_rx_branch_sync_on_slot(s, absolute_slot, nb_slot_frame)` | No — `UE_thread()`-local | **This is the concrete mapping `nr_rx_branch_sync_on_slot()`'s header comment defers to this document**: today `absolute_slot` is a single ever-increasing counter shared by the one loop; a per-branch design gives each branch's own `nr_rx_branch_sync_t` its own `last_absolute_slot`, fed the SAME `absolute_slot` value if all branches share one physical sample clock (the common case), or a branch-local counter if a future design lets branches free-run at different rates. The out-of-order-job rejection path (`on_slot()` returning -1) is aimed at a *worker* consuming a branch's jobs out of sequence (memory: "unsigned slot delta breaks concurrent CPI"), not at this producer-side counter itself, which is monotonic by construction in the current single-loop code. |

## Cross-cutting notes

1. **The one genuinely dual-natured symbol (`is_synchronized`/`rx_continuity` pairing) is the crux
   of the eventual wiring design**: the AcquisitionOwner-side detector must become a *notifier*
   that every active branch (not a single `UE->is_synchronized`) reacts to independently, since
   P05 requires "Branch loss of lock increments its lock epoch, clears its stale grants/CPI
   history and reacquires locally" — i.e. per-branch, not one shared flag for all branches. This
   is exactly what `nr_rx_branch_lose_lock()` (P03) + `nr_rx_branch_sync_reset()` (this task)
   together are for, once wired: the AcquisitionOwner calls `nr_rx_branch_set_rf_discontinuity()`
   once (common-mode, all active branches), and each branch INDEPENDENTLY decides whether to also
   call `nr_rx_branch_lose_lock()` + `nr_rx_branch_sync_reset()` on itself.
2. **Every field flagged "Yes" in the `PHY_VARS_NR_UE`? column needs one `PHY_VARS_NR_UE` instance
   per branch** for a genuinely independent per-branch design (`max_pos_acc`/`max_pos_iir`,
   `is_synchronized`, `common_vars.freq_offset`, `freq_off_acc`) — this is the same conclusion
   P03's audit reached independently ("Reusing `PHY_VARS_NR_UE` per branch... means this array is
   naturally what becomes 4 branch-owned... instances") and this plan does not revisit that design
   choice, only confirms which specific fields the per-slot loop actually touches.
3. **Not read for this plan** (out of the brief's `nr-ue.c`/`nr-ue-ru.c` scope, flagged so the next
   wiring pass knows where to look, not claimed as covered here): the CFO trim loop's own
   estimation logic and `g_sfo_ppm_ema` live in `phy_procedures_nr_ue.c`/
   `nr_pdsch_passive_decode.c` (P03 audit lines 37, 170); the PDCCH/PUSCH passive-queue globals
   (P03 audit's `nr_pdcch_passive_queue.c` etc. tables) are separate per-branch work items the plan
   assigns to later tasks (P06+), not P05.
4. **Confirms rather than duplicates the P03 audit**: every H/B classification in the tables above
   agrees with `docs/passive_branch_globals_audit.md`'s classification of the same underlying
   fields where both documents cover the same symbol (`shiftForNextFrame`, `is_synchronized`/
   `rx_continuity`, `s_rxts_*`, the rebase-epoch mechanism, `s_reinits`). This plan adds the
   STATEMENT-level file:line detail (where in the per-slot loop each read/write actually happens)
   that a wiring pass needs and the audit — a globals inventory, not a control-flow trace —
   deliberately did not provide.

## What this plan does NOT do

Per the brief's controller scope ruling: it does not edit `nr-ue.c`/`nr-ue-ru.c`, does not
introduce a per-branch `PHY_VARS_NR_UE` array, and does not change how `UE->is_synchronized`/
`UE->max_pos_acc`/`stream_status` are read or written today. G1 tests 5 ("compare branch
acquisition and digital correction against standalone replay") and the full G1 exit criterion
cannot pass until that wiring lands — see the P05 ledger entry.
