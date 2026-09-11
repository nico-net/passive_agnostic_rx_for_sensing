# Passive receiver: decoder / re-encoder namespace and TLS-reuse audit (P09)

adaptive_RX_pipeline.md Stage 2, gate G2 ("no resource namespace collision").
Date 2026-09-11 (Europe/Zurich). Tree `/home/sens/NICOLA/adaptive-rx-sensing`, branch
`merge/adaptive-sensing`, audited at HEAD `2566d256d2`. Offline audit: no radio (X410 unreachable),
so every claim below is code reading plus the P02 replay fixture and the offline gtest suites.

Scope, per the P09 brief: the identifiers handed to the shared `nrLDPC_coding_interface`, and the
thread-local storage in the files that feed it. One collision is FIXED here (DL decode
`harq_unique_pid`, section 3); the others are RECORDED, not fixed, with the reason each is out of
this task's proven scope.

---

## 1. Why the identifier matters

`nrLDPC_TB_decoding_parameters_t::harq_unique_pid` is not a label. On the AAL/bbdev backend,
`nrLDPC_coding_aal.c:654` and `:742` compute

```
segment_offset        = harq_unique_pid * NR_LDPC_MAX_NUM_CB + i
pruned_segment_offset = segment_offset % active_dev.num_harq_codeblock
```

and use it to address the device's HARQ-combined buffers. Two transport blocks in flight at the
same moment under the same id therefore read and write each other's soft bits.

Two honest qualifications, because the rest of this document rests on them:

* **The hazard is on the DECODE side.** The encoding parameter struct carries the same field
  (`nrLDPC_coding_interface.h:47`) but no backend in this tree indexes a buffer by it. The
  re-encode paths (1000/3000/4000 below) keep disjoint ids for hygiene and for backends that may
  key on it, not because a measured corruption exists today.
* **Disjointness is necessary, not sufficient, on an AAL device.** The `% num_harq_codeblock`
  prune can fold two disjoint ids onto the same buffer if the device's codeblock count is small.
  Keeping ids disjoint removes the collisions this software controls; it does not remove that one.
* The soft-decoder backend used by every measurement in this tree (`nrLDPC_coding_segment`) keeps
  no per-id state, which is precisely why the collision fixed in section 3 is invisible in replay.

---

## 2. Identifier assignment sites

Every `harq_unique_pid` write and every tag-producing helper, from
`grep -rn "harq_unique_pid\|TAG_BASE\|harq_pid_tag" openair1/`. "Unique across" is judged over the
four axes the brief names: **branch x direction x decode-vs-reconstruction x UE/session**.

