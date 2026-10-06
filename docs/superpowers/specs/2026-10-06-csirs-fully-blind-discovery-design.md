# Fully blind CSI-RS discovery (all rows 1-18) — design

Date: 2026-10-06. Branch base: `ota/x410-dgx-2026-10-06` (`04fcabbf07`). Status: approved by operator in chat
(architecture, approach A); this file is the written spec.

## 1. Goal and success criteria

The receiver discovers, with no cell knowledge and no manual knobs, every CSI-RS resource the cell transmits:
row (TS 38.211 Table 7.4.1.5.3-1, rows 1-18), frequency-domain allocation, symbols l0/l1, CDM type, density,
**RB start and extent**, periodicity/offset and scramblingID. Allowed inputs: what the receiver already derives
(PCI, frame/slot timing, the started grid, MIB facts, its own decoded DCIs).

Success (gates, in order):
- G-A offline: synthetic fixtures for every row 1-18 (random RB extent, ID, period, + PDSCH-like load with DM-RS
  and SSB) are recovered exactly; existing CSI-RS ctests unchanged with the feature off.
- G-B replay: the 2026-10-06 raw capture (`/home/nicola/NICOLA/ota_runs/raw_pci421_aligned_20261006`, PCI 415)
  replayed with **only** `ISAC_CSIRS_DISC=1` (no `ISAC_CSIRS_BLIND_RB/_ROWS/_PER_SLOT`, no
  `ISAC_PDCCH_SCAN_EVERY_SLOT`) confirms the TRS pair `1:0:52:1:4/8:…:3:415` with the right extent.
- G-C OTA: same on air, 4 RX, time from PBCH lock to all confirmations **< 5 s**, 0 RFSTALL, scan-queue drops
  not worse than the current baseline, X410 released cleanly.

Defaults stay unchanged (feature off) until G-C passes.

## 2. Architecture (approach A: footprint first, sequence last)

```
pdsch_processing() every DL/flexible slot (PHY RT thread)
  └─ U1 slot feed ──► GPU-resident ring of CRB-aligned grids [ant][14][12*N_RB] (unified memory)
                         │ (worker thread, not RT)
                         ▼
                      U2 footprint detector  (GPU kernel; CPU reference)
                         │ footprints {symbols, comb, density, RB start/extent, period, offset, strength}
                         ▼
                      U3 hypothesis builder  (CPU) → legal (row, fd bitmap, l0, l1, cdm, density) per footprint
                         ▼
                      U4 sequence sweep      (GPU kernel; CPU reference): scramblingID 0..1023 × slot offset,
                         │                    32-RE sub-band blocks over the footprint extent, 4 RX non-coherent
                         ▼ winners {hypothesis, ID, slot offset, z}
                      U5 confirm + export    (CPU, existing code): append to g_st with ID/extent/period, confirm via
                                              feed/hit/period; on confirm register in nr_csirs_monitor (dynamic) so the
                                              existing nr_ue_csi_rs_sensing_capture() decodes it and exports CFR;
                                              rate matching via nr_csirs_blind_rt_rate_match_all (unchanged).
```

### U1 slot feed (`nr_csirs_disc_feed.{h,c}`)
- Hook: `pdsch_processing()` (`openair1/SCHED_NR_UE/phy_procedures_nr_ue.c`, next to the `nr_csirs_monitor` block),
  every slot whose TDD direction may carry DL (unknown TDD ⇒ every slot). It needs all 14 symbols FEP'd: reuse
  `slot_fep_map`, FEP missing symbols only while the detector requests data (duty-cycled, see budget).
- Copies `rxdataF[ant][sym][first_carrier_offset…]` into a CRB-aligned `c16_t grid[ant][14][12*N_RB]` slot entry of a
  ring (default 640 slots, configurable), allocated with cudaMallocManaged when the GPU backend is present, else
  aligned malloc. Entry carries `abs_slot`, `frame`, `slot`. Never blocks: if the consumer has not released the
  oldest entry, the slot is dropped and counted (`csirs_disc_feed_drops`).
