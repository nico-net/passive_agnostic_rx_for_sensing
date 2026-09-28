### Task 13: Blind DM-RS and data scrambling identities (DL and UL)

`rt.c:5712/5722` set `dlDmrsScramblingId = dlDataScramblingId = PCI` with a comment that this must be re-verified per deployment. An estimator already exists (`nr_dmrs_id_estimate.{h,c}`) but (a) `nr_pdsch_passive_queue.c:892` only accumulates when the TB CRC already passed — circular: with a wrong ID the CRC never passes; (b) it covers 0..1023 while `scramblingID0/1` range over 0..65535; (c) its decision is never applied. The data identity (`dataScramblingIdentityPDSCH`, 0..1023) has no estimator: only the TB CRC can tell.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.{h,c}`, `tests/nr_dmrs_id_estimate_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (~892), `nr_pdcch_blind_monitor_rt.c` (~5712-5722), `nr_pdsch_passive_decode.c` (data-ID hypothesis)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c`, `nr_pdcch_blind_monitor.c:4310` (UL: same, falls back to PCI today)
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_scrambling_id_sweep.{h,c}` + `tests/nr_scrambling_id_sweep_test.cc`

**Interfaces:**
- Produces:
  ```c
  /* DM-RS estimator gains a search range; accumulate() no longer needs a CRC. */
  void nr_dmrs_id_set_range(nr_dmrs_id_state_t *s, uint32_t first, uint32_t count);  /* default 0..1023, stage 2 = 1024..65535 */
  /* Data identity: ordered TB-CRC hypothesis walk. */
  typedef struct { uint16_t order[1024]; int n, pos; int latched; uint32_t tries; } nr_scrambling_id_sweep_t;
  void nr_scrambling_id_sweep_init(nr_scrambling_id_sweep_t *s, uint16_t pci, int dmrs_id /* -1 unknown */);
  int  nr_scrambling_id_sweep_current(const nr_scrambling_id_sweep_t *s);   /* ID to decode with */
  void nr_scrambling_id_sweep_feed(nr_scrambling_id_sweep_t *s, int tb_crc_ok);
  ```

- [ ] **Step 1: Failing tests.**
  - `nr_dmrs_id_estimate_test.cc`: `FindsAnIdAboveTheOldRange` — synthesize DM-RS with N_ID = 40000 (use the test file's existing synthesis helper), set the range to stage 2, accumulate WITHOUT any CRC input, assert `decided` and the id. `WrongRangeDoesNotDecide` — N_ID 40000 with range 0..1023 → not decided (margin gate).
  - `nr_scrambling_id_sweep_test.cc`: `OrderIsPciThenDmrsIdThenRest` (pci 64, dmrs_id 700 → order[0]=64, order[1]=700, then 0..1023 without duplicates, n=1024); `LatchesOnFirstCrcPass` (feed 0 three times then 1 → latched = order[3], `current` stays there); `DmrsIdAboveDataRangeIsSkipped` (dmrs_id 40000 → order[1] = 0).

- [ ] **Step 2: Implement** `nr_scrambling_id_sweep.c` (pure) and the estimator range. For the estimator's cost at 65536 candidates: run stage 2 only if stage 1 (0..1023) fails its margin, on the queue thread, throttled to one accumulate per N jobs — measure µs/accumulate in the test and put the number in the commit.

- [ ] **Step 3: Wire DL.** Queue ~892: drop the `crc &&` precondition (DM-RS-only; keep the `dmrsConfigType == 0` guard — the estimator is type-1 only, record type-2 as a known limit in the out-of-scope table). Keep one estimator state per nSCID value seen in the DCI (scramblingID0 vs scramblingID1). rt ~5712: use the decided ID for the job's nSCID if decided, else PCI. rt ~5722 / decode: take the data ID from a per-RNTI `nr_scrambling_id_sweep_t` in `rnti_dec_t`, seeded `(PCI, decided DM-RS ID)`. **Only advance the data-ID sweep for an RNTI whose Technique D context has converged and whose CRC rate is 0 over ≥ 20 TBs** — otherwise a config mismatch would be misread as a scrambling mismatch and the walk would burn 1024 LDPC decodes for nothing.

- [ ] **Step 4: Wire UL** the same way in `nr_pusch_passive_decode.c` (DM-RS estimator already included there — check what it does with the decision; apply it to `ul_dmrs_scrambling_id`/`pusch_identity`, and the data-ID sweep to `data_scrambling_id`), replacing the PCI fallback at `nr_pdcch_blind_monitor.c:4310` only when a decision exists.

- [ ] **Step 5:** Build + `ctest -R 'test_nr_dmrs_id_estimate|test_nr_scrambling_id_sweep|test_nr_pdsch|test_nr_pdcch_blind_monitor'`; commit `Passive DL/UL: discover DM-RS (0..65535) and data scrambling identities instead of assuming the PCI`.

---