| # | Site | Expression | Range | Unique? | Collision scenario if NO |
|---|------|-----------|-------|---------|--------------------------|
| 1 | `nr_pdsch_passive_decode.c:712` (was `:703`) | `nr_pdsch_passive_harq_tag(t_view_branch, harq_process_nbr)` = `2000 + branch*16 + hpn%16` | 2000-2063 | **YES (after this task)** | Was `2000 + harq_process_nbr`: see section 3. FIXED. |
| 2 | `nr_pdcch_blind_monitor_rt.c:110` `blind_harq_tag()` | `3000 + ue_slot*16 + harq_pid%16` | 3000-3255 | **NO (branch axis)** | Passive DL *re-encode*. Namespaced by UE and by type, not by branch. P06a fans one occasion to N branches; each branch's job carries the same `(rnti, harq_pid)` and therefore the same tag. Recorded, not fixed: `nr_isac_pdsch_data_aided_submit()` reaches `nr_pdsch_data_aided.c:128`, which is an **encode** call, and per section 1 no backend in this tree indexes state by an encode-side id. Fixing it also requires the P10 CFR-ABI change (`nr_pdsch_passive_queue.c:231` already carries a `TODO(P10)`), so it is deliberately left to P10 rather than half-done here. |
| 3 | `phy_procedures_nr_ue.c:2137` | `1000 + dlsch_config->harq_process_nbr` | 1000-1015 | YES | Attached-UE DL re-encode. One session, one HARQ array, no branch fan-out on the attached path. Out of scope per the brief. |
| 4 | `nr_dlsch_decoding.c:76` | `2 * harq_pid + cw_idx` | 0-31 | YES for its own users | Attached-UE DL decode (upstream OAI). Collides with sites 5 and 6 below, which is the finding in section 4.1. |
| 5 | `nr_ulsch_decoding.c:129` | `= ULSCH_id` | 0-5 as called | **NO** | Passive UL decode. See section 4.1 -- a real present-day collision, recorded not fixed (UL is P08). |
| 6 | `nr_dlsch_coding.c:171` / `nr_ulsch_coding.c:137` | `= i` / `2*harq_pid + ULSCH_id` | small | n/a | Upstream OAI encode paths, unreachable under `--passive-rx`. Listed for completeness. |
| 7 | `nr_pusch_passive_decode.c:1278` | `NR_PUSCH_PASSIVE_DA_TAG_BASE + ctx` | 4000-4005 | YES on its own axes | Passive UL re-encode, strided by decode-context index. No UL branch fan-out exists yet (P08). |
| 8 | `nr_pusch_data_aided.c:89` | `TB.harq_unique_pid = harq_pid_tag` | pass-through | n/a | Consumes site 7's value. |
| 9 | `nr_pdsch_data_aided.c:128` | `TB_parameters.harq_unique_pid = harq_pid_tag` | pass-through | n/a | Consumes site 2's value (via `nr_pdsch_passive_job_t::harq_pid_tag`). |
| 10 | `nr_pusch_passive_decode.c:82` `PASSIVE_UL_HARQ_TAG_BASE 4000` | -- | -- | **DEAD** | Defined, documented as the UL decode namespace, and **never used** anywhere. The UL decode's real id is site 5. See 4.1. |

The base map, as it now stands, is centralised in `nr_passive_harq_tag.h`'s header comment: 0-31
attached DL decode, 1000+ attached DL re-encode, 2000+ passive DL decode, 3000+ passive DL
re-encode, 4000+ passive UL re-encode. Bases are 1000 apart; that spacing is now a named constant
(`NR_PASSIVE_HARQ_NAMESPACE_SPAN`) with a `static_assert` against it, instead of four comments in
four `.c` files -- two of which had already drifted into declaring the same base (sites 7 and 10).

---

## 3. The collision fixed here

**Before.** `nr_pdsch_passive_decode.c:703` computed `NR_PDSCH_PASSIVE_HARQ_TAG_BASE +
dlsch_config->harq_process_nbr`. That is namespaced by submitter TYPE only.

P07 gave every DL job a `branch_id`/`physical_channel` (`nr_pdsch_passive_queue.h:104-107`) and
P06a's fan-out (`nr_pdsch_passive_queue_enqueue_fanout()`, called at
`nr_pdcch_blind_monitor_rt.c:2367`) enqueues one occasion's grant to every active branch. Each
branch decodes its own grant with its own independently numbered DCI HARQ process field, and
several queue consumers run concurrently (`NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS`). So branch 0's
and branch 1's job on the same occasion produced the **identical** id. This is the common case, not
a corner case: the fan-out copies the same `dlsch_pdu` to every branch, so the HARQ process numbers
are not merely able to coincide, they are equal by construction.

**After.**

```c
harq_unique_pid = NR_PDSCH_PASSIVE_HARQ_TAG_BASE
                + (branch_id % NR_RX_BRANCH_MAX) * NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE
                + (harq_process_nbr % NR_PDSCH_PASSIVE_HARQ_BRANCH_STRIDE);
```

**Stride bound, as arithmetic.** The stride is 16 because that is the full span of the DCI
HARQ-process-number field (4 bits, TS 38.212 7.3.1.2.1) -- anything smaller would alias two HARQ
processes of the SAME branch, which is the collision being removed, only one axis over.

```
highest id = BASE + (NR_RX_BRANCH_MAX - 1) * STRIDE + (STRIDE - 1)
           = 2000 + (4 - 1) * 16 + 15
           = 2000 + 48 + 15
           = 2063
next type's base = BASE + NR_PASSIVE_HARQ_NAMESPACE_SPAN = 3000
2063 < 3000, headroom 937 ids.
```

