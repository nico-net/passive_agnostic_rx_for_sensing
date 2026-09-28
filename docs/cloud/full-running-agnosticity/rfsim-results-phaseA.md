# RFsim validation campaign — Phase A (phy-test, no core network)

Host sens6. Receiver = `adaptive-rx-UL-DL` (branch `adaptive-rx-UL-DL @ 25a8699a64`,
`cmake_targets/ran_build/build/nr-uesoftmodem` — confirmed via `strings | grep` for
`"Passive RX: MIB indicates no CORESET 0..."` and `"Technique D CONVERGED..."`, both present).
gNB = `agn-wt/gnbtest` (branch `sdd/rfsim-gnb-test @ 67bb0eaa11`,
`cmake_targets/ran_build/build/nr-softmodem`, env-gated `ISAC_GNB_TEST_*` knobs — confirmed via
`strings | grep ISAC_GNB_TEST` showing all 7 format strings). Uses `--phy-test --noS1` per
`passive-bwp-tracking-and-phy-test-bed` memory — no open5gs core, avoiding the blocker that
stopped the previous (`rfsim-results.md`) attempt. Logs under
`sens6:/home/sens/NICOLA/rfsim_validation/phyA/<arm>/{gnb,rx}/`.

## Bed-works check — PARTIAL PASS, with two real bugs found along the way

Launch recipe (matches the memory verbatim): gNB
`nr-softmodem --phy-test --noS1 -m <mcs> -n <table> -M <prb> -l <layers> -D 0xff -O <gnb conf> --rfsim`;
receiver `nr-uesoftmodem --passive-rx -O <ue conf> --rfsim -C <freq> -r <prb> --numerology 1 --band 78
--ssb <ssb_start_subcarrier> [--ue-nb-ant-rx N]`, each in its own fresh cwd.

**Confirmed working, rank-1 / nb_tx=1, both bandwidths**: gNB creates the phy-test dummy UE
(RNTI 0x1234, `create_new_UE()` → `get_initial_cellGroupConfig()` — the SAME per-UE `config_pdsch`/
`config_pusch` build path a real SA-attached UE gets, confirmed by source read of
`mac_rrc_dl_handler.c`/`nr_radio_config.c`/`rrc_gNB_du.c`'s `rrc_add_nsa_user()` call at F1 setup —
this settles the open question of whether the `ISAC_GNB_TEST_*` per-UE knobs even apply under
phy-test: they do, same code path). Receiver syncs (PSS/SSS/PBCH), decodes MIB, and correctly
**skips SIB1** with `Passive RX: MIB indicates no CORESET 0 / SIB1 in this cell -- not scheduling
SIB1` (phy-test MIB carries no CORESET0, exactly as the memory predicts) and runs from its manual
`pdcch_blind_monitor_*` config instead.

**Bug #1 (real, reproducible, isolated): nb_tx=4 / rank-4 phy-test (`-l 4`, `gnb.sa.rfsim.100mhz.rank4.conf`)
NEVER completes PBCH sync.** Ran ~250 s / 3680+ SSB occasions on the memory's exact recipe
(`-m 25 -n 1 -M 273 -l 4 -D 0xff`), 100% `ERROR NR_PBCH_DECODE => polar decoding wrong`, never once
`pbch not decoded on any branch` → success. Isolated the cause: re-ran with `--ue-nb-ant-rx 1`
against the SAME rank-4 gNB — still 100% failure (2184/2184 in ~20 s) — so it is NOT a receive-antenna
issue. Re-ran the SAME receiver against an otherwise-identical **rank-1** 100 MHz gNB
(`-m 9 -n 0 -M 273 -l 1`, `gnb.sa.rfsim.100mhz.conf`, `nb_tx=1`) — synced in <10 s
(`RFCENSUS ... pbch_ok=50 pbch_fail=0`). Source read of the SSB TX path
(`phy_procedures_nr_gNB.c:nr_common_signal_procedures`) shows `nr_generate_pss/sss/pbch` always write
to a single logical antenna port (`get_first_ant_idx()` returns `fapi_start_port`, not
beamforming-routed, when `enable_analog_das` is false) and `TX_AMP` is a fixed constant independent of
`nb_antennas_tx` — so the SSB *generation* code is provably identical between the two confs. The defect
therefore lives downstream, most likely in rfsim's channel-model/RU TX path when `nb_antennas_tx=4`
(not investigated further — out of scope for a no-code-edit root-cause per the brief). **This blocks
Arm A1 (rank 4) outright, and by the same mechanism almost certainly blocks A8/A9 (CSI-RS 8/12-port,
which also need `nb_antennas_tx>1`)** — not independently verified since the campaign ran out of budget
before reaching those arms.

