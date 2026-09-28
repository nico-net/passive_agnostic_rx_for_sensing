# Round 3 SSB overlap lane

## Current status

Investigating a blind PDSCH RE-exclusion gap. Worktree is clean at `sdd/gap-ssb` HEAD
`239eb144acd398968d5869a75e083e540ddefa26` (base for Round 3). No edits, build, or radio run yet.

## Retained OCUDU evidence — 2026-09-27

Read `/tmp/ocudu_passive/ocudu_dl_g4_vr4/passive.log` and the matching gNB log. The passive log
records `SYNCDIAG ... ssbIndex=0` at line 68 after blind sync; this supplies the detected SSB symbol
index. It has 492,455 lines / 70,700,054 bytes, SHA256
`eccf4eb0d4650589b17e63caf704932191f07383154c8857b10db3a9f2df7f0d`.

Validation-only gNB records 178,042 C-RNTI PDSCH grants. Parsing its `PDSCH` PHY lines against
the detected SSB's four-symbol span `[2,6)` and 240-subcarrier frequency span (the matching run's
example grant is PRB `[0,23)`, symbols `[2,14)`, at gNB log line 2972) finds 163,983 grants
overlapping both axes. The gNB log is 24,763,969 lines / 1,539,564,504 bytes, SHA256
`b1fa32178b98e07d13adcda0a285859a49185e5cf59250aaa79d390811cc030d`. This establishes that the
retained run exercises real SSB/PDSCH overlap. Its full score is not interpreted as evidence for
the SSB behavior.

## Source audit

At the Round-3 base, `nr_dlsch_extract_rbs()` in
`sens6:/home/sens/NICOLA/agn-wt/gap-ssb/openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c`
removes DM-RS and CSI-RS REs, with no SSB mask. The passive grant builder in
`nr_pdsch_passive_decode.c` supplies blind CSI-RS rate matching only. Measured SHA256s:

- `nr_dlsch_demodulation.c`: `cf81a0c7bb51b7e2ffebf23e96a8adcbddf8a576a3da4a634c3b1a9793ee08b1`
- `nr_pdsch_passive_decode.c`: `e0460700b2e486f240ea489ff73591c057a5ec6ba28134a403159dbfc69923ff`
- `nr_initial_sync.c`: `f67e9804a4e4a22fa9819762b8fc34069b1b19d63734e7aabcf49d8e599ec33c`

The source audit and retained overlap count indicate a real gap. **Retract the proposed next step above**:
there is no existing runtime seam that identifies each actual SSB burst occasion in the passive decoder,
so a minimal mask cannot yet be connected without assuming a period or taking configuration truth.

## Runtime event-seam audit — 2026-09-27

Read-only search of the Round-3 base shows:

- `nr_initial_sync.c` runs the PSS/SSS/PBCH acquisition and stores `ssb_index` and
  `ssb_start_subcarrier`, but that is an acquisition event only. In the retained run it reports
  `ssbIndex=0` once at passive log line 68.
- `nr_pdsch_passive_decode.c` has no SSB index/start coordinate use. Its extraction caller removes
  DM-RS and CSI-RS only.
- `pss_search_time_nr()` call sites are acquisition/measurement code; the passive PDSCH slot path
  does not run a PSS detector on each slot.
- `nr_process_pbch_symbol()` is called from `pbch_processing()`, but the only call to that function
  is the PHY simulation path. `get_ssb_index_in_symbol()` instead gates tracking from
  `nrUE_config.ssb_table.ssb_period` when configured or a hard-coded 20 ms default. Neither source
  is an acceptable blind phase source for this lane.

Therefore no source change or RED was made: a mask keyed to acquisition alone misses later SSB
bursts; projecting it with the 20 ms fallback or SIB1/config would violate the agnosticity rule. A
safe fix needs a separate, falsifiable per-slot burst detector that emits a blind event/phase before
the PDSCH worker consumes that slot, including a reviewed false-positive contract and symbol/frequency
coordinates. This is larger than the current extraction-only lane. G1-G3/G5 were not run; G4 was not
launched. No commit.

Evidence commands on sens6 worktree `gap-ssb` at `239eb144acd398968d5869a75e083e540ddefa26`:

- `git grep -n "pss_search_time_nr(" -- openair1` and
  `git grep -n "nr_process_pbch_symbol(" -- openair1` found no passive PDSCH call site.
