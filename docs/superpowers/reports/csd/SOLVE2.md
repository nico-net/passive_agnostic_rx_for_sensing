# SOLVE2 — dedicated PDSCH replay reliability

DGX aarch64 / GB10, `csd/solve2`, 2026-10-09. Offline captures only.

The all-or-nothing failure has two demonstrated persistent-state causes: noisy
known-RNTI DCIs can delete the working layout from the global HARQ heuristic,
and the default solver scrambling hypothesis can inherit a different CORESET's
process-wide identity estimate. The fixes filter evidence used for global pruning,
reopen statistically pruned layouts after credited failures, and bind each default
probe to the decoded PCI. Additional flags preserve physical-hypothesis search
progress, refresh unconfirmed per-CORESET identities, and keep discovery sampling
from starving finite verification dwells. All new behavior defaults off.

The final prospective campaign is reported separately from diagnostic and stopped
campaigns below. No radio, hardware utility, sudo, push, or merge was used.

## Final validation protocol

The fourth campaign fixes all solver flags plus
`ISAC_PDCCH_NID_GPU_RECHECK_MS=250`, retains `ISAC_PDCCH_NID_GPU_DUTY=1` and
`ISAC_PDCCH_DED_BUDGET_SHARE=2`, and uses the unchanged build-10 receiver.
It runs five old-capture replays followed by ten capture-b replays. Every run is
serialized through `heavy.sh`; no diagnostic result is substituted into the gate.
The complete final run table and per-RNTI comparison follow this protocol.

[MEASURED] **Capture b: 10/10**, using at least five RNTIs with full-TB CRC
support per run (raw solve counts: 35, 34, 33, 29, 34, 35, 34, 35, 36, 35).
**Capture 20261008: 4/5** with all four main RNTIs and full-TB support
(raw counts: 4, 4, 4, 4, 1). All fifteen runs reached EOF.

The audit checks 357 confirmation events,
including re-solves: **0 unpaired**, **0 wrong default-probe identities**,
and **0 deletions of working layouts 1664/2104**. Raw solves without later
full-TB support are listed below and excluded from the conservative gate counts.
These checks preserve the existing two-independent-CRC acceptance bar; they do
not assert that a representative layout is uniquely identified after two probes.

Per-RNTI CRC comparison uses zero for absent/unsolved RNTIs. Cohort means and best
A values are both shown; individual runs need not match the best A run. Final
means below the best A count: 0xe2de.

**Strict best-run CRC parity is not fully met:** e2de averages 5.4 full TBs
versus A4’s 7. All per-RNTI cohort means exceed the A3/A4 cohort means.
The solve-count gates pass; the CRC-yield qualification must not be omitted.

| RNTI | A3/A4 mean | Campaign 4 mean | Best A count |
|---|---:|---:|---:|
| 0x0db1 | 12.0 | 29.6 | 15 |
| 0x37f3 | 39.5 | 214.5 | 79 |
| 0x5186 | 296.5 | 474.8 | 411 |
| 0x73de | 0.0 | 0.1 | 0 |
| 0x9f86 | 19.0 | 77.6 | 30 |
| 0xc61e | 0.5 | 6.7 | 1 |
| 0xce76 | 12.0 | 37.4 | 17 |
| 0xd76e | 10.5 | 23.4 | 11 |
| 0xdb3e | 12.5 | 75.8 | 23 |
| 0xe169 | 32.5 | 52.9 | 49 |
| 0xe2de | 3.5 | 5.4 | 7 |
| 0xe513 | 1.5 | 61.4 | 3 |

| Run/log directory | EOF | Solved | With full-TB CRC | Total full-TB CRC | Unpaired locks |
|---|---|---:|---:|---:|---:|
| `replay_solve2_r4_b1_200624` | yes | 35 | 33 | 1789 | 0 |
| `replay_solve2_r4_b2_200753` | yes | 34 | 32 | 1649 | 0 |
| `replay_solve2_r4_b3_200922` | yes | 33 | 29 | 1405 | 0 |
| `replay_solve2_r4_b4_201053` | yes | 29 | 24 | 1078 | 0 |
| `replay_solve2_r4_b5_201223` | yes | 34 | 31 | 1332 | 0 |
| `replay_solve2_r4_b6_201352` | yes | 35 | 32 | 1378 | 0 |
| `replay_solve2_r4_b7_201524` | yes | 34 | 31 | 1408 | 0 |
| `replay_solve2_r4_b8_201654` | yes | 35 | 30 | 1600 | 0 |
| `replay_solve2_r4_b9_201825` | yes | 36 | 32 | 1628 | 0 |
| `replay_solve2_r4_b10_201955` | yes | 35 | 32 | 1480 | 0 |
| `replay_solve2_r4_old1_195711` | yes | 4 | 4 | 302 | 0 |
| `replay_solve2_r4_old2_195848` | yes | 4 | 4 | 300 | 0 |
| `replay_solve2_r4_old3_200149` | yes | 4 | 4 | 308 | 0 |
| `replay_solve2_r4_old4_200319` | yes | 4 | 4 | 361 | 0 |
| `replay_solve2_r4_old5_200458` | yes | 1 | 1 | 126 | 0 |

Full-TB CRC comparison; a zero includes an RNTI not discovered/solved.

