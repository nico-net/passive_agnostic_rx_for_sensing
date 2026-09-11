# Passive DL decode chain -- antenna-indexing audit and single-branch view mechanism (P07)

Plan: `adaptive_RX_pipeline.md` (`adaptive-rx-sensing`, `merge/adaptive-sensing`, inspected at
`d1b563f511`). Task: P07 (Stage 2, `adaptive_RX_pipeline.md` sec 4). Written BEFORE the code
change it recommends; line numbers are as of the commit above (pre-change). Every citation was
read in this session, not copied from `docs/passive_branch_globals_audit.md` (P03), which covers
the same files' GLOBALS; this document covers the ANTENNA AXIS of every call in the chain.

Chain audited: `nr_pdsch_passive_queue.c` consumer -> `nr_pdsch_passive_decode()` -> FEP ->
channel estimation -> `nr_rx_pdsch()` (incl. `nr_dlsch_extract_rbs()`) -> LLR/descramble ->
`passive_ldpc_decode()` -> `nr_isac_pdsch_data_aided_submit()`. The replay entry
(`nr_passive_replay_capture.c:285`) calls the same `nr_pdsch_passive_decode()` directly and is
therefore a second caller of the chain, not a separate chain.

## 1. Where the antenna dimension enters, per function

Legend for "antenna source": **fp(ue)** = `ue->frame_parms.nb_antennas_rx` read INSIDE the callee
from the `ue` argument; **fp(arg)** = an explicit `NR_DL_FRAME_PARMS *` argument; **explicit** =
an integer antenna-count/index argument.