The bound is a `static_assert` in `nr_passive_harq_tag.h`, so raising `NR_RX_BRANCH_MAX` past 62
(or the stride past 250) fails the build rather than silently spilling into the 3000 re-encode
range. It is also restated at runtime in `PassiveHarqTag.StrideBoundHoldsForTheConfiguredBranchCount`
so the suite reports it, not only the compiler.

Both arguments are reduced modulo their declared ranges rather than trusted: `harq_process_nbr`
arrives from a blindly decoded DCI and `branch_id` from a producer-filled job, so a malformed value
must stay inside this submitter type's namespace (where it can at worst alias another passive DL
decode) rather than reach the 3000 range (where it would alias a different submitter). This mirrors
`blind_harq_tag()`'s existing `harq_pid % 16`.

**branch_id = 0 reproduces the pre-P09 value exactly** (`2000 + hpn`), which is why the legacy
replay is byte-identical; it is pinned by `PassiveHarqTag.BranchZeroReproducesTheLegacyTag`.

**Where the branch identity comes from.** `t_view_branch`, the thread-local set by
`nr_pdsch_passive_branch_view()` immediately before each decode (`nr_pdsch_passive_queue.c:192`,
`nr_passive_replay_capture.c:271`). Its declaration moved up in the file so
`passive_ldpc_decode()` can read it; nothing else changed about it. No new field was added to
`nr_pdsch_passive_job_t` -- `sizeof` stays 384, so the P02 fixture's `job_bytes` header check is
untouched.

---

## 4. Findings recorded, not fixed

### 4.1 The passive UL decode tags every transport block `0` (Important)

`nr_pusch_passive_decode.c:1072` declares `int ulsch_id = 0;` and passes it to
`nr_ulsch_decoding()`, which does `TB_parameters->harq_unique_pid = ULSCH_id`
(`nr_ulsch_decoding.c:129`). So **every** passive UL decode is tagged `0`, in every one of the up
to `NR_PUSCH_PASSIVE_MAX_CTX` = 6 concurrent decode contexts, and `0` is also what
`nr_dlsch_decoding.c:76` emits for attached `harq_pid=0, cw_idx=0`. The file's own
`PASSIVE_UL_HARQ_TAG_BASE 4000` (site 10) was written to prevent exactly this and is never
referenced -- the intent is documented but not wired.

Not fixed here for two reasons, both from the brief: the UL path has no branch fan-out yet (P08),
and the fix is not the one-line change it looks like -- `ulsch_id` also indexes
`gnb->ulsch[ulsch_id]` inside `nr_ulsch_decoding()`, so it cannot simply be set to a namespaced
constant; the id would have to be overridden after the fact or the callee taught to separate the
two roles. That is a change to UL decode plumbing with no offline test to prove it, i.e. outside
this task's proven scope. **Recommended for P08.**

### 4.2 The passive DL re-encode (3000 range) is not branch-strided (Minor today)

Site 2 above. Left for P10 with the CFR-ABI change it belongs to; see the table entry for the
reasoning and for why it is currently harmless in this tree.

---

## 5. Thread-local storage audit

`grep -n "__thread"` over the seven files the brief names, plus the two queue files that feed
them. The question asked of each: **does its content depend on which JOB (branch/session) is being
processed, or is it pure thread-scratch that the current job fully overwrites before reading?**

Pure thread-scratch is safe to reuse across jobs precisely because every read is preceded by a
write from the same job. The dangerous shape is a TLS whose content depends on branch identity but
which is written on one job and read on another.

### 5.1 Branch-dependent, and correctly scoped (the positive example)

| Site | What | Verdict |
|------|------|---------|
| `nr_pdsch_passive_decode.c:206` (moved from `:897`) `t_view_branch` | P06a's branch identity for the armed view | **CORRECT.** Written by `nr_pdsch_passive_branch_view()` at the top of the function, unconditionally, on every path including the legacy `phys < 0` early return -- so it can never retain a previous job's branch. This is the pattern the rest of the table is measured against, and it is what P09's tag now reads. |
| `:894-896` `t_view_ue`, `t_view_rxdata`, `t_view_phys` | the shadow UE and its single-antenna rxdata | **CORRECT.** Same function, same unconditional refresh; `t_view_phys` is re-armed or set to -1 every call, and the frame-parms staleness check re-copies on a cell change. |