| RNTI | A3 | A4 | b1 | b2 | b3 | b4 | b5 | b6 | b7 | b8 | b9 | b10 | Median |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0x0db1 | 9 | 15 | 31 | 30 | 27 | 21 | 31 | 31 | 31 | 30 | 32 | 32 | 31.0 |
| 0x37f3 | 79 | 0 | 341 | 224 | 213 | 227 | 150 | 204 | 155 | 254 | 211 | 166 | 212.0 |
| 0x5186 | 182 | 411 | 532 | 514 | 460 | 206 | 508 | 480 | 514 | 511 | 514 | 509 | 510.0 |
| 0x73de | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| 0x9f86 | 8 | 30 | 85 | 96 | 76 | 4 | 82 | 88 | 82 | 87 | 94 | 82 | 83.5 |
| 0xc61e | 1 | 0 | 11 | 10 | 0 | 0 | 4 | 3 | 7 | 11 | 11 | 10 | 8.5 |
| 0xce76 | 7 | 17 | 46 | 41 | 30 | 19 | 39 | 29 | 45 | 42 | 42 | 41 | 41.0 |
| 0xd76e | 10 | 11 | 41 | 33 | 6 | 0 | 24 | 7 | 28 | 34 | 32 | 29 | 28.5 |
| 0xdb3e | 23 | 2 | 87 | 99 | 53 | 6 | 49 | 90 | 61 | 101 | 116 | 96 | 88.5 |
| 0xe169 | 16 | 49 | 56 | 51 | 41 | 54 | 30 | 68 | 31 | 57 | 71 | 70 | 55.0 |
| 0xe2de | 0 | 7 | 10 | 7 | 0 | 0 | 7 | 4 | 7 | 4 | 10 | 5 | 6.0 |
| 0xe513 | 0 | 3 | 64 | 63 | 77 | 81 | 53 | 53 | 53 | 49 | 58 | 63 | 60.5 |

Old-capture main RNTIs, full-TB CRC:

| RNTI | old1 | old2 | old3 | old4 | old5 |
|---|---:|---:|---:|---:|---:|
| 0x8f5a | 142 | 143 | 140 | 148 | 126 |
| 0xb876 | 73 | 71 | 70 | 93 | 0 |
| 0xe01e | 50 | 48 | 48 | 43 | 0 |
| 0x13ae | 37 | 38 | 50 | 77 | 0 |

Solves with no subsequent full-TB CRC (their independent CB confirmation pairs still pass):

- b1: 0x7c8a, 0xdbc2
- b2: 0x73de, 0xdbc2
- b3: 0x7c8a, 0xacd6, 0xb733, 0xdbc2
- b4: 0x3699, 0xacd6, 0xb733, 0xd093, 0xdbc2
- b5: 0xac03, 0xb909, 0xdbc2
- b6: 0x4433, 0x62a6, 0xdbc2
- b7: 0x62a6, 0xac03, 0xdbc2
- b8: 0x4433, 0x62a6, 0x7c8a, 0xac03, 0xdbc2
- b9: 0x62a6, 0x73de, 0xb909, 0xdbc2
- b10: 0x1a61, 0x62a6, 0x7c8a
- old1: none
- old2: none
- old3: none
- old4: none
- old5: none

Campaign log list: `replays/campaign_logs.txt`; receiver hash:
`bf9601162b6ea9a4f1e1def6cd9f4bc9b5273f88be29534017498317774c39ff`.
All new behavior remains default off. The exact measured environment below
includes the shorter existing GPU recheck setting; it is necessary to reproduce
this campaign rather than the failed third campaign.


## Root cause investigation

The first prospective campaign was rejected after capture-b results **28, 0, 0**
(the queued fourth run was cancelled before starting). Its binary and logs are
preserved as `replays/campaign1_binary.sha256`, `campaign1_logs.txt`, and
`solve2_campaign1_aborted.log`. This was not an accepted 9/10 campaign.

[MEASURED] In its bad second run (`replay_solve2_b2_183641`), none of 9362 credited
search probes contained layout 1664, while the good first run probed that successful
layout repeatedly. Explicit identities and fair probing cannot recover a hypothesis
removed from the global resolver's alive set. The revised candidate reopens pruned
layouts of the active FDRA families **per RNTI** after 32 credited CRC failures.
The global resolver's heuristic state is not mutated, and a reopened hypothesis still
has to parse the current DCI and pass the existing two-CRC confirmation rule.
`ISAC_DCI11_PRUNELOG=1` records elimination counters for causal tracing.


[MEASURED, recovery campaign run 1] `replay_solve2_r2_b1_185819` exposes the
first fatal decision directly. At global observation 40, while accepting RNTI 5186
at frame/slot 741.1, the resolver removes layouts **1664 and 2104**:
`seen=29 pass=25 impossible=0 harq=3/28`. This is HARQ-sequence pruning, not a
field-domain refutation. Earlier observations mix RNTI 365e (e.g. AL2 mismatches
22, 7, 26, 4, 11) with clean 5186 DCIs. The global relative-HARQ score is not
proof that a layout is impossible for every RNTI; its irreversible removal makes
the solver's remaining thousands of probes unable to recover it.

With per-RNTI recovery enabled, the same run later records a CRC pass at signal
slot **19472**, token **9578785**, RNTI **37f3**, actual DM-RS/data identity
**415/415**, ports **0x1**, S=1, add-pos=1, table=1. Its logged layout set includes
both removed layouts. The run finishes with **23** reported solves, **20** with
subsequent full-TB CRC support; all 23 have independently audited confirmation
pairs. Thus the repaired path is exercised in a run with the bad early decision,
not merely in a run where the heuristic happened to retain the answer.

[MEASURED, deterministic counterfactual on recorded DCI sequences] The standalone
resolver trace replays the exact `DCIRAW` order with BWP 217 and length 49, with
no receiver scheduling or GPU timing involved:

| Recorded sequence | Control | First deletion of 1664 | Final alive | HARQ evidence at deletion |
|---|---|---:|---|---|
| r2_b1, 8291 DCIs | original | observation 40 | no | true 3/28; wrong layout 1509: 12/28 |
| r2_b1 | HARQ_SEQ=0 | none | yes | — |
| r2_b1 | retain mm <= AL/2 (minimum 1) | none | yes | 2433 noisy observations excluded |
| r2_b7, 8698 DCIs | original | observation 27 | no | true 3/22; wrong layout 1509: 12/22 |
| r2_b7 | HARQ_SEQ=0 | none | yes | — |
| r2_b7 | retain mm <= AL/2 (minimum 1) | none | yes | 2647 noisy observations excluded |

The original traces exactly reproduce the live pruning counters and frame/slot.
The added `ISAC_DCI11_OBSERVE_MM=1` uses the existing anonymous re-encoding
mismatch acceptance bar for global layout statistics. Known-RNTI payloads above
that bar still reach the per-RNTI CRC solver; they cannot poison the global HARQ
statistics. Layout recovery remains a bounded-failure fallback. This changes the
cause demonstrated by the recorded-input control rather than relying solely on
recovering after a late first success.