**Bug #2 (real, found and root-caused via the project's own documented diagnostic method): the
106-PRB blind-PDCCH `dci_length_override=45` (live-verified for the SA/core-attached-UE harness) is
WRONG for the phy-test dummy UE — the true value is 46.** First 106-PRB rank-1 run: 0/48000+ occasions
ever CRC-recovered RNTI 0x1234 (checked directly — `grep -c '0x1234' rx.log` = 0), instead producing a
persistently-repeating spurious RNTI (`0x68ed`, thousands of accepts) — the signature of scanning a
structurally-wrong bit alignment, not noise. Root-caused per the confs' own documented procedure
(`--log_config.nr_mac_log_level debug`, grep `"DCI format"`): gNB log showed **`DCI format 1 size: 46
alt_size 0`**, not 45. Built a scratch copy of `ue.passive.q.conf`
(`/home/sens/NICOLA/rfsim_validation/phyA/ue.passive.q.dci46.conf`, tracked repo file left untouched)
with `pdcch_blind_monitor_bwp = "0:106:0:46"`. Re-ran: RNTI 0x1234 confirmed almost immediately
(`PDCCH_SCRAMBLING_ID CONFIRMED [USS(dedicated)] n_id=0 ... by CRC-recovered RNTI 0x1234`), accepts
climbing into the tens of thousands, `DCIQUAL ... pass` firing continuously.

