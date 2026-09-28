# Round 3 CBG lane — read-only audit

Date: 2026-09-27. No edits, build, lock acquisition, or radio run.

## Verdict

The passive receiver has no per-CBG PDSCH decode/reassembly path. DCI width can reserve CBG bits,
but the DL extractor discards CBGTI+CBGFI, and the returned grant / PDSCH job carries no CBG bitmap,
flush state, or per-CBG history. Existing decode/HARQ is whole-TB only. This is a precise offline
receiver gap, but neither current simulated gNB route can emit CBG retransmissions, so live
validation is unavailable and no implementation was attempted in this audit.

## Spec contract

ETSI TS 138 214 V17.16.0, §5.1.7.1–.2: CBG count is `M=min(N,C)`; initial TB transmission may be
assumed to contain every CBG; on retransmission CBGTI marks which groups are transmitted, CBGFI
determines whether same-CBG soft information is combinable or may have been flushed, and each CBG
contains the same code blocks as the initial TB. The primary source is [ETSI TS 138 214 V17.16.0,
§5.1.7](https://www.etsi.org/deliver/etsi_ts/138200_138299/138214/17.16.00_60/ts_138214v171600p.pdf).

Implication for future falsifiable tests: initial-TX group payload/codeblock identity must be retained;
retransmitted CBGTI-selected groups must map to those same code blocks, CBGFI=1 may combine with the
matching group history, and CBGFI=0 must not reuse potentially flushed same-group LLRs. TB CRC success
alone does not establish these per-group invariants.

## Receiver source evidence (sens6 worktree)

Owned tree `/home/sens/NICOLA/agn-wt/gap-cbg`, branch `sdd/gap-cbg`, HEAD
`239eb144acd398968d5869a75e083e540ddefa26`; clean at start and end.

- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:3987` consumes
  `f.cbg` with `(void)read_field(...)`; it neither extracts nor retains CBGTI/CBGFI. The DL
  `nr_pdcch_blind_result_t` in `nr_pdcch_blind_monitor.h:165-205` has no CBG mask/flush members.
- `nr_pdcch_blind_monitor_rt.c:6198-6367` constructs a whole PDSCH PDU/job from decoded MCS, RV,
  NDI, HARQ PID, and allocation, but no CBG metadata. Deferred decode receives the whole grant and
  `nr_pdsch_passive_queue.c:1154` invokes the whole-TB decoder/soft-combining path.
- `nr_pdcch_blind_dci_size()` documents CBG width as zero because its `pdsch_CGB_Transmission`
  assumption is NULL and explicitly says that assumption was not independently re-verified
  (`nr_pdcch_blind_monitor.c:2981`). `cbg_bits` can describe DCI width, but that is not CBG decode.
- Existing `nr_pdcch_blind_monitor_test.cc:174` only packs a zero-valued CBG field; other `cbg_bits`
  hits are defaults or UL layout cases. No test checks DL CBGTI/CBGFI extraction, partial-CBG
  selection/reassembly, initial-TX identity, or flush behavior.
- Existing bounded reserved-MCS parameter history is `nr_harq_init_tx.h`: 64-entry LRU keyed by
  `(RNTI,HARQ PID)`, NDI-gated; it stores MCS/TBS/base graph parameters, not code blocks or per-CBG
  LLR/state. The LDPC accelerator tag in `nr_pdcch_blind_monitor_rt.c:123-134` uses a 16-process
  stride and `harq_pid % 16`; if a 5-bit DL HARQ PID is enabled, PID 0 and 16 alias. Current default
  is four bits, so this is a conditional extension risk, not evidence of a current 16-process-bed
  failure.

Relevant SHA256s:

- `nr_pdcch_blind_monitor.c`: `b5a0ebdba1a964e74becf12d3fc91ae33eb6499f4e2ffd75ead31f2d5092f305`
- `nr_pdcch_blind_monitor.h`: `1c4773e28d3dfae28e9ab643c6b629c41f4ec6e51c985ce96d3901178e770f5f`
- `nr_pdcch_blind_monitor_rt.c`: `91cbc770ce051d671d455ab38b986d024567f132b8765afa250253d64131e9ed`
- `nr_pdsch_passive_decode.c`: `e0460700b2e486f240ea489ff73591c057a5ec6ba28134a403159dbfc69923ff`
- `nr_harq_init_tx.h`: `3bdd1d56b593d41f2de1c3c8b650e130b3007301b76ab64cb6709d2d653e7ae8`
- `tests/nr_pdcch_blind_monitor_test.cc`: `f756f057ec8e4e1be95b4590bd330a82d3e63b31f277a245aaf0eefd55330aeb`
- `openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c`: `032595b11661b3f10b1ebb580a9d9a259031bb0a9f67f55c1b2db08a1cd46eee`
- `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_primitives.c`: `f3e36c8bcce036ef92d0fd5abd1835cd2808dd094ece2252a7b30f59fa12f9c0`
- `tests/passive_rx/gnb.sa.rfsim.conf`: `64867daf01b087eaae8da7180622e6323bfeb0a0c642d6c8b8a3e461c7405b32`

## Simulated gNB capability

- OAI rfsim: the current gNB `nr_radio_config.c:3591` sets
  `pdsch_servingcellconfig->codeBlockGroupTransmission = NULL`; this fixture's DCI size is therefore
  the no-CBG case. The scheduler copies CBG config only if an RRC CBG config exists. Current OAI
  rfsim cannot produce CBG-based retransmissions on this route.
- OCUDU at `/home/sens/NICOLA/repos/ocudu`, HEAD `9e7afd24b43c0fe2a04b296cdeeebdb1116ecd05`, was
  inspected read-only (existing dirty configs and untracked `build/` preserved). Its RRC ASN.1 and
  config conversion can represent PDSCH CBG setup (`lib/scheduler/config/ue_configuration.cpp:333-340`),
  and DCI packing supports optional CBGTI/CBGFI, but the live DL scheduler `build_dci_f1_1()` has an
  explicit TODO at `lib/scheduler/support/dci_builder.cpp:253-256` to set `cbg_transmission_info`;
  its UL builder also lists CBG as TODO at lines 406-412. Search of `lib/scheduler` found no other
  writes to those DCI fields. ASN/config representation alone is not transmitter support. Current
  OCUDU ZMQ bed likewise cannot be used for live CBG evidence.
- No CBG-enabled config exists in the retained `rfsim-local`/`rfsim-val` passive fixtures. Those trees
  have unrelated dirty/untracked state; they were not edited.

OCUDU source SHA256s:

- `lib/scheduler/support/dci_builder.cpp`: `0ec9edf4703415b198d516bb1df7e3d741a007274d28c6cfaf60aae738f84536`
- `lib/scheduler/config/ue_configuration.cpp`: `a0254990fc1c66e2ee924e34f05067f2add03af9170c60ea932baa1b96039c1b`
- `lib/ran/pdcch/dci_packing.cpp`: `4b965a1f58e9234b533db49d630bd20fea94de1a6c568ac00d4303263485c84a`
- `include/ocudu/ran/pdcch/dci_packing.h`: `25e331a813652283c97a47c6db4727a2b17a4203bcce724476f9e902f7f18fe5`
- `tests/unittests/ran/pdcch/dci_packing_test.cpp`: `71ca96df9c837ec068b5751275e16c5b03896622fcd9d972f72ed1853aa10afc`

## Initial audit disposition (superseded by checkpoint below)

No production implementation was attempted in the initial audit. Live validation is unavailable
until a simulated gNB emits non-all-ones CBGTI retransmissions and CBGFI transitions.

## Offline design + source-contract RED checkpoint — 2026-09-27 23:17 +0200

Parent/operator clarification: lack of a simulated emitter blocks LIVE CBG only; offline receiver
implementation/tests remain required. Scope at this checkpoint is tests and design only; production
files are unchanged. Worktree `/home/sens/NICOLA/agn-wt/gap-cbg` remains at
`239eb144acd398968d5869a75e083e540ddefa26` (`sdd/gap-cbg`). `git status --short` is exactly one
untracked test: `?? openair1/PHY/NR_UE_TRANSPORT/tests/cbg_contract_test.py`.

Added `openair1/PHY/NR_UE_TRANSPORT/tests/cbg_contract_test.py`, SHA256
`5b9fa49ed65704cda9d1cc9328d0950e1d24fb7aec8ba16dbf3b2847ff81a6b9`. Running
`python3 -m unittest -v openair1/PHY/NR_UE_TRANSPORT/tests/cbg_contract_test.py` exits 1 with
6/6 expected failures; captured output `/tmp/cbg-contract-red.log`, SHA256
`c22c377015128e8ba67ed6d9748be5bf28200503cf4774210c386e5a1ac057ef`. Failures independently show
missing decoded CBGTI/CBGFI fields, dropped decoder/runtime/queue metadata, missing bounded per-CBG
state, missing explicit full-PID-safe bounded history, and absent bit-identity/selection/flush tests.
This is deliberately labeled a *source-contract RED*, not behavioral or executable state-machine
coverage: no helper/production code exists yet, and no behavior has been passed off as tested.

### Proposed implementation contract for review (not implemented)

1. Decode CBGTI and CBGFI separately from the DCI layout; preserve their widths/configuration as well
   as values. Carry them through blind result -> normal decoded grant -> deferred queue job. Do not
   reinterpret a combined opaque field as two values without resolving its bit layout.
2. Use the standards-defined code-block/CBG mapping (38.214 §5.1.7 and the associated 38.212
   segmentation), not equal-sized arbitrary chunks. On an initial/new-NDI TB, all configured CBGs
   are present; cache reference code-block/TB bits to check that a later reconstructed TB is
   bit-identical before accepting CRC success. On retransmission, CBGTI=1 selects present groups;
   CBGTI=0 groups are absent and must not be decoded as erasures from this transmission.
3. Keep soft history bounded and independent for `(RNTI, codeword, HARQ PID, NDI, CBG index)` with
   at most 64 LRU contexts, full 5-bit PID range 0..31, and at most the configured 8 CBG slots per
   codeword. New NDI flushes the process. Ensure decoder/accelerator softbuffer tags encode the
   whole key without `%16` aliasing or cross-CBG collision; eviction clears all referenced soft
   state.
4. CBGFI=1 permits combining selected groups with matching prior same-CBG history; CBGFI=0 first
   invalidates/flushed history for each selected group, then processes fresh LLRs. It must not flush
   unselected groups. TB CRC is evaluated only after all required code blocks/groups are assembled.
5. Required executable tests: CBGTI/CBGFI DCI widths/field preservation; selected-group retransmission
   skips unselected groups and reassembles the original TB; CBGFI=0 flushes only selected-group LLRs
   while CBGFI=1 combines; PID 0 and 16 remain distinct and PID 31 is in-bounds; 64-context eviction
   is bounded and cannot alias; initial full-TB decode and later CBG reconstruction are bit-identical.
   Initial TX invariants must be asserted against deterministic payload/codeblock vectors, not a
   mock which simply returns the expected TB.

### Open design questions before production edits

- Blind decoding must infer which DCI layouts/CBG widths and CBGFI presence are plausible without
  reading gNB configs/logs/SIB1. A non-blind RRC configuration source is not assumed. The current
  `cbg_bits` width hypothesis alone does not establish CBG count `N`, codeword count, or CBGFI
  presence; confirm the legal hypothesis set and how ambiguity is represented before designing the
  production API.
- Confirm the existing LDPC softbuffer ownership/lifetime and accelerator tagging contract before
  choosing whether bounded CBG state owns LLR buffers or holds references. Eviction/NDI/CBGFI flush
  must release exactly the matching history and must be safe for queued decode jobs.
- The test just added is intentionally structural. Before claiming G1-G3, replace/extend its
  behavior assertions with tests linked to the production CBG state helper and actual decode/reassembly
  contracts. No build, radio, commit, or G1-G6 claim is made here.

No live CBG run was attempted because both inspected simulated gNBs lack a CBG retransmission emitter.
No production file changed; test/design review is the next gate.

## Independent upstream-blocker / backend audit — 2026-09-28

Read-only audit of the same `sdd/gap-cbg` tree at `239eb144acd398968d5869a75e083e540ddefa26`.
No build, test execution, radio activity, source/test edits, or commit. The only worktree delta remains
the inherited untracked structural test above. This audit confirms the blocker and narrows the
minimum viable implementation contract; the earlier “per-CBG state helper” proposal is incomplete
unless arbitrary segment selection and CBG-aware rate matching reach every decoder backend.

### Spec semantics and bit order (measured against primary ETSI PDFs)

- TS 38.214 V17.16.0 §5.1.7.1: `M=min(N,C)`; `N` is configured maximum CBGs/TB and `C` comes
  from TS 38.212 §7.2.3 segmentation. Let `M1 = C mod M`, `K1=ceil(C/M)`, `K2=floor(C/M)`.
  For `M1>0`, CBGs `[0,M1)` each contain `K1` consecutive CBs starting at CB0; the remaining
  `M-M1` CBGs each contain `K2` consecutive CBs. When `M1=0`, all M groups have K2 CBs. This is
  contiguous, near-equal grouping, not a free partition. §5.1.7.2: initial NDI may assume all groups
  present; on retransmission CBGTI bit 1 means present, 0 absent; CBGFI=0 says earlier received
  instances of the *transmitted* same CBGs may be corrupted; CBGFI=1 says they are combinable;
  retransmitted CBGs contain the same CBs as the initial TB.
- CBGTI order is significant: §5.1.7.2 says for two configured codewords the first N bits from the
  field MSB belong to TB1 and the second N to TB2; within a TB the first M bits map in order and
  MSB maps CBG#0. TS 38.212 V17.10.0 DCI 1_1 field definition says CBGTI width is 0 or 2/4/6/8
  based on the configured maximum CBG count and max codewords; CBGFI is a separate trailing 0/1-bit
  field controlled by `codeBlockGroupFlushIndicator`. The receiver must carry the layout hypothesis
  (`N/codeword count`, `CBGTI` width, CBGFI presence) separately from the raw combined field width.
- Source corroborates field order: `nr_pdcch_blind_monitor.c:3127` reads the low `nbits` immediately
  below `pos` and describes MSB-first field order; `:3987` consumes `f.cbg` as one opaque field, so
  no CBGTI/CBGFI split/value is retained. Blind extraction cannot take a gNB config as input under
  agnosticity rules; it needs explicit blind layout alternatives or a standards-grounded hypothesis
  source. Current `cbg_bits` is an aggregate override, not semantic evidence. Treating CBGTI bits as
  host-endian bit positions or inferring CBGFI simply from `f.cbg>0` would be wrong.
- Primary sources: [ETSI TS 138 214 V17.16.0 §5.1.7](https://www.etsi.org/deliver/etsi_ts/138200_138299/138214/17.16.00_60/ts_138214v171600p.pdf)
  and [ETSI TS 138 212 V17.10.0 DCI 1_1 / §5.4.2.1](https://www.etsi.org/deliver/etsi_ts/138200_138299/138212/17.10.00_60/ts_138212v171000p.pdf).

### Rate matching and passive-HARQ evidence

- `openair1/PHY/NR_TRANSPORT/nr_tbs_tools.c:22-37` defines `nr_get_E(G,C,Qm,Nl,r)` with
  `Cprime=C` (“assume CBGTI not present”) and distributes E over all C CBs. The passive decoder
  calls it with full `TB_parameters.C` at `nr_pdsch_passive_decode.c:1190,1203`. That is correct
  for initial all-CB transmissions but cannot describe partial-CBG retransmission.
- TS 38.212 §5.4.2.1 explicitly makes `C'=C` when CBGTI is absent, otherwise `C'` is the number of
  scheduled CBs. For an unscheduled CB, `E_r=0`; for scheduled CBs, E distribution is by scheduled
  ordinal `j` among `C'`, not original CB index `r`, with floor/ceil division of
  `G/(Nl*Qm*C')`. Thus the decoder needs a selected-original-CB list/mask plus per-original-CB E and
  packed LLR prefix offsets. Reusing `nr_get_E(G,C,...)` or just skipping unscheduled CB decode will
  miscompute E and every subsequent LLR start for many partial selections.
- Existing private passive HARQ (`nr_pdsch_passive_decode.c:1006-1044,1112-1140,1350-1410`) has one
  thread-local transient `b/c/d`, plus a global 16-entry locked/busy LRU keyed only by `(RNTI, PID)`.
  It retains soft `d` only while the *whole* TB CRC fails (`soft_valid=!ok`); decoded `c`/success bits
  and any successful CBs are not persistent. NDI transitions reset logical validity only through the
  single-entry replacement/update flow; TB CRC is evaluated only after all C segments decode and
  full reassembly (`:1286-1332`). Busy entries are skipped; eviction excludes busy entries.
- For CBG, state must persist at least per CB (soft circular buffer `d`, hard decoded codeword `c`,
  validity) and per-TB metadata (`NDI`, first TBS/segmentation, inferred CBG grouping/layout and
  completed groups). A key needs RNTI, full HARQ PID, codeword (passive path currently handles one),
  and NDI/generation. New NDI invalidates all CB state. CBGFI=0 invalidates only prior soft/hard
  state for selected CBs before decoding their current LLRs; CBGFI=1 combines selected-CB LLRs.
  Unselected CBs must not be submitted to decoder or cleared. Only assemble/check TB CRC when every
  CB has a valid decode; compare deterministic initial-TX vs reassembled TB/codeblocks in tests.
  Same-key concurrent queued jobs must serialize or be rejected without mutating state; do not wait
  under the RX producer path. Keep bounded cache/bytes, skip safely on busy/allocation failure, and
  clear all associated backend-owned HARQ slots on NDI change and eviction. Queue carries grants by
  value (`nr_pdsch_passive_queue.h:73-124`), so CBG layout/selection/flush metadata must be copied
  into the job, not referenced from producer stack/config storage.

### CPU/CUDA/AAL interface audit and implementation size

- Common interface is `nrLDPC_TB_decoding_parameters_t` in
  `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h:45-85`. Existing
  `nb_segments_to_decode` only means “decode first N segments”; all paths interpret it that way.
  CPU `nrLDPC_coding_segment_decoder.c:256-305` loops `r=0..n_dec-1`, choosing E/LLR offset by
  original r, and stores `d`, `c`, and `decodeSuccess` at original-r offsets. CUDA
  `nrLDPC_coding_cuda_decoder.c:203-258` likewise loops prefix r and creates one GPU segment per
  prefix member; AAL `nrLDPC_coding_aal.c:461-...` constructs ops for every `i=0..p->C-1`, with
  backend HARQ address `(harq_unique_pid * NR_LDPC_MAX_NUM_CB)+i` modulo device capacity. AAL also
  has distinct setup/enqueue/harvest loops over all p->C. These are three materially separate backend
  paths; changing only the CPU helper or only `nr_get_E` does not implement CBG decode.
- Smallest sound shared contract: keep `C` as full segmentation count, add a bounded selected-CB
  bitmap/list (not prefix count), full-CB E/LLR-offset metadata or a common derivation driven by
  `G,C_selected,Qm,Nl`, and per-CB clear/combine policy. Every backend must submit only selected
  original CB indexes, point d/c/decodeSuccess at original-CB slots, and use the packed LLR offset
  of that CB. `d_to_be_cleared` currently applies uniformly to every segment; for partial selection a
  TB-wide flag can remain safe only if unscheduled segments never enter the backend, while selected
  segments share CBGFI behavior. Preserve full `C` in CRC type/K'/reassembly. If unsupported backend
  combinations cannot honor arbitrary selected CBs and per-segment flush, fail closed to CPU or
  report unsupported; never silently decode as whole-TB.
- The current passive cache uses tag `2000 + PID`; decoder backends already distinguish CB state by
  original segment index, so selected CBGs should keep one TB tag and distinct original CB indexes,
  rather than inventing group IDs in `harq_unique_pid`. Audit device-capacity modulo collisions and
  clean all corresponding CB slots on eviction before claiming hardware safety. Current default
  DCI PID is 4 bits; 5-bit PID support requires range 0..31 and tag capacity/collision evidence.
- Practical size verdict: the receiver-side state-machine can be bounded, but a production-safe
  implementation is **not a light/local patch**. It changes blind DCI layout hypotheses, by-value
  grant/job contract, selected-CB rate matching/offsets, persistent CB-valid state/reassembly, the
  public decoder interface, and CPU+CUDA+AAL iteration/cleanup, with executable behavioral tests per
  backend. A CPU-only selected-CB helper would falsely imply support when CUDA/AAL can still decode
  all segments. Do not begin implementation until backend policy and blind layout alternatives are
  accepted; offline behavioral fixtures can be built first, but the inherited structural RED test is
  only source-shape coverage and is not proof of decode behavior.

Relevant current source SHA256s (sens6 worktree):

- `nr_pdcch_blind_monitor.c` `b5a0ebdba1a964e74becf12d3fc91ae33eb6499f4e2ffd75ead31f2d5092f305`
- `nr_pdcch_blind_monitor.h` `1c4773e28d3dfae28e9ab643c6b629c41f4ec6e51c985ce96d3901178e770f5f`
- `nr_pdcch_blind_monitor_rt.c` `91cbc770ce051d671d455ab38b986d024567f132b8765afa250253d64131e9ed`
- `nr_pdsch_passive_decode.c` `e0460700b2e486f240ea489ff73591c057a5ec6ba28134a403159dbfc69923ff`
- `nr_pdsch_passive_queue.c` `5d0b6348ed4f3fc63011be8c276f15926c7bbcb5f12300cec8d2444d9de89ddc`
- `nr_pdsch_passive_queue.h` `6a7765607be24ed5ff5d961de6748e0b9aab70339c60c06ef5051e8ad8df7878`
- `nr_tbs_tools.c` `ff471ff3e7d8678cea18fd36cd60bea4fa27ea2e255380705424612bb7a24584`
- `nrLDPC_coding_interface.h` `41b2bedb71a83516f6aa827288b98a357e2f1fa2b9e5b069f82f04a0f56fe7ce`
- CPU segment backend `nrLDPC_coding_segment_decoder.c` `100f00313e578858b960daf4817c2c81e39f75f3b2e344936c3366a9f8633fd6`
- CUDA backend `nrLDPC_coding_cuda_decoder.c` `680fad29e3caf2864a1f70efc60c4ff886749a39be9c123932ea52aed7e791ed`
- AAL backend `nrLDPC_coding_aal.c` `6be0f5b6fdf96f7e9a886b6030e945e2921038c24885c1e606023497cd157d0a`
