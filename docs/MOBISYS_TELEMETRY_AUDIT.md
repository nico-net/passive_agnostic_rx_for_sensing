# MobiSys telemetry audit: existing measurements, gaps, and proposed changes

Date: 2026-10-20. Branch claude/optimistic-euler-mpdyvh, based on adaptive-rx-UL-DL @ 3f0d921.

Status: read-only audit. Nothing was run on the DGX, sens6, or over the air. This file contains no measurement.

## 0. Scope, sources, and limits

- Sources (read in full, the only evidence for the facts below):
  - `/tmp/claude-0/-home-user-passive-agnostic-rx-for-sensing/3ba5e17c-523a-59e0-a1de-318fd90f06eb/scratchpad/audit_c.md` (C runtime, cited as "audit_c").
  - `/tmp/claude-0/-home-user-passive-agnostic-rx-for-sensing/3ba5e17c-523a-59e0-a1de-318fd90f06eb/scratchpad/audit_storage.md` (storage and analysis tools, cited as "audit_storage").
  - `/tmp/claude-0/-home-user-passive-agnostic-rx-for-sensing/3ba5e17c-523a-59e0-a1de-318fd90f06eb/scratchpad/audit_lifecycle.md` (log lines and lifecycle, cited as "audit_lifecycle").
  - `/tmp/claude-0/-home-user-passive-agnostic-rx-for-sensing/3ba5e17c-523a-59e0-a1de-318fd90f06eb/scratchpad/plan.md` (proposed design, cited as "plan").
- PROJECT_MEMORY.md is 4696 lines. CLAUDE.md asks for a full read before work starts. This audit read only its section 0 (evidence labels and the host table). The rest was not read, and nothing from it is used as evidence here.
- The storage audit gives HEAD as 3f0d9217fa. This file uses the short form 3f0d921.
- Working tree: when this file was written, git status showed uncommitted changes in openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c, nr_passive_metrics.h, nr_passive_metrics_json.c, and tests/nr_passive_metrics_test.cc, and an untracked tools/mobisys/ directory. This audit did not read or use them. Everything here describes HEAD.
- Line numbers are copied from the sources. Where two sources disagree, both values are shown in section 4.8.
- Path prefixes used by the sources:
  - D = openair1/PHY/NR_UE_TRANSPORT/
  - RT = D/nr_pdcch_blind_monitor_rt.c
  - BM = D/nr_pdcch_blind_monitor.c
  - Q = D/nr_pdsch_passive_queue.c
  - SW = D/nr_pdsch_config_sweep.c
  - decode.c = D/nr_pdsch_passive_decode.c
  - NU = executables/nr-ue.c
  - USRP = radio/USRP/usrp_lib.cpp

## 0.1 Evidence labels

| Label | Meaning in this file |
|---|---|
| `[CODE-READ, 3f0d921]` | Read in code by one of the audits. Not run. |
| `[PLANNED]` | Design proposal in plan.md. No code exists. |
| `[CITED, <source section>, not re-checked]` | A source reports a document or a sandbox result. This audit did not check it. |
| `[UNCHECKED]` | The sources say "not checked", or they do not say. |

Nothing in this file is `[MEASURED]`, `[OTA VERIFIED]`, or `[SIM VERIFIED]`.

## 1. Existing metrics and exact definitions

Conventions used by the tables:

- "ISAC_METRICS" is the JSON line `SENSING: ISAC_METRICS {json}`, schema 1. All ISAC_METRICS counters are cumulative since process start unless marked (G = gauge).
- Log line text is copied from the sources. Fields are shown as in the source, with `...` where the source abbreviates.
- "Ev" gives the evidence label for the row.

### 1.1 Slot and IQ acquisition

| Metric | Log line or ISAC_METRICS key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| abs_slot (G) | ISAC_METRICS `abs_slot` (from nr_ue_diag_producer_absolute_slot) | NU:67 (declaration), NU:1645 (store) | RF slot index | last completed RF slot at snapshot | none (slot count) | each ISAC_METRICS line | -1 = unknown. Process-local hyperframe (section 4.10) | [CODE-READ, 3f0d921] |
| t_mono_ns | ISAC_METRICS `t_mono_ns` | D/nr_passive_metrics.c collect (51-210) | ns | snapshot instant | CLOCK_MONOTONIC | each ISAC_METRICS line | "the only timestamp" in ISAC_METRICS | [CODE-READ, 3f0d921] |
| pci (G) | ISAC_METRICS `pci` | nr_passive_metrics_pci, written at nr-ue.c:1198 | PCI | -1 before PBCH lock | none | each line | _Atomic int, relaxed | [CODE-READ, 3f0d921] |
| acq_state, acq_transitions, acq_sync_losses, acq_pbch_locks, acq_sib1_decodes | ISAC_METRICS keys from nr_passive_acq_snapshot() | D/nr_passive_acq_state.c | state name, counts | acquisition FSM | none | each line | acq_sync_losses is the closest counter to IQ loss (section 1.11) | [CODE-READ, 3f0d921] |
| ACQ_STATE | `SENSING: ACQ_STATE %s -> %s (updates=%lu time_in_prev=%lu regressions=%lu) evidence[...]` | D/nr_passive_acq_state.c:106 (also acq_state.c:106) | updates (count) | state transition | none | event | time_in_prev counts update() calls, not seconds or slots (section 4.9) | [CODE-READ, 3f0d921] |
| ACQ_STATE LOST | `ACQ_STATE %s -> LOST (receive-stream discontinuity, no hysteresis; losses= updates= time_in_prev=)` | acq_state.c:158 | counts | event | none | event | | [CODE-READ, 3f0d921] |
| Slot arrival (internal) | nr_passive_note_slot_arrival stores into g_slot_arr_ns[128]. Surfaces only as EVMGRANT arr2enq and SCAN_E2E | Q:131 (call site NU:1648-1655) | ns | RF read returned -> ring store | CLOCK_MONOTONIC | every slot | 0 if the slot is more than 128 slots old | [CODE-READ, 3f0d921] |
| Producer wall (internal) | nr_ue_diag_producer_wall_ns | NU:1641, NU:1654 | ns | per slot | CLOCK_REALTIME | every slot | Not in ISAC_METRICS. Do not subtract from MONOTONIC stamps | [CODE-READ, 3f0d921] |
| RF timestamp (internal) | nr_ue_diag_rf_timestamp (published) | NU:1749 | samples | per read | not stated | per read | Source does not give the clock | [UNCHECKED] |
| RFCENSUS | `SENSING: RFCENSUS slots=%ld ssb_slots=%ld pbch_ok= pbch_fail= rf_pow ref bad shiftForNextFrame max_pos_acc frame=` | NU:2001 (audit_c) or NU:2002 (audit_lifecycle) | slots, counts, dBFS | census window | none (frame field only) | every 2000 slots = 1 s at mu=1 (NU ~1960 per audit_c, NU:1939 per audit_lifecycle) | Counters reset each window | [CODE-READ, 3f0d921] |
| RFPOW, ADCPEAK, ANTPOW | `RFPOW ch<i>=dBFS(rf=dBm,clip=n)`; `ADCPEAK ch peak dBFS hot=%`; `ANTPOW` | NU:2032, NU:2040, ~NU:2060 | dBFS; hot = percent of samples above 0.9 FS | per window | none | per window | Power and clipping only | [CODE-READ, 3f0d921] |
| CHPOW | `CHPOW sample=r dBFS ch..` | USRP:2244 (audit_c "usrp_lib.cpp:2244") | dBFS | per sample | none | throttled | | [CODE-READ, 3f0d921] |
| RXDISCONT | `SENSING: RXDISCONT abs_slot=%d frame=%d slot=%d expected=%llu actual=%llu delta=%lld prev_consumed=%ld n=%ld -> INVALIDATING SYNC` | NU:1705 (audit_lifecycle; audit_c gives NU ~1696-1715) | samples (delta) | event | none in the line (abs_slot is in the line) | event, no cap | audit_c calls it an RF timestamp discontinuity ("== an overrun/drop") | [CODE-READ, 3f0d921] |
| RXDISCONT counter | s_rxts_discont_total; `n=` field | NU:1705 | count | cumulative | none | event | Not in ISAC_METRICS | [CODE-READ, 3f0d921] |
| RFTSDISC | `RFTSDISC n= prev_raw= cur_raw= expected= delta= requested= returned= prev_returned= uhd_err=(%s) more_fragments= has_time_spec=` | USRP:2480 | samples; UHD error text | event; cumulative s->rx_discontinuities | none | event, no cap | Device-level timestamp discontinuity | [CODE-READ, 3f0d921] |
| USRP_RING_* | `USRP_RING recv exception / fault / RECOVER attempt epoch / RESTART / first packet wait_ms / frozen ts`; `USRP_RING_RECOVER attempt=%d reason epoch index` | USRP:1161-1383 (audit_c). Named sites: USRP:1321, 1259, 1268/1272, 1278, 1234, 1339, 1348, 1370, 1383 | ms, index | event | none (wait_ms is a duration) | event | Ring fault codes: 1 = UHD metadata error (includes ERROR_CODE_OVERFLOW), 2 = ring overrun, 3 = exception (USRP:696) | [CODE-READ, 3f0d921] |
| Ring epoch | isac_usrp_ring_epoch() | USRP:1031; read via dlsym by NU ~1675 | epoch counter | bumped per recovered fault | none | per fault | Exported, not in ISAC_METRICS | [CODE-READ, 3f0d921] |
| isac_usrp_ring_traffic | isac_usrp_ring_traffic(copied, direct) | USRP:1038 | bytes | | | | Exported but never called. nr_passive_metrics_t.usrp_ring_copy_bytes and usrp_ring_direct_bytes (metrics.h:77) are neither filled nor serialised | [CODE-READ, 3f0d921] |
| RFSTALL | `RFSTALL ...` (lost lock, reacquiring); `RFSTALL recovered by full device re-init (n=%d)`; `RFSTALL USRP_RX_START/READ reason=%s received= requested= metadata=` | NU:2104, NU:2126; USRP:2155; fragment line USRP:2300 | event | watchdog: 8 consecutive 1-s windows with pbch_ok==0 (audit_c) | none | event | Max 4 re-inits, then exit(3) (audit_c) | [CODE-READ, 3f0d921] |
| DECIMTIMING | `DECIMTIMING call decim_us budget_us` (LOG_W) | USRP:2401 | us | per call | CLOCK_MONOTONIC (audit_c section 5: "software decimation wall (MONOTONIC)") | first 20 calls, then every 200th | Decimation wall time against budget | [CODE-READ, 3f0d921] |
| Short reads | `UE slot: got %d of %d RF samples` (LOG_W); `Short RX read: got %d of %d samples, retrying`; `RFSTALL radio read failed; refusing invalid IQ`; `RFSTALL failed/non-contiguous short-read continuation`; `short RX read exhausted`; `RFSTALL readFrame got %d of %d RF samples` | NU; nr-ue-ru.c:612, :605, :626, :641; NU:832 | samples | event | none | event | | [CODE-READ, 3f0d921] |
| Sync loss counter | acq_sync_losses | acq_state.c:150 (nr_passive_acq_note_sync_loss) | count | cumulative | none | each ISAC_METRICS line | Closest ISAC_METRICS counter to IQ loss | [CODE-READ, 3f0d921] |

### 1.2 PDCCH blind decoding

| Metric | Log line or ISAC_METRICS key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| pdcch_occasions, pdcch_candidates, pdcch_accepts, pdcch_accepts_c | ISAC_METRICS keys from nr_pdcch_blind_monitor_counters | RT:1936 | counts (occasions, candidates, DCI accepts, C-RNTI accepts) | cumulative | none | each ISAC_METRICS line | pdcch_candidates is an aggregate count only. There is no per-candidate event | [CODE-READ, 3f0d921] |
| Blind PDCCH summary | `blind PDCCH monitor summary: occasions= candidates= accepts= dci10[..] dci01[..] dci00[..] ulscan[..] held[..] efloor cfr_submits pdsch_decode[try crc_ok (%) skip_rv unsup over_cap data_submits k0_wait] scanq[queued done drop_full drop_stale maxlag] dci_wide_probes last_reject last_reject_rnti` | RT:8983-9013 (audit_lifecycle); audit_c gives RT 8920-9100 for the wider block | counts, percentages | cumulative | none | summary period (10 s or 20 s, section 4.11) | last_reject_rnti prints an RNTI (section 5) | [CODE-READ, 3f0d921] |
| SCAN_E2E | `SCAN_E2E occ= wait_us p50/p90/p99 run_us pre_us q_us` | D/nr_pdcch_passive_queue.c:273 | us ("<=" bin upper bounds) | wait = arrival -> scan start; run = scan run; pre = arrival -> enqueue; q = enqueue -> taken | CLOCK_MONOTONIC (enq_ns at D/nr_pdcch_passive_queue.c:363; take_ns at :203; occasion start and end at :239, :249) | every 4000 scan occasions | Cumulative. 50-us bins for occ, 10-us bins for pre and q, capped at 100 bins. Values above 5 ms (occ) or 1 ms (pre, q) saturate in the last bin | [CODE-READ, 3f0d921] |
| BTIM | `BTIM [name n mean max tot]`; `BTIM occ_total_us_hist`; `over_slot(us)=x/y`; `BANK_HANDOFF` | RT ~9090-9115 | us | per phase | MONOTONIC (btim_now at RT:2075) | summary period | cumulative | [CODE-READ, 3f0d921] |
| rnti_seen | `blind PDCCH rnti_seen utc_ns=%lld rnti=0x%x sfn=%d slot=%d fmt=%s cce=%d al=%d` | RT:3474; clock read RT:7818-7821 | ns | DCI accept (log is deferred; clock is read at accept) | CLOCK_REALTIME | every accepted DCI | The only per-DCI line with sfn, slot, rnti and time. Comment at RT:3472 says it is for grant-loss scoring | [CODE-READ, 3f0d921] |
| ul_seen | `blind PDCCH ul_seen utc_ns= rnti= sfn= slot= fmt=0_1 cce= al=` | RT:7187 | ns | UL accept | CLOCK_REALTIME | every UL accept | | [CODE-READ, 3f0d921] |
| DCIREJECT | `DCIREJECT (F.S) rnti= class= L= reason=` | RT:7538 | slot | event | none | event (throttling not stated) | | [CODE-READ, 3f0d921] |
| GRANTDROP | `GRANTDROP (F.S) rnti= class= why=` | RT:1108 | slot | event | none | event | | [CODE-READ, 3f0d921] |
| DCIQUAL | `DCIQUAL rnti= class= mismatched_bits=` | RT:7668 | bits | event | none | event | | [CODE-READ, 3f0d921] |
| MSG4 CANDIDATE | `MSG4 CANDIDATE (F.S) tc_rnti=` | RT:7682 | RNTI, slot | event | none | event | Prints an RNTI | [CODE-READ, 3f0d921] |

### 1.3 DCI validation