One caller of `nr_pdsch_passive_decode()` does NOT arm a view: the in-line (non-deferred) RT path at
`nr_pdcch_blind_monitor_rt.c:2375`, which runs on the UE's receive thread. `t_view_branch` there is
its initialiser, 0 -- which is the correct answer, because that path is the `defer == false` branch
and fan-out happens only on the deferred one (`nr_pdsch_passive_queue_enqueue_fanout()`, `:2367`),
so it is single-branch by construction. It is correct today for a structural reason, not by
scoping: if a future change ever fanned out in-line, that path would need to arm a view like the
other two callers do. Stated here so the assumption is on the record rather than implicit.

### 5.2 Branch-dependent content, NOT branch-scoped -- but written-before-read within one job

| Site | What | Verdict |
|------|------|---------|
| `nr_pdsch_passive_decode.c:641` `g_harq` | private HARQ context (b/c/d buffers, `processedSegments`, abort flag) | **SAFE, by thread confinement not by scoping.** Its content is entirely branch-dependent (it holds one branch's soft bits), but a thread never has two decodes in flight, and `passive_ldpc_decode()` resets `processedSegments = 0` and re-fills every buffer before reading. `passive_harq_prepare()` reallocates only on an `n_rb_dl` change, which is cell-wide, not branch-wide. Note this is *why* the LDPC id matters: `g_harq` isolates the SOFTWARE state per thread; `harq_unique_pid` is what isolates the ACCELERATOR's state, and only the second one was missing. |
| `:349` `t_seg_K/F/C/Z`, `:354` `t_seg_E/R/lbrm/BG` | segmentation + rate-match parameters carried out of the decode for the census | **SAFE.** Written by `passive_ldpc_decode()` for the TB it just processed and read by its immediate caller on the same thread before the next decode. Branch-dependent content, single-job lifetime. |
| `:1765-1766` `llr`/`llr_cap`, `:1267` `toFree`, `:1794-1800` `toFree2..5` | LLR buffer and channel-estimate allocations | **SAFE.** Capacity-grown scratch; contents fully rewritten per job. Heap-backed with a TLS pointer deliberately (the AoA AVX-alignment fault recorded in `PASSIVE_RX_ONLY_HANDOVER.md`). |
| `nr_pdsch_data_aided.c:119-238` (`seg_storage`, `c_segs`, `coded_bits`, `segments`, `scrambled`, `mod_syms`, `isac_h`, `isac_k/l`, `sym_*`) | re-encode scratch | **SAFE.** Same shape: every element consumed is written from the current job's TB first. `isac_h` is grown by antenna count, which is a view property, but is rewritten per submission. |
| `nr_pusch_data_aided.c:81-158`, `nr_pusch_passive_decode.c:881-883`, `nr_pdcch_blind_monitor_rt.c:2239-2241` | the UL and PDCCH equivalents | **SAFE**, same argument. |

### 5.3 Cross-job state carried deliberately, with a snapshot/restore

| Site | What | Verdict |
|------|------|---------|
| `slot_fep_nr.c:64` `nr_slot_fep_fo_override_hz` | per-job FO sampled on the receive thread | **CORRECT, and it is a mutable-configuration snapshot in the G2 sense.** `nr_pdsch_passive_queue.c:183/204` saves the previous value, sets the job's own, and restores it -- and the job carries `fo_hz` BY VALUE from the producer rather than reading the live value, which is the point (`nr_pdsch_passive_queue.h:118-121`). |
| `nr_pdsch_data_aided.c:33` `nr_isac_abs_slot_override` | slow-time index for the CPI grid | **CORRECT.** Set to `job.absolute_slot` and cleared to 0 around the submit (`nr_pdsch_passive_queue.c:229/234`). Restored on the success path; the only path that sets it. |
| `nr_pdsch_data_aided.c:34` `nr_isac_data_aided_force` | force flag | Set and cleared by its own callers; not job-derived on the passive path. |

**Restore-on-error (G2's "restore reused-worker state even on errors"):** `nr_slot_fep_fo_override_hz`
is restored unconditionally after `nr_pdsch_passive_decode()` returns, including on the
ERROR/UNSUPPORTED statuses -- the restore is before the status is examined. `nr_isac_abs_slot_override`
is only ever set inside the CRC-OK branch and cleared on the next line, so there is no error path
between set and clear. Neither is an early-return hazard today. This holds by inspection of straight-line
code, not by a fault-injection test; it would stop holding if any `continue`/`goto` were added between
those pairs.

### 5.4 Not TLS, but in the same class

| Site | What | Verdict |
|------|------|---------|
| `nr_dl_channel_estimation.c:31` `nr_dl_chest_nvar_ant[]` (`extern __thread`, read at `nr_pdsch_passive_decode.c:1349/1661/2095`) | per-RX-antenna noise variance | **SAFE.** Indexed by PHYSICAL antenna, overwritten by the channel estimation of the current job, read in the same job. The file's own comment at `:1357` already states the approximation (it carries the most recent call's values). |
| `nr_pusch_passive_decode.c:89` `g_gnb[NR_PUSCH_PASSIVE_MAX_CTX]` | per-context `PHY_VARS_gNB` | **SAFE, and deliberately indexed rather than thread-local** -- its own comment records that TLS of this size produced the AoA alignment fault. |
| `nr_pdsch_passive_decode.c:1087` `g_view_unsupported_multilayer_br[]`; `nr_pdsch_passive_queue.c:87-89`, incremented at `:176/212/223`, `g_br_decoded[]`/`g_br_crc_ok[]`/`g_br_stale_epoch[]` | per-branch counters | **CORRECT.** `_Atomic`, indexed `branch_id & (NR_RX_BRANCH_MAX - 1)`, relaxed ordering. Counters only; no decode state. |

No TLS was found whose content depends on branch identity and which is read on a later job without
an intervening write from that job. The one that comes closest is `g_harq`, and it is protected by
thread confinement plus a full per-job rewrite, not by luck.

---

## 6. What was and was not validated end-to-end

Stated plainly, because the replay harness cannot exercise the failure this fix removes:

* **Validated by test:** the tag arithmetic, in isolation -- injectivity over the whole
  (branch x HARQ process) product, the branch-0 legacy pin, the out-of-range guard, and the stride
  bound. 24/24 in `nr_rx_branch_test` (19 before, 5 added).
* **Validated by replay:** that nothing else moved. Legacy replay `REPLAY PASS: identical DL
  controls=34 failed=0 raw UL=24; no radio opened`, exit 0. `replay_branch_view.sh` 34/34/31/34/34
  of 38 records, unchanged from P07's recorded baseline.
* **NOT validated end-to-end.** `replay_branch_view.sh` runs **one view per process**
  (`ISAC_DL_BRANCH_VIEW=<phys>`, a fresh `nr-uesoftmodem` per view), so at no point do two branches'
  jobs with the same `harq_process_nbr` exist concurrently in one process. The harness as it stands
  cannot produce the collision, therefore it cannot demonstrate its removal. The branch-view runs
  DO exercise non-zero `branch_id` (views 1/2/3 resolve `branch_id = phys` via
  `nr_pdsch_passive_branch_view_resolve()`'s no-branch-set path, so they ran on tags 2016+/2032+/2048+
  and produced payloads identical to the reference) -- that shows the new tag values are harmless,
  not that the collision is gone.
* **And it could not be observed even if it were reachable in replay**, because the backend linked
  in this tree (`nrLDPC_coding_segment`) keeps no per-`harq_unique_pid` state. The corruption needs
  an AAL/bbdev accelerator, which this tree has never run.

So: **this fix rests on code inspection plus an isolated arithmetic unit test.** It is not backed by
an observed failure before and an observed pass after. Section 1's citation of
`nrLDPC_coding_aal.c:654` is the mechanism argument; the hazard is real in the code, it is simply
not reachable by any instrument available offline.