| Function | File:line | Takes `ue`? | Antenna source | Per-antenna buffers touched |
|---|---|---|---|---|
| queue consumer `nr_pdsch_passive_queue_thread()` | `nr_pdsch_passive_queue.c:102-108, 150, 165, 190` | yes (`g_ue`, one process-wide `PHY_VARS_NR_UE*`) | fp(ue) only to size `rxdataF_sz`; the per-consumer scratch `g_rxdataF[idx]` (`:95`) is allocated for `nb_antennas_rx` rows | passes the WHOLE per-antenna `rxdataF` to decode and to the data-aided submit |
| `nr_pdsch_passive_decode()` scope guard | `nr_pdsch_passive_decode.c:920` | yes | fp(ue): `n_ports > fp->nb_antennas_rx` rejects a grant with more DM-RS ports (layers) than receive antennas | -- |
| FEP, multi-antenna path | `:1047-1066` | yes | fp(ue): one `nr_slot_fep_ant_snapshot()` task per `ant < fp->nb_antennas_rx` | reads `ue->common_vars.rxdata[ant]` (`:1060`), writes `rxdataF[ant]` |
| FEP, single-antenna path | `:1067-1069` -> `nr_slot_fep()` `slot_fep_nr.c:102-181` | yes (`is_synchronized`, `cont_fo_comp`, `phy_cpu_stats`) | fp(arg): `frame_parms->nb_antennas_rx` loop at `slot_fep_nr.c:149-163` | `rxdata[aa]` / `rxdataF[aa]` for every `aa`; common FO only (`:65-67`), NO per-branch table |
| `nr_slot_fep_ant()` | `slot_fep_nr.c:213-300` | yes | explicit `ant` + fp(arg) assert (`:227`) | `rxdata[ant]` -> `rxdataF[ant]`; applies `common_fo + nr_ue_get_branch_fo_hz(ant)` (`:52-58`) -- the ONLY consumer of the process-wide `g_branch_fo_hz[]` table (`:60-97`) |
| `nr_pdsch_channel_estimation()` | `nr_dl_channel_estimation.c:1315`; per-antenna tasks `:1383-1429`, serial fallback `:1419-1464` | yes | **fp(ue)**: `fp = &ue->frame_parms` (`:1330`), loops `aarx < fp->nb_antennas_rx` | reads `rxdataF[aarx]`, writes `dl_ch_estimates[nl*fp->nb_antennas_rx + aarx]` (`:1290, 1429`) and TLS `nr_dl_chest_nvar_ant[aarx]` (`:1310, 1476`); returns the cross-antenna MEAN nvar |
| nvar normalisation | `nr_pdsch_passive_decode.c:1153-1161` | -- | fp(ue): divisor includes `fp->nb_antennas_rx` (default `ISAC_RX_NVAR_FIX=0`) | -- |
| per-branch nvar substitution (`ISAC_RX_NVAR_PERBRANCH`, default 1) | `:1172-1197` | -- | `nr_dlsch_planned_branch(fp->nb_antennas_rx, Nl)` (`nr_dlsch_demodulation.c:77-110`): engages ONLY at `nbRx == 4 && nl == 1`; returns -1 (keep the mean) otherwise | reads `nr_dl_chest_nvar_ant[dl_branch]` |
| DMRSFO tracker (`ISAC_DMRS_FO_APPLY`) | `:1256-1366` | -- | estimates from `pdsch_dl_ch_estimates[0]` (antenna 0 of whatever the chain sees); apply loop `:1353` writes `nr_ue_set_branch_fo_hz(a, ...)` for `a < fp->nb_antennas_rx` | process-wide statics `s_cfo_ema`/`s_sfo_ema`/`g_sfo_ppm_ema` (`:1258, :1338`) -- one EMA for the whole process, i.e. CROSS-BRANCH state in independent mode |
| BRANCHFO differential estimator (`ISAC_RX_BRANCH_FO`) | `:1368-1403` | -- | gated `fp->nb_antennas_rx > 1`; loops `a < fp->nb_antennas_rx`; writes `nr_ue_set_branch_fo_hz(a, ...)` when the env is set (`:1391`) | `pdsch_dl_ch_estimates[a]`, static `s_fo_ema[]` |
| CHESTDIAG probe | `:1436-1520` | -- | fp(ue) loops (diagnostic only) | `pdsch_dl_ch_estimates[l*nb+a]` |
| `nr_chest_time_domain_avg()` | `:1564-1567` | -- | explicit `fp->nb_antennas_rx` argument | all `(layer, antenna)` slices |
| `nr_rx_pdsch()` | `nr_dlsch_demodulation.c:831-1530`; called `nr_pdsch_passive_decode.c:1712-1716` | yes (`fp = &ue->frame_parms` `:858`, `do_ml`, `chest_time`, `phy_cpu_stats`, `phy_sim_*`) | **both**: explicit `nbRx` argument (`:847`, used for `matrixSz`, `rxdataF_ext[nbRx]`, `log2_maxh`, the MRC/selection state at `:860-1160`) AND fp(ue) inside `nr_dlsch_extract_rbs()` (`:238`, loop `:299`, index `(l * fp->nb_antennas_rx) + aarx` `:303-304`). The two MUST agree -- passing `nbRx = 1` with the real 4-antenna `fp` would extract 4 rows into a 1-row `rxdataF_ext`. | `rxdataF[aarx]`, `dl_ch_estimates[(l*nb)+aarx]`, `ptrs_phase_per_slot[nb][..]` (`nr_pdsch_passive_decode.c:1628-1630`) |
| MRC / selection diversity (`ISAC_RX_MRC_MODE`, default 0 = branch 0 only) | `nr_dlsch_demodulation.c:860-1160`, decision at `:1078-1160` | -- | engages ONLY at `nl == 1 && nbRx == 4` (`:904, :1078`); `nbRx == 1` sets `t_mrc_nb_rx = 1, t_mrc_rx_index = 0` (`:904-906`) with no mode logic at all | -- |
| forced branch / mask (`nr_dlsch_force_branch`, `nr_dlsch_force_mask`, `nr_dlsch_last_branch`) | `nr_dlsch_demodulation.c:37-60`; consumed `:1099-1160` | -- | TLS pins, honoured only inside the `nbRx == 4` block | -- |
| selection-diversity RETRY (`g_branch_retry_enabled()`, `ISAC_RX_BRANCH_RETRY`, default ON) | `nr_pdsch_passive_decode.c:252-260`, loop `:1906-1956` | yes | gated `fp->nb_antennas_rx > 1 && cw->Nl == 1`; walks `b < fp->nb_antennas_rx`, pins `nr_dlsch_force_branch(b)`, re-runs `nr_rx_pdsch()` and `passive_ldpc_decode()` per branch | re-reads every antenna's `rxdataF`/estimates; nvar swapped per branch from `nr_dl_chest_nvar_ant[b]` (`:1919-1925`) |
| subset scan (`ISAC_SUBSET_SCAN`, default off) | `:1962-2012` | yes | gated `fp->nb_antennas_rx == 4 && cw->Nl == 1` | all antennas via `nr_dlsch_force_mask()` |
| `passive_ldpc_decode()` | `:681-856` | yes (`nrLDPC_coding_interface` only) | none -- LLR domain, no antenna axis | -- |
| `nr_isac_pdsch_data_aided_submit()` | `nr_pdsch_data_aided.c:51`; antenna clamp `:191-197`; loop `:281-290` | yes (`fp`, `nrLDPC_coding_interface`) | fp(ue): `isac_nof_ant = min(nr_isac_aoa_antennas(), fp->nb_antennas_rx)` (0 -> 1) | reads `rxdataF[a]` for `a < isac_nof_ant`; `nr_isac_submit_cfr_multi(..., isac_nof_ant, ...)` |