| Metric | Log line | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| DCI 1_1 length locked | `DCI 1_1 length locked coreset=%llu rnti=0x%x len=%d ...` (also `coreset=bwp-tracker`) | RT:5776 (bwp-tracker at RT:3586) | bits | length search -> locked | none | event | Prints an RNTI | [CODE-READ, 3f0d921] |
| DCI 1_1 additional length | `DCI 1_1 additional length coreset= rnti= len= replaced=` | RT:5744 | bits | event | none | event | | [CODE-READ, 3f0d921] |
| DCI length RELOCK | `DCI length RELOCK rnti=0x%04x old=%d new=%d` | RT:1680 | bits | event | none | event | | [CODE-READ, 3f0d921] |
| DCI length unresolved | `DCI length unresolved after %d occasions` (LOG_W) | RT:5801 | occasions | event | none | event | | [CODE-READ, 3f0d921] |
| UL automatic length | `UL automatic DCI length locked: %d rnti=0x%x (occasions= polar_decodes=)`; `additional length`; `unresolved` | RT:6096; RT:6089; RT:6116 | bits | event | none | event | | [CODE-READ, 3f0d921] |
| BWP | `BWP UNRESOLVED entry=`; `BWP RESOLVED entry= len= size= start= ind_bits= tda_bits= after %u grants` | RT:3559, RT:3569; RT:3582 | fields | event | none | event | | [CODE-READ, 3f0d921] |
| DCI11 layouts | `DCI11_LAYOUT armed: %d layouts consistent with dci_length=%u`; `no legal layout sums to dci_length`; `type-1 layout %d passed its CRC: FDRA staging DISARMED`; `live set refuted by TB CRC`; `DCI11_LAYOUT n=%llu observed ...` (the source line continues with a count of plausible layouts) | RT:1566, RT:1558, RT:1583, RT:1598, RT:1621 | counts | event | none | event | | [CODE-READ, 3f0d921] |
| UL layout lock | `ULLAYOUT LOCK`, `BWP_LOCK`, `SEED_LOCK`, `FIT`, `K2_BORROW`, `ULLAYOUT_CRC_LOCK`, `ULLAYOUT_CRC_EVIDENCE`, `ULLAYOUT STATS`, `reset: cell changed` | D/nr_pdcch_ul_layout_lock.c:574, 735, 754, 843, 819, 1309, 1338, 1435, 906 | fields; `abs=` on SEED_LOCK | event | `abs=` is a slot (SEED_LOCK at :754) | event | | [CODE-READ, 3f0d921] |
| DL layout | `DL layout family PREFERRED by TB CRC: rnti= layout_id=`; `DL layout pin SEED rnti= layout_id=` | RT:7437; RT:7512 | ids | event | none | event | | [CODE-READ, 3f0d921] |
| DCI payload hex | see section 5 | nr_pdcch_blind_monitor_rt.c:7376 | hex | event | none | first 12, then every 2000th (audit_c) | Privacy-sensitive | [CODE-READ, 3f0d921] |

### 1.4 Grant queueing

| Metric | Log line or ISAC_METRICS key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| scanq_* | scanq_queued, scanq_processed, scanq_drop_full, scanq_drop_stale, scanq_drop_epoch, scanq_max_lag (G, slots), pdcch_inline_drop_epoch | D/nr_pdcch_passive_queue.c:95-100, 440 | counts; slots | enqueue -> dequeue | none | each ISAC_METRICS line | scanq_max_lag = max(producer_slot - occasion_slot) at dequeue. It never decays | [CODE-READ, 3f0d921] |
| pdschq_* | pdschq_queued, pdschq_decoded, pdschq_crc_ok, pdschq_drop_full, pdschq_drop_stale, pdschq_drop_epoch, pdschq_stale_after_decode, pdschq_max_lag; puschq_drop_epoch | D/nr_pdsch_passive_queue.c get_stats (~2600). g_queued at enqueue_one success; g_decoded at queue.c:1720 (OK or FAIL); g_crc_ok at queue.c:1980 (excludes layout probes) | counts | cumulative | none | each ISAC_METRICS line | pdschq_decoded includes layout probes, about 0.4 percent per PROJECT_MEMORY [CITED, audit_c section 1 table, not re-checked] | [CODE-READ, 3f0d921] |
| PDSCHQ | `PDSCHQ queued= decoded= crc_ok= (%) dropped[full= stale=] max_lag_slots=x/spf slot_groups= multi-grant dropped_narrow=` | RT:9048 (audit_c: RT ~9040-9066) | counts | cumulative | none | summary period (10 s or 20 s) | | [CODE-READ, 3f0d921] |
| PDSCHQ per-RNTI | `PDSCHQ per-rnti 0xRNTI:ok/n ...` | RT:9056 | counts per RNTI | cumulative | none | summary period | Prints RNTIs (section 5) | [CODE-READ, 3f0d921] |
| PDSCHQ batches | `PDSCHQ batches= multi-grant= groups_taken= dropped_narrow=` | Q:2361 | counts | cumulative | none | | schedule.batches, batches_multi, slot_groups are not serialised to ISAC_METRICS (audit_c section 10) | [CODE-READ, 3f0d921] |
| PDSCHQ_PROBE_LAST | `PDSCHQ_PROBE_LAST probes dropped=` | Q:2494 | counts | event (every 500 drops) | none | event | | [CODE-READ, 3f0d921] |
| P99AGE | `P99AGE slots(<2/4/8/12/16/18/24/32/inf) pdsch_enq_real=[..] pdsch_enq_probe=[..] scan_deq=[..] scanq_fresh retry_chains retry_yields retry_gated` | Q:2590 (p99_dump) | slots; 9 bins | slot age at enqueue (and at scan dequeue) | slot counts | summary period | Cumulative histograms | [CODE-READ, 3f0d921] |
| pdschq_reasons | ISAC_METRICS `pdschq_reasons{queue_expired, future_unready, copy_expired, unsupported, decode_error, gpu_fallback}` | pdsch_schedule_json (nr_passive_metrics_json.c:46-62 per audit_lifecycle section 2) | counts | cumulative | none | each ISAC_METRICS line (only with csirs_disc enabled) | | [CODE-READ, 3f0d921] |
| PUSCHQ | `PUSCHQ queued decoded crc_ok dropped[...] max_lag_slots`; `PUSCHQ2 consumers= real[...] probe[...]` | N/nr_pusch_passive_monitor_rt.c:313, 320; D/nr_pusch_passive_queue.c:843 | counts | cumulative | none | summary period | | [CODE-READ, 3f0d921] |
| UL_E2E | `UL_E2E rnti= st= prb= mcs= pick= arr2deq= wall= tot= cpu_us= stg=` | D/nr_pusch_passive_queue.c:450 | us | per UL grant | not stated | per UL grant | Prints an RNTI | [UNCHECKED] |
| UL_PUSCH_CRC | `UL_PUSCH_CRC rnti= ok= mcs= prb= ta= t= cpu_us= status= reason= ...` | D/nr_pusch_passive_monitor_rt.c:99 | us, status | per UL grant | `t=` clock not stated | per UL grant | Prints an RNTI | [CODE-READ, 3f0d921] |

### 1.5 PDSCH FEP, channel estimation, demodulation, LLR

| Metric | Log line or key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| Stage totals | `PDTIM calls=N fep[n mean cpu max tot sd] chest[..] alloc[..] demod[..] ldpc[..]` | decode.c:424 (audit_c also lists 361, 326, 339; pdtim_report ~400). audit_storage gives 155-411 | us (uint32, truncated from ns/1000) | cumulative since start | MONOTONIC wall and CLOCK_THREAD_CPUTIME_ID (pdtim_now) | every 200 decode calls, counted racily | Accumulators are plain globals (section 4.13) | [CODE-READ, 3f0d921] |
| PDTIMPCT | `PDTIMPCT us fep[n= p50= p90= p99=] chest.. demod.. ldpc.. sum[..] ldpc_tb= ldpc_cb=` | decode.c:361 | us, 10-us bins | cumulative | MONOTONIC | every 200 decode calls | Percentile = bin centre (b*10+5). Pools all classes | [CODE-READ, 3f0d921] |
| PDTIMCLS | `PDTIMCLS us real[n mean p50 p90 p99 max gt5ms gt20ms share_gt1/5/20ms] retry[..] probe[..]` | decode.c:326 (audit_lifecycle: N/nr_pdsch_passive_decode.c:326) | us, 10-us bins | body start (FEP start) -> body end (LDPC end); class real = first body of a decode call | MONOTONIC | every 200 decode calls | Primary source for the dashboard "decode p50/p99" (section 4.1) | [CODE-READ, 3f0d921] |
| PDTIMCLSST | `PDTIMCLSST ms(<1/1-5/5-20/>20ms bodies) ...` | decode.c:339 | ms | cumulative | MONOTONIC | every 200 decode calls | | [CODE-READ, 3f0d921] |
| PDTIMSLOW | `PDTIMSLOW t= class= us= fep=wall/cpu ...` | decode.c:280 | us | body over 20 ms | MONOTONIC and thread CPU | first 200 bodies | | [CODE-READ, 3f0d921] |
| Per-grant FEP done | t_fep_done_ns = nr_td_gw_now_ns() | decode.c:3013 | ns | | MONOTONIC (grantwork clock) | per body, only when owned IQ and !gpu_llr | Feeds latency_note(FEP) | [CODE-READ, 3f0d921] |
| First body wall | t_dec_first_ns, t_dec_body_ns | decode.c:4098-4116 | ns | body start -> end | MONOTONIC | per decode call | Feeds EVMGRANT e2e field 5 | [CODE-READ, 3f0d921] |
| Body stage markers | passive_decode_body: pdt_fep at decode.c:2918; LDPC end at decode.c:3600-3641 | | | | | | | [CODE-READ, 3f0d921] |
| EVMGRANT stg | `stg=fep:w/c/g,chest..,demod..,ldpc..,prep:,wq:` | Q:1642 (stg from decode.c:4080) | us (w = wall, c = CPU, g = GPU wait) | per decode | MONOTONIC and thread CPU | per decode call | First body only (audit_c) | [CODE-READ, 3f0d921] |
| HARQC | `HARQC first= retx_combined=a/b tbs_from_first= busy_skip=`, with init_tx hit=, evict= and dev_blocks fields | decode.c:1205 | counts | cumulative | none | summary period | No per-event combine line | [CODE-READ, 3f0d921] |
| LDPCDIAG | `LDPCDIAG ok= seg_fail= tb_fail= zero_tb= iface_err= segs_decoded` | decode.c:1300 (audit_lifecycle); decode.c:1299 (audit_c). See section 4.8 | counts | cumulative | none | summary period | | [CODE-READ, 3f0d921] |
| Stats lines (no timing) | `SEGIDX`, `LLRCLIP`, `BITPOS`, `RBHIST`, `MCSHIST`, `PIPEDIAG` | RT 8920-9100; decode.c 1085-1300 (audit_c) | counts | cumulative | none | summary period | | [CODE-READ, 3f0d921] |

### 1.6 LDPC, GPU, and CRC

| Metric | Log line or key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| LDPC outcomes | ISAC_METRICS ldpc_ok, ldpc_seg_fail, ldpc_tb_fail, ldpc_zero_tb (from nr_pdsch_passive_ldpc_counters); ldpc_tb_cpu, ldpc_tb_cuda | D/nr_pdsch_passive_decode / nr_pdsch_passive_ldpc_tb_decoders | counts | cumulative | none | each ISAC_METRICS line | Code-block and TB outcomes | [CODE-READ, 3f0d921] |
| CUDA LDPC breaker | ISAC_METRICS ldpc_cuda_errors, ldpc_cuda_fallbacks, ldpc_cuda_poisoned, ldpc_cuda_disabled (0/1/2), ldpc_cuda_breaker_trips | dlsym ldpc_cuda_get_counters4 (CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:186) | counts; state | cumulative | none | each line | Reads 0 when the plugin is not loaded, which looks the same as healthy | [CODE-READ, 3f0d921] |
| CUDA LDPC timeout | g_timeout_ms (200 ms) | nrLDPC_coding_cuda_decoder.c:141, 230 | ms | breaker and timeouts | CLOCK_MONOTONIC | per call | Not exported as latency | [CODE-READ, 3f0d921] |
| CB0 GPU | ISAC_METRICS td_cb0_gpu{submits, items, ok, errors, timeouts, bypassed, sticky, trips, state, mode, failed, skipped} | ldpc_cb0_* in libldpc_cuda | counts; state 0/1/2 | cumulative | none | each line | | [CODE-READ, 3f0d921] |
| DL GPU memory | ISAC_METRICS dl_gpu_allocs, dl_gpu_late_alloc_denied, dl_gpu_transfer_bytes, dl_gpu_transfer_refused, gw_pool_refused | nr_dl_gpu_get_stats (rt.c ~9183); nr_td_gw_pool_refused | counts; bytes | cumulative | none | each line | Summed over 5 dlsym'd modules. 0 if a module is not loaded | [CODE-READ, 3f0d921] |
| GPU front end (dead) | ISAC_METRICS gpu_fep_stale_pre, gpu_fep_stale_post, gpu_fep_selfcheck_fail, gpu_fep_selfcheck_skipped, gpu_fep_disabled | forced to 0 in queue.c get_stats ("GPU front end is gone") | | | | | Always 0 | [CODE-READ, 3f0d921] |
| GPUPASS | `GPUPASS ... launches graph_hits fallback ... wait_us_p50<= prep_us[p50<= p99<=] run_us[..] post_us[..] gpu start/span p50/p99` | census() in D/nr_gpu_pass_dl.c (calls at 433, 635; format 191-209); stats from nr_gpu_pass_graph.cu:1398 | us, log2 buckets (bucket upper bound, factor 2) | at warm-up and census points | host and cudaEvent | "not strictly periodic" (audit_c; confirm at dl.c:433/635) | Factor-2 resolution | [CODE-READ, 3f0d921] |
| GPUPASS_UL_TIME | `GPUPASS_UL_TIME grant_us[old/pass_ok/pass_fail/shadow n p50 p99 max]` | D/nr_gpu_pass_ul.c:254, 259 | us | per UL pass | host | "periodic (see file)" per audit_storage | | [CODE-READ, 3f0d921] |
| Device GPU spans | cudaEvent stamps: launch to first node, first node to done; split C/D/L spans. Histogram g_stamp_hist[2][5][32] | D/nr_gpu_pass_graph.cu:418-429; hist_add at :1204 | us, log2 | per pass | cudaEvent | per pass | Reported in GPUPASS and as EVMGRANT gq= | [CODE-READ, 3f0d921] |
| Host GPU spans | nr_gp_tt[26] (thread-local): prep_ns, launch_ns, t_ret - t_entry, t_try | D/nr_gpu_pass_dl.c:612-618 | us | per pass | host MONOTONIC | per pass | EVMGRANT gp= (and gq=, li= if seq set) | [CODE-READ, 3f0d921] |
| LDPC launch and poll | ul_now_us() and nr_ss_add(NRSS_LDPC) around launch and poll | D/nr_pdsch_gpu_ldpc.cu ~955-975 | us | launch -> poll complete | host MONOTONIC | per call | Includes host polling latency (cudaStreamQuery loop, 25-us nanosleep after 60 us) | [CODE-READ, 3f0d921] |
| LDPC per-call GPU us | gpu_us via cudaEventElapsedTime | D/nr_pdsch_gpu_ldpc.cu:975 | us | | cudaEvent | only with `dg` debug flag | Off by default | [CODE-READ, 3f0d921] |
| CB0 events | cudaEventDisableTiming events | CODING/nrLDPC_cuda/ldpc_cb0.cu:387-945 | none | | | | Sync only. No timing | [CODE-READ, 3f0d921] |
| LDPC print times | `PRINT_TIMES` printf: input copy, output copy, CUDA sync runtime | CODING/nrLDPC_cuda/ldpc_decoder.cu | us | every 500 messages | CLOCK_MONOTONIC | compile-time only | Off in production builds (audit_c) | [CODE-READ, 3f0d921] |
| CRC outcome | st OK or FAIL | queue.c:1700-1725 | | | MONOTONIC | per grant | Feeds g_crc_ok and obs or sense records | [CODE-READ, 3f0d921] |
| Discovery GPU charge | CUDA event elapsed, rounded up to us | openair1/PHY/TOOLS/nr_gpu_budget_cuda.h | us | | cudaEvent | per discovery | Feeds gpu_budget_used_pct | [CODE-READ, 3f0d921] |

