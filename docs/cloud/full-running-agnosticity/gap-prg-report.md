# Round 3 PRG lane — Codex

## 2026-09-27 22:46+02:00 — bounded read-only feasibility audit

Status: fixture development required; no RED, build, unit-test run, radio, commit or G1–G6 pass. Per orchestration hold, neither host lock was acquired. Owned receiver worktree is only sens6 `/home/sens/NICOLA/agn-wt/gap-prg`, clean branch `sdd/gap-prg` at `239eb144acd398968d5869a75e083e540ddefa26`. No local receiver worktree was created for PRG. Only this report is changed by this lane.

### Receiver path — present, live efficacy not established

- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1480-1508`: DCI1_1 and eligible RNTI only, per-RNTI arms `{wideband,2,4}`; default arm0 preserves original allocation. `ISAC_PRG_SWEEP=0` disables it. Explicit PRG and GPU-supplied LLRs bypass the sweep; live arm provenance must identify CPU/GPU route.
- `nr_arm_sweep.h:149-248`: 32 incumbent trials with independently healthy link and <=8 CRC passes trigger exploration. Wilson separation selects a winner; 384 exploratory outcomes without separation latch arm0. These are code constants, not measured receiver results. Existing 16 arm-sweep unit cases test synthetic success/failure sequences; they do not prove waveform channel estimation.
- `nr_scrambling_id_sweep.h:76-92`: link health requires a recent common-class CRC or another RNTI's dedicated CRC within256 noted outcomes. A live fixture must measure that evidence rather than force it. `nr_pdsch_passive_decode.c:3482-3490` emits exploration/latch telemetry.
- `nr_pdsch_prb_set.c:150-156` splits on CRB-aligned PRG boundaries; segmented decode at1524 onward performs separate channel estimation and data-order gathering. Segmented PT-RS is explicitly rejected at1602; CSI-RS parity mismatch rejected at1648. Fixture stages should first isolate PRG without these interacting features, then measure coexistence separately; no unsupported grant may be counted as a PRG success.
- Do not require recovered size4 exactly when size2 also decodes: finer segmentation may be observationally equivalent. Require justified CRC recovery with bounded cost and report the actual selected size. Wrong exact-size certainty would exceed the receiver's evidence.

### Fresh OCUDU source findings

Inspected `/home/sens/NICOLA/repos/ocudu-test` at `153246e00d60a9333fbb1b8073e1fae9bd9d695f`; inherited dirt is `tests/unittests/scheduler/support/isac_test_knobs_test.cpp` plus untracked `build/`. Neither changed. `git grep -n ISAC_OCUDU_TEST_PRG -- apps include lib tests` produced zero matches (exit1): the requested knob does not exist.

1. Scheduler `lib/scheduler/ue_context/ue_channel_state_manager.h:51-64`, `get_precoding()`: sets `nof_rbs_per_prg=nof_rbs`, appends exactly one recommended PMI. One-port case returns no precoding object.
2. `include/ocudu/fapi/p7/messages/tx_precoding_and_beamforming_pdu.h:12-21` has one `prgs_info prg`, not a vector. Builder `.../builders/tx_precoding_and_beamforming_pdu_builder.h:33-37`, `set_pmi()`, overwrites `pdu.prg.pm_index`.
3. `lib/fapi_adaptor/mac/p7/pdu_translators/pdsch.cpp:82-103`, `fill_precoding_and_beamforming()`: loops all MAC `prg_infos` but repeatedly calls that single-field setter. Merely adding scheduler entries would retain only the last PMI.
4. `lib/fapi_adaptor/phy/p7/pdu_translators/pdsch.cpp:174-175`, `convert_pdsch_fapi_to_phy()`: unconditionally constructs `precoding_configuration::make_wideband(single PMI)`; PRG size is not used to construct the PHY precoding grid.
5. **Additional size2 blocker:** `include/ocudu/ran/precoding/precoding_constants.h:15-18` sets `MIN_PRG_SIZE=4`, `MAX_NOF_PRG=ceil(MAX_NOF_PRBS/4)`. `include/ocudu/phy/support/precoding_configuration.h:41-70` asserts both size and count. A PHY-only n2 override still fails unless these limits and dependent capacity tests are addressed.
6. PHY already has a per-PRG coefficient container (`precoding_configuration.h`, PointA-anchored) and PDSCH processor passes the same `pdu.precoding` into DM-RS generation (`lib/phy/upper/channel_processors/pdsch/pdsch_processor_impl.cpp:123-138`). This makes a narrow test-fixture injection plausible without implementing adaptive per-subband CSI/scheduler selection.
7. RRC factory `lib/scheduler/config/serving_cell_config_factory.cpp:84-106` selects static wideband for noninterleaved mapping; n2/n4 must also be advertised consistently to the **active UE**. Changing only PHY while advertising wideband would create an invalid stimulus. This RRC configuration must never seed the passive receiver.

### Multi-stream bed gaps

- Canonical broker `/home/sens/NICOLA/rfsim-integ/tests/passive_rx/ocudu/ocudu_zmq_broker.py:86-91,114-140` exposes six scalar endpoints and one DL/UL/passive queue/index set. Harness `run_ocudu_passive.sh:600,640-641` checks six fixed ports and passes one RX/TX channel to the passive. It is not a 2x2/4x4 passive fixture.
- Existing `/home/sens/NICOLA/repos/srs-ue/utils/zmq_mimo_relay.py` provides2 DL channels and a2x2 mixer, but no passive fanout, UL observation, passive consumption clock, or4-channel path. Its rendezvous is per received block, with timeout fallback to unmixed forwarding (`pump():149-161`); it also uses `min(len(xbuf[0]),len(xbuf[1]))` without retaining either longer tail. Thus unequal message sizes lose samples, and fallback cannot silently count as valid fixed-channel G4. Reuse ideas, not inherited validation claims.
- `configs/gnb_zmq_n78_tdd_2x2.yaml` supplies two radio streams. Its comment saying srsUE only decodes rank1 is stale relative to current `repos/srs-ue/lib/src/phy/phch/pdsch_nr.c:550-587`, which contains multi-layer equalization and layer de-mapping. No new multi-layer live capability is claimed. Rank1 over2/4 TX and RX ports is sufficient to isolate changing precoders; spatial rank is a separate variable and must be reported explicitly.

### Route verdict and falsifiable next steps

**Preferred bounded route: test-only PHY fixture, not a full scheduler feature.** Keep stock scheduler/FAPI behavior when knob unset; with knob2/4, apply deterministic distinct normalized precoding matrices at CRB-aligned boundaries to dedicated PDSCH and its DM-RS, preserve rank/TBS/resources, and publish matching static bundling to the active UE. Gate strictly to the intended dedicated grants so common/control allocations stay stock. No dynamic-bundling or CSI-driven scheduler implementation is required for this fixture. Before any edit, independently review legal bundling encoding and the dedicated-grant discriminator.

Test-first plan (not executed):

1. Extend PHY-adaptor unit fixture `tests/unittests/fapi_adaptor/phy/p7/pdu_translators/dl_pdsch_pdu_test.cpp`: given knob4 and a two-port dedicated PDU spanning multiple CRB groups, assert multiple normalized, distinct matrices and preserved all-other-PDU fields. Current wideband path must fail this assertion. Add knob2 assertion/capacity boundary case, unset/invalid knob byte-identical control, and common-PDU unchanged case. A compile-only missing helper is not the requested behavioral RED.
2. Add waveform/resource-grid check with real PDSCH+DM-RS mapper: independent per-PRG projection confirms data and pilots use identical matrices, correct group origin for nonzero BWP start, and no discontinuity outside PDSCH. Exercise2/4 ports. This catches a superficially correct container with wrong sample output.
3. Multi-stream broker self-test before radio: distinct sample-index ramps on every antenna, unequal message sizes and asynchronous request order; assert exact alignment and no loss for UE/passive, shared backpressure, late join, failure/restart,2 then4 streams. Any fallback/drop makes the run VOID.
4. Receiver offline test feeds actual encoded IQ through existing passive decode and checks bytes/CRC, never assigns `freq_alloc.prg` from fixture truth. Wideband-control path remains identical; changed-precoder fixture must supply valid independently observable link-health CRCs, then demonstrate trigger/recovery. If all arms already decode, it is not a receiver RED: report robustness and no justification for forcing segmented decode. Existing Boolean bandit tests are not a substitute.
5. Only after gates/build scheduling:2x2, then4x4, each size2/4 with n>=3 alternated sweep-off/on300s arms, plus stock wideband no-regression controls. Pin tree/binary/config/scorer SHA; record active UE attachment, independent mapped-grid truth, per-grant CRC/TB bytes, link-health trigger, selected arm, CPU/queue drops and unsupported counts. Use current ownership-safe runner semantics and host flock, never legacy cleanup.

Estimate (engineering estimate, not measured duration): narrow OCUDU fixture+unit/grid tests0.5–1 day;2/4-stream passive broker/harness and alignment tests1–2 days; receiver offline integration and live campaign another0.5–1 day. Full scheduler→FAPI per-PRG extension would add multi-day scope and is unnecessary for this test request. **Overall live requirement is multi-day from the present bed, so stop/report per lane instruction; do not start production edits or radio implicitly.** A separately authorized, bounded fixture-only milestone is the next useful action.

### Provenance SHA256

- Receiver `nr_arm_sweep.h`: `4e43c74d45242462f505e0cbec16c0447505e0600d6b997b8ec9d557847e3081`.
- Receiver `nr_pdsch_passive_decode.c`: `e0460700b2e486f240ea489ff73591c057a5ec6ba28134a403159dbfc69923ff`.
- OCUDU `ue_channel_state_manager.h`: `68a72d0180f520c7d9e98f8dff8671a8f909f24f6476468b67c416643ac0baef`.
- OCUDU FAPI message: `4ea748d5718d68b01ddd403249a53907724dfa2d53d4506ef51469f9d6b71fba`; builder: `d806dd26a868535a24709ec1f1af3c1e55c198a15a0a8995916e6d1a81967b50`.
- OCUDU MAC translator: `02c3927b2143d2997815a854c37f278b199d4395461dc70a2f6a149febb4ba6f`; PHY translator: `653647f4e9ff55b348c5f4de623615003da2cf69502756f5517e5d9a8e2d4bad`.
- OCUDU precoding constants: `0ff7e626e691d15dcae1cfe37fbe593a24aade3d74d644f1574050f64860764e`; existing knobs: `26997c28ba93bfa4e966e0b0eb86f41c55e1c1f3449bb38556e66a9783e1f330`.
- Canonical scalar broker: `f7dca9d8c8c3168ccb4656f194f0b67e4f0cecfedf6c8eb607bd21bca0d50a80`; two-channel relay: `cf74d701e3a36b7aeea619485864b3ba9cafb02bc4fea74a4cbd9b241854cc79`.