[CODE-READ; historical measured replay logs] The solver's `scr=-1` hypothesis does not
mean PCI at the decode boundary. `nr_pdcch_blind_monitor_rt.c` copies a PDU whose DM-RS
identity comes from the process-wide estimator and whose data identity comes from the
legacy data-ID walk. Only nonnegative solver identities override those fields. The
process-wide estimator pools unconfirmed layout probes from different CORESETs.
Consequently the same solver hypothesis changes meaning after a shared decision, and
can remain wrong even when the serving CORESET independently decides the correct PCI.

In the supplied B2 replay the shared estimator confirms 415 at grant 749; 11 RNTIs
report solves. In B6 it decides 442 at grant 65; the serving CORESET later decides 415
at grant 144, but its default probes inherit the shared 442. B6 has 8574 logged search
probes and no CRC pass. B3/B4 also latch shared 442 and only solve neighbor identities
that explicitly override it. Both B2/B6 lock DCI length 49. The exact inherited logs
are under `/home/nicola/NICOLA/ota_runs/solve_ab/`.

[CODE-READ] A second defect makes a wrong CORESET identity permanent: the estimator
counts multiple variants of the same DCI's pilots as separate grants and stops at its
first coherence decision. The solver then suppresses other discovered identity
candidates. No CRC failure reopens that decision.

[MEASURED in supplied logs, not a new campaign] A1's reported single solve (0x9f86,
scr=685, add_pos=2) has no subsequent full-TB CRC. A3/A4 also contain late solves with
no subsequent CRC support. Raw `SOLVED` totals alone do not establish the false-lock gate.

[MEASURED, new diagnostic replay] `replay_diag_off1_173326` reproduces the failure:
8532 credited search probes, zero solves. Of these, 7576 default probes actually used
DM-RS 442 / data 415. The serving CORESET had decided 415. The token-linked identity
log establishes the values passed to the decoder; the old `scr=-1` line alone did not.

[CODE-READ; MEASURED] The single-symbol solver also admitted rows requiring two
front-loaded DM-RS symbols (type-1/maxLength-2 antenna-port codepoints >=12,
type-2/maxLength-2 >=24). In `replay_diag_explicit1_173505`, the first two serving
solutions selected layout 1654 through ports 0x10 with a single-symbol mask. They
produced only 20 and 10 full TB passes. The first combined candidate excludes those
unsupported rows; legal single-symbol rows of maxLength-2 tables remain searchable.

[CODE-READ; MEASURED] The search cursor was an index modulo a list whose size and
ordering change with every DCI. It did not represent a persistent physical hypothesis.
In `replay_diag_combined1_174530`, the correct layout/start/add-position/table combination
was first tested successfully only at signal slot 29944. Before the first solve there
were over 7000 probes. Seven RNTIs eventually solved, but only six had subsequent full
TB CRC support and total yield was low. The first coverage candidate counted probes
per stable layout and scrambling identity, retaining progress when readings regroup.
Further old-capture runs exposed that progress must also distinguish TDRA indices
and individual physical variants: a high-MCS grant must not consume a table/start
trial that is eligible only on a later QPSK grant. The final implementation balances
trial totals across layouts, then selects the least-tried eligible (TDRA, S, add-pos,
table) tuple under that identity. Sparse history is bounded to 4 MiB per RNTI;
exhaustion starts a fresh search round. No hypothesis becomes an irreversible decision.

[CODE-READ; MEASURED, old capture] `diag_old1_180130` solved only 8f5a.
The neighbor's anonymous search found four b876 hits and one e01e hit; neither
reached the existing five-distinct-payload lock threshold. SOLVE's stable sampling
had unintentionally extended `JOINT_ANON_RATE=0.1` from banked CORESETs to initial
unverified discovery dwells as well. Their finite dwell consequently received only
one tenth of its former opportunities. The new discovery flag restores the legacy
unthrottled dwell admission while keeping stable sampling on banked CORESETs.

## Candidate changes

- `ISAC_DCI11_OBSERVE_MM=1`: apply the existing anonymous mismatch bar to global
  layout observations; noisy known-RNTI grants remain eligible for CRC decoding.

- `ISAC_PDSCH_SOLVER_LAYOUT_RECOVER=N`: after N credited failed probes, reopen
  the active FDRA families’ statistically pruned layouts for that RNTI. Expired IQ
  is not failure evidence. Recovery stays enabled for post-solve refinement.
- `ISAC_PDSCH_SOLVER_SCR_EXPLICIT=1`: bind the default hypothesis to the decoded PCI
  for both DM-RS and data; explicit solver alternatives override both as before.
- `ISAC_PDSCH_DMRS_ID_REFRESH=N`: count at most one DM-RS estimator observation per
  increasing signal slot; reopen a frozen estimate after N independent failed slots.
- `ISAC_PDSCH_SOLVER_DMRS_DOMAIN=1`: exclude antenna-port rows requiring double-symbol
  DM-RS from the solver that currently emits only single-symbol masks.
- `ISAC_PDCCH_JOINT_ANON_DISCOVERY=1`: keep full-rate unverified discovery dwells
  when stable bank sampling is enabled; retain the existing five-payload lock bar.
- `ISAC_PDSCH_SOLVER_COVERAGE=1`: stable per-layout/per-identity fairness, with
  separate trial history for TDRA, S, additional position and MCS table.
- Existing `ISAC_PDSCH_SOLVER_PROBELOG=1` now logs actual decoder identities with the
  solver token, and complete layout sets on credited CRC passes for independent-pair auditing.

All new behavior defaults off. CSI-RS contract unchanged. No CUDA, GPU/FEP or chest
implementation changes.

## Validation

Diagnostic runs (capture b, same B baseline, GPU enabled, EOF reached). The coverage
rows below use the first implementation, before per-TDRA/physical-variant history:

| Run | Added flags | Solved | Full TB CRC per RNTI |
|---|---|---:|---|
| diag_off1_173326 | none | 0 | none |
| diag_explicit1_173505 | SCR_EXPLICIT | 2 | 37f3:20, 5d93:10 |
| diag_combined1_174530 | SCR_EXPLICIT, DMRS_ID_REFRESH=64, DMRS_DOMAIN | 7 | 37f3:21, db3e:24, 5186:80, 9f86:8, ce76:3, e169:7; 5d93:0 |
| diag_coverage1_175523 | preceding flags + COVERAGE | 10 | 37f3:158, b3d3:41, 5d93:33, e6aa:22, a07a:10, e513:30, 9f86:18, 5186:65, e169:18, 3383:3 |
| diag_ranked1_175823 | preceding flags + SCR_CLEAN=0 | 11 | b3d3:32, 37f3:242, 9f86:82, 5186:408, db3e:59, 5d93:47, 0211:1, ce76:16, e6aa:23, a07a:9, e169:6 |

Names are relative to `/home/nicola/NICOLA/wt/rr-csd-t/replays/replay_<name>/run.log`.
The coverage and ranked runs have a matching independent CRC probe pair (different DCI and signal slot, compatible table/identity and intersecting layout set containing the chosen representative) and subsequent full-TB CRC support for every solve. Ranked alternatives improved 5186 from 65 to 408 full passes (the supplied A4 has 411); per-RNTI parity is not yet established. These are diagnostic experiments, not the acceptance campaign. No acceptance claim yet.


Old-capture diagnostic runs:

| Run | Configuration | Main RNTIs solved | Full TB CRC |
|---|---|---:|---|
| diag_old1_180130 | coverage + ranked alternatives | 1/4 | 8f5a:73 |
| diag_old_discovery1_180456 | preceding + ANON_DISCOVERY | 3/4 | 8f5a:164, b876:108, e01e:43 |
| diag_old_pref1_180920 | preceding + NL_PREF=1 | 1/4 | 8f5a:174; neighbor verified late, only b876 length locked |
| diag_old_rate05_181632 | preceding + ANON_RATE=0.5 | 3/4 | 8f5a:33, e01e:20, 13ae:0; all four lengths found early, first correct solve late |

In the latter run 13ae had one correct two-layer CRC probe at slot 17092,
37 accepted DCIs total, but only 32 probes. `NL_PREF=32` delayed confirmation
when its candidate had no one-layer reading. The subsequent fixed candidate uses a one-grant fallback and bank anonymous rate 0.5.

## Interim recovery campaign (eight runs, stopped before run 9)

[MEASURED] This fixed build used the final coverage history and layout recovery,
but not the later evidence-quality gate. All eight runs reached EOF and had
independent confirmation pairs; none is omitted. It was stopped because the
second observed false pruning (b7, observation 27, `harq=3/22`, zero impossible
fields) delayed the first successful probe until signal slot 25644, reducing
full-TB yield. The waiting ninth job was cancelled before execution; no active
receiver or foreign job was interrupted. Logs and binary hash are preserved in
`replays/campaign2_logs.txt`, `campaign2_binary.sha256`, and
`solve2_campaign2_interim.log`. This is not a completed 9/10 acceptance campaign.

| Run/log directory | EOF | Solved | With full-TB CRC | Total full-TB CRC | Unpaired locks |
|---|---|---:|---:|---:|---:|
| `replay_solve2_r2_b1_185819` | yes | 23 | 20 | 954 | 0 |
| `replay_solve2_r2_b2_185954` | yes | 29 | 25 | 1535 | 0 |
| `replay_solve2_r2_b3_190124` | yes | 28 | 24 | 1437 | 0 |
| `replay_solve2_r2_b4_190300` | yes | 31 | 27 | 1598 | 0 |
| `replay_solve2_r2_b5_190600` | yes | 31 | 27 | 1358 | 0 |
| `replay_solve2_r2_b6_190738` | yes | 30 | 26 | 1561 | 0 |
| `replay_solve2_r2_b7_191041` | yes | 18 | 17 | 461 | 0 |
| `replay_solve2_r2_b8_191221` | yes | 26 | 24 | 1102 | 0 |

Full-TB CRC comparison; a zero includes an RNTI not discovered/solved.

| RNTI | A3 | A4 | b1 | b2 | b3 | b4 | b5 | b6 | b7 | b8 | Median |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0x0db1 | 9 | 15 | 32 | 32 | 31 | 31 | 30 | 31 | 11 | 31 | 31.0 |
| 0x37f3 | 79 | 0 | 138 | 210 | 225 | 231 | 195 | 223 | 85 | 154 | 202.5 |
| 0x5186 | 182 | 411 | 432 | 517 | 480 | 537 | 483 | 518 | 187 | 465 | 481.5 |
| 0x73de | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| 0x9f86 | 8 | 30 | 58 | 107 | 81 | 97 | 81 | 110 | 1 | 70 | 81.0 |
| 0xc61e | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| 0xce76 | 7 | 17 | 5 | 44 | 31 | 43 | 40 | 41 | 5 | 17 | 35.5 |
| 0xd76e | 10 | 11 | 3 | 38 | 2 | 40 | 30 | 34 | 0 | 7 | 18.5 |
| 0xdb3e | 23 | 2 | 0 | 122 | 59 | 137 | 47 | 117 | 18 | 34 | 53.0 |
| 0xe169 | 16 | 49 | 42 | 60 | 44 | 52 | 54 | 66 | 38 | 43 | 48.0 |
| 0xe2de | 0 | 7 | 2 | 9 | 0 | 11 | 6 | 10 | 2 | 4 | 5.0 |
| 0xe513 | 0 | 3 | 43 | 61 | 81 | 67 | 52 | 63 | 31 | 52 | 56.5 |

Solves with no subsequent full-TB CRC (their independent CB confirmation pairs still pass):

- b1: 0x4433, 0xacd6, 0xdb3e
- b2: 0x4433, 0x7c8a, 0xacd6, 0xdbc2
- b3: 0x4433, 0x7c8a, 0xacd6, 0xdbc2
- b4: 0x4433, 0x7c8a, 0xacd6, 0xdbc2
- b5: 0x4433, 0x7c8a, 0xacd6, 0xdbc2
- b6: 0x4433, 0x7c8a, 0xacd6, 0xdbc2
- b7: 0xb3d3
- b8: 0x4433, 0xac03