Cross-branch state reached from this chain that is NOT on the antenna axis but is shared across
whatever the process decodes (already classed (B) in the P03 audit, listed here for completeness):
`g_sfo_ppm_ema` / DMRSFO EMAs (`nr_pdsch_passive_decode.c:299, 1258`), `g_branch_fo_hz[]`
(`slot_fep_nr.c:80`), the aggregate LDPC/LLR/shape counters (`:262-395`) and `nr_dlsch_forced_*`
TLS pins (`nr_dlsch_demodulation.c:37-45`, thread-local so per-consumer, not per-branch).

## 2. Where MRC / selection diversity / rescue live -- and what turns each off

| Mechanism | Env / default | Engagement condition | Off when the chain sees `nb_antennas_rx == 1`? |
|---|---|---|---|
| MRC / strongest-branch / branch-0 selection | `ISAC_RX_MRC_MODE` (default 0), `ISAC_RX_BRANCH_MIN_DB` | `nr_rx_pdsch()` `nl == 1 && nbRx == 4` | yes -- `:904-906` takes the `nbRx != 4` path unconditionally |
| forced branch / mask pins | `nr_dlsch_force_branch()/force_mask()` TLS | same block | yes (never reached); the retry that sets them is also off (next row) |
| selection-diversity retry ("rescue") | `ISAC_RX_BRANCH_RETRY` (default 1) | `fp->nb_antennas_rx > 1 && Nl == 1 && !ldpc_ok` | yes -- gate fails at 1 antenna |
| subset scan | `ISAC_SUBSET_SCAN` (default off) | `fp->nb_antennas_rx == 4` | yes |
| planned-branch nvar substitution | `ISAC_RX_NVAR_PERBRANCH` (default 1) | `nr_dlsch_planned_branch()` returns >= 0 only at `nbRx == 4` | yes -- the mean over ONE antenna is that antenna's own nvar, which is exactly "noise estimate from that channel only" |
| BRANCHFO differential estimate / apply | `ISAC_RX_BRANCH_FO` (default off) | `fp->nb_antennas_rx > 1` | yes |
| per-branch FO table in FEP | `nr_ue_get_branch_fo_hz(ant)` in `nr_slot_fep_ant()` | only the multi-antenna FEP path calls `nr_slot_fep_ant()`; the 1-antenna path calls `nr_slot_fep()`, which reads no table | yes |
| DMRSFO CFO/SFO EMA + apply | `ISAC_DMRS_FO_APPLY` (default off), `ISAC_SFO_CORRECT` | unconditional (`dmrs_first >= 0`) | **NO** -- the EMA is process-wide and the apply loop would write `g_branch_fo_hz[0]` from whichever branch happened to decode. This one needs an explicit gate in view mode (see sec 4). |

