# gNB test-only env-gated injection — report

Lane `gnbtest`, worktree `sens6:/home/sens/NICOLA/agn-wt/gnbtest`, branch `sdd/rfsim-gnb-test`,
commit `67bb0eaa11`. `nr-softmodem` built clean via `lane-make.sh` (plus the `params_libconfig` and
`rfsimulator` dlopen'd plugin targets, needed to actually run the binary). No file under
`gnb_remote_logs/`, `cuLogs/`, or sens4/X410 was touched. Test-only; not intended for merge into
`adaptive-rx-UL-DL`.

All seven knobs default OFF (env var unset). Verified by code read that every new code path is
inside an `if (isac_gnb_test_env_long(...))` / `getenv(...)` guard with no other change to the
unconditional default path — unset behaviour is unchanged source, not just "should be fine".

## Per-knob verdict

### 1. `ISAC_GNB_TEST_USS_AL = "a1,a2,a4,a8,a16"`
**File:line**: `openair2/GNB_APP/gnb_config.c:1618` (right after the existing
`memcpy(config.num_agg_level_candidates, ...)`).
**Verdict: REAL, LIVE-CONFIRMED.** Note: this table already had an *undocumented* `.conf`-level
override (`uess_agg_levels`, `GNB_UESS_AGG_LEVEL_LIST_IDX`) that the rfsim-validation-plan audit
missed — the env var is layered on top of that, applied last, so it always wins. Live smoke
(standalone `nr-softmodem -O gnb.sa.rfsim.conf --rfsim`, config-build happens before RU/PHY bring-up,
no UE attach needed):
```
[NR_MAC] ISAC_GNB_TEST_USS_AL=4,0,0,0,0 active: UESS aggregation candidates overridden (test-only)
```
Confirmed firing exactly once, at cell-config-build time.

### 2. `ISAC_GNB_TEST_VRB_IL = 2|4`
**File:line**: `openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c` (`config_pdsch()`, right after the
existing `pdsch_Config->prb_BundlingType` block, ~old line 1750).
**Verdict: REAL for RRC IE + DCI bit; the gNB PHY TX does NOT actually interleave.**
- Setting `vrb_ToPRB_Interleaver` non-NULL is exactly the condition
  `gNB_scheduler_primitives.c:1772` (`prepare_dci_X1`, DCI format 1_1) and
  `nr_mac_common.c:3227-3234` (`nr_dci_size`) already check to set
  `dci_pdu_rel15->vrb_to_prb_mapping.val = 1` and reserve the DCI bit — so the RRC IE and the DCI
  bit both follow automatically, no extra scheduler change needed.
- BUT: `gNB_scheduler_dlsch.c:962` hardcodes `pdsch_pdu->VRBtoPRBMapping = 0; // non-interleaved`
  unconditionally in the FAPI PDU handed to L1, and `openair1/PHY/NR_TRANSPORT/nr_dlsch.c:689`'s
  resource-mapping code is headed `// Non interleaved VRB to PRB mapping` with no interleaved code
  path at all. **So this knob signals "interleaved" to the UE in DCI while the actual PDSCH RE
  placement stays contiguous.** A real UE that trusts the bit and de-interleaves would get it wrong;
  a passive receiver doing the same must be aware this is a signalling-only test surface on this
  gNB, not a physical VRB permutation.
Not live-smoked: `config_pdsch()` only runs when building a UE's own dedicated `PDSCH-Config`
(three call sites, all per-UE: initial `SpCellConfig`, BWP-switch reconfig, `get_initial_SpCellConfig`
— confirmed by reading all three callers, none is cell-common/startup), which needs a UE to reach
RRC connection. The local open5gs AMF NGAP listener (127.0.0.1:38412) was not up in this session
(no SCTP listener found, `amfd` process present but not serving) and standing it up was out of
scope for a "cheap" smoke — code-read verification only for this knob.

### 3. `ISAC_GNB_TEST_PRG = 2|4`
**File:line**: `nr_radio_config.c` (`config_pdsch()`, right after the existing
`prb_BundlingType.choice.staticBundling->bundleSize` assignment, ~old line 1750).
**Verdict: PRG=4 REAL (RRC IE only); PRG=2 REFUSED (spec/crash reason logged); gNB precoding PRG
size is NOT driven by this field at all — confirmed.**
- `NR_PDSCH_Config__prb_BundlingType__staticBundling__bundleSize` (generated ASN1 enum) only has
  two values, `{n4, wideband}` — there is no static "n2". A true PRG=2 needs `dynamicBundling`,
  which `gNB_scheduler_primitives.c:1777` `AssertFatal(1==0, "Dynamic PRB bundling type currently
  not supported\n")`s as soon as a UE with that config reaches DCI 1_1 prep. Setting it would crash
  the gNB, so the knob logs `LOG_E` and does nothing for `PRG=2` rather than doing that.
- Independent of the RRC field entirely: `gNB_scheduler_dlsch.c:974` hardcodes
  `pdsch_pdu->precodingAndBeamforming.prg_size = pdsch_pdu->rbSize;` — one PRG spanning the WHOLE
  scheduled allocation, always, regardless of `prb_BundlingType`/`bundleSize`. **The RRC field
  changes nothing observable in the actual precoding** — exactly the "say so" case the plan
  anticipated.
Not live-smoked, same reason as #2 (per-UE config path, no open5gs core up this session).

### 4. `ISAC_GNB_TEST_DL_DMRS_ID0` / `_DL_DMRS_ID1` / `_DL_DATA_ID`
**File:line**: `nr_radio_config.c` (`config_pdsch()`, at the existing
`scramblingID0/scramblingID1 = NULL` lines ~old 1738-1739, and `dataScramblingIdentityPDSCH = NULL`
~old 1744).
**Verdict: REAL, confirmed used for actual scrambling, not just signalled.**
- DL DM-RS: `gNB_scheduler_primitives.c:348` `scramblingID = dmrs.n_scid ? dmrs_Config->scramblingID1
  : dmrs_Config->scramblingID0;` feeds `dmrs_parms`, which `gNB_scheduler_dlsch.c:935` assigns
  directly to `pdsch_pdu->dlDmrsScramblingId` — the live FAPI PDU field L1 uses for the DM-RS
  sequence.
- DL data: `gNB_scheduler_dlsch.c:926`
  `pdsch_pdu->dataScramblingId = pdsch_Config && pdsch_Config->dataScramblingIdentityPDSCH ?
  *pdsch_Config->dataScramblingIdentityPDSCH : *scc->physCellId;` — falls back to PCI only when
  NULL, uses our injected value otherwise.
Not live-smoked (per-UE config path, same open5gs blocker as #2/#3).

### 5. `ISAC_GNB_TEST_UL_DMRS_ID0` / `_UL_DMRS_ID1` / `_UL_DATA_ID`
**File:line**: `nr_radio_config.c` (`config_pusch()`, `dataScramblingIdentityPUSCH = NULL` ~old
1592, `scramblingID0/1 = NULL` ~old 1616-1617).
**Verdict: REAL, confirmed used, UL-side mirror of #4.**
- UL DM-RS: `gNB_scheduler_primitives.c:1522-1531` reads `pusch_Config->dataScramblingIdentityPUSCH`
  and the mapping-type-A/B `scramblingID0` directly for PUSCH DM-RS/scrambling derivation.
- UL data: `gNB_scheduler_ulsch.c:2320-2321`
  `if (ul_bwp->pusch_Config && ul_bwp->pusch_Config->dataScramblingIdentityPUSCH)
  pusch_pdu->data_scrambling_id = *ul_bwp->pusch_Config->dataScramblingIdentityPUSCH;` — same
  pattern, live FAPI PDU field, not signalling-only.
Not live-smoked, same open5gs blocker.

### 6. `ISAC_GNB_TEST_DL_TYPEB_K0 = k0`
**File:line**: `nr_radio_config.c`, end of `nr_rrc_config_dl_tda()` (right before its closing
brace, ~old line 1096), mirroring `set_TimeDomainResourceAllocation()`'s PUSCH typeB entries
(`get_SLIV(0, 13)`, same S/L as the PUSCH helper).
**Verdict: REAL, LIVE-CONFIRMED entry added; scheduler does NOT pick it on its own (as predicted) —
use knob 7 to force it.**
Live smoke (same standalone run as #1, cell-common config-build, no UE needed — this function runs
at cell bring-up, unlike `config_pdsch`/`config_pusch`):
```
[NR_RRC] ISAC_GNB_TEST_DL_TYPEB_K0=2 active: extra DL TDRA entry index=3 added, mappingType=typeB k0=2 (test-only)
```
Confirms this test cell's `get_dl_tda()` produces 3 default entries (indices 0/1/2 — basic, CSI-RS,
TDD-mixed-slot) before ours, so the new entry landed at index 3, as the running code's own
`pdsch_TimeDomainAllocationList->list.count - 1` reports (not hand-assumed).

### 7. `ISAC_GNB_TEST_DL_TDA_IDX = idx`
**File:line**: `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch.c:32` (`get_dl_tda()`, forced-return
check inserted before the existing CSI-RS/mixed-slot logic).
**Verdict: added exactly as the plan called for; code-read verified, NOT live-fired.**
Confirmed by code read that `get_dl_tda()` — read once via a lazy-init static, so the env var costs
nothing on the scheduling hot path after the first call — otherwise only ever returns 0, 1
(CSI-RS slot), or 2 (TDD mixed slot), so knob 6's index-3 entry is unreachable without this.
`ISAC_GNB_TEST_DL_TDA_IDX=%d active` string is present and correctly formatted in the built binary
(`strings nr-softmodem | grep ISAC_GNB_TEST_DL_TDA_IDX`), confirming it compiled/linked, but it was
never live-triggered this session because that requires actual DL scheduling activity (an
RRC-connected UE with DL traffic), which needs the same open5gs core that was unavailable — see
below. **Not yet verified live that the DCI TDRA field and PDSCH timing actually follow the forced
index** — flagged as the first thing to check once a smoke is run with a working core.

## Why several knobs are code-read-only, not live-smoked

Knobs 2-5 and 7 only exercise inside a per-UE dedicated `PDSCH-Config`/`PUSCH-Config`/scheduler-hot-path
build, which needs at least one UE to reach RRC connection over the `tests/passive_rx/` harness
(SA gNB + local open5gs core + active UE). This session found the local open5gs AMF was not
serving NGAP (`amfd` process present but no SCTP listener on 127.0.0.1:38412) — standing up/
debugging the core is a separate, non-trivial task outside a "cheap ≤3 min smoke", so per the
brief's own fallback rule ("otherwise just a clean build plus a code-read verification per knob")
these five knobs got a thorough code-read verification (exact file:line, confirmed propagation into
the real FAPI/scheduling struct the gNB actually uses, not just the RRC IE) instead of a live run.
Knobs 1 and 6 (cell-common config, no UE needed) were live-confirmed via a standalone
`nr-softmodem -O gnb.sa.rfsim.conf --rfsim` smoke (killed after ~20s with `timeout -s INT`; the run
also hit an unrelated `--sa` cmdline-flag mistake on my part, immaterial to the two knobs under
test, which logged and exited cleanly beforehand).

## Build detail
- `ninja`/`make` targets built: `nr-softmodem`, `params_libconfig`, `rfsimulator` (both dlopen'd
  plugins, needed just to run the binary at all — no source under them was touched).
- No `nr-uesoftmodem` rebuild needed: all three changed files
  (`openair2/GNB_APP/gnb_config.c`, `openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c`,
  `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch.c`) are gNB-only sources, not linked into the UE
  target.
- Zero compiler warnings/errors attributable to the new code (full build log reviewed for `error`
  matches — all hits were unrelated ASN1 type names like `S1AP_TypeOfError.c.o`).