[MEASURED] `replay_diag_clean1_191904` enables `OBSERVE_MM=1` on the recovery
candidate: 29 solves, 25 with subsequent full-TB passes; first good probe at
absolute slot 16170. Working layouts 1664/2104 remain alive. Full-TB CRC counts:
37f3:172, 59ab:24, e513:59, e6aa:33, 5186:515, 2152:13, 3383:8, ce76:41,
db3e:92, b3d3:37, d76e:22, 9f86:110, 101b:111, e2de:4, 0211:12, 5d93:42,
1a61:2, cbb6:3, a07a:32, 5aea:5, 0db1:32, 4e0a:4, e169:45, d026:5, ac03:5.
All independent confirmation pairs pass the audit. This is a diagnostic run,
not part of the prospective final campaign.

[MEASURED] `replay_diag_clean_rate1_192054` changes only bank anonymous admission
from 0.5 to 1: 35 solves, 32 with full-TB support, all independent pairs valid.
It discovers c61e (absent from every 0.5-rate interim run), obtains two independent
probe passes at slots 20284/20886 and five subsequent full-TB passes. Anonymous
queue statistics report zero drops through 28000 completed items. Other full-TB
counts are retained in `replays/diag_clean_rate1.json`. The final prospective
campaign uses rate 1, avoiding deterministic sampling gaps for sparse RNTIs.

## Build and focused checks

[MEASURED, DGX aarch64 / GB10] Built in `cmake_targets/ran_build/build_solve2`
with CUDA architecture 121, four RX antennas, RelWithDebInfo, tests enabled.
Builds and all tests/replays use the orchestration `heavy.sh` lock, `-j6` maximum,
`ISAC_MLOCK=0`, and the wrapper's safe CPU affinity. Four new regressions cover reordered layouts/identities, separate TDRA start-symbol
trials, MCS eligibility, and credited-failure recovery. The solver, blind-monitor,
and DCI-length suites pass (3/3 CTest
entries, 18.49 seconds). Full `ninja tests` built successfully. Full CTest (excluding only
`nr_td_sim_test`) completed in 336.60 s: 210 passed / 5 failed of 215 enabled tests,
plus 15 disabled. The final receiver rebuild and focused 3/3 suite also pass
(18.39 s, `solve2_build10.log`, `solve2_focused10.log`). Failures: documented ARM `test_nr_pusch_ra0_qam256`, `dft_test`,
`test_nr_modulation`; the untouched `test_nr_td_grantwork` expects managed memory
although `nr_td_gw_unified()` now unconditionally returns false; `nr_polar_sc_cuda_test`
was not built by the aggregate target. Building that target explicitly and running
it passed (1.88 s). The later layout-recovery regression also passes. Validation
logs are copied into `replays/solve2_*test.log` and `replays/solve2_full_ctest.log`.

## Remaining variation observed in the fixed campaign

[MEASURED] Capture-b run `r3_b6` reports ce76 multiple times because its candidate
is reopened after the existing 256-full-failure guard. Its valid CB confirmation
pairs do not yield a full-TB pass in that replay. It is counted once in the raw
solve total and excluded from the full-TB-supported total. Absence of a subsequent
full-TB CRC is not universally explained by a solve near EOF.

[MEASURED] Old-capture `r3_old2` discovers the neighbor much later than `r3_old1`:
b876's first accepted raw DCI is frame 860.10 instead of 550.15 (roughly 3.1 s of
signal time later). It has only 51 admitted b876 DCIs, 60 e01e DCIs, and eight
13ae DCIs; no good b876/e01e probe. It reports 8f5a and 13ae, with full-TB support
only for 8f5a. The working layouts survive; the per-CORESET neighbor identity is
249. This is not the previous irreversible-layout failure.

## Campaign 3 capture-b CRC comparison

[MEASURED] With missing/unsolved RNTIs counted as zero in every repetition, the
third campaign’s ten-run mean exceeds the supplied good-run cohort mean (A3/A4) for every
RNTI observed in that cohort. This is an aggregate improvement, not uniform
best-run parity: campaign-3 means for e169 (47.1) and e2de (4.5) remain below their
best A-run values (49 and 7), and individual repetitions can be lower for other
RNTIs. Therefore a strict interpretation requiring every RNTI in every run to
meet its best A count is **not met**. Both raw counts and the aggregation are
shown; no blanket CRC-yield gate pass is claimed.

| RNTI | A3/A4 mean | Campaign 3 mean | A3/A4 best |
|---|---:|---:|---:|
| 0x0db1 | 12.0 | 29.7 | 15 |
| 0x37f3 | 39.5 | 183.6 | 79 |
| 0x5186 | 296.5 | 470.6 | 411 |
| 0x73de | 0.0 | 0.1 | 0 |
| 0x9f86 | 19.0 | 70.3 | 30 |
| 0xc61e | 0.5 | 3.9 | 1 |
| 0xce76 | 12.0 | 30.6 | 17 |
| 0xd76e | 10.5 | 17.6 | 11 |
| 0xdb3e | 12.5 | 55.5 | 23 |
| 0xe169 | 32.5 | 47.1 | 49 |
| 0xe2de | 3.5 | 4.5 | 7 |
| 0xe513 | 1.5 | 64.3 | 3 |

[MEASURED] The complete third campaign gives capture b **10/10** with at least
five full-TB-supported RNTIs, but the older capture only **3/5** with all four
main RNTIs. It is not an accepted complete gate. Raw/CRC tables follow.