## 3. Mechanism options for the single-branch view

Required property: the decode chain sees `nb_antennas_rx == 1` and its antenna 0 IS the branch's
`physical_channel`, from FEP through the data-aided submit, with one consistent value.

**(a) Shadow `NR_DL_FRAME_PARMS` + `rxdata` pointer array threaded through the entry points.**
Fails as literally stated: only `nr_slot_fep()` takes `fp` as an argument. `nr_pdsch_channel_
estimation()` (`:1330`), `nr_rx_pdsch()`/`nr_dlsch_extract_rbs()` (`:858`, `:299`) and
`nr_isac_pdsch_data_aided_submit()` all take `ue` and read `ue->frame_parms` internally, and
`nr_rx_pdsch()` must additionally receive an `nbRx` argument that agrees with that `fp`. A shadow
`fp` can therefore only be delivered INSIDE a shadow `ue`. Refined form, **(a')**: a thread-local
shallow copy of `PHY_VARS_NR_UE` (`sizeof == 5,661,504` B measured with gdb on the current binary;
`NR_DL_FRAME_PARMS` alone is 1,400,752 B) with `frame_parms.nb_antennas_rx = 1` and
`common_vars.rxdata` pointed at a 1-entry array `{ue->common_vars.rxdata[phys]}`. Every pointer
field aliases the real buffers (LDPC coding interface, thread pool, `rxdata` rows, `phy_sim_*`),
so nothing is duplicated except the scalars. Nothing on this chain WRITES to `ue` except the
`phy_cpu_stats` timers (verified by grepping `ue->` in all five files; the only writes are
`start/stop_meas_nr_ue_phy` and `phy_sim_*` copies that are NULL here), so writes landing in the
copy lose nothing. Cost: one 5.6 MB memcpy per consumer thread at first use (NOT per job -- at
~0.5 ms it would roughly double the 775 us/job budget `nr_pdsch_passive_queue.h:27` records),
then per job a refresh of the handful of mutable scalars the chain reads (`is_synchronized`,
`cont_fo_comp`, `dl_Doppler_shift`, `freq_offset`, `common_vars.freq_offset`, `chest_freq`,
`chest_time`, `do_ml`) plus a cheap staleness check on the frame-parms fields the chain indexes by
(`nb_antennas_rx`, `ofdm_symbol_size`, `samples_per_slot_wCP`, `N_RB_DL`, `first_carrier_offset`).
Diff: one helper (~40 lines) in `nr_pdsch_passive_decode.c`, two call sites (consumer, replay),
ZERO changes inside FEP/chest/demod/data-aided. Every mechanism in sec 2 switches itself off by
its own existing `nb_antennas_rx`/`nbRx` gate, so "MRC off, retry off, rescue off, BRANCH_FO off,
own-branch nvar only" is obtained by construction rather than by five new conditionals; only the
DMRSFO EMA/apply needs one explicit gate.

**(b) `rx_ant_mask`/`branch_phys` on `nr_pdsch_passive_job_t`, honoured at every antenna loop.**
Requires editing every row of the table in sec 1 that says fp(ue): `nr_pdsch_channel_estimation()`
(both the task and the serial path), `nr_dlsch_extract_rbs()`, `nr_rx_pdsch()`'s `rxdataF_ext`
sizing and MRC block, `nr_chest_time_domain_avg()`, the data-aided loop, and the six loops in
`nr_pdsch_passive_decode.c` -- plus threading the job's field down through three function
signatures that today take only `ue`. That is a cross-file signature change on shared OAI PHY
code (`nr_dl_channel_estimation.c`, `nr_dlsch_demodulation.c`) that the attached-UE path also
calls, with a real chance of silently disagreeing with `nbRx` somewhere. Larger, less local, and
no more correct.

**Decision: (a').** Smallest and most local diff; the antenna count reaches every callee through
the one path they already read it from, so the view cannot be half-applied. Does NOT touch
`PHY_VARS_NR_UE`'s layout, `nr-ue.c`, or `nr-ue-ru.c` -- no conflict with the dirty files.

Why not even simpler -- "just pin `nr_dlsch_force_branch(phys)` and keep 4 antennas"? That gives a
selection-diversity decode, not an independent one: FEP/chest still run all four antennas, nvar is
still a cross-antenna mean unless the planned-branch substitution fires, the data-aided submit
would still read `rxdataF[0..3]`, and the BRANCHFO/DMRSFO apply loops still write the shared table.
It is the mechanism the plan's sec 2.3 retraction is written against, not the fix.

## 4. What the view mode must add explicitly (not obtained for free)

1. **Multilayer rejection with its own counter.** `n_ports > fp->nb_antennas_rx` (`:920`) already
   rejects `Nl > 1` at 1 antenna as `UNSUPPORTED`, but that is indistinguishable from PTRS/CSI-RM/
   TBS=0 rejections. Add `unsupported_multilayer_in_branch_view` (process-wide `_Atomic`), bumped
   only when the view is active, and a `reason` string on the result so the verdict trace names it.
2. **DMRSFO EMA/apply gate**: in view mode do not update `s_cfo_ema`/`s_sfo_ema`/`g_sfo_ppm_ema`
   and never call `nr_ue_set_branch_fo_hz()` (sec 2, last row). Consequence, stated: the
   `ISAC_SFO_CORRECT` stage reads `g_sfo_ppm_ema` and therefore stays at 0 in view mode -- the
   per-branch tracker is future work (P09's TLS/global audit), not silently shared.
3. **Job identity**: `uint8_t branch_id; int8_t physical_channel;` on `nr_pdsch_passive_job_t`.
   Placed in the 2-byte padding between `rnti` (offset 0x150) and `harq_pid_tag` (0x154), so
   `sizeof == 384` is unchanged and the P02 fixture's `job_bytes == sizeof` header check still
   passes. The recorded fixture jobs were built by field assignment from an uninitialised stack
   struct (`nr_pdcch_blind_monitor_rt.c:2159, 2340`), so those two bytes are GARBAGE in the
   fixture -- the replay must overwrite them from its own view resolution, never read them.
4. **Data-aided submit carries the identity?** `nr_isac_submit_cfr_multi()` (`nr_isac.h:72`) has
   no branch/rx_id parameter; the engine is a singleton keyed only on `source`. **P10 requirement,
   recorded here**: the CFR ABI needs `(branch_id, physical_channel)` (or the P03 `rx_id`) on every
   submission. Until then the identity stops at the job/consumer (`TODO(P10)` in the code), and a
   multi-branch live run would fold every branch's CFR into one engine -- which is why independent
   mode is only exercised through the replay in this task.
5. **Live independent mode before P06**: the producer (`nr_pdcch_blind_monitor_rt.c`) tags every
   job `0/0` (this task zero-initialises the two new fields there; P06 will assign real ones). With
   `n_active > 1`, an untagged job resolves to the branch whose `physical_channel` matches (i.e.
   phys 0 if active) or else the lowest-id active branch -- documented, counted, and visible in the
   trace, not hidden.

## 5. Verdict trace

No per-job outcome line existed (`TBRESULT` needs `ISAC_PDSCH_TBPARM=1` and carries no job index or
branch). Added: `ISAC_PDSCH_VERDICT_TRACE=1` -> one stdout line per DL job,
`PDSCH-VERDICT job=<idx> rnti=<hex> crc=<ok|fail|unsupported|error> reason=<...> Nl=<n>
branch=<b> phys=<p>`, printed by the two callers (consumer, replay) through one helper. Off by
default; the legacy replay's verdict lines and `REPLAY PASS: identical DL controls=34 failed=0 raw
UL=24; no radio opened` are the bit-identity witnesses for the default path.