- Grid alignment to CRB 0 uses the started grid (as today's blind search); a grid offset is a known limitation
  logged at arm time.

### U2 footprint detector (`nr_csirs_disc_fp.{h,c}`, `nr_csirs_disc_gpu.cu`)
- Per slot, per antenna-summed RE power p(sym, k). Contrast c(sym,k) = p / median of same-symbol REs in ±1 RB.
- Per (sym, k) a time series over the ring. For each legal period P ∈ {4,5,8,10,16,20,32,40,64,80,160,320,640}
  that fits ≥ 3 occurrences in the window and each offset o: lattice score = mean c on the lattice / mean c off it.
- Clustering: REs sharing (P, o) and a 12-periodic comb within one symbol pair/quad form a footprint; RB extent =
  longest contiguous run of RBs whose comb REs pass; density from comb spacing (3 REs/RB = density 3, 1 RE every RB
  = 1, every other RB = 0.5 even/odd).
- Structural exclusions (mask before clustering): SSB REs (known), CORESET#0/PDCCH symbols in occasion slots,
  PDSCH DM-RS REs (from MIB dmrs-TypeA-Position and decoded DCIs when available) — and an intrinsic test: energy
  present in > 60 % of slots is not a CSI-RS lattice (CSI-RS period ≥ 4 slots).
- Output list sorted by strength, capped (e.g. 32 footprints).

### U3 hypothesis builder (`nr_csirs_disc_hyp.{h,c}`)
- For each footprint, enumerate all (row, freq_domain bitmap, l0, l1, cdm, density) whose RE pattern
  (via the same mapping as `get_csi_mapping_parms`/`nr_generate_csi_rs`) equals or tightly covers the footprint
  comb in the footprint's symbols; includes rows 6-18 multi-bit bitmaps and **row 9** (fix the existing matcher).
- Rank by fit quality (pattern IoU) and keep the top few per footprint. Port count and CDM are ambiguous from
  energy alone → keep all consistent hypotheses; U4 separates them by sequence.

### U4 sequence sweep (`nr_csirs_disc_sweep.{h,c}`, kernels in `nr_csirs_disc_gpu.cu`)
- For each hypothesis and each slot of the footprint lattice in the ring: generate the reference sequence on the
  device for scramblingID n_ID ∈ 0..1023 and slot-number offset δ ∈ 0..slots_per_frame-1 (c_init per TS 38.211
  7.4.1.5.2), map per the hypothesis (CDM codes per port), correlate per 32-RE block within the RB extent, combine
  blocks and antennas non-coherently, average over lattice slots. z normalised like `correlate_blocks_shift`
  (noise ≈ 1). Pattern: `idsweep_gpu.cu` (device Gold generator, one thread block per (ID, δ)).
- δ sweep only if δ = 0 fails (our slot numbering normally matches the cell's after PBCH/SFN).
- Output per hypothesis: best (n_ID, δ, z), second best z (ambiguity margin).
- CPU reference implementation of the same math (slow, used by tests and as no-GPU fallback on a slot subsample).

### U5 confirm + export (changes in `nr_csirs_blind_rt.c`, `nr_csirs_monitor.{h,c}`)
- Winners with z ≥ 4 and margin ≥ 1.5 are appended to g_st/g_zp (`nr_csirs_blind_append`) with scramb_id,
  start_rb/nr_of_rbs, and seeded period/offset; the existing feed/record_hit/infer_period confirms them on new slots.
- Scoring of appended candidates uses all antennas (non-coherent) and only the candidate's RB extent.
- IDSWEEP/pin no longer chosen by EPR; the legacy IDSWEEP is disabled when `ISAC_CSIRS_DISC=1`.
- New `nr_csirs_monitor_add_dynamic()` registers confirmed NZP resources (bounded list, removable on
  revoke/epoch), so `pdsch_processing()` FEPs and decodes them via `nr_ue_csi_rs_sensing_capture()` and CFR export
  works without a manual `csirs_monitor`.
- The CSI-RS search no longer depends on PDCCH occasions when `ISAC_CSIRS_DISC=1` (driven from U1/U5 instead).

## 3. Threads and budget (< 5 s, 4 RX real time)
- PHY RT thread: only the U1 copy (≈ 0.6 MB per slot at 217 PRB × 4 RX ≈ 0.05-0.1 ms) plus extra FEP of symbols not
  already FEP'd, duty-cycled (`ISAC_CSIRS_DISC_DUTY`, default every slot during discovery, every 8th once converged).
- `csirsDisc` worker thread (pinned, non-RT, A725 or spare X925): runs U2→U4 on GPU batches every ~50 ms of data.
- GPU budget: U4 ~ (hypotheses ≤ 64) × 1024 IDs × lattice slots ≤ 8 × ~650 REs ≈ 3.4·10⁸ MACs per batch, well
  inside GB10 in ≪ 100 ms. Convergence target: TRS-class (P ≤ 20 slots) < 1 s, P = 160 slots ≈ 3 periods ≈ 0.25 s of
  data after lock; worst legal P=640 needs ≥ 3 × 320 ms ≈ 1 s. Hence < 5 s.

## 4. Error handling
- No GPU / CUDA failure → CPU reference path on a subsample, logged once; never blocks the RT thread.
- Ring overflow → drop + counter. Epoch change / sync loss → flush ring and footprints (existing epoch hooks).
- Ambiguous winners (margin < 1.5) are not confirmed; logged as `CSIRS_DISC AMBIGUOUS`.
- All new log lines prefixed `SENSING: CSIRS_DISC`; metrics added to `ISAC_METRICS` (`csirs_disc_*`).

## 5. Configuration
- `ISAC_CSIRS_DISC=1` enables (default off). `ISAC_CSIRS_DISC_RING` (slots, 640), `ISAC_CSIRS_DISC_CORE`,
  `ISAC_CSIRS_DISC_DUTY`, `ISAC_CSIRS_DISC_BACKEND=auto|cpu|gpu`. Existing `ISAC_CSIRS_BLIND_*` knobs keep working
  but are not needed.

## 6. Testing
- Unit (gtest): U2 on synthetic grids (each row 1-18, random extent/period/offset, with PDSCH-like noise, DM-RS,
  SSB) — exact footprint recovery; U3 table tests vs TS 38.211 for all rows; U4 CPU vs GPU bit/score agreement and
  exact ID recovery; U5 integration through `nr_csirs_blind_rt_test_bridge`.
- Replay (G-B) and OTA (G-C) run by the orchestrator, never by codex agents.
- Existing CSI-RS tests (`test_nr_csirs_blind_search`, `_synth`, `_rt` bridge, observer) must pass unchanged.

## 7. Known limits
- Resources beamformed away from the receiver are undetectable (inherent).
- UE-specific aperiodic CSI-RS (no periodicity) is out of scope.
- Grid alignment relies on the started grid being CRB-aligned (true for the aligned recipe; logged otherwise).