| Run/log directory | EOF | Solved | With full-TB CRC | Total full-TB CRC | Unpaired locks |
|---|---|---:|---:|---:|---:|
| `replay_solve2_r3_b1_192255` | yes | 34 | 27 | 1275 | 0 |
| `replay_solve2_r3_b2_192556` | yes | 33 | 29 | 1460 | 0 |
| `replay_solve2_r3_b3_192728` | yes | 36 | 33 | 1509 | 0 |
| `replay_solve2_r3_b4_192904` | yes | 35 | 31 | 1529 | 0 |
| `replay_solve2_r3_b5_193033` | yes | 35 | 30 | 1262 | 0 |
| `replay_solve2_r3_b6_193210` | yes | 35 | 28 | 1138 | 0 |
| `replay_solve2_r3_b7_193511` | yes | 38 | 32 | 1576 | 0 |
| `replay_solve2_r3_b8_193645` | yes | 32 | 30 | 1449 | 0 |
| `replay_solve2_r3_b9_193814` | yes | 37 | 34 | 1470 | 0 |
| `replay_solve2_r3_b10_193945` | yes | 31 | 26 | 660 | 0 |
| `replay_solve2_r3_old1_194115` | yes | 4 | 4 | 316 | 0 |
| `replay_solve2_r3_old2_194255` | yes | 2 | 1 | 113 | 0 |
| `replay_solve2_r3_old3_194553` | yes | 4 | 4 | 332 | 0 |
| `replay_solve2_r3_old4_194727` | yes | 4 | 4 | 333 | 0 |
| `replay_solve2_r3_old5_194904` | yes | 1 | 1 | 143 | 0 |

Full-TB CRC comparison; a zero includes an RNTI not discovered/solved.

| RNTI | A3 | A4 | b1 | b2 | b3 | b4 | b5 | b6 | b7 | b8 | b9 | b10 | Median |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0x0db1 | 9 | 15 | 28 | 30 | 31 | 25 | 31 | 31 | 30 | 32 | 32 | 27 | 30.5 |
| 0x37f3 | 79 | 0 | 200 | 222 | 167 | 242 | 144 | 169 | 211 | 217 | 168 | 96 | 184.5 |
| 0x5186 | 182 | 411 | 434 | 481 | 508 | 512 | 499 | 458 | 513 | 476 | 515 | 310 | 490.0 |
| 0x73de | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0.0 |
| 0x9f86 | 8 | 30 | 64 | 82 | 82 | 80 | 47 | 54 | 78 | 96 | 95 | 25 | 79.0 |
| 0xc61e | 1 | 0 | 0 | 0 | 3 | 3 | 7 | 4 | 11 | 0 | 11 | 0 | 3.0 |
| 0xce76 | 7 | 17 | 25 | 31 | 39 | 51 | 33 | 0 | 41 | 30 | 41 | 15 | 32.0 |
| 0xd76e | 10 | 11 | 6 | 6 | 37 | 33 | 13 | 0 | 36 | 7 | 38 | 0 | 10.0 |
| 0xdb3e | 23 | 2 | 13 | 51 | 107 | 62 | 39 | 28 | 69 | 88 | 98 | 0 | 56.5 |
| 0xe169 | 16 | 49 | 43 | 41 | 58 | 46 | 38 | 38 | 56 | 65 | 52 | 34 | 44.5 |
| 0xe2de | 0 | 7 | 0 | 0 | 11 | 8 | 6 | 2 | 11 | 0 | 4 | 3 | 3.5 |
| 0xe513 | 0 | 3 | 74 | 81 | 60 | 77 | 54 | 63 | 61 | 80 | 64 | 29 | 63.5 |

Old-capture main RNTIs, full-TB CRC:

| RNTI | old1 | old2 | old3 | old4 | old5 |
|---|---:|---:|---:|---:|---:|
| 0x8f5a | 156 | 113 | 142 | 143 | 143 |
| 0xb876 | 73 | 0 | 88 | 88 | 0 |
| 0xe01e | 49 | 0 | 39 | 43 | 0 |
| 0x13ae | 38 | 0 | 63 | 59 | 0 |

Solves with no subsequent full-TB CRC (their independent CB confirmation pairs still pass):

- b1: 0x0a51, 0x4433, 0x62a6, 0x73de, 0x7c8a, 0xacd6, 0xdbc2
- b2: 0x62a6, 0x7c8a, 0xacd6, 0xdbc2
- b3: 0x62a6, 0x73de, 0xdbc2
- b4: 0x62a6, 0xac03, 0xdbc2, 0xea0b
- b5: 0x1a61, 0x62a6, 0x73de, 0xac03, 0xdbc2
- b6: 0x0a51, 0x1a61, 0x62a6, 0x73de, 0xac03, 0xce76, 0xd76e
- b7: 0x62a6, 0x73de, 0x7c8a, 0xac03, 0xd093, 0xdbc2
- b8: 0x73de, 0x7c8a
- b9: 0x62a6, 0x7c8a, 0xdbc2
- b10: 0x4433, 0x5aea, 0x62a6, 0xb909, 0xc61e
- old1: none
- old2: 0x13ae
- old3: none
- old4: none
- old5: none

## Completed campaign 3 (old-capture gate failed)

The completed `solve2_r3_*` campaign uses build 10 without rebuilding or
changing flags between runs. Receiver SHA-256:
`bf9601162b6ea9a4f1e1def6cd9f4bc9b5273f88be29534017498317774c39ff`.
The exact log list is `replays/campaign3_logs.txt`; parsed per-RNTI counts and
confirmation token pairs are `replays/solve2_r3_{b1..b10,old1..old5}.json`.
The two preceding campaigns and all diagnostic runs are excluded from these
prospective gate counts.

## Exact replay environment

The candidate command below is the B baseline plus SOLVE2 flags. It uses the
same replay wrapper, base configuration, capture samples and rate as SOLVE.md.
No radio command was run. Acceptance results and remaining limits are stated above.