### 1.7 Retries and background work

| Metric | Log line or key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| RECOV | `RECOV submitted rescued stale expired dropped_full dropped_old yielded late_ms_p50/p99 fb_ok fb_fail fb_nocredit nosnap` | D/nr_pdsch_recovery.c:150 via Q:2577 | counts; ms | cumulative | none (ms histogram) | each p99_dump (summary period) and at stop | Not in ISAC_METRICS. audit_c says the async worker is at nr_pdsch_recovery.c:174 | [CODE-READ, 3f0d921] |
| RECOVLATE | `RECOVLATE rnti pid ndi tbs recovered_cfg tot_us` | Q:2153 | us | per late rescue | not stated | per late rescue | Prints an RNTI | [UNCHECKED] |
| Retry gate | P99AGE retry_chains, retry_yields, retry_gated | Q:2590 | counts | cumulative | none | summary period | Only place retry yield appears in a log line | [CODE-READ, 3f0d921] |
| LAYOUT_PROBE | `LAYOUT_PROBE n=%lu cb0_ok=%lu` | Q:1686 | counts | cumulative | none | printed at n=100, 400, then every 2000 | Layout-probe job CB0 success | [CODE-READ, 3f0d921] |
| K0_PROBE | `K0_PROBE slot rb K reached hits first_k0>=2 mask added` | Q:1492 | slot | event | none | event | | [CODE-READ, 3f0d921] |
| DL probes | `DL_RANK_PROBE`; `DL_CDM_PROBE` | Q:1804; Q:1812 | | event | none | event | | [CODE-READ, 3f0d921] |
| UL retry | `UL_RETRY rnti= CRC OK with cdm_groups= S= L= (reading said ...) late tot_us=` | D/nr_pusch_passive_queue.c:495 | us | event | not stated | event | Prints an RNTI | [CODE-READ, 3f0d921] |
| UL probes | `UL_PUSCH_PROBE rnti= dci= target= ...`; `UL_PUSCH_NO_PROBE ...` | D/nr_pusch_passive_queue.c:368; D/nr_pusch_solver.c:377 | | event | none | event | Prints an RNTI | [CODE-READ, 3f0d921] |
| ULLAYOUT probe | `ULLAYOUT_PROBE rnti len target best second third` | D/nr_pdcch_ul_layout_lock.c:1036 | | event | none | event | Prints an RNTI | [CODE-READ, 3f0d921] |
| DMRS ID async | `DMRS_ID_ASYNC done queued dropped`; `DMRS_SCAN ... rnti`; `DMRS_ID per-CORESET nid` | Q:1077, Q:214, Q:264, Q:1042; drop counter g_dscan_dropped at Q:232; worker thread "dmrsScan" at Q:270, 305 | counts | event | none | event | Prints an RNTI in DMRS_SCAN | [CODE-READ, 3f0d921] |
| UL NID background | `UL_NID_BG on ...`; `UL_NID_HIT rnti data_id dmrs_id hyp tbs crc slot=%ld`; `UL_NID_DROPPED` | N/nr_ul_nid_bg.c:83, 325, 240 | slot | event | none | event | UL_NID_HIT prints an RNTI and a slot | [CODE-READ, 3f0d921] |
| CB0 batches | `TD_CB0_SCHED batches= us_per_iter item_us sig_us tokens_us b_items g_target last: n distinct new_sigs build_us batch_wall_us compute_us iters threads backend` | D/nr_td_cb0_wire.c:436 (every 500 batches, first) | us | per batch (rate-limited) | not stated | every 500 batches | No enqueue time, completion time, or job id (audit_lifecycle section 2) | [CODE-READ, 3f0d921] |
| CB0 premise alarm | `TD_CB0_PREMISE_ALARM rnti tda slot hyp S L k0 mask table tb=PASS cb0=FAIL` | D/nr_td_cb0_wire.c:514 (audit_c) or :515 (audit_lifecycle); also SW:1206 | | event | | event | Prints an RNTI | [CODE-READ, 3f0d921] |
| CB0 aggregates | ISAC_METRICS td_cb0_* (us_per_item is cpu_us / items, mean only) | nr_td_cb0_wire_stats (D/nr_td_cb0_wire.c) | us per item | cumulative | CPU us | each line | Mean only | [CODE-READ, 3f0d921] |
| Joint anon hit | `JOINT_ANON_HIT #n (async)` | RT:2381, RT:3388 | count | event | none | event | | [CODE-READ, 3f0d921] |
| Other background | nr_dl_traffic_probe_rt.c ("SCHED_OTHER worker"), CSI-RS discovery (csirs_disc_*), BGDISC lane (ISAC_BG_LANE gate), UE context writer, sense writer, obs writer (dead) | audit_c section 8 | | | | | | [CODE-READ, 3f0d921] |

### 1.8 RNTI discovery and layout probing

| Metric | Log line or key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| PDSCH solver progress | `PDSCH_SOLVER per-rnti` (with the why field); `PDSCH_SOLVER stats grants= probes= probe_ok= fulls= full_ok= candidates=` | D/nr_pdsch_solver.c:910, 917, 921 (every 1000 or 500 planned grants) | counts; ms | cumulative | none | count-throttled | Prints an RNTI | [CODE-READ, 3f0d921] |
| PDSCH solver events | `PDSCH_SOLVER rnti= SOLVED by %u CRC16 + %u CRC24 passes, P(chance)`; CANDIDATE; `full TB CRC OK #%u` | nr_pdsch_solver.c:1244; CANDIDATE 1218; full TB CRC OK at 1321; reopening at 283, 1325; learnt S/table at 1355, 1361 | counts | event | none | event | Solve time is signal time, not wall time (section 4.15) | [CODE-READ, 3f0d921] |
| UL solver | `UL_PUSCH_SOLVER SOLVED rnti= layouts= dci= probes=` | N/nr_pusch_solver.c:433 | counts | event | none | event | | [CODE-READ, 3f0d921] |
| USS boot | `USS_BOOT_HIT count frame slot rnti` | RT:2728 | count; slot | event | none | event | Prints an RNTI | [CODE-READ, 3f0d921] |
| RNTI scramble hit | `RNTI_SCRAMBLE_HIT direction= rnti= len= AL= CCE=` | RT:3226, RT:3395, RT:3656 | | event | none | event | Prints an RNTI | [CODE-READ, 3f0d921] |
| Search space and scrambling | `SEARCH_SPACE INFERRED [%s] by CRC-recovered grants: monitored AL={..}`; `PDCCH_SCRAMBLING_ID CONFIRMED [%s] n_id=%u (assumed = PCI %u) by CRC-recovered RNTI` | RT:2208; RT:7740 | | event | none | event | | [CODE-READ, 3f0d921] |
| DMRS ID | `DMRS_ID %s CONFIRMED` or `DMRS_ID %s MISMATCH` (n_id=) | N/nr_dmrs_id_estimate.c:239, 243 | | event | none | event | Both outcomes are separate lines | [CODE-READ, 3f0d921] |
| MAC TA (RAR) | `MAC_TA RAR (F.S) ra_rnti= rapid= ta= -> tc_rnti=` | Q:601 (also MAC_TA CE at queue.c:641) | RNTI, timing advance | event | none | event | Prints ra_rnti and tc_rnti (section 5) | [CODE-READ, 3f0d921] |
| Per-RNTI sweep context | `SWEEP: new per-RNTI context rnti=0x%04x`; `SWEEP: evicted CONVERGED context rnti` | SW:585; SW:579 | RNTI | event | none | event | Prints an RNTI | [CODE-READ, 3f0d921] |

### 1.9 Config promotion, withdrawal, and epoch

| Metric | Log line or key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| CONFIG_EPOCH | `SENSING: CONFIG_EPOCH %u -> %u class=%s cause=%s scope=%s` | D/nr_passive_cfg_epoch.c:132 | epoch number | event | none (no timestamp) | event | Classes SOFT, HARD_REVERIFY, HARD_RESET. Causes at cfg_epoch.c:82-100. Epoch counter is not in ISAC_METRICS (sense records carry it) | [CODE-READ, 3f0d921] |
| CONFIG_EPOCH continuity | `CONFIG_EPOCH continuity gap_samples=%lld gap_ms=%.3f` | D/nr_passive_cfg_epoch.c:136 | samples; ms | event | none | event | gap_ms is a duration, not a timestamp | [CODE-READ, 3f0d921] |
| CONFIG_EPOCH pending | `CONFIG_EPOCH pending cause=SI_MODIFICATION_ANNOUNCED boundary=%lu` | D/nr_passive_cfg_epoch.c:366 | boundary | event | none | event | Dispatch is queued off-thread (cfg_epoch.c:121-140) | [CODE-READ, 3f0d921] |
| Listener capacity | `listener capacity exhausted` | D/nr_passive_cfg_epoch.c:234 | | event | none | event | | [CODE-READ, 3f0d921] |
| Field book | `SWEEP: FIELDBOOK PROMOTED %s=%d (rnti=0x%04x)` (SW:3159); `FIELDBOOK SUSPECT %s=%d (rnti contradicts)` (SW:3162); `FIELDBOOK WITHDRAWN (total %u) after rnti=` (SW:3166); `FIELDBOOK rnti tda FAIL-OPEN ...` (SW:3043); `FIELDBOOK ... UNTRUSTED (winner kept)` (SW:2275); `FIELDBOOK rnti tda SUSPECT/withdraw ... FIELD cause cleared` (SW:2281); `FIELDBOOK epoch bump (hard trigger) -> epoch %u generation %u` (SW:3263) | D/nr_pdsch_config_sweep.c | counts | event | none | event | No time or abs_slot on any of these lines. Prints RNTIs | [CODE-READ, 3f0d921] |
| Field book counters | ISAC_METRICS td_fb_promotions, td_fb_withdrawals, td_fb_failopens, td_fb_pruned_contexts, td_fb_untrusted_ctx | nr_pdsch_config_sweep_fieldbook_stats (D/nr_pdsch_config_sweep.c) | counts | cumulative | none | each line | No log line prints td_fb_ literally. The log tag is FIELDBOOK | [CODE-READ, 3f0d921] |
| Technique D converged | `SENSING: Technique D CONVERGED rnti=0x%x tda=%u S=%u L=%u mask=0x%x table=%u%s%s` | Q:1901 (queue consumer), Q:2044, RT:8844 | RNTI, tda, S, L | event | none | event | Suffixes add BC12a census, cb0, and excl data | [CODE-READ, 3f0d921] |
| Technique D per-RNTI | `SWEEP: rnti=0x%04x CONVERGED tda=%u mapping=%c k0=%u mcs_table= dmrs_add_pos= dmrs_max_len= (ok/total trials ...)` | SW:3177 (LOG_W) | trials | event | none | event | Private to the RNTI until a second agrees (source text) | [CODE-READ, 3f0d921] |
| Technique D armed | `Technique D ARMED: ... (configuration 0x%llx)` | RT:1829 | | event | none | event | | [CODE-READ, 3f0d921] |
| TD exclusion | `TD_EXCL_RESTART_ALARM rnti= tda= restarts= phases=` (SW:2741); `SWEEPSTAT scored= stale= created= reindexed= live= ...` (SW:3006); `SWEEP: ORACLE_RESTORE` (SW:2616); `TYPEB_LATCH` (SW:2678) | | counts | event | none | event | | [CODE-READ, 3f0d921] |
| TD exclusion counters | ISAC_METRICS td_excl_restarts, td_excl_truncs, td_excl_restart_alarms | nr_pdsch_config_sweep_excl_restart_stats | counts | cumulative | none | each line | | [CODE-READ, 3f0d921] |
| TDRA census | ISAC_METRICS td_sib1_tdra_{match,mismatch,none}_{10,11,unk}, td_deftab_{match,mismatch}_{10,11}, td_deftab_na | nr_td_census_get / nr_td_census_deftab (D/nr_td_order.c) | counts | at first convergence | none | at first convergence | | [CODE-READ, 3f0d921] |
| ACQ carrier | `ACQ carrier CONFIRMED from SIB1` (acq_state.c:324); `MISMATCH` (:330); `ISAC_ACQ_RETUNE {...}` (:349); `CONFIRMED without SIB1 carrier info` (:276); `HYPOTHESIS` (:302) | D/nr_passive_acq_state.c | | event | none | event | | [CODE-READ, 3f0d921] |
| CSS0 | `SENSING: CSS0 autoconf from MIB/SIB1 -- coreset(...) bwp=[a..b) ss(...) dci10(...)` (RT/BM:481); `CSS0 autoconf REFUSED (...)` (BM:307) | | | event | none | event | | [CODE-READ, 3f0d921] |
| CORESET | `CORESET VERIFIED by fresh dedicated DCI: offset= observed_span=` (BM:788); `CORESET VERIFIED by lookahead lane %d` (BM:1583); `multi-CORESET bank add index= offset= span= symbol= mapping= len=` (N/nr_pdcch_coreset_bank.c:437); `CORESET bank STALE index= slot=` (:207); `CORESET bank REMOVED index= offset=` (:81); `Phase 3 autodiscover -- CORESET duration %d` (BM:2513) | | | event | none | event | Bank STALE line has a slot; the others do not | [CODE-READ, 3f0d921] |
| SIB1 | `[NR_RRC] SIB1 decoded` (openair2/RRC/NR_UE/rrc_UE.c:487); `SENSING: SIB1 CELL mcc=%03d mnc=%0*d cell_identity=0x%09llx tac=%ld n_plmn=%d` (rrc_UE.c:526); `SENSING: ACQ_EVENT sib1_decoded (n=%lu, state=%s)` (acq_state.c:146); `PASSIVE: SIB1 common facts PCI=...` (BM:5581); `loaded from CACHE` (BM:5660) | | | event | none | event | No timestamp. Prints cell identity (section 5) | [CODE-READ, 3f0d921] |
| TDD | `TDD from SIB1 %s: p1 period=...` (acq_state.c:217); `TDD from SIB1 ABSENT` (config_ue.c:2685) | | | event | none | event | | [CODE-READ, 3f0d921] |
| Cell detect and sync | `Cell Detected with GSCN: %d, SSB SC offset: ...` (N/nr_initial_sync.c:828); `ISAC_ACQ_CFO_TRIAL coarse_hz=%d` (:746); `[UE%d] In synch, rx_offset %d samples` (:939); `Initial sync successful, PCI: %d` (:941); `Initial sync: pbch decoded sucessfully, ssb index %d` (:167); `synch Failed:` (NU:350); `synch retry: Rx gain increased` (NU:348); `Got synch: hw_slot_offset %d, carrier off %d Hz` (NU:339) | | | event | none | event | No timestamp on any of these | [CODE-READ, 3f0d921] |
| ACQ events | `SENSING: ACQ_EVENT pbch_locked (n=%lu, state=%s)` (D/nr_passive_acq_state.c:136) | | | event | none | event | No timestamp | [CODE-READ, 3f0d921] |
| Timing anchor and handoff | `ISAC_ACQ_TIMING_ANCHOR pci sfn timestamp` (NU:1247); `ISAC_ACQ_TIMING_HANDOFF age_frames drift_shift offset integral` (NU:1469); `ISAC_ACQ_DRIFT {json}` (NU:1240-1247) | | | event | "timestamp" field, clock not stated | event | | [UNCHECKED] |
| RX recover | `RX_RECOVER epoch gap_samples resync_ms` (NU:1328); `TIMEMUT %s %ld -> %ld (delta %+ld) at %s:%d ts=%ld consumed=%ld` (NU:895) | | samples; ms | event | MONOTONIC for resync_ms (per audit_c section 5) | event | ts is the radio sample timestamp | [CODE-READ, 3f0d921] |
| TRACKLOCK | `TRACKLOCK frame= slot= ssb= pbch_crc_ok=1 max_pos_acc a -> b (delta) sampleShift=` | N/../phy_procedures_nr_ue.c:1231 (path as written in audit_lifecycle; capped at 25 lines) | | event | none | event | | [CODE-READ, 3f0d921] |