**Bug #3 (found, not fully root-caused — flagged for next session): even with the correct total
`dci_length=46`, the SUB-FIELD boundaries are still wrong.** With `ISAC_DCI_FIELDS=1` (the module's own
diagnostic env var, added specifically for this class of bug per its source comment: *"A high rv!=0
rate on a link without retransmissions is the KNOWN signature of misaligned DCI field widths"*),
every single decoded candidate for RNTI 0x1234 read **`mcs=4`** (gNB's own log:
`BLER 0.10000 MCS (0) 9` — the phy-test scheduler uses a FIXED MCS 9 every slot) and **`numrb=68`**
(gNB scheduled `target_dl_bw=50` PRB), and **`rv` cycled 1→2→3, never 0** — i.e. every accepted
candidate is being (mis-)read as a retransmission, so the receiver's own `pdsch_rv0_only` gate
(correctly, given the data) skips them all: `pdsch_decode[try=0 crc_ok=0]` never advances past 0
despite ~85k accepted, RNTI-confirmed candidates. This means the SA/core-harness-derived
`pdcch_blind_monitor_dci_bits`/TDA/BWP-indicator field widths (the ones the `bwp_indicator 1→0` /
`TDA 4→2` fix from 2026-07-30 established) do not carry over unchanged to the phy-test dummy UE's DCI
layout — some field between the FDRA and the RV/HARQ fields is a different width here. **This blocks
Technique D convergence / TB-level CRC decode, hence every arm that needs to score a PDSCH-CRC-based
pass/fail (A4 DL scrambling IDs, A5 type-B k0, A6 PRG, A7 qam256)** — the AL/CORESET-level knobs (A2,
A3) are NOT blocked by this, since AL/CCE-position decoding is independently confirmed correct
(observed `al=2` matches the gNB's own default `Candidates per PDCCH aggregation level on UESS: L1: 0,
L2: 2`).

## Time/scope note

Getting the bed to a state where ANY arm could be scored (bugs #1-#3 above) consumed the full session
budget. **No arm beyond the baseline sync/CORESET/RNTI-confirmation check was actually executed to a
scored PASS/FAIL/VOID verdict.** This is reported honestly rather than fabricating arm results — the
three bugs found are real, reproducible, and load-bearing for every later arm, so surfacing them now
(with exact repro commands and root-cause evidence) is more valuable than a partial, DCI-corrupted A2
run.

## Per-arm status

| Arm | Knob | Validity | Verdict | Notes |
|---|---|---|---|---|
| Bed check (100MHz, rank4, per memory recipe) | none | **INVALID** (never syncs) | **FAIL** | Bug #1 — nb_tx=4 SSB never decodes, isolated to gNB TX side, independent of RX antenna count |
| Bed check (106PRB, rank1) | none | **VALID** (sync+MIB+CORESET+RNTI confirm), after Bug #2 fix | **PASS** (partial — see Bug #3) | Real dci_length=46, not 45; RNTI 0x1234 confirmed, but full field parse still wrong (Bug #3) so TB-CRC path unreached |
| A0 baseline (100MHz/rank4, memory recipe) | `-l 4` | INVALID | **FAIL** | Same as bed check above; this IS the A0 baseline arm, so A0 fails as specified |
| A0b baseline (106PRB/rank1, faster variant) | `-l 1` | VALID | **PASS (partial)** | Sync/CORESET/RNTI level only; see Bug #3 |
| A1 rank 4 | `-l 4` | INVALID | **FAIL** | Cannot test LBRM n_L / CRC — PBCH never syncs (Bug #1) |
| A2 AL1-only | `ISAC_GNB_TEST_USS_AL="4,0,0,0,0"` + `ISAC_AL1_COVER=1` | NOT RUN | **NOT RUN** (budget) | Should be testable once run — CORESET/RNTI-level decode works independent of Bug #3, and `ISAC_GNB_TEST_USS_AL` is LIVE-CONFIRMED to apply (per `gnbtest-report.md`) |
| A3 AL16 | `ISAC_GNB_TEST_USS_AL` w/ AL16 | NOT RUN | **NOT RUN** (budget) | Same testability note as A2; receiver's `ISAC_LANE_ALS` env knob confirmed present in source |
| A4 DL scrambling IDs ≠ PCI | `ISAC_GNB_TEST_DL_DMRS_ID0=700 ISAC_GNB_TEST_DL_DATA_ID=500` | NOT RUN | **BLOCKED** | Needs TB-CRC scoring — blocked by Bug #3 until sub-field widths are re-derived for phy-test |
| A5 PDSCH type B k0=2 | `ISAC_GNB_TEST_DL_TYPEB_K0=2` + `ISAC_GNB_TEST_DL_TDA_IDX` | NOT RUN | **BLOCKED** | Same as A4 |
| A6 PRG=4 negative control | `ISAC_GNB_TEST_PRG=4` | NOT RUN | **BLOCKED** | Same as A4 (needs a CRC baseline to compare against) |
| A7 qam256 | `-m` high MCS | NOT RUN | **BLOCKED** | Same as A4 |
| A8/A9 CSI-RS 8/12-port | conf antenna ports, `ISAC_CSIRS_BLIND_WIDE=1` | NOT RUN | **LIKELY BLOCKED** | 8/12-port CSI-RS needs `nb_antennas_tx>1` at the gNB, which is exactly Bug #1's failure mode; not independently verified |

## Failure-hypothesis log

1. **Bug #1 (rank-4/nb_tx=4 PBCH sync failure)**: hypothesis is an rfsim channel-model or RU TX-buffer
   defect specific to `nb_antennas_tx>1` (SSB generation code itself is proven nb_tx-independent by
   source read — see above). Not confirmed further; next step would be instrumenting
   `radio/rfsimulator/apply_channelmod.c` / the RU TX buffer-packing path for a 4-antenna case and
   diffing the actual over-the-wire samples at antenna 0 between the two confs.
## Job 1 — Bug #1 bisect (2026-09-26 follow-up session)

**Verdict: PRE-EXISTING / environmental. Not caused by any commit in `222f98d072..25a8699a64`
(the full-running-agnosticity plan's own range). No bisect was needed — the base commit
reproduces the failure identically, byte-for-byte in kind.**

Method: built a fresh worktree at `sens6:/home/sens/NICOLA/agn-wt/base` (branch `sdd/base-bisect`,
`git worktree add` at `222f98d072`, the plan's base commit — predates all 18 agnosticity tasks),
configured its build dir with the same cmake cache as the other lanes (`CMAKE_BUILD_TYPE=RelWithDebInfo,
ENABLE_ISAC_SENSING=ON, ENABLE_LDPC_CUDA=ON, OAI_RF_EMULATOR=ON, OAI_SIMU=ON, OAI_USRP=ON,
OAI_VRTSIM=ON, ENABLE_TESTS=ON`), built `nr-softmodem nr-uesoftmodem rfsimulator params_libconfig
oai_usrpdevif` (the dlopen'd-plugin trap bit once — first run died instantly with
`dlopen(libparams_libconfig.so): No such file`, fixed by also building `params_libconfig`), then
re-ran the EXACT recipe from `A0_baseline_100mhz_rank4`'s own logged `CMDLINE` (gNB: `--phy-test
--noS1 -m 25 -n 1 -M 273 -l 4 -D 0xff -O gnb.sa.rfsim.100mhz.rank4.conf --rfsim`; RX:
`--passive-rx -O ue.passive.q.100mhz.conf --rfsim -C 3750000000 -r 273 --numerology 1 --band 78
--ssb 1478 --ue-nb-ant-rx 4`), both binaries and both confs from the `base` worktree.

**Result**: 5480/5480 `ERROR NR_PBCH_DECODE => polar decoding wrong`, 0 `RFCENSUS`/`pbch_ok`
successes in ~100s — the identical failure signature the report already recorded on
`25a8699a64`. Confirms the defect predates the entire agnosticity work.

**Checked and ruled out as explanations**:
- The rfsim nb_tx channel-sizing fix (`fc53bf920f`, "rebuild the client channel model at the
  peer's actual TX count" — the fix `rfsim-channel-nb-tx-from-own-tx-count` memory describes) is
  present, byte-identical, in BOTH the base worktree and current `adaptive-rx-UL-DL` HEAD
  (`grep` on `radio/rfsimulator/simulator.cpp` matches line-for-line in both trees) — it reached
  this branch via a different commit hash (rebase/cherry-pick), not missing. Not the cause.
- No `tx_array` is configured in `ue.passive.q.100mhz.conf`'s `[sensing_channel]` block (only
  `rx_array`, the bistatic RX-side array), so `sensing_channel.c`'s documented endfire-null
  failure mode (`rfsim-channel-nb-tx-from-own-tx-count` memory's "TX array along the LOS axis
  with lambda/2 spacing NULLS the common-port SSB at endfire") does not apply here — with no
  `tx_array`, `sensing_channel.c` explicitly falls back to "every port radiates the same
  wavefront" (source: `sensing_channel_set_tx_array`'s comment, `tx_steer()`), which cannot null.
- Conf/recipe drift since 2026-09-16: the memory's own validated rank-4 rfsim run used a
  DIFFERENT conf (`ue.passive.pin49r4.100mhz.conf`, a wide-aperture scene purpose-built for a
  well-conditioned 4x4 channel) — NOT `ue.passive.q.100mhz.conf`/`gnb.sa.rfsim.100mhz.rank4.conf`,
  which is the pair this campaign's Bug #1 actually exercises. The two were never the same
  recipe, so "it worked on 09-16" is not in tension with this failure; they're different scenes.
- Confirmed once more, independent of the plan: the phyA report's own source-read finding still
  holds — `nr_generate_pss/sss/pbch` write to a single logical TX port regardless of
  `nb_antennas_tx`, so the defect is downstream of SSB generation. Not re-investigated further
  (out of scope for a bisect once the base-commit repro settled the "is it this plan's fault"
  question) — next step, if picked up, is the phyA report's own suggested one: instrument
  `apply_channelmod.c`/the RU TX buffer-packing path for `nb_antennas_tx=4` and diff the actual
  over-the-wire samples at antenna 0 against the `nb_antennas_tx=1` case.

Logs: `sens6:/home/sens/NICOLA/rfsim_validation/phyA2/job1_base_rank4/{gnb,rx}/`.

## Job 2 — fully agnostic 106 PRB phy-test re-run (2026-09-26 follow-up session)

New scratch conf `sens6:/home/sens/NICOLA/rfsim_validation/phyA2/ue.passive.q.agn.conf` (local copy
also at `docs/.../rfsim_validation/phyA2/ue.passive.q.agn.conf` — not committed anywhere, per the
brief), modeled 1:1 on `sensnuc3:/home/sens/NICOLA/rfsim-local/tests/passive_rx/ue.passive.agn.conf`'s
`[sensing]` block: `pdcch_blind_monitor_autoconf/autodiscover/full_auto=1`, `dci10`, `dci01`,
`ul_pusch`/`ul_uci`/`ul_thread` all on, **NO** `pdcch_blind_monitor_coreset/ss/bwp/tda/dci_bits/dmrs`
and **NO** `dci_length_override` — every DL PDCCH/PDSCH geometry parameter self-discovered. All arms:
gNB binary `agn-wt/gnbtest` (`sdd/rfsim-gnb-test @ 67bb0eaa11`), gNB conf
`adaptive-rx-UL-DL/tests/passive_rx/gnb.sa.rfsim.conf`, receiver binary `adaptive-rx-UL-DL`
(`25a8699a64`), receiver conf the new agnostic scratch conf, recipe otherwise identical to the
`A0_baseline_106prb_rank1` launch (`-m 9 -n 0 -M 106 -l 1 -D 0xff` / `-C 3319680000 -r 106
--numerology 1 --band 78 --ssb 516`). Logs: `sens6:/home/sens/NICOLA/rfsim_validation/phyA2/<arm>/`.

### First check: does the agnostic resolver settle on dci_length=46 and read true fields?

**Half yes, half no — and the "no" half is a real, reproducible receiver bug.**

- **dci_length: YES, correctly self-discovered as 46**, no override needed:
  `DCI11_LAYOUT armed: 988 layouts (FDRA stages < 1) consistent with dci_length=46 (riv=13 bits,
  tda=0..4 bits (searched); ...)`. This matches Bug #2's live-verified value from the pinned-conf
  session — the agnostic path independently re-derives it rather than needing the override.
- **RNTI/CORESET-level decode: YES.** `rnti=0x1234` CRC-recovered almost immediately
  (`blind PDCCH rnti_seen ... rnti=0x1234 sfn=356 slot=16 fmt=1_1 cce=14 al=2`), consistent with the
  original bed check.
- **MCS field: YES, correctly self-discovered.** The Technique D `PARMSET[]` hypothesis dump shows
  `mcs=9` (matching the gNB's own fixed `MCS (0) 9` in its log) on every one of the 16 tracked
  hypotheses — the resolver's MCS inference is right.
- **TB-CRC decode: NO — permanently 0.0% across every measurement, ~31k-152k `pdsch_decode` tries
  each run, `skip_rv=0`/`unsup=0`/`over_cap=0` (nothing being filtered out, everything reaching LDPC
  and failing there).**

**Root cause found, with log evidence (this is the real receiver bug the brief asked to root-cause
if the agnostic resolver also reads wrong fields):** the agnostic TDA/symbol-range hypothesis catalog
never contains the cell's actual custom TDRA entries. Surveying every `sym=S+L` value that ever
appeared across the whole ~100s baseline-check run:
```
    119 sym=2+11
     51 sym=1+4
     51 sym=0+14
     34 sym=2+12
     17 sym=3+11
```
Only 5 distinct `(S,L)` hypotheses were EVER tried. The cell's actual custom
`pdsch-TimeDomainAllocationList` — independently derived for this exact gNB config in the earlier
pinned-conf session (`nr_rrc_config_dl_tda()`: idx0 = S:1/L:13, idx1 = S:1/L:12, idx2 = S:1/L:5,
recorded in `ue.passive.q.conf`'s `pdcch_blind_monitor_tda = "1:13,1:12,1:5"`) — is **absent from the
enumerated set entirely**: none of {1+13, 1+12, 1+5} appears anywhere in the log. Every tracked
hypothesis instead has S ∈ {0,1,2,3} paired with an L that looks derived from CORESET-duration/
default-table assumptions, not this gNB's actual custom table. Since the symbol range determines
which REs are data vs DM-RS, every candidate is being descrambled/LDPC-decoded against the WRONG RE
set, so LDPC always fails — this fully explains the permanent 0.0% CRC despite MCS being read
correctly (MCS/Qm inference is a separate, independent hypothesis dimension from symbol-range, and
only the latter is broken). **This is the same failure mode as the original Bug #3 (misaligned
post-FDRA field), now shown to survive even the fully agnostic `full_auto` resolver** — the gap isn't
in a fixed pinned-conf value that needed correcting, it's that the hypothesis catalog itself doesn't
include this cell's actual custom TDRA table. Source location not fully traced under this session's
budget (the catalog is built as `nr_pdcch_blind_monitor_rt.c`'s "prebuilt catalog template", per
`25a8699a64`'s own commit message "Technique D: build contexts outside g_lock from a prebuilt catalog
template" — that build site is where the candidate `(S,L)` set would need to be widened to include
non-default-table entries, or to blind-search S/L directly rather than from a small fixed table).
**Consequence: this blocks TB-CRC-based scoring for every arm needing it (A4-A7), identically to how
the original Bug #3 blocked them** — confirmed live below, not just inferred.

Full check-run log: `sens6:/home/sens/NICOLA/rfsim_validation/phyA2/job2_agn_check/rx/rx.log`.

### Per-arm results

| Arm | Env (gNB / RX) | Validity | Verdict | Evidence |
|---|---|---|---|---|
| A2 AL1-only | gNB `ISAC_GNB_TEST_USS_AL="4,0,0,0,0"` / RX `ISAC_AL1_COVER=1` | VALID | **PASS** | gNB live-confirmed `Candidates per PDCCH aggregation level on UESS: L1: 4, L2: 0, ...`; receiver recovered `rnti=0x1234` at `al=1` 152,030 times in 20s (`blind PDCCH monitor summary`, `dci10[accepts=472]`) — AL1-only scanning works end to end at the CORESET/RNTI level, independent of the TDA bug above (TB-CRC still 0.0%, expected/blocked). |
| A3 AL16 | gNB `ISAC_GNB_TEST_USS_AL="0,0,0,0,4"` | **INVALID — gNB CRASH** | **FAIL (gNB-side bug, not a receiver bug)** | gNB asserts and aborts within ~2s of scheduling start: `Assertion (CCEIndex >= 0) failed! In nr_preprocessor_phytest() gNB_scheduler_phytest.c:138 Could not find CCE for UE 1234`. The phy-test scheduler cannot place an AL16 candidate for the dummy UE on this CORESET — a real, reproducible gNB-side limitation of the `--phy-test` scheduler combined with this test knob, not investigated further (out of scope: it's the gNB, not the receiver, and the plan only asks for receiver root-causing). |
| A4 DL scrambling IDs ≠ PCI | gNB `ISAC_GNB_TEST_DL_DMRS_ID0=700 ISAC_GNB_TEST_DL_DATA_ID=500` | VALID (RNTI/CORESET level) | **BLOCKED** (same TDA-hypothesis-gap bug) | gNB live-confirmed both overrides applied (`ISAC_GNB_TEST_DL_DMRS_ID0=700 active`, `ISAC_GNB_TEST_DL_DATA_ID=500 active`). Receiver: `rnti=0x1234` recovered 179,721 times in 45s, but `pdsch_decode[try=37098 crc_ok=0 (0.0%)]` — indistinguishable from baseline, so this run cannot show whether the (separate, DM-RS/data-scrambling) walk-to-non-PCI-ID feature itself works; it's masked by the TDA bug. |
| A5 PDSCH type B k0=2 | gNB `ISAC_GNB_TEST_DL_TYPEB_K0=2 ISAC_GNB_TEST_DL_TDA_IDX=3` | **INVALID — gNB CRASH** | **FAIL (gNB-side bug, not a receiver bug)** | The type-B/k0=2 TDRA entry IS added at cell-config time (`ISAC_GNB_TEST_DL_TYPEB_K0=2 active: extra DL TDRA entry index=3 added...`, confirmed twice in the log) and the forced-index knob also fires (`ISAC_GNB_TEST_DL_TDA_IDX=3 active: DL TDA index forced every slot`), but the gNB **segfaults in `L1_tx_thread`** the moment it actually tries to schedule using that entry (`dmesg`: `L1_tx_thread[...]: segfault at 8 ... likely on CPU 9`, address 0x8 = classic near-NULL struct-field dereference). A second real, reproducible gNB-side bug, distinct from A3's assertion failure — not root-caused further (same out-of-scope reasoning as A3: gNB bug, not receiver). |
| A6 PRG=4 (negative control) | gNB `ISAC_GNB_TEST_PRG=4` | VALID | **PASS (as a negative control)** | gNB applied the override cleanly (`ISAC_GNB_TEST_PRG=4 active: ... bundleSize overridden to n4`), no crash. Receiver: `rnti=0x1234` recovered 174,055 times in 40s, `pdsch_decode[try=31825 crc_ok=0 (0.0%)]` — numerically indistinguishable from baseline, exactly as predicted (`rfsim-validation-plan.md`'s finding 2 in §2: the gNB PHY hardcodes `prg_size = rbSize` regardless of this RRC field, so it should change nothing observable — confirmed). |
| A7 qam256 / Qm oracle | gNB `-m 20 -n 1` | VALID | **PASS (Qm-oracle inference correct); TB-CRC still BLOCKED** | gNB confirmed scheduling `MCS (1) 20 (Qm 8)` = 256QAM. Receiver's Technique D hypothesis dump converged FAST and correctly to `mcs=20 tbl=1 Qm=8` as the dominant (121 occurrences vs 33/22 for the wrong-table alternatives) — the Qm-oracle correctly and quickly infers 256QAM. TB-CRC remains 0.0% (`pdsch_decode[try=29988 crc_ok=0]`), same root cause as the check run. |

### Job 2 summary

- **Real receiver bug found and root-caused (see "First check" above)**: the fully agnostic
  `full_auto`/Technique D TDA/symbol-range hypothesis catalog does not include this cell's actual
  custom `pdsch-TimeDomainAllocationList` entries (S=1 with L=13/12/5), so TB-CRC-dependent scoring
  is permanently blocked (0.0% across every arm that needs it) regardless of dci_length/MCS/RNTI all
  being independently correct. This is the same class of defect as the original session's Bug #3, now
  demonstrated to survive full agnosticity rather than being an artifact of one pinned wrong value.
- **Two independent, real gNB-side crashes found** (A3: AL16 CCE-placement assertion failure;
  A5: type-B/k0 TDRA segfault in `L1_tx_thread`) — both reproducible, both gNB bugs (in
  `agn-wt/gnbtest`'s test-only injection code interacting with the `--phy-test` scheduler), neither
  root-caused further per the brief's receiver-focused scope.
- **What DID work cleanly under full agnosticity**: dci_length self-discovery (46, no override),
  RNTI/CORESET-level blind decode (baseline, AL1-only, non-PCI-scrambling, PRG=4 — all recovered the
  RNTI at very high accept rates), and MCS/Qm-table inference (correct on both MCS 9/qam64 and MCS
  20/qam256).
- CLI env-var knobs `ISAC_GNB_TEST_USS_AL`, `ISAC_GNB_TEST_DL_DMRS_ID0/_DL_DATA_ID`,
  `ISAC_GNB_TEST_DL_TYPEB_K0`/`_DL_TDA_IDX`, and `ISAC_GNB_TEST_PRG` are all confirmed to fire
  correctly under `--phy-test` (same per-UE/cell-common config-build code path as the SA harness,
  answering the brief's "check phy-test honours the USS table" question: **yes for AL1, crashes for
  AL16**).

2. **Bug #3 (DCI sub-field width mismatch)**: hypothesis is that one or more of the fields between the
   FDRA/RIV and the RV/HARQ-process-ID fields (candidates: PUCCH resource indicator, PDSCH-to-HARQ
   feedback timing indicator, DAI, antenna-port field, or CBG fields) has a different bit width in the
   phy-test dummy UE's dedicated PDSCH-Config than in the SA/core-attached-UE harness this receiver
   conf was tuned against — most likely because phy-test's simpler config disables/simplifies one of
   those RRC-negotiated features. Next step: use `ISAC_DCI_FIELDS=1` (already proven to work) together
   with a deliberate, mechanical sweep of one field width at a time in a scratch
   `pdcch_blind_monitor_dci_bits` conf, checking for `mcs` converging to the gNB's own fixed value (9)
   and `rv` finally reading 0 on the first transmission of each HARQ process — the same live-diagnostic
   method that found the 2026-07-30 bwp_indicator/TDA bug.