```bash
#!/bin/bash
set -eu
cd /home/nicola/NICOLA/wt/rr-csd-t
B=$PWD/cmake_targets/ran_build/build_solve2
O=/home/nicola/NICOLA/wt/rr-orchestration
export REPLAY_PREFIX='' REPLAY_OUT=$PWD/replays REPLAY_LAST=$PWD/replays/last
export BASECFG=$PWD/tests/passive_rx/dgx/ue.passive.ota.chestfast.replay.cfg
export CAP=${CAP:-/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b} CAP_SAMPLES=1474560000 CAP_TICK=${CAP_TICK:-208029025} ISAC_MLOCK=0
base=(ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_JOINT=1 ISAC_CORE_PLAN=0
  ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_DUTY=1 ISAC_COREMAP_IDSWEEP_DUR=1
  ISAC_RECONF=1 ISAC_METRICS=1 ISAC_CSIRS_DISC=1 ISAC_GPU_SELFCHECK=0
  ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1
  ISAC_PDCCH_KNOWN_PRESCREEN=1 ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_PDCCH_SCAN_TIMING=1
  ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24
  ISAC_PDSCH_SOLVER_REUSE=1 ISAC_PDSCH_INLINE_POOL=1 ISAC_PDSCHQ_PROBE_LAST=1
  ISAC_PDSCH_SOLVER_PROBE_RATE=0.5 ISAC_PDSCH_SOLVER_PROBE_BURST=16 ISAC_PDSCH_SOLVER_PROBE_FILL=25
  ISAC_PDSCH_SNR_GATE=1 ISAC_PDSCH_EVM_GRANT=1 ISAC_PDSCH_SOLVER_SCR=1 ISAC_PDCCH_JANON_ALL_CORESETS=1
  ISAC_PDCCH_JOINT_ANON_LEN_HINT=1 ISAC_PDCCH_FEP_CACHE=1 ISAC_PDSCH_SOLVER_BACKOFF=200
  ISAC_PDSCH_SOLVER_SCR_CSIRS=1 ISAC_PDSCH_DMRS_ID_PER_CORESET=1 ISAC_PDCCH_JOINT_ANON_RATE=0.1
  ISAC_PDCCH_DED_BUDGET_SHARE=2 ISAC_PDSCH_CORES=8,9,12,13,14 ISAC_RX_MRC_PER_RNTI=1
  ISAC_RX_MRC_ALIGN=2 ISAC_CHEST_DELAY_SHARE=0 ISAC_CHEST_PDP_DIAG=0 ISAC_PDSCH_SOLVER_PROBELOG=1)
solve=(ISAC_PDSCH_SOLVER_NL_PREF=32 ISAC_PDSCH_SOLVER_SCR_CLEAN=1
  ISAC_PDSCH_SOLVER_THREAD_CTX=1 ISAC_PDCCH_JOINT_ANON_STABLE=1)
name=${1:-solve2_b1}; shift || true
"$O/heavy.sh" "$O/csd_replay_user.sh" "$name" "$B" 0.25 "${base[@]}" "${solve[@]}" "$@"

# Invoke the script above with:
# bash solve2_run.sh solve2_b1 ISAC_PDSCH_SOLVER_SCR_EXPLICIT=1 \
#   ISAC_PDSCH_DMRS_ID_REFRESH=64 ISAC_PDSCH_SOLVER_DMRS_DOMAIN=1 \
#   ISAC_PDSCH_SOLVER_COVERAGE=1 ISAC_PDSCH_SOLVER_LAYOUT_RECOVER=32 \
#   ISAC_PDSCH_SOLVER_SCR_CLEAN=0 ISAC_PDCCH_JOINT_ANON_DISCOVERY=1 \
#   ISAC_PDSCH_SOLVER_NL_PREF=1 ISAC_PDCCH_JOINT_ANON_RATE=1 \
#   ISAC_DCI11_OBSERVE_MM=1 ISAC_DCI11_PRUNELOG=1 ISAC_DCI_RAW_LOG=1 \
#   ISAC_PDCCH_NID_GPU_RECHECK_MS=250
# Second capture: same flags, with these environment overrides:
# CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008 CAP_TICK=210036277
```

## Reproduction and audit details

Configure (the writable CPM cache is a local copy of the existing dependency cache):

```bash
ISAC_MLOCK=0 CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp \
  /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh cmake -S . \
  -B cmake_targets/ran_build/build_solve2 -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_USRP=ON -DOAI_SIMU=ON \
  -DENABLE_ISAC_SENSING=ON -DNB_ANTENNAS_RX=4 -DENABLE_LDPC_CUDA=ON \
  -DLDPC_CUDA_ARCH=121 -DUHD_LIBRARIES=/usr/local/lib/libuhd.so \
  -DUHD_INCLUDE_DIRS=/usr/local/include -DCPM_SOURCE_CACHE=/tmp/solve2-cpm
ISAC_MLOCK=0 CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp \
  /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ninja \
  -C cmake_targets/ran_build/build_solve2 -j6 nr-uesoftmodem test_nr_pdsch_solver
ISAC_MLOCK=0 /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ctest \
  --test-dir cmake_targets/ran_build/build_solve2 \
  -R '^(test_nr_pdsch_solver|test_nr_pdcch_blind_monitor|test_nr_pdcch_dci_length_sweep)$' \
  --output-on-failure -j3
```

RNTI totals are distinct; reopening and re-solving the same RNTI never increases
the phone count. The log audit joins `probe`, `identity`, and `evidence` by `tok`. For each
`SOLVED` line it requires two earlier credited phase-0 CRC passes with different
DCI numbers **and different absolute signal slots**, compatible MCS tables,
the solved add-position and scrambling choice, identical actual DM-RS/data IDs,
and the chosen layout in the intersection of both complete evidence sets.
The identity record must also match each probe's RNTI, CORESET key and absolute
slot. A full-TB count uses only `identity crc=1 probe=0`; aggregate solver
`full_ok` is not used because post-solve rate-match exploration can still be a
code-block probe. A late solve without a subsequent full-TB pass is shown
separately, even if its independent probe pair passes the audit. The CRC checks
provide the receiver's existing false-lock evidence, not a mathematical claim
that random CRC collisions are impossible.

## Recorded-input control reproduction

The local harness `replays/solve2_layout_trace.c` reads only `DCIRAW` records,
initializes the unchanged resolver with BWP 217, unknown TDRA width and 49-bit
DCI, and reports the first alive-to-dead transition of layout 1664. It does not
feed PDSCH CRC feedback, so survival cannot be attributed to later successful
probes protecting that layout. Compile and run under the same heavy-job lock:

```bash
O=/home/nicola/NICOLA/wt/rr-orchestration
ISAC_MLOCK=0 "$O/heavy.sh" gcc -O2 -std=gnu11 -ffunction-sections -fdata-sections \
  -I. -Iopenair1/PHY/NR_UE_TRANSPORT replays/solve2_layout_trace.c \
  openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.c \
  openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.c -Wl,--gc-sections -lm \
  -o replays/solve2_layout_trace
for run in replay_solve2_r2_b1_185819 replay_solve2_r2_b7_191041; do
  ISAC_MLOCK=0 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 \
    "$O/heavy.sh" replays/solve2_layout_trace "replays/$run/run.log" 217 49 1664
  ISAC_MLOCK=0 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=0 \
    "$O/heavy.sh" replays/solve2_layout_trace "replays/$run/run.log" 217 49 1664
  ISAC_MLOCK=0 ISAC_DCI11_HARD_REFUTE=1 ISAC_DCI11_HARQ_SEQ=1 \
    "$O/heavy.sh" replays/solve2_layout_trace "replays/$run/run.log" 217 49 1664 1
done
```

Harness source (save as the path above; no receiver code changes are needed):

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nr_pdcch_dci11_layout_sweep.h"
int main(int argc, char **argv) {
  if (argc != 5 && argc != 6) { fprintf(stderr,"usage: trace log bwp_size dci_length layout [max_mm_per216]\n"); return 2; }
  int bwp=atoi(argv[2]), len=atoi(argv[3]), target=atoi(argv[4]);
  FILE *f=fopen(argv[1],"r"); if(!f) return 2;
  nr_dci11_resolver_t *r=calloc(1,sizeof(*r));
  nr_dci11_resolver_init_fdra(r,0,bwp,(int)ceil(log2(bwp*(bwp+1)/2.0)),NR_DCI11_TDA_UNKNOWN,len);
  char *line=NULL; size_t cap=0; unsigned count=0; int deaths=0; unsigned skipped=0;
  while(getline(&line,&cap,f)>0) {
    char *s=strstr(line,"DCIRAW "); if(!s) continue;
    int frame,slot,cce; unsigned rnti,dl,al,mm; unsigned long long payload;
    if(sscanf(s,"DCIRAW %d.%d rnti=0x%x len=%u cce=%d al=%u mm=%u payload=0x%llx",&frame,&slot,&rnti,&dl,&cce,&al,&mm,&payload)!=8 || dl!=len) continue;
    const unsigned bar = argc == 6 ? (unsigned)atoi(argv[5]) * al / 2 : 0;
    if (argc == 6 && mm > (bar ? bar : 1)) { skipped++; continue; }
    int was=target<r->n_hyp && r->alive[target];
    nr_dci11_resolver_observe_rnti(r,nr_dci_bits_from_u64(payload),rnti); count++;
    if(was && !r->alive[target]) {
      int best=-1; double rate=-1;
      for(int i=0;i<r->n_hyp;i++) if(r->alive[i]&&r->harq_pairs[i]>=16 && (double)r->harq_inc[i]/r->harq_pairs[i]>rate) {best=i;rate=(double)r->harq_inc[i]/r->harq_pairs[i];}
      printf("ELIMINATED layout=%d raw=%u frame=%d slot=%d rnti=%04x seen=%u pass=%u impossible=%u harq=%u/%u rv_at=%u best=%d best_harq=%u/%u\n",target,count,frame,slot,rnti,r->seen[target],r->pass[target],r->impossible[target],r->harq_inc[target],r->harq_pairs[target],r->off[target].rv,best,best<0?0:r->harq_inc[best],best<0?0:r->harq_pairs[best]); deaths++;
    }
  }
  printf("FINAL skipped=%u raw=%u layouts=%d alive=%d target=%d target_alive=%d deaths=%d seen=%u pass=%u impossible=%u harq=%u/%u rv_at=%u\n",skipped,count,r->n_hyp,r->n_alive,target,target<r->n_hyp&&r->alive[target],deaths,r->seen[target],r->pass[target],r->impossible[target],r->harq_inc[target],r->harq_pairs[target],r->off[target].rv);
  free(line); free(r); fclose(f); return 0;
}
```

## Discovery recheck investigation

[MEASURED] `r3_old5` never publishes nID 10163 and admits zero b876, e01e or
13ae DCIs. GPU logs show serving nID 62922 published, followed by
`MAINTAIN -> PAUSED` with one published/verified CORESET. Only eleven GPU passes
occur in the whole replay. The existing default paused recheck is 10000 ms of
wall time; the occupancy wake cannot establish completeness of other cells that
share the already-covered full-band footprint. No GPU-owned source is changed.

[MEASURED] `replay_diag_old_share1_195213` forces the existing dedicated budget
share to 1. It still solves only 8f5a: nID 10163 is eventually published but does
not reach bank verification, and no b876 raw DCI is admitted. This does not fix
the old-capture gate. The next controlled setting is the existing
`ISAC_PDCCH_NID_GPU_RECHECK_MS=250`, retaining budget share 2.

[MEASURED] `replay_diag_old_recheck250_195410` keeps budget share 2 and changes
only the paused GPU recheck to 250 ms. Both CORESETs verify; b876's first raw DCI
is frame 335.14. All four main RNTIs have independently audited pairs and full
TBs: 8f5a:139, b876:159, e01e:63, 13ae:78. There are 110 GPU passes. The fourth
prospective campaign retains this setting for both captures, with the same
receiver binary as campaign 3. GPU source and kernels remain unchanged.

[MEASURED, campaign 4 old capture] Main-RNTI results are **4, 4, 4, 4, 1**;
the requested 4/5 gate is met. In the fifth run GPU discovery does recheck
(93 passes, 73 publishing nID 10163), but the neighbor geometries are grid_rb=1
and later grid_rb=156/groups=2. The valid grid_rb=0 neighbor does not reach bank
verification. Only the serving 8f5a is decoded. This residual geometry failure
is retained in the table, not hidden by the successful recheck diagnostic.
GPU-owned geometry/kernel files have not been edited.

## Commit checks

[MEASURED] `git diff --check` is clean. The required sens6 comparison is clean:

```bash
git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures \
  'tests/passive_rx/*.conf' tests/passive_rx/sens6_host_snapshot_2026-09-30
```

Only the owned runtime/solver/queue code, solver regressions, and this report
are included. No GPU/CUDA, PDSCH FEP/chest implementation, capture or radio
configuration files are changed. No merge or push is performed.