### 1.10 CPU and GPU utilisation

No CPU or GPU utilisation sampler exists in the C code. audit_c section 0, item 5, checked for NVML, /proc/stat, and nvidia-smi and found none.

| Metric | Log line or key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| Per-thread CPU on slow decodes | `DECSLOW t= rnti mcs Nl rb probe st wall_us bodies body_us stage_us cpu_us vcsw ivcsw minflt majflt core=a->b` | queue.c:1559 | us; context-switch and fault counts | one decode call over 20 ms | MONOTONIC wall; getrusage per thread | first 300 per run | Only source of per-thread CPU and fault counts in the receiver path. Prints an RNTI | [CODE-READ, 3f0d921] |
| Gauge: GPU budget | ISAC_METRICS gpu_budget_used_pct (G, %.3f) | openair1/PHY/TOOLS/nr_gpu_budget.c (nr_gpu_budget_metrics) | percent | | logical admission budget | each ISAC_METRICS line | Not hardware utilisation. Zero unless ISAC_GPU_BUDGET is enabled. audit_lifecycle says it is baked on for --passive-rx (nr-uesoftmodem.c:285-292) | [CODE-READ, 3f0d921] |
| GPU budget counters | ISAC_METRICS discovery_yields, rt_gpu_wait_max_us, rt_gpu_fallbacks, discovery_overruns | nr_gpu_budget_metrics | counts; us (max RT wait for GPU) | cumulative | not stated | each line | | [CODE-READ, 3f0d921] |
| SRS CPU | ISAC_METRICS srs_us | nr_srs_passive_metrics (D/nr_srs_passive.c) | us (CPU) | cumulative | not stated | each line | | [CODE-READ, 3f0d921] |
| Sense cost | ISAC_METRICS sense_prod_us_<src>, sense_pack_us_<src>, sense_prod_count_<src>, sense_pack_count_<src>, sense_rec_us, sense_write_us, sense_rec_count, sense_write_count | nr_sense_cost_add (nr_sense_api.c:150, 160, 350); clock nr_sense_now_ns (nr_sense_api.h:44) | us (mean = us / count) | cumulative | nr_sense_now_ns (clock not stated) | each line | Zero if sensing is disabled | [CODE-READ, 3f0d921] |
| Telnet resources | printresources uses RUSAGE_SELF | softmodem-common.c:170 | | on telnet request | getrusage | on request | Telnet only | [CODE-READ, 3f0d921] |
| PDCCH thread cost | ISAC_PDCCH_TIMING block | RT:2000 (audit_lifecycle section 5); presence flag baked in code (no getenv) | us | | | | | [CODE-READ, 3f0d921] |
| Offline per-run CPU | cpu_pct, max_rss_kb from rx/time.txt (/usr/bin/time -v) | tests/passive_rx/dgx/score_rx.py (audit_storage section 2) | percent; kB | per run | external | per run | Offline DGX scorer, not in the receiver | [CODE-READ, 3f0d921] |
| cpu.txt | read by scan2_stats.py next to run.log | tests/passive_rx/scan2_stats.py | | | | | Producer of cpu.txt not identified in the sources | [UNCHECKED] |

### 1.11 IQ overflows, drops, and timeouts

| Metric | Log line or key | file:line | Unit | Start -> end | Clock | Cadence | Reliability | Ev |
|---|---|---|---|---|---|---|---|---|
| UHD overflow counter | None cumulative. `s->num_overflows` field exists (legacy) | USRP:673 | count | | | | Not exported. Overflow shows up only in USRP_RING fault code 1 and in RFTSDISC | [CODE-READ, 3f0d921] |
| Sample lifetime drops | ISAC_METRICS pdschq_drop_stale, scanq_drop_stale; pdschq_reasons queue_expired, future_unready, copy_expired | nr_passive_samples_valid (D/nr_passive_sample_lifetime.h), one-frame guard | counts | at dequeue | slot based | each line | Sample overwritten before decode. This is effective IQ loss in the decoder | [CODE-READ, 3f0d921] |
| Sense drops | ISAC_METRICS sense_cfr_drop, sense_rec_drop, sense_io_errors, sense_cfr_no_sink | nr_sense_stats (D/nr_sense_api.c:410) | counts | cumulative | none | each line | Ring-full drops and IO errors | [CODE-READ, 3f0d921] |
| Async log drops | `{LOG} ISAC_LOG_ASYNC dropped N records` | OAI logger (ISAC_LOG_ASYNC=1 forced for --passive-rx, executables/nr-uesoftmodem.c:288) | records | | | | Ring-full drops in the log path (audit_storage section 1) | [CODE-READ, 3f0d921] |
| ISAC_METRICS line loss | `LOG_W "buffer too small"`; line is dropped | nr_passive_metrics.c (16384 B buffer, :217) | | per emit | | per emit | Whole record lost. The warning is printed | [CODE-READ, 3f0d921] |
| Timeouts | No generic "stage X timed out" line. `DCI length unresolved after N occasions` (RT:5801); `PDSCH_SOLVER ... never decoded a full TB in 256 grants: reopening`; `DMRS_ID ... undecided after N grants`; CUDA LDPC 200 ms timeout (not exported) | | | | | | Timeouts are not counted as one metric | [CODE-READ, 3f0d921] |
| Recovery expiry | RECOV expired, dropped_full, dropped_old | D/nr_pdsch_recovery.c:150 | counts | cumulative | | each p99_dump | Not in ISAC_METRICS | [CODE-READ, 3f0d921] |
| Epoch-dropped jobs | ISAC_METRICS pdschq_drop_epoch, scanq_drop_epoch, puschq_drop_epoch | | counts | | | | Epoch-dropped jobs never reach latency histograms | [CODE-READ, 3f0d921] |

### 1.12 Schema and key counts

- This file does not total the ISAC_METRICS keys. Keys are listed in sections 1.1 through 1.11.
- Keys that are always 0 or never written: obs_pushed, obs_written, obs_dropped (writer never opened); gpu_fep_* (forced to 0); gpu_wait histogram bins (no caller). Source: audit_c section 1, and section 4.6 below.
- Fields in the struct that are not serialised: pdsch_schedule_enabled (gating only), usrp_ring_copy_bytes, usrp_ring_direct_bytes, csirs_confirm_resource_* raw. Source: audit_c section 1.

## 2. Existing file formats and paths

### 2.1 Receiver text log (run.log, rx.log, stdout)

| Item | Detail | Ev |
|---|---|---|
| Writer | OAI LOG_x macros. `SENSING:` is the stable prefix. | [CODE-READ, 3f0d921] |
| Time prefix | None by default. Option "time" gives "sec.usec" (CLOCK_MONOTONIC). "wall_clock" gives CLOCK_REALTIME. "utc_time" also exists (common/utils/LOG/log.c:90-103, 612, 652-671). Receiver .cfg files set only options=("chanmod") | [CODE-READ, 3f0d921] (audit_lifecycle section 0) |
| Harness prefixes | gawk `gettimeofday()-t0` "%.3f " (tests/passive_rx/dgx/rfsim_arm.sh:41; ul_rfsim_arm.sh:44). stamp.py unix "%.6f " (tests/passive_rx/sa_bed/stamp.py). campaign.py "%.3f " seconds since start unless the line starts with a digit. These are arrival times at the pipe, not event times | [CODE-READ, 3f0d921] |
| Colour codes | ANSI colour escapes. Every parser strips them | [CODE-READ, 3f0d921] (audit_storage section 1) |
| Async | ISAC_LOG_ASYNC=1 forced for --passive-rx | [CODE-READ, 3f0d921] |
| Capture | Plain shell redirect `> "$OUT/run.log" 2>&1` (run_arm.sh, run_adaptive_receive_test.sh, p99_replay.sh, run_passive_rx.sh). tee is used only for verdict files | [CODE-READ, 3f0d921] |
| Rotation | None. No size cap and no compression in the writers. Logs reach 110+ MB | [CODE-READ, 3f0d921] |
| Stop | campaign.py escalates SIGINT, then SIGTERM, then SIGKILL (CAMPAIGN_GRACE_S and CAMPAIGN_TERM_GRACE_S, 30 s). Watchdogs use `sudo pkill -9` (flagged in source as "prefer SIGINT") | [CODE-READ, 3f0d921] |

### 2.2 metrics.jsonl (ISAC_METRICS_PATH)

| Item | Detail | Ev |
|---|---|---|
| Env | ISAC_METRICS_PATH. Read in nr-uesoftmodem.c:325 for the dashboard too | [CODE-READ, 3f0d921] |
| Open | fopen "a". Line-buffered. Opened lazily under a static mutex (nr_passive_metrics.c:226-241) | [CODE-READ, 3f0d921] |
| Buffer | 16384 bytes (nr_passive_metrics.c:217). Current JSON is about 6-8 KB | [CODE-READ, 3f0d921] |
| Log line | `SENSING: ISAC_METRICS {json}` at LOG_A (nr_passive_metrics.c:228). Emit range nr_passive_metrics.c:214-244 | [CODE-READ, 3f0d921] |
| Cadence | 20 s, or 10 s with GPU budget (section 4.11) | [CODE-READ, 3f0d921] |
| Gaps | No emission while unsynced or during outage. Emission runs only when scan consumers run | [CODE-READ, 3f0d921] |
| Keys | schema, t_mono_ns, abs_slot, pci, then the groups in section 1 | [CODE-READ, 3f0d921] |
| Content | Aggregate counters only. No RNTI or payload | [CODE-READ, 3f0d921] (audit_storage section 8) |
| Readers | Dashboard (monitor.py, `--metrics`), scan2_stats, p99_stats, goal_metrics, ota_stats | [CODE-READ, 3f0d921] |

### 2.3 Sensing API v2 (records.jsonl and .cfr)

| Item | Detail | Ev |
|---|---|---|
| Enable | [sensing] config section. Off by default (tests/passive_rx/adaptive_no_hints.conf sets sensing.enable = 0; audit_storage section 5). On with `--sensing.enable 1 --sensing.records_path P --sensing.cfr_path P` | [CODE-READ, 3f0d921] |
| Open | O_CREAT, O_WRONLY, O_TRUNC, mode 0644 (nr_sense_api.c:222-223). TRUNCATES on every start. World-readable | [CODE-READ, 3f0d921] |
| records.jsonl | One JSON object per line. `schema: 2`. type in {session, grant, rs}. Common keys: epoch, identity_gen, dedicated_epoch, abs_slot, frame, slot, t_mono_ns, rx_ts, pci. Unknown is null | [CODE-READ, 3f0d921] |
| session record | rx_id, n_rx, rx_gain_db, carrier_dl_hz, fs_hz, scs_khz, n_rb_dl, build, rx_array | [CODE-READ, 3f0d921] |
| grant record | All schema-1 obs fields plus dl_coverage, verif, k2, dci_slot, ta fields (nr_sense_grant_to_json in obs.c ~150). Emitted via nr_sense_emit_grant_meta (queue.c:1962-1972) only when nr_sense_enabled() | [CODE-READ, 3f0d921] |
| rs record | source in {ssb, csirs, pdsch_dmrs, pdsch_data, pusch_dmrs, pusch_data, prach, srs}, direction, rnti, alloc, ports, noise_var, n_ant, n_re, slot_frac, cfr_frame_id (null for PRACH), k_abs_convention = crb0_point_a | [CODE-READ, 3f0d921] |
| CFR file | NRCF v1, little endian. Header `<4sHHQBBHIQffII`: magic, version 1, header_len, frame_id (u64), source (u8: SSB 0, CSI-RS 1, PDSCH DM-RS 2, PDSCH data 3, PUSCH DM-RS 4, PUSCH data 5, PRACH 6 (record only), SRS 7), n_ant (u8), reserved (u16, must be 0), n_re (u32), abs_slot (u64), slot_frac (f32), noise_var (f32), epoch (u32), identity_gen (u32) | [CODE-READ, 3f0d921] |
| CFR body | u16 k_abs[n_re], u8 l_sym[n_re], pad to 4 bytes, then antenna-major float32 IQ pairs | [CODE-READ, 3f0d921] |
| CFR limits | Readers enforce n_ant 1..8 and n_re 1..46200. Frame-ID gaps mean dropped frames. A trailing drop cannot be inferred | [CODE-READ, 3f0d921] |
| Readers | monitor.py CfrReader and tools/sense_api/read_sense.py. Both parse the header separately. load() holds everything in memory (OOM risk on long runs). scan() streams | [CODE-READ, 3f0d921] |
| Rings | Example config: cfr_antennas 4, cfr_ring_frames 32, cfr_max_re 46200, rec_ring_slots 1024, about 53 MiB per rx (tests/passive_rx/sense_api.example.cfg) | [CODE-READ, 3f0d921] |
| Counters | sense_cfr_drop, sense_rec_drop, sense_io_errors, sense_cfr_no_sink | [CODE-READ, 3f0d921] |
| Note | audit_c calls records.jsonl "the live per-grant file path today if sensing is configured" | [CODE-READ, 3f0d921] |

### 2.4 obs.jsonl (ISAC_OBS_PATH): dead writer

| Item | Detail | Ev |
|---|---|---|
| Status | Dead in HEAD. nr_passive_obs_open() has no non-test caller. The getenv block was deleted from executables/nr-uesoftmodem.c by cleanup commit a4d116f | [CODE-READ, 3f0d921] |
| Effect | nr_passive_obs_fast_open stays false. Both push hooks are no-ops: D/nr_pdsch_passive_queue.c:1961 and D/nr_pusch_passive_decode.c:2820. ISAC_METRICS obs_pushed, obs_written, obs_dropped are always 0 | [CODE-READ, 3f0d921] |
| Still built | The record build at queue.c:1934-1960 still runs when nr_ue_ctx_enabled() or nr_sense_enabled() is true | [CODE-READ, 3f0d921] |
| Stale text | Header comment nr_passive_obs.h:7 and PROJECT_MEMORY.md:758, 1295 are stale (audit_c) | [CITED, audit_c section 0, not re-checked] |
| Still referenced | campaign.py sets ISAC_OBS_PATH. monitor.py, health_snapshot read or set it. monitor and health_snapshot prefer obs rows when present | [CODE-READ, 3f0d921] |
| Schema (as designed) | schema, abs_slot, t_mono_ns, frame, slot, pci, dir, rnti, rnti_class, start_rb, nb_rb, start_sym, nb_sym, mcs, mcs_table, qm, nl, dmrs_symb_pos, dmrs_scrambling_id, tbs, harq_pid, rv, ndi, crc, nvar, snr_db, fo_comp_hz, delay_samples, carrier_hz, scs_khz, fs_hz. Unknown is null | [CODE-READ, 3f0d921] |
| Timing (as designed) | t_mono_ns = CLOCK_MONOTONIC when record built = decode completion (queue.c:1938). No enqueue, dequeue, or arrival time | [CODE-READ, 3f0d921] |
| Writer design | Ring of whole records, drop-newest when full (nr_passive_obs.c:267-290). Writer flushes when ring empty or every 256 lines | [CODE-READ, 3f0d921] |
| Privacy (as designed) | rnti and TB parameters per grant. No payload bytes | [CODE-READ, 3f0d921] |

### 2.5 Replay captures