- `git grep -n "ssb_index\|ssb_start_subcarrier" -- openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c`
  returned no matches.
- Relevant source SHA256s: `nr_initial_sync.c`
  `f67e9804a4e4a22fa9819762b8fc34069b1b19d63734e7aabcf49d8e599ec33c`, `pss_nr.c`
  `7c00ab0bb66c88ef8bedd57608260f09db7f2429e9b1b84ee0fe33f161e11f9e`,
  `phy_procedures_nr_ue.c` `1c6a6f56f3e6d9f0907d306227eea1129c89dc4f194fa642c54ea0a3e4732616`,
  `nr_dlsch_demodulation.c` `cf81a0c7bb51b7e2ffebf23e96a8adcbddf8a576a3da4a634c3b1a9793ee08b1`,
  `nr_pdsch_passive_decode.c` `e0460700b2e486f240ea489ff73591c057a5ec6ba28134a403159dbfc69923ff`.

## 2026-09-28 ~11:00 — G5 Important fix: production-path tests (Claude, ssb lane finisher)

Finding (G5 00:56): the 7 gtests link only `nr_ssb_rate_match.h`, so they stay green if the production
detector, `ssb_unav`/G, or `nr_dlsch_extract_rbs` packing is disconnected. Also found: the uncommitted
`nr_ssb_rate_match_test.cc` had a post-G5 draft section (`SsbProductionContract`, 4 tests) calling
functions that exist nowhere (`nr_ssb_rm_observe_pair`, `nr_ssb_rm_pack_rb`, `nr_ssb_rm_G`, ...) — it
did not compile and was still helper-only; removed.

Fix (no behaviour change intended except the two items flagged below):
- New `openair1/PHY/NR_UE_TRANSPORT/nr_ssb_rate_match.c` (in PHY_NR_UE) holds the production logic that
  was inline in `nr_pdsch_passive_decode()`: `nr_ssb_rm_candidates`, `nr_ssb_rm_observe` (PSS/SSS bin
  gather + detector), `nr_ssb_rm_plan` (masks, segmented data-order mask, `unav`, fail-closed rules),
  `nr_ssb_rm_first_data_symbol`. The decoder now calls these; declarations in `nr_transport_proto_ue.h`.
- `nr_dlsch_extract_rbs()` returns the packed RE count; `nr_rx_pdsch()` uses it for `nb_re_pdsch` when an
  SSB mask is given (deletes the duplicated DM-RS/CSI/SSB re-count loop, so extraction and count cannot drift).
- New plain-C `tests/nr_ssb_rate_match_prod_check.c` (ctest `test_nr_ssb_rate_match_prod`), linked against
  PHY_NR_UE: independent PSS/SSS generator + transmitter model -> production candidates/observe/plan ->
  `nr_get_G` -> real `nr_rx_pdsch()` per symbol; asserts every LLR sign in order, sum(dl_valid_re)*Qm == G ==
  transmitted bits. Scenes: A full-band over observed SSB (unav 960); B no SSB on air (0 events, candidates on
  data); C grant fully inside SSB PRBs (symbols 2..5 empty, first data symbol 6); D segmented/interleaved PRB list
  in a BWP starting at CRB 10; E1 DM-RS on SSB RE refused; E2 DM-RS on SSB symbol but grant off SSB PRBs decoded;
  F SI-RNTI refused; G other PCI -> no event.
- Behaviour change 1 (needed for live evidence): the detector now runs for every grant that overlaps an SSB
  candidate in TIME (frequency left to the mask). Reason: the OAI phy-test scheduler (`get_rb_alloc` over
  `vrb_map`) keeps PDSCH off the SSB PRBs in SSB slots, so with frequency gating the detector never ran in an
  SSB slot and no live SSB event could ever be logged. New named line `PDSCH SSB-OBS n= frame= slot= symbols=
  overlap_re= refused=`. The fail-closed DM-RS/SI-RNTI/PT-RS rule now applies only when the grant actually has
  SSB REs (scene E2 guards this).
- Behaviour change 2 (latent defect found): the early PSS/SSS FEP used `nr_slot_fep` even on a slot-share cache
  hit, overwriting cached symbols with a differently FO-compensated FFT (4-RX: no per-branch FO) that the main
  FEP then skipped. Now skipped on a cache hit, and otherwise uses the same FEP routine/FO as the main FEP.
Build G1 of the new targets started (flock-wrapped, lane-make) at ~11:01; RED/GREEN evidence follows.