| Item | Detail | Ev |
|---|---|---|
| ISAC_PASSIVE_REPLAY_CAPTURE=<file> | D/nr_passive_replay_capture.c:109; executables/nr-uesoftmodem.c:535. Opened O_EXCL, mode 0600. At most 512 MiB, 16 frames. Local same-build ABI (not an interchange format) | [CODE-READ, 3f0d921] |
| Contents | Slot IQ, DL control payloads and jobs (RNTIs, DCI bits), and tb_hash of decoded TBs (hash only, no TB bytes) | [CODE-READ, 3f0d921] |
| Replay input | ISAC_PASSIVE_REPLAY_INPUT | [CODE-READ, 3f0d921] |
| OTA run | run_adaptive_receive_test.sh sets `ISAC_PASSIVE_REPLAY_CAPTURE="$OUT/replay.bin" ISAC_PASSIVE_REPLAY_FAILURES=1` | [CODE-READ, 3f0d921] |
| GPU dumps | ISAC_PDSCH_GPU_DUMP, ISAC_PDCCH_GPU_DUMP, ISAC_GPU_DUMP write $root/replays/*.bin. Maximum 128 grants. Demod, LLR, and IQ inputs | [CODE-READ, 3f0d921] |
| Raw IQ recorders | capture_raw (rx<c>.sc16, capture_info.json; refuses overwrite). capture_stream (rx0.sc16, IQ_SHA256SUMS, JSON; async disk writer). Under tests/passive_rx/raw_baseline/ | [CODE-READ, 3f0d921] |
| Raw IQ replay input | ISAC_RAW_IQ_DIRECTORY, ISAC_RAW_IQ_SAMPLES, ISAC_RAW_IQ_FIRST_TICK, ISAC_RAW_IQ_CHANNELS, ISAC_RAW_IQ_RATE, ISAC_RAW_IQ_CENTER, ISAC_RAW_IQ_SPEED, ISAC_RAW_IQ_SHIFT, ISAC_RAW_IQ_DECIM, ISAC_RAW_IQ_GAP_*. Read only (radio/COMMON/raw_iq.c) | [CODE-READ, 3f0d921] |

### 2.6 uectx/1 JSONL (ISAC_UECTX_PATH)

| Item | Detail | Ev |
|---|---|---|
| Open | nr_ue_ctx_open(NULL, 16384, 5.0) at nr-uesoftmodem.c:333 (audit_storage) or ~335 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| Default | Path NULL maps to /dev/null (nr_passive_ue_ctx.c:548). Per-RNTI histories are not persisted in the default passive start. Only UECTX_STATS at shutdown (nr-uesoftmodem.c:671) | [CODE-READ, 3f0d921] |
| Snapshot period | 5.0 s (argument to nr_ue_ctx_open) | [CODE-READ, 3f0d921] |
| Fields (when enabled) | t_mono_ns, first_abs_slot, last_confirmed_abs_slot, ue_reconfig abs_slot (N/nr_passive_ue_ctx.c:128-204) | [CODE-READ, 3f0d921] |
| Content when enabled | Per-RNTI histories: cfg change timeline, stats (grants_dl, grants_ul, crc_ok, bytes_dl, bytes_ul, mcs_hist, layers_hist, snr_ema, fo_ema, ta_ema, prb ranges) | [CODE-READ, 3f0d921] |

### 2.7 Campaign run-directory layout (tests/passive_rx/campaign/campaign.py)

| Item | Detail | Ev |
|---|---|---|
| Campaign | `campaign.py new --name --site --cell [--root /home/nicola/NICOLA/campaigns]` creates `<root>/<YYYYMMDD>_<name>/` with manifest.json (created_utc, site, cell, notes, hostname, arch, kernel, git_commit, git_branch, git_dirty, sens6_frozen_ok, uhd_version, lscpu, nvidia-smi, /proc/cmdline, ulimit -r) | [CODE-READ, 3f0d921] |
| Run | `campaign.py run <campaign> --arm A --secs N` creates `runs/NNN_<arm>/` with cmd.txt, env.txt (ISAC_, NR_, LDPC env only), run.json (atomically replaced: arm, secs, start_utc, end_utc, rc, timed_out, escalation_stage, status, wall_s), rx.log, nic.csv (epoch, rx_missed_errors, rx_packets at 1 Hz), metrics.jsonl and obs.jsonl (from env), verdict.json | [CODE-READ, 3f0d921] |
| Index | Appends index.jsonl: run, arm, verdict, score | [CODE-READ, 3f0d921] |
| Summary | `campaign.py summarize` writes summary.json and summary.md per arm | [CODE-READ, 3f0d921] |
| Verdicts | verdict.py: VALID, INTERRUPTED, VOID_RFSTALL (FAULT regex an alternation of RFSTALL, RFTSDISC, RXDISCONT, rx xport timed out, No CHDR, device_init failed, Assertion), VOID_NIC_LOSS, VOID_NO_SYNC, VOID_NO_SIB1 | [CODE-READ, 3f0d921] |
| Note | Run-dir naming is not enforced in-tree for the DGX OTA launcher (audit_storage section 2) | [CODE-READ, 3f0d921] |

### 2.8 Other run directories

| Launcher | Layout | Ev |
|---|---|---|
| tests/passive_rx/dgx/rfsim_regress.sh | `OUT=/tmp/rfsim_regress_<YYYYmmdd_HHMMSS>/base_rN`; `<arm>/gnb/gnb.log`, `<arm>/rx/rx.log`, `<arm>/rx/time.txt`; `rx_rc=` appended to time.txt | [CODE-READ, 3f0d921] |
| tests/passive_rx/run_passive_rx.sh (rfsim bed, not OTA) | Default out_dir /tmp/passive_rx. gnb.log, ue1..3_active.log, ue_rx1..3.log, ping, iperf, udp logs, traffic time series | [CODE-READ, 3f0d921] |
| tests/passive_rx/run_adaptive_receive_test.sh (sens6 OTA) | `mktemp -d /home/sens/NICOLA/captures/adaptive_ul_dl_mrc2.XXXXXX`. receiver.conf, source_commit.txt, source.patch, source_files.sha256, checksums.txt, duration_s.txt, arm.txt, nic_missed_before/after.txt, run.log, void_attempt<N>.log, process_exit.txt, validity.txt (VALID, VOID_NO_SIB1, VOID_ABNORMAL_EXIT_<rc>, VOID_NIC_MISSED, VOID_RF_DISCONTINUITY) | [CODE-READ, 3f0d921] |
| tests/passive_rx/captures/run_arm.sh (frozen) | `$BASE/${ARM}_<HHMMSS>` with run.log, nic.csv, bracket.txt, cfo_mislock, verdict.txt (one line), and `${ARM}_LAST_VALID` pointer | [CODE-READ, 3f0d921] |
| DGX OTA launcher (not in this repo) | `~/NICOLA/ota_runs/<arm>_<n>_<HHMMSS>`; replay dirs `$ROOT/replays/replay_<name>_<HHMMSS>` | [CITED, audit_storage section 2, not re-checked] |
| tests/passive_rx/ocudu | `$BASE/$(date +%Y%m%d_%H%M%S)` with gnb.log, gnb.yaml, passive.log, claims/PX, summary.txt (via tee) | [CODE-READ, 3f0d921] |

### 2.9 Dashboard

| Item | Detail | Ev |
|---|---|---|
| Start | common/utils/nr_dashboard.c, called from executables/nr-uesoftmodem.c:~325 on every --passive-rx start. Forks a detached `python3 monitor.py --single --port 8080 --run DIR --log LOG [--metrics $ISAC_METRICS_PATH]`, pinned to core 2, SCHED_IDLE, nice 19, RLIMIT_AS 4 GiB. Drops root to SUDO_UID. flock /tmp/rxdash-<uid>-<port>.lock | [CODE-READ, 3f0d921] |
| Run dir | dirname of /proc/self/fd/1. Stdout must be a regular file. A pipe disables auto run-dir detection and falls back to `--follow ~/NICOLA/ota_runs/*` | [CODE-READ, 3f0d921] |
| Logs | dashboard.log in the run dir (or /tmp/dashboard.log in follow mode) | [CODE-READ, 3f0d921] |
| Cost | 1.2 percent of one core at 370 KB/s, RSS 25 MB. Figure is from docs/superpowers/reports/csd/DASHBOARD-ALWAYSON-20261020.md | [CITED, audit_storage section 2, not re-checked] |
| HTTP | 127.0.0.1:8080. Endpoints: /health, /api/runs, /api/state, /api/compare, /api/summary.md, /api/log, /api/log/download (raw log), /api/scan, /api/flags | [CODE-READ, 3f0d921] |
| Discovery globs | Run dir: `run.log`, `rx.log`, `*.log[.gz]`, `*metrics*.jsonl`, `*obs*.jsonl`, `*records*.jsonl`, `*cfr*`, cmd.txt, env.txt, label.txt, run.json, cells.json (discover(), monitor.py line 419) | [CODE-READ, 3f0d921] |
| Charts | Hand-written inline SVG in monitor.html (lineChart). No chart library. Light and dark themes | [CODE-READ, 3f0d921] |

### 2.10 Analysis outputs and plotting

| Item | Detail | Ev |
|---|---|---|
| Output formats | JSON per run (p99_stats, goal_metrics, score_rx, scan2_stats) and Markdown (ota_stats, summary.md, campaign summary.md). Text for health_grid | [CODE-READ, 3f0d921] |
| CSV | Only two CSV writers: campaign nic.csv and uectx_report.py (csv module) | [CODE-READ, 3f0d921] |
| PNG or figures | No PNG pipeline for run results. The only PNGs are dashboard screenshots (tests/passive_rx/monitor/screenshots.js, Playwright) | [CODE-READ, 3f0d921] |
| matplotlib | Used only by upstream-OAI legacy tools: tools/plots/dl_graph.py, tools/plots/ul_bler_vs_snr_graph.py, common/utils/data_recording/sync_validation_demo.py | [CODE-READ, 3f0d921] |
| pandas | Imported by tools/plots/dl_graph.py only | [CODE-READ, 3f0d921] |
| pyarrow, parquet | Used nowhere | [CODE-READ, 3f0d921] |
| Python requirements | tests/passive_rx/requirements-dgx.txt: `pyzmq>=25`. tools/plots/requirements.txt pins matplotlib==3.8.3, numpy==1.21.5. Monitor, rxparse, campaign, and score scripts are stdlib only. numpy is used by tools/ota_stats.py, raw_baseline, sysan, autofront, survey, sense_api --npz | [CODE-READ, 3f0d921] |
| Sandbox import check | numpy and pandas importable; matplotlib and pyarrow not installed. This was a sandbox check, not a DGX result | [CITED, audit_storage section 0, item 3, not re-checked] |
| Test runner | unittest (stdlib). Only nr_dashboard_spawn is wired into ctest (CMakeLists.txt:2406-2409). The roughly 24 other Python tests are run by hand | [CODE-READ, 3f0d921] |

## 3. Missing measurements

These gaps are facts from the sources. Section 7.9 says which gaps the plan leaves open.

| # | Gap | Why it matters | Ev |
|---|---|---|---|
| 3.1 | No DCI-accept timestamp on the PDSCH job. The job carries an enqueue time, not the DCI decode time. | Decode latency cannot start at the DCI. audit_c uses arr2enq as a proxy. Early-start grants can make arr2enq zero or negative. | [CODE-READ, 3f0d921] |
| 3.2 | The only accept-time stamp is rnti_seen utc_ns, on CLOCK_REALTIME, in a log line. | It cannot be joined to MONOTONIC stamps without a calibration. There is no key for it in ISAC_METRICS. | [CODE-READ, 3f0d921] |
| 3.3 | No per-candidate events. pdcch_candidates is an aggregate count. | PDCCH blind search cost per candidate cannot be seen. | [CODE-READ, 3f0d921] |
| 3.4 | No UHD overflow counter in ISAC_METRICS. isac_usrp_ring_traffic is never called. usrp_ring_copy_bytes and usrp_ring_direct_bytes are never filled. | Overflow appears only in log lines (RFTSDISC, RXDISCONT, USRP_RING_*). acq_sync_losses is the closest key. | [CODE-READ, 3f0d921] |
| 3.5 | No CPU or GPU utilisation sampling. No NVML, /proc/stat, or nvidia-smi in C. gpu_budget_used_pct is a logical budget. | Hardware utilisation cannot be compared across runs from the receiver alone. | [CODE-READ, 3f0d921] |
| 3.6 | No lifecycle timestamps. Cold start, cell detect, PBCH lock, SIB1, CORESET VERIFIED, bank add, DCI length locked, Technique D CONVERGED, FIELDBOOK PROMOTED or WITHDRAWN, CONFIG_EPOCH, and ACQ_STATE have no timestamp or abs_slot. | Time to lock and time to convergence cannot be measured from the log. Only rnti_seen, ul_seen, EVMGRANT, ISAC_METRICS, RXDISCONT, TIMEMUT, and RX_RECOVER have a clock (audit_lifecycle section 1). | [CODE-READ, 3f0d921] |
| 3.7 | No PHY-emitted gNB ground truth. The gNB side has only MAC-scheduled lines (LOG_D "Filling Format 1_1 DCI of size", DCI11_WIDTHS, and UE stats), and some need gNB debug level. | Matches are "scheduled", not "transmitted and detected". No per-slot or per-DCI scheduler trace exists in openair2. | [CODE-READ, 3f0d921] |
| 3.8 | No working join key between receiver and gNB per DCI. rnti_seen carries (sfn, slot, rnti). No script in the tree implements the join. score_ocudu_run.py uses only its first utc_ns. | Grant-loss scoring against the gNB cannot be done per event. score_r13 matches on DCI payload size in time order, not on frame, slot, or RNTI (audit_lifecycle section 3). | [CODE-READ, 3f0d921] |
| 3.9 | No wall-clock field in ISAC_METRICS. There is no calibration between t_mono_ns and wall time. | Cross-host and cross-log timing is unreliable (PROJECT_MEMORY section 0.3 clock note, CITED). The plan adds t_real_ns (section 7.4). | [CODE-READ, 3f0d921] |
| 3.10 | No per-job id, enqueue time, or completion time on async jobs (RECOV, DMRS scan, UL NID, TD CB0). | Async job latency cannot be measured. Only counters exist. | [CODE-READ, 3f0d921] |
| 3.11 | No windowed histograms. PDTIM and pdschq_latency are cumulative since process start. | A late regression is diluted. Windowed p99 needs differences of consecutive lines (audit_c section 12). | [CODE-READ, 3f0d921] |
| 3.12 | No heartbeat independent of RX sync. No ISAC_METRICS line while unsynced. | Stalls can only be seen as gaps in t_mono_ns (audit_c section 12). | [CODE-READ, 3f0d921] |
| 3.13 | No process CPU, RSS, or fault counters in ISAC_METRICS. Only DECSLOW (slow decodes) and telnet. | Per-run CPU cost of the receiver is only in the offline score_rx output. The plan adds proc_* keys (section 7.4). | [CODE-READ, 3f0d921] |
| 3.14 | No queue occupancy and no GPU in-flight depth. | Back-pressure in the GPU pass cannot be seen. | [CODE-READ, 3f0d921] |
| 3.15 | CUDA LDPC timing is off (ldpc_cb0 uses cudaEventDisableTiming). The 200 ms timeout is not exported. | GPU LDPC time is only partly visible (GPUPASS, gp, gq, and stg fields). | [CODE-READ, 3f0d921] |
| 3.16 | Epoch and identity generation are not in ISAC_METRICS. | Only log lines and sense records carry them. | [CODE-READ, 3f0d921] |
| 3.17 | Recovery counters (RECOV) are not in ISAC_METRICS. Retry yields appear only in P99AGE. | Async rescue health is only in a log line. The plan adds recov_* keys (section 7.4). | [CODE-READ, 3f0d921] |
| 3.18 | No per-grant data in ISAC_METRICS (all keys are aggregate). Per-grant data is in the EVMGRANT log line, and in records only if sensing is on. | Per-grant analysis depends on stdout or on opt-in files. | [CODE-READ, 3f0d921] |
| 3.19 | obs.jsonl is not produced. The default start therefore has no structured per-grant file. | The only structured per-grant file is records.jsonl, which is off by default. | [CODE-READ, 3f0d921] |
| 3.20 | HARQ combining has counters only (HARQC), no per-event line. | Per-TB combining cannot be traced. | [CODE-READ, 3f0d921] |
| 3.21 | pdsch schedule batches are not exported (out->batches, batches_multi, slot_groups unused in JSON). | Batch structure of PDSCH scheduling is only in PDSCHQ batches line. | [CODE-READ, 3f0d921] |
| 3.22 | No hyperframe from the air. H-SFN is not decoded from SIB1. abs_slot hyperframe is local. | Absolute time across a long run is not available (section 4.10). | [CODE-READ, 3f0d921] |
| 3.23 | No timeout counter. Timeouts are scattered across specific log lines. | Timeout rate cannot be computed from one metric. | [CODE-READ, 3f0d921] |
| 3.24 | No PDSCH TB payload bytes, by design. tb_hash only. | Payload-level scoring needs a separate, controlled path. | [CODE-READ, 3f0d921] |

## 4. Incorrect or ambiguous timing definitions

### 4.1 Four "decode p50 and p99" definitions

The dashboard uses this order (rxparse.py, latency_view, about line 1190 per audit_storage, 1189-1206 per audit_lifecycle):

1. PDTIMCLS us real[...] p50, p90, p99, max, mean. Label: "PDTIMCLS real (decode body, us)".
2. Else PDTIMPCT us ... sum[n p50 p90 p99] (same sum, all classes pooled).
3. Else EVMGRANT dec_us of the last 4000 grants of the DL aggregate, exact percentiles.

Targets (rxparse DEFAULT_THRESHOLDS): p50 < 500 us and p99 < 3000 us.

goal_metrics.py applies p99 < 3000 us to EVMGRANT e2e tot, not to the body. It has no p50 gate (audit_storage section 0, item 4, and section 7).

The sources name three non-equivalent p99 values: body, whole call, and end to end. They also use two or three percentile conventions (section 4.17). Any telemetry claim must name which one it reports. Evidence: [CODE-READ, 3f0d921] (audit_storage section 7, audit_lifecycle section 5).

### 4.2 PDTIMCLS real and PDTIMPCT (decode body)

| Item | Definition in code | Ambiguity or defect | Ev |
|---|---|---|---|
| Body | Sum of wall times of fep, chest, demod, and ldpc stages of one decode body. alloc is excluded. Each stage uses CLOCK_MONOTONIC around the stage (pdtim_add at decode.c:365-386; passive_decode_body starts at pdt_fep decode.c:2918, ends after LDPC at decode.c:3600-3641) | Excludes queue wait, IQ copy, GPU pass dispatch outside the four stages, and the DCI-accept-to-enqueue span. GPU graph wait is included only inside the stage walls | [CODE-READ, 3f0d921] |
| Class real | First body of a full decode call. retry = later bodies of the same call. probe = layout or solver probe | Dashboard uses real only if present | [CODE-READ, 3f0d921] |
| Histogram | 10-us bins, PDTIM_HB=2000, last bin >= 19.99 ms. Percentile = bin centre b*10+5 | Resolution is 10 us. Values in the last bin are capped | [CODE-READ, 3f0d921] |
| Window | Cumulative since process start. Printed every 200 decode calls | Never forgets. A late regression is diluted | [CODE-READ, 3f0d921] |
| Trigger | Every 200 decode calls, counted racily across threads (decode.c:393-424 in audit_lifecycle) | The print cadence is not exact | [CODE-READ, 3f0d921] |

### 4.3 EVMGRANT dec_us

| Item | Definition | Ambiguity | Ev |
|---|---|---|---|
| Span | (dec_t1 - dec_t0) / 1000 around the whole nr_pdsch_passive_decode() call, including recovery retry bodies. Printed as %.0f us | Differs from PDTIMCLS real (first body only). Includes retries | [CODE-READ, 3f0d921] |
| Start and end | dec_t0 at Q:1546 (audit_c section 4); span given as queue.c:1540-1573 (audit_storage). dec_t1 at Q:1577 | Two line ranges (section 4.8) | [CODE-READ, 3f0d921] |
| Start point | Decode call start, after dequeue and IQ copy | IQ copy and dequeue wait are not in dec_us | [CODE-READ, 3f0d921] |

### 4.4 EVMGRANT e2e (arr2enq, qwait, pre, dec, first_body, bodies, tot, lag)

| Field | Definition | Ambiguity | Ev |
|---|---|---|---|
| arr2enq | Slot sample read time (nr_passive_slot_arrival_ns) to enqueue | Enqueue is the batch-add time of the grant, not the DCI decode time. Early-start grants (job.es_sym1) can have enqueue before slot arrival, so the value can be 0 or negative. `es=sym,lead_us` marks them | [CODE-READ, 3f0d921] |
| qwait | Enqueue to dequeue (taken_ns, queue.c:1236) | Queue wait | [CODE-READ, 3f0d921] |
| pre | Dequeue to decode start | | [CODE-READ, 3f0d921] |
| dec | dec_t0 to dec_t1 | Same as dec_us | [CODE-READ, 3f0d921] |
| first_body | First body wall (field 5) | | [CODE-READ, 3f0d921] |
| tot | E2E_US(arr, dec_t1): slot arrival to decode end. -1 if a timestamp is missing | Only field the dashboard parses (rxparse RX["evm_e2e"]). Shown as e2e_us p50, p90, p99, "not yet drawn in the UI" | [CODE-READ, 3f0d921] |
| lag | Producer slots ahead at completion | | [CODE-READ, 3f0d921] |
| Recovered grants | goal_metrics replaces tot with RECOVLATE tot_us | RECOVLATE has no stated clock (section 1.7) | [CODE-READ, 3f0d921] |

Fields are MONOTONIC (slot arrival stamp and decode stamps). Evidence: [CODE-READ, 3f0d921] (audit_c section 4).

### 4.5 pdschq_latency histograms

| Item | Definition | Defect | Ev |
|---|---|---|---|
| Bins | Fixed bounds 100, 250, 500, 1000, 2000, 4000, 8000, 16000 us, plus >16000. 9 bins per stage | p99 is resolved only to a bin | [CODE-READ, 3f0d921] |
| Stage names | JSON names: enqueue_dequeue, enqueue_fep, fep_ldpc, enqueue_ldpc, gpu_wait | Enum names differ: NR_PDSCH_LAT_{QUEUE, FEP, LDPC, TOTAL, GPU_WAIT} (schedule.h:13). The mapping is in section 4.6 | [CODE-READ, 3f0d921] |
| Gating | Emitted only when nr_csirs_disc_enabled() (owned flag). Counts only samples with enqueue_ns != 0 | When the flag is off, samples are silently dropped (start == 0) | [CODE-READ, 3f0d921] |
| Population | LAT_FEP counts every dequeued decode, including CRC fail and deferred. LDPC and TOTAL count CRC_OK and CRC_FAIL only | The bins do not share one sample population | [CODE-READ, 3f0d921] |
| Percentile | hist_percentiles() returns the bin upper bound (conservative). Last bin is inf | Upper bound, not interpolated | [CODE-READ, 3f0d921] |
| Probes and epochs | Layout probes are counted in pdschq_decoded but not in g_crc_ok. Epoch-dropped jobs never reach latency histograms | Counts in different keys do not match | [CODE-READ, 3f0d921] |
| Cumulative | Cumulative since start. Windowing needs differencing | | [CODE-READ, 3f0d921] |

Evidence for the bounds and the gating: audit_c section 1, row for pdschq_latency; section 12.

### 4.6 enqueue_fep, gpu_wait, and the stage map

| JSON name | Enum | What it actually measures | Ev |
|---|---|---|---|
| enqueue_dequeue | NR_PDSCH_LAT_QUEUE | Enqueue to dequeue (queue wait) | [CODE-READ, 3f0d921] |
| enqueue_fep | NR_PDSCH_LAT_FEP | Enqueue to FEP done. Includes queue wait. Not pure FEP | [CODE-READ, 3f0d921] |
| fep_ldpc | NR_PDSCH_LAT_LDPC | FEP done to decode done | [CODE-READ, 3f0d921] |
| enqueue_ldpc | NR_PDSCH_LAT_TOTAL | Enqueue to decode done | [CODE-READ, 3f0d921] |
| gpu_wait | NR_PDSCH_LAT_GPU_WAIT | Never written. There is no latency_note(...GPU_WAIT) caller. Always zeros | [CODE-READ, 3f0d921] |

The name enqueue_fep suggests FEP only. It includes queue wait. Evidence: audit_c section 1 table and section 0, item 4.

### 4.7 Percentile conventions

| Convention | Used by | Result | Ev |
|---|---|---|---|
| Bin centre b*10+5 us | PDTIMCLS, PDTIMPCT | Centre of the 10-us bin | [CODE-READ, 3f0d921] |
| Bin upper bound | pdschq_latency (hist_percentiles) | Conservative | [CODE-READ, 3f0d921] |
| Nearest rank, index round(q*(n-1)) | exact_percentiles (rxparse) for EVMGRANT dec_us and e2e | Exact on the last up to 4000 grants | [CODE-READ, 3f0d921] |
| Upper bound of log2 bucket (2^i us) | GPU stamps (hist_q) | Factor-2 resolution | [CODE-READ, 3f0d921] |
| "<=" bin upper bound | SCAN_E2E | Saturates above 5 ms (occ) or 1 ms (pre, q) | [CODE-READ, 3f0d921] |

### 4.8 Line-number disagreements between sources

Each row gives the value in each source. The sources are not reconciled.

| Item | Value in one source | Value in another source | Ev |
|---|---|---|---|
| RFCENSUS emit | NU:2001 (audit_c) | NU:2002 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| RFCENSUS period loop | NU ~1960 (audit_c) | NU:1939 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| LDPCDIAG | decode.c:1299 (audit_c) | nr_pdsch_passive_decode.c:1300 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| TD_CB0_PREMISE_ALARM | nr_td_cb0_wire.c:514 (audit_c) | nr_td_cb0_wire.c:515 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| dec_t0 and dec_t1 | queue.c:1546 and queue.c:1577 (audit_c, audit_lifecycle) | queue.c:1540-1573 (audit_storage) | [CODE-READ, 3f0d921] |
| nr_passive_e2e_trace | queue.c ~137 (audit_c) | Q:145 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| EVMGRANT print | Q:1642 (audit_c, audit_lifecycle) | nr_pdsch_passive_queue.c ~1623-1655 (audit_storage) | [CODE-READ, 3f0d921] |
| ISAC_METRICS emit | nr_passive_metrics.c:214-244 (audit_c) | nr_passive_metrics.c:216-244 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| PDTIM print sites | decode.c:424, 361, 326, 339 (audit_c) | decode.c:393-424 (audit_lifecycle); 155-411 (audit_storage) | [CODE-READ, 3f0d921] |
| Stage timer span | pdtim_add (~375) (audit_c) | pdtim_add at 365-386 (audit_lifecycle) | [CODE-READ, 3f0d921] |
| Summary block | RT:8983-9013 (audit_lifecycle) | RT 8920-9100 (audit_c, wider) | [CODE-READ, 3f0d921] |

### 4.9 time_in_state and time_in_prev are update counts

ACQ_STATE time_in_prev is a count of update() calls. It is not seconds or slots. The increment is g_snap.time_in_state++ at acq_state.c:124. The dashboard heartbeat is on the summary period. Evidence: [CODE-READ, 3f0d921] (audit_lifecycle section 1; audit_storage section 5).

### 4.10 abs_slot is process-local

- abs_slot = (hfn * 1024 + frame) * slots_per_frame + slot (NU:1513 and NU:1593-1596, audit_c section 4 and audit_lifecycle section 4).
- hfn starts at 0 (absolute_slot init at NU:1113). It increments when decoded_frame_rx <= prev_frame_rx (NU:1507-1512).
- The PBCH gives SFN mod 1024 only. H-SFN is not decoded from SIB1.
- On resync, abs_slot is recomputed from the previous hfn and the new SFN (NU:1513).
- decoded_frame_rx is reset to MAX_FRAME_NUMBER-1 at NU:1725 and NU:2144.
- So abs_slot is not continuous across RXDISCONT or reacquisition. The epoch, CONFIG_EPOCH, and gap_samples are the only markers.
- The SSB-only abs slot (phy_procedures_nr_ue.c:1140) has no hfn.

Joins across runs or hosts by abs_slot are not valid. Evidence: [CODE-READ, 3f0d921].

### 4.11 ISAC_METRICS cadence: 10 s or 20 s

| Source | Statement | Ev |
|---|---|---|
| audit_c section 1 | 20 s; 10 s if nr_gpu_budget_enabled(). summary_due_now at RT:667 (CAS on CLOCK_MONOTONIC) | [CODE-READ, 3f0d921] |
| audit_lifecycle header and section 6 | 10 s with GPU budget (RT:667-677); GPU budget is baked on for --passive-rx (setenv at nr-uesoftmodem.c:285-292) | [CODE-READ, 3f0d921] |
| audit_storage section 0 and section 1 | 20 s | [CODE-READ, 3f0d921] |
| audit_lifecycle header | PROJECT_MEMORY says 20 s. The code gives 10 s. "Doc/code drift" | [CITED, audit_lifecycle section 5, not re-checked] |

Consequence: in passive mode the code reading gives 10 s. Any rate or time axis built on a 20 s assumption is wrong by a factor of two. The dashboard TimeMap interpolates between ISAC_METRICS anchors (flagged `shortcut:`). The emitter runs on whichever scan consumer completes an occasion first, so spacing is not exact.

### 4.12 Clock domains

| Clock | Used by | Ev |
|---|---|---|
| CLOCK_MONOTONIC | ISAC_METRICS t_mono_ns; slot arrival stamps; EVMGRANT stamps; PDTIM wall; SCAN_E2E; BTIM; decode and GPU host spans; obs t_mono_ns (completion) | [CODE-READ, 3f0d921] |
| CLOCK_REALTIME | rnti_seen utc_ns (RT:3474; read at RT:7818-7821); ul_seen utc_ns (RT:7187); nr_ue_diag_producer_wall_ns (NU:1641, 1654); nr_spin_now_ns (audit_c) | [CODE-READ, 3f0d921] |
| CLOCK_THREAD_CPUTIME_ID | pdtim_now CPU part (PDTIM cpu fields) | [CODE-READ, 3f0d921] |
| OAI log prefix, option "time" | CLOCK_MONOTONIC. Not set by the .cfg files | [CODE-READ, 3f0d921] |
| OAI log prefix, option "wall_clock" | CLOCK_REALTIME | [CODE-READ, 3f0d921] |
| Harness prefixes | Arrival time at the pipe (gawk, stamp.py, campaign.py) | [CODE-READ, 3f0d921] |

Mixing rule: rnti_seen (REALTIME) cannot be subtracted from EVMGRANT (MONOTONIC) directly. Sample ring arrival stamps are MONOTONIC. Evidence: audit_c section 12, audit_lifecycle section 0.

### 4.13 Racy PDTIM globals

- g_pdtim_ns, g_pdtim_n, g_pdtim_max, g_pdtim_ss, g_pdtim_cpu_ns, and g_pdtim_calls (decode.c:121-127) are plain globals. Every PDSCH consumer thread writes them.
- The histograms (g_pdtim_hist, g_cls_*) are atomic.
- Under N consumers the plain globals are racy. A thread sanitizer would flag them (audit_c section 0, item 7; the audit does not say whether TSAN has run).
- The PDTIM print trigger (every 200 calls) is counted across threads with the same race.

Evidence: [CODE-READ, 3f0d921].

### 4.14 Non-atomic snapshot

ISAC_METRICS collect() reads public getters, each with a relaxed atomic load or a weak-symbol getter. It takes no lock. Fields are read at different instants. Ratios such as crc_ok / decoded can be above 1 for a short time. Evidence: [CODE-READ, 3f0d921] (audit_c section 1).

### 4.15 Solver time is signal time

The PDSCH solver line `solve=ms` is signal-time slots * 0.5 ms, assuming mu=1. It is not wall time. Evidence: [CODE-READ, 3f0d921] (audit_c section 8; audit_storage section 2).

### 4.16 SCAN_E2E saturation

The histograms use "<=" bin upper bounds. Values above 5 ms (occ) or above 1 ms (pre, q) fall into the last bin. A p99 read from a saturated histogram is an upper bound that is too low. Evidence: [CODE-READ, 3f0d921] (audit_c section 12).

### 4.17 Other timing ambiguities

| Item | Ambiguity | Ev |
|---|---|---|
| obs t_mono_ns | Completion time, not arrival. There is no arrival in the record | [CODE-READ, 3f0d921] |
| rnti_seen | Log line is deferred (nr_post_accept_defer). Clock is read at accept | [CODE-READ, 3f0d921] |
| EVMGRANT stg= | First body only. dec_us covers all bodies | [CODE-READ, 3f0d921] |
| Epoch-dropped and probe jobs | Probes count in PDTIM probe class and pdschq_decoded, but not in crc. Epoch-dropped jobs are missing from latency histograms | [CODE-READ, 3f0d921] |
| Lines without time | Lines carry no wall time of their own. The dashboard interpolates | [CODE-READ, 3f0d921] |
| Logger time | Default has no time prefix. Pre-2026-10-20 logs lack e2e= and PDTIM fields | [CODE-READ, 3f0d921] |
| Monitor time axis | Linear interpolation between ISAC_METRICS anchors (shortcut) | [CODE-READ, 3f0d921] |
| p99_stats vs goal_metrics | Disagree by design (section 4.1) | [CODE-READ, 3f0d921] |

## 5. Privacy-sensitive fields currently persisted

Default means "on with no environment variable". The receiver is passive. RNTIs are temporary identifiers, but they link grants to one UE for the lifetime of the C-RNTI (audit_storage section 8).

| # | Artifact or field | Where, and what it contains | On by default | Ev |
|---|---|---|---|---|
| 5.1 | RNTIs in log lines | About 200 format sites with `rnti=0x` in D/ (audit_c). About 150 LOG sites with rnti in openair1/PHY/NR_UE_TRANSPORT (audit_storage). Examples: EVMGRANT (rnti, mcs, tbs, rv, ndi, pid, snr, rb, F.S, dec_us, e2e), DECSLOW, RECOVLATE, PDSCH_SOLVER, FIELDBOOK and SWEEP, TD_CB0_PREMISE_ALARM, PDSCHQ per-rnti, BOOTTABLE, last_reject_rnti, rnti_seen (RT:3474), UL_PUSCH_CRC, UL_E2E, DCI 1_1 length locked, UL automatic DCI length locked, DMRS_SCAN, DATA_ID_WALK, VRB_IL and PRG | Yes. No environment gate. The default log contains C-RNTIs (audit_c section 11) | [CODE-READ, 3f0d921] |
| 5.2 | MAC TA fields | MAC_TA RAR: ra_rnti, tc_rnti, ta range (queue.c:601, 641) | Yes | [CODE-READ, 3f0d921] |
| 5.3 | DCI payload hex | nr_pdcch_blind_monitor_rt.c:7376 `DL raw evidence n= len= rnti= payload=0x...` (first 12, then every 2000th). rt.c:3622 `payload=0x%s bits= candidates=` (audit_c). nr_pdcch_ul_discovery.c:427 `UL raw sample rnti= len= payload=0x` | Yes | [CODE-READ, 3f0d921] |
| 5.4 | PDSCH TB payload bytes | Never logged. dec.tb goes only to the data-aided path nr_isac_pdsch_data_aided_submit (queue.c ~1975, when want_data). Whether that path persists bytes is not checked | No bytes persisted (audit_storage says "No TB payload BYTES are persisted anywhere") | [CODE-READ, 3f0d921] |
| 5.5 | Command line | `CMDLINE:` line with full argv. manifest.json /proc/cmdline. campaign cmd.txt | Yes (log). Campaign files on run | [CODE-READ, 3f0d921] |
| 5.6 | Cell identity | `SENSING: SIB1 CELL mcc= mnc= cell_identity= tac=` (rrc_UE.c:526). PCI in ISAC_METRICS and the log | Yes | [CODE-READ, 3f0d921] |
| 5.7 | SIB1 cache | ISAC_SIB1_CACHE_DIR, default /tmp/passive_rx. Files /tmp/passive_rx/sib1_common_pci*_hash*.bin and `..._last_hash.bin` (nr_pdcch_blind_monitor.c:5595-5625). Directory created 01777 (world-writable) | Cell-level config, no UE data. On after SIB1 decode | [CODE-READ, 3f0d921] |
| 5.8 | Sensing records.jsonl | Per-grant rnti, allocation, DM-RS, ports. rs records carry rnti. PRACH preamble and RAR association. SRS track ids. Written O_TRUNC, mode 0644 (nr_sense_api.c:222-223) | Only if sensing.enable = 1 and a path is set | [CODE-READ, 3f0d921] |
| 5.9 | Sensing .cfr | Per-UE channel frequency response per antenna. Derived from DM-RS and data REs of specific UEs. Mode 0644 | Same as 5.8 | [CODE-READ, 3f0d921] |
| 5.10 | Replay capture | replay.bin. Slot IQ, DCI payloads, RNTIs, job structs, tb_hash. Mode 0600, O_EXCL | Opt-in (ISAC_PASSIVE_REPLAY_CAPTURE) | [CODE-READ, 3f0d921] |
| 5.11 | GPU dumps | Demod, LLR, and IQ inputs of up to 128 grants in $root/replays/*.bin. File mode not stated | Opt-in env | [UNCHECKED] for file mode |
| 5.12 | Raw IQ | Full-bandwidth IQ of all UEs and the cell in the capture window (rx*.sc16). Stored under ~/NICOLA/ota_runs/raw_* | Operator-run | [CITED, audit_storage section 8, not re-checked] |
| 5.13 | Dashboard HTTP | 127.0.0.1:8080. Per-RNTI table (rnti hex, grants, crc, mcs buckets, layers, prb_mean, snr, tput, first and last t; top 200 per cell and direction; RNTI_CAP 4096). First 40 new-RNTI events. /api/log and /api/log/download serve raw log lines. /health obs.top_rnti | Always on, local only | [CODE-READ, 3f0d921] |
| 5.14 | Dashboard run-dir logs | dashboard.log in the run dir; /tmp/rxdash-<uid>-<port>.lock | Always | [CODE-READ, 3f0d921] |
| 5.15 | Analysis outputs | ota_stats --json (per-RNTI persistence counts, RNTI sets); scan2_stats dci_set (in memory, Jaccard only); quiettiming_data/parse.py crc_rntis list in runs.json; docs/superpowers/reports/csd/*.json (aggregate) | When the tool is run | [CODE-READ, 3f0d921] |
| 5.16 | p99_stats, goal_metrics, health_grid outputs | Stated as RNTI-free | Not sensitive per source | [CITED, audit_storage section 8, not re-checked] |
| 5.17 | ISAC_METRICS | Aggregate counters. No RNTI and no payload | Not sensitive per source | [CODE-READ, 3f0d921] |
| 5.18 | obs.jsonl | Per-grant rnti and TB parameters. Not produced in HEAD | Dead | [CODE-READ, 3f0d921] |
| 5.19 | uectx/1 JSONL | Per-RNTI histories. Not persisted when path is NULL | Off in default passive start | [CODE-READ, 3f0d921] |
| 5.20 | Core dump | Whole process memory: IQ rings, RNTIs, TBs, LLRs. Launchers set ulimit -c 0 unless CORE is set. Host core_pattern not checked | Off in launchers | [CODE-READ, 3f0d921] |
| 5.21 | run dir source.patch | git diff --binary of uncommitted code (run_adaptive_receive_test.sh) | Always in that launcher. Code, not UE data | [CODE-READ, 3f0d921] |
| 5.22 | Host metadata | manifest.json (hostname, kernel, lscpu, nvidia-smi); env.txt (ISAC_, NR_, LDPC env) | Campaign runs | [CODE-READ, 3f0d921] |

Note on the 5.1 count: audit_c and audit_storage give different numbers (about 200 and about 150). This file does not choose one.

## 6. Performance overhead and concurrency risks

No overhead number in this section is measured. The sources give code facts only.

### 6.1 Overhead sources

| Item | Where | What the sources say | Ev |
|---|---|---|---|
| Per-grant EVMGRANT line | Q:1642 | LOG_A, unthrottled, one per full decode (CRC OK and fail). audit_c says it is "NOT safe" to add per-grant logging in the consumer thread. nr_pdcch_blind_monitor_rt.c comments blame such logging for timing runaways | [CODE-READ, 3f0d921] |
| PDTIM clock reads | decode.c, pdtim_now | Two clock reads per stage (CLOCK_THREAD_CPUTIME_ID and CLOCK_MONOTONIC). pdtim_enabled() returns 1, so always on (decode.c:131-135). Cost is not measured in any source | [CODE-READ, 3f0d921]; cost [UNCHECKED] |
| DECSLOW getrusage | Q:1559 | Only on decode calls over 20 ms. First 300 per run | [CODE-READ, 3f0d921] |
| ISAC_METRICS collect | D/nr_passive_metrics.c | Relaxed atomic loads and weak-symbol getters. dlsym(RTLD_DEFAULT, ...) for CUDA LDPC counters on each collect. Whether the dlsym result is cached is not stated | [CODE-READ, 3f0d921]; caching [UNCHECKED] |
| ISAC_METRICS emit | nr_passive_metrics.c:216-244 | One line per 10 s or 20 s. Written by a scan consumer, outside the phase2 lock. fprintf under a static mutex. Line-buffered | [CODE-READ, 3f0d921] |
| Fixed local buffers | sense_cost and cb0 JSON | 2048-byte local buffers. Adding keys to them can overflow, and the whole line is then lost | [CODE-READ, 3f0d921] |
| SCAN_E2E histograms | D/nr_pdcch_passive_queue.c:273 | Cumulative. Updated every 4000 scan occasions | [CODE-READ, 3f0d921] |
| GPU LDPC poll | nr_pdsch_gpu_ldpc.cu ~955-975 | cudaStreamQuery loop with 25-us nanosleep after 60 us. The polling time sits inside the timed launch span | [CODE-READ, 3f0d921] |
| Discovery GPU charge | nr_gpu_budget_cuda.h | CUDA event elapsed time, rounded up to us | [CODE-READ, 3f0d921] |
| Async logging | ISAC_LOG_ASYNC=1 | Ring-full drops are counted and printed. Under load the LOG_A path is "rate/IO sensitive" (audit_c section 12) | [CODE-READ, 3f0d921] |
| Log volume | Launchers | No rotation. Runs reach 110+ MB | [CODE-READ, 3f0d921] |
| Dashboard | nr_dashboard.c | 1.2 percent of one core at 370 KB/s, RSS 25 MB. Core 2, SCHED_IDLE, nice 19 | [CITED, audit_storage section 2, not re-checked] |
| Sensing rings | sense_api example config | About 53 MiB per rx | [CODE-READ, 3f0d921] |
| Replay capture | nr_passive_replay_capture.c | Up to 512 MiB, 16 frames | [CODE-READ, 3f0d921] |
| Plan 1-s sampler | plan.md | Runs in the exporter. Reads /proc and calls nvidia-smi each second if present. Cost not assessed in the plan | [PLANNED] |

### 6.2 Concurrency risks

| # | Risk | Where | Effect | Ev |
|---|---|---|---|---|
| 6.2.1 | Racy PDTIM accumulators | decode.c:121-127 | Wrong totals under N consumers. Plain globals | [CODE-READ, 3f0d921] |
| 6.2.2 | Non-atomic ISAC_METRICS snapshot | nr_passive_metrics.c collect | Ratios above 1 for a short time. Fields at different instants | [CODE-READ, 3f0d921] |
| 6.2.3 | ISAC_METRICS depends on a scan consumer | RT:9157 `if (sum_due) nr_passive_metrics_emit()` | No lines during acquisition or outage. A starved consumer means gaps | [CODE-READ, 3f0d921] |
| 6.2.4 | Silent-looking record loss | 16 KB buffer | Whole ISAC_METRICS line lost. LOG_W "buffer too small" is printed | [CODE-READ, 3f0d921] |
| 6.2.5 | File open failure | nr_passive_metrics.c | LOG_W, then the file is off for the run | [CODE-READ, 3f0d921] |
| 6.2.6 | Log sink under load | OAI logger | LOG_A may drop under load (audit_c section 12) | [CODE-READ, 3f0d921] |
| 6.2.7 | Async retry drops | nr_pdsch_recovery.c | dropped_full and dropped_old counts. Not in ISAC_METRICS | [CODE-READ, 3f0d921] |
| 6.2.8 | dmrsScan worker drops | Q:232 | g_dscan_dropped. Not in ISAC_METRICS | [CODE-READ, 3f0d921] |
| 6.2.9 | Epoch-dropped jobs | queue | Missing from latency histograms | [CODE-READ, 3f0d921] |
| 6.2.10 | abs_slot change at reacquisition | NU:1725, NU:2144 | Mapping between abs_slot and samples changes | [CODE-READ, 3f0d921] |
| 6.2.11 | Stdout pipe design | plan.md | The receiver's stdout would go to the exporter. What happens when the exporter stalls, exits, or closes the pipe (blocked writes, SIGPIPE) is not analysed in any source | [UNCHECKED] |
| 6.2.12 | Dashboard with a pipe | nr_dashboard.c | Run-dir detection fails. Dashboard falls back to --follow (audit_storage section 4) | [CODE-READ, 3f0d921] |
| 6.2.13 | Stop method for the exporter | campaign.py, watchdogs | Campaign uses SIGINT then SIGTERM then SIGKILL. Watchdogs use `sudo pkill -9`. The plan does not say how the exporter is stopped. Project rule: receiver stops with SIGINT (CLAUDE.md) | [CODE-READ, 3f0d921]; plan silent |
| 6.2.14 | Clock mixing in the exporter | rnti_seen (REALTIME) with EVMGRANT and ISAC_METRICS (MONOTONIC) | A join by time is wrong without calibration. Plan adds t_real_ns | [CODE-READ, 3f0d921] |
| 6.2.15 | Shared /tmp | SIB1 cache, rfsim outputs, rxdash lock | World-writable directory and shared paths | [CODE-READ, 3f0d921] |
| 6.2.16 | Cross-host timestamps | PROJECT_MEMORY section 0.3 clock note | Cross-host timing unreliable unless re-measured | [CITED, PROJECT_MEMORY section 0.3 (read by this audit; not one of the four sources), not re-checked] |

Item 6.2.16 is a note from the PROJECT_MEMORY header the audit read. It is not from the four sources. Treat it as a pointer only.

## 7. Proposed changes and implementation locations

Source for this section: plan.md, unless marked otherwise. Everything here is [PLANNED] (no code exists yet).

### 7.1 Architecture

- An offline exporter, tools/mobisys/export.py, reads the receiver's stdout through a pipe and the ISAC_METRICS stream.
- The receiver hot path is unchanged except for additive ISAC_METRICS keys, emitted every 10 s from the existing summary emitter.
- The plan adds no new log lines and no new flags. Existing lines (EVMGRANT, rnti_seen, lifecycle lines) are parsed from stdout.

### 7.2 Modes

- `--experiment-mode lab|commercial`, default commercial. Fixed when the exporter starts. Written into manifest.json. It is an argument of the exporter, not a receiver flag.

### 7.3 Commercial privacy boundary (plan)

- The exporter accepts only a pipe on stdin. It refuses a regular file. Reading a persisted raw log would mean the prohibited persistence already happened.
- Documented launch line: `nr-uesoftmodem ... 2>&1 | python3 tools/mobisys/export.py --experiment-mode commercial --out DIR`.
- The operator must not redirect the receiver to a file.
- Aggregates in memory over 60-s windows, with k-suppression at K=10.
- Persists only allow-listed datasets and columns.
- Fail closed: unknown dataset, unknown column, or non-allow-listed string value raises PolicyViolation. The row is refused and counted. The exporter exits nonzero in commercial mode when this happens.
- Exporter error messages never echo input text in commercial mode.
- ISAC_METRICS_PATH file is aggregate-only and allowed.
- The dashboard (monitor.py) serves the raw log over local HTTP. It is lab-only. With stdout piped it has no run.log to serve.

### 7.4 C change (plan)

Files: nr_passive_metrics.h, nr_passive_metrics.c, nr_passive_metrics_json.c, and its unit test (the test file name is not given in the sources). All under D/.

New ISAC_METRICS keys (schema stays 1, additive):

| Key | Source | Note | Ev |
|---|---|---|---|
| t_real_ns | CLOCK_REALTIME at emit | For mono-to-wall calibration against gNB logs | [PLANNED] |
| cfg_epoch | nr_cfg_epoch_current() | | [PLANNED] |
| cfg_identity_gen | nr_cfg_epoch_identity_gen() | | [PLANNED] |
| proc_utime_us | getrusage(RUSAGE_SELF) | | [PLANNED] |
| proc_stime_us | getrusage(RUSAGE_SELF) | | [PLANNED] |
| proc_maxrss_kb | getrusage(RUSAGE_SELF) | | [PLANNED] |
| proc_minflt | getrusage(RUSAGE_SELF) | | [PLANNED] |
| proc_majflt | getrusage(RUSAGE_SELF) | | [PLANNED] |
| recov_* | nr_pdsch_recovery_stats() (read-only call) | Exact key names not given | [PLANNED] |
| retry_yield_* | nr_pdsch_retry_yield_stats() (read-only call) | Exact key names not given | [PLANNED] |

The plan says the calls may use weak symbols, as sense, prach, srs, and csirs already do.

Buffer rule (audit_c section 10, the audit's recommendation): add new keys in a separate buffer. Do not add to the 2048-byte sense_cost or cb0 buffers. The current JSON is about 6-8 KB against the 16384-byte limit. Evidence: [CODE-READ, 3f0d921] for the buffer facts; the rule itself is an audit recommendation, not a plan item.

### 7.5 Python package tools/mobisys/ (plan)

| File | Purpose |
|---|---|
| policy.py | Modes, dataset and column allow-lists with privacy class, string-value enums, fail-closed Sink |
| hist.py | Log-linear mergeable histogram: 32 sub-buckets per power of two, us. Merge, quantile, to_rows, from_rows |
| parse.py | Line parsers: EVMGRANT (generic key=value; e2e fields; es=; stg=), rnti_seen, ISAC_METRICS json, lifecycle lines, CONFIG_EPOCH, FIELDBOOK, RECOVLATE, LAYOUT_PROBE, UL_RETRY, RXDISCONT, RFTSDISC, RFCENSUS, USRP_RING_*, RFSTALL, PDTIMCLS (legacy dashboard latency). SFN unwrap (frame 0..1023) to ext_frame and ext_slot |
| export.py | CLI. Reader thread parses and never blocks (bounded queue, put_nowait, loss counter). Aggregator. Writer thread (row groups, rotation by rows, parquet if pyarrow, else jsonl; format in manifest). 1-s system sampler (/proc pid and host; nvidia-smi if present, else null) |
| groundtruth.py | Lab only. Imports OAI gNB MAC LOG_D "[DLSCH/PDSCH/PUCCH]" lines (needs --log_config.nr_mac_log_level debug). Classifies TP, FP, FN, correct DCI with failed TB, correct TB, missed TB, dropped or undecidable. Labels the source as scheduled, not PHY-emitted |
| plots.py | Figures from persisted datasets only. Lists missing inputs. Never synthesizes. Wilson CIs. Sessions as units |
| tests | unittest, test_*.py next to the code. Fixtures from real format strings. Adversarial privacy tests |
| examples/ | Small datasets from a synthetic fixture. Provenance "synthetic". Plots watermarked |

Dependencies: stdlib and numpy. pyarrow and matplotlib optional at import, required for parquet or plots (plan header).

### 7.6 Datasets

| Mode | Datasets |
|---|---|
| Lab | rx_grants, dci_attempts, discovery_events, configuration_events, retry_events, slot_health, system_telemetry, latency_histograms, summary.json, manifest.json |
| Commercial | cell_aggregates, discovery_summary, system_telemetry, latency_histograms (60-s windows, suppressed), summary.json, manifest.json |

The plan does not say whether lab-mode datasets keep RNTIs (rx_grants and dci_attempts suggest they might). This is not decided in any source. See 7.8.

### 7.7 Documents (plan)

- docs/MOBISYS_TELEMETRY_AUDIT.md (this file).
- docs/MOBISYS_METRICS.md.
- docs/MOBISYS_EXPERIMENT_PROTOCOL.md.
- docs/MOBISYS_PRIVACY.md.
- docs/MOBISYS_FINAL_REPORT.md.
- PROJECT_MEMORY section 11.13 row for the new keys.

### 7.8 Files that must not be edited, and why

The user constraint in plan.md: "no new flags (no new ISAC_* env vars, no new nr-uesoftmodem CLI options, no new .conf keys)" and "Do NOT edit decoder-thread or queue code". The plan lists these as off limits:

| File or group | Why (plan and audits) | Ev |
|---|---|---|
| D/nr_pdsch_passive_queue.c and .h | Decoder thread and queue. EVMGRANT is emitted here | [PLANNED] |
| D/nr_pdsch_passive_decode.c and .h | Decode body, PDTIM | [PLANNED] |
| D/nr_pdsch_passive_schedule.h | Latency bins and enum | [PLANNED] |
| D/nr_pdsch_recovery.c and .h | Async retry worker | [PLANNED] |
| D/nr_pdsch_solver.c and .h | Solver | [PLANNED] |
| D/nr_pdsch_config_sweep.c and .h | Config sweep and field book | [PLANNED] |
| D/nr_td_*.c | Technique D, grantwork, CB0 wire | [PLANNED] |
| D/nr_pdsch_gpu_*, D/nr_gpu_pass_*, CODING/nrLDPC_cuda/* | GPU pass and LDPC | [PLANNED] |
| D/nr_pusch_passive_* | UL queue and decode | [PLANNED] |
| D/nr_pdcch_passive_queue.* | Scan queue | [PLANNED] |
| D/nr_pdcch_blind_monitor*.c, D/nr_pdcch_blind_phase2.c | Blind monitor and phase 2 | [PLANNED] |
| D/nr_passive_sample_lifetime.h, D/nr_passive_job_epoch.h | Sample lifetime and epoch | [PLANNED] |
| executables/nr-ue.c | RX loop, RT priority | [PLANNED] |
| radio/USRP/usrp_lib.cpp | Ring producer | [PLANNED] |
| Existing log lines | Parsers depend on text (rxparse.py, ota_stats.py, p99_stats.py, scan2_stats.py). Do not reformat | [PLANNED] |

Also not to be edited (CLAUDE.md, project instruction): tests/passive_rx/captures/*, tests/passive_rx/*.conf, and tests/passive_rx/sens6_host_snapshot_2026-09-30/*. audit_storage lists two more frozen paths (tests/passive_rx/ota and dgx_host_snapshot_2026-09-30). The sources do not reconcile the two lists.

### 7.9 Gaps the plan leaves open

The plan does not change the files in 7.8. So these gaps from section 3 stay open unless another route is found:

- No DCI-accept timestamp on the PDSCH job (3.1).
- No per-candidate events (3.3).
- No UHD overflow counter (3.4). The plan's files do not cover usrp_lib.cpp or nr-ue.c.
- No lifecycle timestamps (3.6). The plan adds none.
- No PHY-emitted gNB ground truth (3.7). The plan uses MAC LOG_D lines, labelled as scheduled.
- No hardware CPU or GPU utilisation from the receiver (3.5). The plan has a 1-s sampler in the exporter (/proc and nvidia-smi).
- No async job ids or times (3.10).
- No hyperframe from the air (3.22).

### 7.10 Deferred (plan)

Needs DGX or X410: before-and-after overhead on the same IQ replay, GPU utilisation, and OTA runs.

## 8. Backward compatibility and migration

### 8.1 Schema

- ISAC_METRICS stays schema 1. New keys are additive.
- Consumers ignore unknown keys. audit_c cites an "obs.h policy" for this. rxparse.py is tolerant (audit_c section 1).
- Fields that are absent read as None, not 0 (rxparse rule, audit_storage section 2).

### 8.2 Log lines

- No existing log line is reformatted.
- Parsers that depend on text: rxparse.py (regex at rxparse.py:283 for PDSCHQ, and about 50 other regexes at lines 248-309), tools/ota_stats.py, tests/passive_rx/p99_stats.py, tests/passive_rx/scan2_stats.py, goal_metrics.py (EVMGRANT, UL_E2E, RECOVLATE), dgx/score_rx.py, tools/highmcs2_data/ev.py (generic key=value over EVMGRANT).
- Rule (audit_c section 10): add new lines or keys rather than reformat.
- The exporter parses EVMGRANT generically as key=value (audit_storage section 10). That survives new fields.

### 8.3 Legacy latency names

Proposal from this audit, not from plan.md. Label [PLANNED]:

- Keep PDTIMCLS, PDTIMPCT, EVMGRANT dec_us, EVMGRANT e2e tot, and the pdschq_latency keys under their current names.
- Any corrected definition gets a new explicit name. Example: a pure-FEP histogram needs a name that is not enqueue_fep, because enqueue_fep includes queue wait.
- Document each name with its start point, end point, clock, and percentile convention (section 4).

### 8.4 Old and partial logs

- Logs before 2026-10-20 lack e2e= and PDTIM defaults. Parsers treat absent fields as None (audit_storage section 11).
- goal_metrics RECOV regex and the EVMGRANT snr_l field work only on stage 2 era logs (audit_storage section 11).
- Logs from before the stage 2 build may lack PDTIM lines (audit_storage section 0, item 9).
- The exporter must accept absent fields and say so in the manifest.

### 8.5 Dead and stale items

| Item | State | Action in sources | Ev |
|---|---|---|---|
| obs.jsonl (ISAC_OBS_PATH) | No writer in HEAD. campaign.py still sets the variable. monitor and health_snapshot still read it | Do not rely on it. Removal not decided | [CODE-READ, 3f0d921] |
| obs_pushed, obs_written, obs_dropped | Always 0 | Keys stay until removed | [CODE-READ, 3f0d921] |
| gpu_fep_* keys | Forced to 0 | Keys stay until removed | [CODE-READ, 3f0d921] |
| DCIRAW (tools/ota_stats.py RE_DCIRAW) | No producer in C | | [CODE-READ, 3f0d921] |
| ISAC_UL_LLR_DUMP | nr_ul_llr_dump.c does not exist. Header only. Inert | | [CODE-READ, 3f0d921] |
| "MIB fields" dashboard regex | No such line in C. Dead | | [CODE-READ, 3f0d921] |
| TSYNC_OBS, CFOTRK, SHIFTCENSUS, SCANQ standalone line | Not in HEAD source | | [CODE-READ, 3f0d921] |
| td_fb_* as log text | Keys only. The log tag is FIELDBOOK | | [CODE-READ, 3f0d921] |
| ISAC flags | 67 ISAC_* getenv names mirrored in tests/passive_rx/monitor/isac_flags.json. Names set with setenv in main() are not listed | | [CODE-READ, 3f0d921] |
| Stale PROJECT_MEMORY lines | PROJECT_MEMORY.md:758 and :1295 (obs), and "every 20 s" (cadence) | Fix in the doc update, section 7.7 | [CITED, audit_c and audit_lifecycle, not re-checked] |

### 8.6 Dashboard compatibility

- Dashboard discovery globs match `*metrics*.jsonl`, `*obs*.jsonl`, `*records*.jsonl`, `*.log`, and `*cfr*` in a run dir.
- New exporter outputs must not use those names inside a run dir (audit_storage section 10). Use a separate --out DIR.
- With stdout piped the dashboard cannot find its run dir (section 6.2.12).

### 8.7 Campaign convention

audit_storage section 10 recommends extending campaign.py (manifest.json, runs/NNN_<arm>/, atomic run.json, index.jsonl, verdict.json) rather than inventing a layout. The plan does not use campaign.py. This is an open decision.

### 8.8 Tests

- Python tests are unittest (stdlib). Only nr_dashboard_spawn is wired into ctest (CMakeLists.txt:2406-2409). Wiring new Python tests needs an add_test line, guarded inside the same if block (audit_storage section 9).
- The plan's tests are not yet in ctest.
-
### 8.9 Migration order (proposal, not in plan.md)

1. Add the new ISAC_METRICS keys in nr_passive_metrics.h, nr_passive_metrics.c, and nr_passive_metrics_json.c, in a separate buffer. Add a unit test. Check line size against the 16384-byte limit.
2. Write the exporter parsers (parse.py) from real format strings. Test them on existing logs and synthetic fixtures.
3. Add the policy layer (policy.py) and adversarial privacy tests before any persisted output.
4. Add hist.py and the aggregators. Test merge and quantile.
5. Write the exporter CLI and the commercial stdin-only mode.
6. Write the docs in section 7.7 and the PROJECT_MEMORY row.
7. DGX or X410 validation (section 7.10). Not done here.

Each step needs its own check. The sources do not give acceptance criteria.

## 9. Not checked by the sources

These are gaps in the evidence, not in the design. Each item is marked "not checked" or "not stated" in the sources.

- Whether the PDTIM and ISAC_METRICS overhead is small. No source measures it.
- Whether a blocked stdout pipe stalls the receiver. Not analysed (6.2.11).
- Whether SIGPIPE or an exporter exit affects the receiver. Not analysed.
- Whether dlsym results are cached in ISAC_METRICS collect (6.1).
- Whether TSAN has been run on the PDTIM globals (4.13).
- Whether the dashboard overhead figure (1.2 percent) is current. It is cited from a report not checked here (audit_storage section 2).
- The producer of cpu.txt used by scan2_stats.py (1.10).
- File mode of GPU dumps (5.11).
- Clock of RECOVLATE tot_us, UL_E2E arr2deq, and UL_PUSCH_CRC t= (1.4, 1.7).
- Clock of the timing anchor timestamp field (1.9).
- Whether lab-mode datasets keep RNTIs (7.6).
- Whether the PROJECT_MEMORY section 11.13 row already exists. The audit did not read PROJECT_MEMORY beyond section 0.
- All statements from PROJECT_MEMORY outside section 0. Not read in full. CLAUDE.md asks for a full read. This audit did not do it.
- Exact unit-test file name for the C change (7.4).
- Exact keys for recov_* and retry_yield_* (7.4).
- Whether the audit's sandbox numpy and pandas import results hold on the DGX (2.10). They are sandbox results.
- Whether the DGX OTA launcher exists in this repo. audit_storage says it does not.
