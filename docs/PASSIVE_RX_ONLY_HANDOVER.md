# Passive 5G NR Receiver — Handover (2026-08-21)

**Read §1 before touching anything.** This session produced five retractions, all from the same
class of mistake: asserting a cause from an aggregate statistic without pinning the measurement
window or checking the alternative. The rules exist so the next agent does not repeat them.

Machine: **sens6** (X410 host). Tree: `/home/sens/NICOLA/openairinterface5g-total-passive-ue`,
branch **`passive-rx-only`** @ `0799c44294`. Everything lives under `/home/sens/NICOLA`.

---

## 1. Rules to respect

**R1 — Never pair log lines by adjacency.** Decodes for different slots interleave in the log.
Pairing `TBPARM` with the following `TBRESULT` produced two mutually contradictory breakdowns
before it was fixed by putting every field on ONE line. Same class of error as the retracted
"20 % dt bias" in CLAUDE.md §8. If you need to correlate two quantities, log them together.

**R2 — sens4's clock runs ~21 min behind sens6.** Do NOT do clock arithmetic to align the gNB log
with a capture. Bracket the capture with `stat -c%s` on the gNB log before and after, then extract
exactly that byte range. This is the only correlation method that has ever worked here.

**R3 — Re-apply the NIC tuning after every reboot.** It is NOT persistent and sens6 rebooted five
times this session. Without it the 100 GbE link silently drops samples. Recipe in §3.

**R4 — Never `pkill -f nr-uesoftmodem` over SSH** — it matches the SSH command line and kills your
session. Use `pkill -9 -x nr-uesoftmodem`.

**R5 — Launch captures detached, with `ulimit -c 0`.** A foreground run at RT priority starves
sshd and the box becomes unreachable; a segfault then dumps a multi-GB core and stalls it further.
Use the `setsid nohup bash -c "ulimit -c 0; exec timeout N ..."` form in §4.

**R6 — Reconcile every derived value against a known-good one.** Deriving the TDRA from SIB1
immediately exposed a real SLIV bug in the new code. Derivation without reconciliation would have
shipped a valid-looking but wrong symbol allocation.

**R7 — A gate that improves a score may be hiding the effect you are measuring.** "MCS 9 fails
100 %" was an artefact of the energy gate selecting a biased subsample; with gates off it is 33 %.
Measure with gates OFF when characterising, with gates ON when scoring.

**R8 — Do not run a build during a capture** (CLAUDE.md, still true), and do not sleep through one.

---

## 2. Status — what works, measured

Full passive receiver: **never attaches, never transmits.** 300 s, operational config, live cell.

| layer | result |
|---|---|
| Sync / PBCH | locks; `RXDISCONT=24`, no crashes, clean exit |
| **CSI-RS** | **58,844** measurements, SNR 7.5 dB, slots 2 & 3 as derived |
| **PDCCH / DCI** | 470,000 occasions, **6,674 accepts**, **100 % RNTI purity** |
| **PDSCH** | **5,145 / 5,621 = 91.5 %** (MCS 10: **99.92 %**) |
| False positives | **0** — no spurious RNTI ever passes PDSCH CRC |

**Validated against the gNB's own scheduler log** (not self-reported):
- DCI position `(SFN, slot, CCE)`: **9,362 / 9,362**; SFN-offset scan peaks sharply at 0 (neighbours ~26 %).
- DCI fields `al/mcs/rv/ndi/harq_pid`: **2,570 / 2,570**.
- TBS exact at 2/8/94/97 PRB → 84/333/3905/4033 bytes.
- `TBS_LBRM` 1,277,992 bits = gNB's 159,749 B × 8.

**Autonomy achieved so far** — common config now derived over the air from SIB1, exact match to the
hand-written config:
```
OTA : BWPStart=0 BWPSize=273 tdra_entries=2
      tda[0] SLIV=40 -> S=1 L=13 ; tda[1] SLIV=85 -> S=1 L=7
      => dci freq_domain_bits=16  time_domain_bits=1
cfg : pdcch_blind_monitor_bwp = "0:273:0:47" , tda = "1:13,1:7"
```

---

## 3. Environment

- X410 data `192.168.20.2` on `enp129s0f1np1`, mgmt `128.178.122.3`. sens6 tailscale `100.65.157.61`.
- gNB is **sens4**: `/home/sens/NICOLA/gnbLogs/gnb.log`, `all_level: debug`, grows ~10 MB/s.
- **NIC tuning (re-apply after every reboot — R3):**
  ```bash
  IF=enp129s0f1np1
  sudo ip link set dev $IF mtu 9000
  sudo ethtool -G $IF rx 8192 tx 8192
  sudo sysctl -w net.core.netdev_max_backlog=250000 net.core.rmem_default=62500000 \
                 net.core.rmem_max=250000000 net.core.wmem_max=250000000
  ping -c1 -M do -s 8972 192.168.20.2   # must succeed
  ```
- **X410 recovery** when UHD reports `rx xport timed out getting a response from mgmt_portal`:
  clearing the UHD claim and stale DPDK state is NOT enough. What works:
  `ssh root@128.178.122.3 "systemctl restart usrp-hwd"`. Also clear orphaned hugepages after a
  crash: `sudo rm -rf /var/run/dpdk/rte /dev/hugepages/rtemap_*`.
- **gNB must be at `max_rank: 1`** (see §5). `cell_cfg.pdsch: max_rank: 1`, `max_ue_mcs: 10`.

---

## 4. Commands

**Build** (sensing off by default on this branch):
```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make nr-uesoftmodem -j8
# to build WITH the sensing pipeline instead:
cmake . -DENABLE_ISAC_SENSING=ON && make nr-uesoftmodem -j8
```

**Run the passive receiver** (detached, per R5):
```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets
sudo -E env ISAC_DISC_NO_RESYNC=1 setsid nohup bash -c "ulimit -c 0; exec timeout 300 \
  ./ran_build/build/nr-uesoftmodem \
  --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3,use_dpdk=1234 \
  -O /home/sens/NICOLA/nrue.passive_rx.conf \
  -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 --ue-rxgain 40 \
  --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation \
  --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
  > /tmp/run.log 2>&1 < /dev/null &
```
`ISAC_DISC_NO_RESYNC=1` is required. The `--time-sync-I / --ntn-initial-time-drift / -A 90` flags
come from the verified x410-100MHz baseline invocation; they configure the timing loop.

**Wait and score:**
```bash
until ! pgrep -x nr-uesoftmodem >/dev/null; do sleep 10; done
grep -a "blind PDCCH monitor summary" /tmp/run.log | tail -1
R=$(grep -a rnti_seen /tmp/run.log | grep -oE "rnti=0x[0-9a-f]+" | sort | uniq -c | sort -rn | head -1 | grep -oE "0x[0-9a-f]+")
echo "real UE $R: $(grep -a TBRESULT /tmp/run.log | grep "rnti=$R" | grep -c CRC_OK) / $(grep -a TBRESULT /tmp/run.log | grep -c "rnti=$R")"
```

**Probes** (all default-off, read-only):

| env | prints |
|---|---|
| `ISAC_PDCCH_DCIGT=1` | per-accept `(SFN.slot, cce, al, mcs, rv, ndi, hid, tda, prb, sym, cdm, ports, nscid, dmrsmask)` |
| `ISAC_PDSCH_TBPARM=1` | TB params + `SEGDIAG` (C/K/Z/F/E/R) + `CHESTDIAG` (per-layer power, Gram orthogonality) + per-RNTI `TBRESULT` |
| `ISAC_CSI_DIAG=1` | per-resource CSI-RS power/noise/SNR |
| `ISAC_OTA_CFG=1` | common config derived from SIB1 (BWP, TDRA, DCI field widths) |
| `ISAC_PDCCH_NO_MISMATCH_GATE=1` | bypass the `dci_thres` gate (diagnosis only) |

**Ground-truth correlation** (R2 — byte offsets, never clocks):
```bash
B=$(ssh sens4 'stat -c%s /home/sens/NICOLA/gnbLogs/gnb.log')   # before
# ... run capture ...
A=$(ssh sens4 'stat -c%s /home/sens/NICOLA/gnbLogs/gnb.log')   # after
ssh sens4 "tail -c +$((B+1)) /home/sens/NICOLA/gnbLogs/gnb.log | head -c $((A-B))" > /tmp/gt.log
```

---

## 5. Limitations (measured, not assumed)

1. **Spatial rank — the hard one.** Rank-4 PDSCH: **0 / 28,624**. Rank-1: 90–99 %. Measured layer
   independence with a branch-normalised Gram determinant: `resid = [1.00, 0.90, 0.44, 0.55]`,
   i.e. ~2.5 effective layers. The gNB precodes toward the *served* UE; we are elsewhere. The
   0.4 λ 2×2 UPA is right for AoA and structurally wrong for MIMO demultiplexing — those goals
   conflict. **Requires `max_rank: 1` on the gNB.**
2. **Retransmissions.** `rv != 0` cannot be decoded (no HARQ history to combine with). This limits
   the **data-aided** source only — DM-RS-based CFR works on those grants, since the DCI decodes
   fine and only the allocation is needed. Loss is RE density, not the measurement.
3. **MCS 9 — unresolved.** 0/472 with gates on, 276/826 (33 %) with gates off. Its
   `Qm/R/rv`/TBS were each verified exact against the gNB. Same MCS+TBS+PRB yields BOTH outcomes,
   which rules out a deterministic parameter bug but not stale state. Run-length data shows MCS 9
   occurs in 3.5-grant bursts vs 26.6 for MCS 10 (transient link adaptation), which is suggestive
   of fading but NOT proven. `max_ue_mcs: 9` on the gNB would settle it.
4. **Antenna imbalance** varies with the array setup; 10–17 dB was measured at one point, ~1 dB
   after the user re-spaced. It shows in both CSI-RS and PDSCH, so it is front-end/array, not code.

---

## 6. Sensing status

The sensing pipeline is **linked out** on this branch (`ENABLE_ISAC_SENSING=OFF` → `nr_isac_stub.c`).
Nothing has produced a range-Doppler map from this data; the figures below are **projections from
measured row rates**, not measured sensing output.

- **Range: fine.** 3.05 m at 273 PRB; 50 % of decodes are full-band, 72 % ≥ 50 PRB.
- **Doppler: PRF-limited.** `vel_max = ±λ·PRF/4`, λ = 8.78 cm.
  PDSCH alone 17 rows/s → **±0.38 m/s**. CSI-RS 196 rows/s. Fused ~213 → **±4.7 m/s**.
- **Drones need ±30 m/s ⇒ PRF ≈ 1.4 kHz.** Row-rate tuning cannot close a 60× gap.
  **Sub-slot sampling is the only lever of the right order**: a full-band data-aided grant is
  comb-1 across 13 symbols, so emit up to 13 rows per grant instead of 1. Already implemented
  (`nr_isac_subslot_config`, `nr_isac_submit_cfr_at(slot_frac)`), currently unused.
- **Range migration bites fast targets**: 30 m/s crosses ~6 range bins in a 0.6 s CPI. Use SHORT
  CPIs — migration compensation was tested and rejected (`CPI_FORMATION_ANALYSIS.md`).
- Row-rate levers, measured: energy gate off = **5×** (22 → 109 accepts/s); `rv != 0` via DM-RS
  ≈ 3–4× more rows; `pdsch_max_per_slot`; more active UEs on the cell.

---

## 7. To implement next (in order)

### 7.1 DCI format 1_0 support — **DONE and OTA-VALIDATED 2026-08-21. See §10.**
All five RNTI variants, search-space-correct sizing/TDRA/DM-RS/MCS semantics. On air: **SIB1
decoded end to end, 123/123 = 100 % PDSCH CRC, every DCI field matching the gNB's own log.**
Remaining gap: the monitor scans ONE search space at a time, so CORESET#0 and the dedicated CORESET
need separate runs (§10.1 item 3).

### 7.2 Parse RRCSetup from decoded PDSCH → learn the dedicated config
RRCSetup is **unciphered** and carried on PDSCH, which this receiver already decodes. Extract
`masterCellGroup` → dedicated CORESET, search space, DM-RS additional position, MCS table, CSI-RS
resources. Then the only remaining externally-supplied item is gone.
**Residual gap, state it honestly:** later `RRCReconfiguration` is **ciphered** after security
activation, and a UE that attached before we started listening never sends RRCSetup again.

### 7.3 Feed the OTA-derived config into the blind monitor
Today `ISAC_OTA_CFG=1` only *prints* the SIB1-derived BWP/TDRA. Wire it so the monitor consumes it
instead of `pdcch_blind_monitor_bwp` / `_tda`, with the config file as override/fallback.

### 7.4 Move the PDSCH decode off the PHY receive thread
CLAUDE.md §12 flags this as "the first thing to break on real hardware". It is the prerequisite for
banking the §6 row-rate gains — pushing decode volume 5–10× is exactly what would expose it.

### 7.5 Enable sub-slot sampling and re-integrate sensing
Build with `-DENABLE_ISAC_SENSING=ON`, `pdsch_decode=2`, `sources="csi_rs,pdsch_data"`, then read
the real `range[res=...] vel[res=... max=...]` off the CPI lines instead of projecting.

### 7.6 Resolve MCS 9
Set `max_ue_mcs: 9` on the gNB so MCS 9 becomes the *sustained* MCS. If it then decodes at ~90 %,
transience/fading is proven and the decoder is exonerated. If it still fails, look at stale
per-`harq_unique_pid` state in the LDPC interface (`g_harq` is `static __thread`).

---

## 10. DCI format 1_0 — what landed (2026-08-21)

Offline-complete, **live-unvalidated** (no gNB was running on sens4 while this was written; the log
had been static for 14 min and there was no gNB process — `pgrep -f gnb` matching your own ssh
command line is a trap, use `pgrep -x`).

**Core.** `nr_pdcch_blind_decode_and_extract_10()` in `nr_pdcch_blind_monitor.c`, alongside the
untouched 1_1 entry point. Field order and widths were taken from TS 38.212 7.3.1.2.1 and then
reconciled field-by-field against this codebase's OWN gNB packer
(`gNB_scheduler_primitives.c`'s `NR_DL_DCI_FORMAT_1_0` case) — derivation plus reconciliation, per R6.

**The structural fact the design rests on**, asserted by a test rather than assumed: all five 1_0
variants (C/TC, SI, RA, P) are exactly **28 + RIV** bits. So ONE polar decode is re-interpreted under
several RNTI hypotheses at no extra decode cost — which is what a blind receiver needs, since it
learns the RNTI only from the CRC.

**Three things change with the SEARCH SPACE, and each is silently wrong rather than loudly wrong if
mixed up.** This is why `nr_pdcch_blind_dci10_ctx_t` exists instead of reusing the BWP config:
- **Frequency reference** (TS 38.212 7.3.1.0): CORESET#0's size in a common SS, the active DL BWP in
  a UE-specific one. The same RIV names a different allocation under each — pinned by a test.
- **PRB origin** (TS 38.214 5.1.2.2.2): the CORESET's lowest RB in a common SS. OAI's own coreset-0
  path does exactly this (`nr_ue_dci_configuration.c:224` sets `BWPStart = cset_start_rb`).
- **TDRA list** (TS 38.214 Table 5.1.2.1.1-1): the DEDICATED pdsch-Config list applies ONLY to
  C-RNTI in a UE-specific SS; SI/RA/TC and C-in-a-CSS take pdsch-ConfigCommon's
  (`pdcch_blind_monitor_tda_common`, defaulting to reuse the dedicated one — correct here, since
  SIB1's list and `pdcch_blind_monitor_tda` were reconciled identical).

**Two semantics that are NOT configurable because the spec fixes them, and both would otherwise be
inherited wrong from the 1_1 config on this exact deployment:**
- `mcs_table` is **always** Table 5.1.3.1-1/qam64 for 1_0 (TS 38.214 5.1.3.1; qam256 is conditioned
  on 1_1). `nrue.passive_rx.conf` sets `mcs_table = 1` (qam256) for 1_1, so applying it to a 1_0
  grant would give the wrong Qm AND the wrong code rate, i.e. the wrong TBS, failing CRC in a way
  that looks like a bad channel.
- `dmrs-AdditionalPosition` is **pos2** for 1_0 with mapping type A regardless of any dedicated DMRS
  config — `fill_dmrs_mask()`'s own `dci_format != NR_DL_DCI_FORMAT_1_0` guard is this rule. Type B
  does read the dedicated value, so the override still applies there.

**`numDmrsCdmGrpsNoData` is DERIVED, not decoded** — 1_0 has no antenna-ports field. TS 38.214
5.1.6.1.3: 1 for a 2-symbol allocation, 2 otherwise; port 1000 only; `nscid = 0`.
**Consequence worth knowing given `max_rank: 1`:** rank-1 DCI 1_1 grants take
`numDmrsCdmGrpsNoData = 1` (`gNB_scheduler_primitives.c:315`, unconditional for single-layer 1_1),
i.e. the branch where a DM-RS symbol still carries data in the unreserved CDM group. An ordinary-
length 1_0 grant takes 2 — the whole-symbol branch. **So enabling 1_0 exercises a different arm of
the data-aided RE enumeration than anything measured so far**, and the `mod_idx == G/(Qm·Nl)`
invariant is the thing to watch. Both arms were checked against `nr_get_G()`'s budget on paper and
`nb_re_dmrs` is derived from `n_dmrs_cdm_groups` throughout, TBS included.

**Rejections that keep a blind scan honest** (each its own test):
- **PDCCH order**: an all-ones frequency-domain field on a C-/TC-RNTI 1_0 is an RA command, and the
  bits after it are preamble/SSB/PRACH-mask. Parsing it as a grant gives a confident wrong
  allocation — the exact failure class this module keeps hitting.
- **Reserved bits**: 15 (SI) / 16 (RA) / 6 (P) spec-fixed zeros. On SI this is a 1-in-32768
  signature and by far the strongest false-accept discriminator this format offers.
- **RA-RNTI ≤ 17920** (TS 38.321 5.1.3's formula maximum) — the only spec-derived way to stop an RA
  hypothesis shadowing a TC/C grant, since a perfectly ordinary C-RNTI grant can also present 16
  zero tail bits.
- **Interleaved VRB-to-PRB**: decoded, reported, and **rejected**. OAI's UE PHY has no
  de-interleaver at all (`nr_ue_procedures.c` sets `vrb_to_prb_mapping`; nothing in
  `openair1/PHY/NR_UE_TRANSPORT` ever reads it). Extracting contiguous PRBs anyway would look like a
  weak channel rather than a wrong one. **If the accept census shows this firing, that is the reason
  SIB1/RAR are not decoding, and a de-interleaver is the fix.**
- **Format 0_0**: 0_0 and 1_0 share a payload size by construction, so the identifier bit is the
  only separator. Also: size-alignment padding is checked for zero.
- MCS ≥ 29 (table 1's reserved range — deliberately different from the 1_1 path's ≥ 28, which is
  table 2's), TB scaling = 3, TDA index past the list (the field is always 4 bits in 1_0, so on a
  2-entry list 14 of 16 code points are impossible).

**Also changed:** `nr_pdsch_passive_grant_t` gained `tb_scaling` (was hardcoded 0 in
`nr_compute_tbs()`), needed for RAR/paging TBS. Default 0 = identity, so nothing else moves.

**Config** (`nrue.passive_rx.conf`, `pdcch_blind_monitor_dci10 = "1"` now set):
`scan[:ss_type[:n_rb_riv[:rb_offset[:length_override[:class_mask[:mux_pattern[:sib1]]]]]]]`,
scan 0 = 1_1 only (previous behaviour), 1 = both, 2 = 1_0 only. **Cost: a second polar decode per
candidate** — the two widths differ (44 vs 47 bits at 273 PRB) so the decode cannot be shared;
budget roughly double the tap's CPU.

**`ISAC_OTA_CFG=1` now also dumps CORESET#0 from the MIB** (`nr_ue_dci_configuration.c`) — `num_rbs`,
`cset_start_rb`, `num_symbols`, mux pattern, SS0 occasions — and prints the four config lines for a
CORESET#0 scan pre-filled. Every constant in that suggestion is read off `fill_coresetZero()`
(always interleaved, reg-bundle 6, **interleaver size 2**, shift index and DM-RS scrambling id both
falling back to the PCI), not from spec text. Do not hand-guess those values.

**Observability:** the periodic summary gained `dci10[accepts=.. C=.. TC=.. SI=.. RA=.. P=..]`, and
`ISAC_PDCCH_DCIGT=1` now prints `fmt=`/`class=`/`mcs=N/tblM`/`prb=..(orgN)`/`vrb=`/`tbs_scal=`.
A single aggregate accept count cannot tell "found the RRCSetup" from "found more fallback grants".

**Tests:** `test_nr_pdcch_blind_monitor` 7 → 35 cases, all passing. The 7 pre-existing 1_1 cases pass
unchanged; the only lines removed from the 1_1 path are the two blocks factored into shared helpers
(`blind_polar_decode`, `blind_mismatched_bits`), verbatim. The split between those two helpers is
load-bearing: the re-encode runs only for an RNTI that already passed its range check.

### 10.0 OTA validation (2026-08-21, live cell, iperf on) — **PASSED**

Two arms, 150 s each, X410, `-r 273`, gNB log bracketed by byte offset (R2). The C-RNTI moved
0x4601 -> 0x461a between the two arms — a re-attach, exactly why it must be re-read every run.

**Arm B — CORESET#0, format 1_0 only (`nrue.passive_rx.coreset0.conf`).** This is the real test:
SI-RNTI is the only genuine format-1_0 traffic this cell carries.
- **123 / 123 = 100.0 % PDSCH CRC.** 126 accepts (125 SI + 1 TC); the single TC accept was a false
  decode and the pre-existing mismatch/persistence gates caught it, so **nothing spurious reached
  extraction**.
- **Every extracted field matches the gNB's own dump**, which is what CLAUDE.md §12 records as
  never having been done for 1_1: `rnti=0xffff`, `cce=0`, `al=4` (gNB prints
  `dci_aggregation_level=2` = log2), `mcs=5`, `mcs_table=0`, `rv=0`, `prb=0+10` vs `vrbs=[0..10)`,
  `sym=2+12` vs `symb=[2..14)`, `cdm=2`, `ports=0x1`, `nscid=0`, `dmrsmask=0x884` vs
  `dl_dmrs_symb_pos=00100010000100`, `vrb=0`. TB side: `tbs=808` bits vs the gNB's `tbs=101` bytes,
  `Qm=2`/QPSK, `bg=2`, `nb_re_dmrs=12`, `dmrs_len=3`.
- **The payload width was confirmed before any decode**: `nr_pdcch_blind_dci10_size(48)` = 28+11 =
  **39**, and the gNB logs `payload_size=39`.
- Coverage 126 / 1049 SI-RNTI PDCCH in the window = **12 %**, limited by the energy gate and by
  scanning only symbol 0. Correctness, not coverage, was what this arm tested.

**Arm A — dedicated CORESET, BOTH formats (`nrue.passive_rx.conf`, `dci10 = "1"`).** A negative
control for the cost of adding a second decode.
- 1_1: **795 accepts, 722/795 = 90.8 % CRC** against §2's 91.5 % baseline — **no regression**.
- 1_1 RNTI purity **795/795 = 0x461a**, matching the gNB's own **24,773/24,773**.
- 1_0: **0 accepts, and that is correct** — the gNB scheduled 24,773 DL PDCCH in the window, ALL
  `type=c-rnti format=1_1`, zero format 1_0. **Zero false accepts from the extra format over
  234,000 occasions**, i.e. the second decode adds no noise floor.

**Three real bugs this OTA run found and fixed** (offline tests could not have caught any of them —
all three are deployment-geometry, not payload logic; the third turned out to be two):

1. **DM-RS sequence reference point — the one that mattered.** First arm-B run: every DCI field
   perfect, **0/32 PDSCH CRC**. `nr_dl_channel_estimation.c:1249` computes the gold-sequence offset
   as `first_rb + (refPoint ? 0 : BWPStart)`, and the monitor hardcoded `refPoint = 0`. TS 38.211
   7.4.1.1.2 references the sequence to CRB 0 *in general* but to the **lowest RB of the CORESET**
   for a format-1_0 PDSCH with CRC scrambled by SI-RNTI in a Type0-PDCCH CSS — which is OAI's own
   `mac->get_sib1 ? 1 : 0`, and which the gNB logs as `ref_point=1`. This cell puts CORESET#0 at
   CRB 1, so the sequence was off by one RB. **0/32 -> 123/123 with that single change.** A wrong
   sequence looks like a dead channel, not like a wrong parameter — the same failure signature as a
   wrong `csirs_monitor` scramb_id. Now scoped to SI-RNTI; RA-/TC-/P-RNTI and all 1_1 keep CRB 0.
   *Known gap:* the spec scopes the exception to Type0-CSS and this monitor cannot tell Type0 from
   Type0A, so a Type0A SI message would additionally need `si_indicator == 0` gating.
2. **PRB origin auto-rule.** Caught from the gNB dump *before* the first run: TS 38.214 5.1.2.2.2
   counts a common-SS 1_0 grant's PRBs from the CORESET's lowest RB, which in the same frame as
   `bwp_start` is `bwp_start + cset_start`, not `cset_start`. This cell's `bwp=[1..49)` would have
   put every SIB1 allocation one RB low.
3. **TBS_LBRM — wrong in BOTH of its terms, and the first fix only got half of it.** The gNB prints
   its own value (`tb_size_lbrm=159749` bytes = **1,277,992 bits**), so this is directly checkable.
   - *Bandwidth*: `nr_compute_tbslbrm()` was fed `BWPSize`, which on a CORESET#0 grant is 48 RB, not
     the 273-RB carrier -> `lbrm=229576`. TS 38.212 5.4.2.1 wants the maximum PRBs across the
     carrier's DL BWPs. Fixed via `nr_pdsch_passive_grant_t::bw_tbslbrm` (0 = previous behaviour).
   - *Modulation order*: re-measuring after that fix gave **950,984, still wrong** — a clean Qm 6-vs-8
     ratio. TS 38.214 5.1.3.2 makes TBS_LBRM's Qm a **CELL** property ("configured with mcs-Table
     qam256 on ANY BWP" -> 8, else 6), NOT the table this grant indexes. Format 1_0 always uses
     table 1/qam64 for its own MCS, so passing the grant's table forced Qm=6. Fixed via
     `mcs_table_lbrm` (<0 = previous behaviour), fed the deployment's configured table.
   - *And the config had to move with it*: the CORESET#0 conf initially set
     `pdcch_blind_monitor_pdsch` mcs_table to 0, reasoning "1_0 forces table 0 anyway" — which is
     true of the grant's MCS and false of TBS_LBRM. It must carry the DEPLOYMENT value (1 here).
   - **Verified on air: `lbrm=1277992`, an exact match to the gNB**, CRC still 100 % (36/36).
   - Why CRC was already 100 % while this was wrong: at an 808-bit TB, N_ref cannot bind, so N_cb = N
     either way. It would have bitten on the first large TB. **A green CRC does not validate every
     parameter feeding it** — this one was only caught by comparing against the gNB's own print.

**Config that worked, all values read off the gNB/MIB rather than guessed:**
```
pdcch_blind_monitor_coreset = "8:1:6:2:2:2:1"    // 48 RB = 8 groups, 1 symbol, bundle 6,
                                                 // interleaver 2, shift = PCI = 2, dmrs id = PCI, type 1
pdcch_blind_monitor_bwp     = "1:48:0"           // CORESET#0 at CRB 1
pdcch_blind_monitor_ss      = "1:0:1:0:0:0:0:0"  // every slot; real SIB1 is AL4/CCE0
pdcch_blind_monitor_dci10   = "2:1:0:-1:0:0:1:1" // 1_0 only, common SS, auto n_rb/offset, mux 1, sib1
```
`ISAC_OTA_CFG=1` printed `CORESET0 num_rbs=48 cset_start_rb=1 num_symbols=1 mux_pattern=1` from the
**MIB alone**, independently reproducing the gNB's `bwp=[1..49) symb=[0..1)` — so this config is now
derivable, not hand-fitted.

### 10.2 Follow-up tests (2026-08-21, same session) — invariant PASSED, one more bug found

**The `mod_idx == G/(Qm*Nl)` data-aided invariant fires CLEAN on format 1_0: 0 misalignments over
748 submissions** (`-DENABLE_ISAC_SENSING=ON`, `sensing.enable = 1`, `sources = "pdsch_data"`,
`pdcch_blind_monitor_pdsch = "2:..."`, CORESET#0 SIB1 grants). This is the arm that had never been
exercised: rank-1 DCI 1_1 grants take `numDmrsCdmGrpsNoData = 1` (DM-RS symbols still carry data),
while an ordinary 1_0 grant takes 2 (whole-symbol). 748/748 PDSCH CRC in the same run.

Arithmetic it confirms: 10 PRB x (12 symbols - 3 DM-RS) x 12 = 1080 REs = G/Qm = 2160/2. Exact.

**BUG FOUND: `[sensing] sources` never gated the `csirs_monitor` submission.** `csi_rx.c`'s own-CSI-RS
caller (:1146) checks `nr_isac_enabled() && nr_isac_source_enabled(CSI_RS)`; the csirs_monitor
caller `nr_ue_csi_rs_sensing_capture()` (:1360) had **no gate at all**, and
`nr_isac_submit_csirs_ls()` had none either -- while the call site in `phy_procedures_nr_ue.c`
asserts the opposite ("the SUBMISSION to sensing stays gated on ISAC inside the capture").
- **Measured**: a run configured `sources = "pdsch_data"` accumulated **126 CSI-RS rows per CPI**
  against 148 pdsch_data rows in the entire run, so the unwanted source dominated the slow-time grid
  and the configured one was a rounding error in it.
- Fixed by gating once inside `nr_isac_submit_csirs_ls()` -- the single point both callers pass
  through. CSI-RS *reception* stays ungated (it is a receive function with its own enable); only the
  sensing *submission* is gated.
- **Verified on air: `occ[csi=0 dmrs=0 data=64 blind=0]`** -- CPIs now built purely from the
  configured source. CPI count drops (468 -> 22 over a comparable window) because closure is now
  driven by the ~5/s data rows instead of ~50/s CSI-RS, which is the correct behaviour, not a loss.
- **Consequence for older sensing results: any capture that set `sources` to something not including
  `csi_rs` while `csirs_monitor` was configured had CSI-RS rows in its grid anyway.** Every
  `tests/passive_rx` conf carries a `csirs_monitor` line, so re-read past source-ablation results
  with that in mind.

**A methodology note worth keeping.** The first read of this run said `occ[... data=0]` and looked
like the submissions were not landing. That was reading 2 CPI lines out of 468; summed over all of
them the total was **149 against 148 submissions**, i.e. every submission did land. A per-CPI count
of a ~1/s source in a 0.65 s CPI is mostly zeros by construction -- sum before concluding.

### 10.3 RA-/TC-RNTI: **CAPTURED** — the energy gate was the blocker (2026-08-21)

**Result: Msg2 and Msg4 both decoded, exactly matching the gNB.**

| what | gNB sent | we captured |
|---|---|---|
| RA-RNTI `0x10b` (Msg2/RAR) | 11 | **11** |
| `0x461e` format 1_0 (1 tc-rnti Msg4 + 8 c-rnti fallback) | 9 | **9** |
| `0x461f` format 1_0 | — | 5 |

RA field content against the gNB's own log, field for field: `mcs=0/tbl0`, `rv=0`, `tda=1` ->
`sym=1+7` (= index 1 of the SIB1-derived COMMON list, confirming RA takes that list, not the
dedicated one), `prb=0+5` vs `vrbs=[0..5)`, `cdm=2`, `ports=0x1`, `nscid=0`, `dmrsmask=0x84` vs
`dl_dmrs_symb_pos=00000010000100`, `vrb=0`, and `refPoint` correctly 0 (NOT the SI-RNTI exception).
Slot 7, matching `DL_TTI.request slot=953.7/966.7/977.7`.

**ROOT CAUSE: the adaptive energy gate at `energy_adapt_factor = 3.0`.** RA/TC PDCCH at CORESET#0
peaks at **2.31x** the noise floor from this receiver (measured, §10.3's table below), so a 3x gate
excludes it BY CONSTRUCTION. SIB1 in slot 1 reaches 41-57x and sails through, which is why the
symptom looked like "only SIB1 is receivable".

**A RETRACTION, and the lesson is R7 verbatim.** The previous version of this section concluded
"no PDCCH energy from that slot ever reaches this receiver ... it is RECEPTION, not the decoder",
citing `ISAC_PDCCH_FULLCRC` never recovering `0x10b`. **That was an artefact of the gate**: the
energy gate runs BEFORE `blind_polar_decode()`, so gated candidates never polar-decode and never
emit a FULLCRC line. The instrument was downstream of the filter it was being used to exonerate.
With gates off, FULLCRC went 38 -> 2080 lines and `0x10b` appeared immediately. R7 says "measure
with gates OFF when characterising, with gates ON when scoring" -- this is exactly that failure.

The energy table itself still stands and is still useful (slot 7 max 2.31x vs slot 1 max 56.76x);
only the CONCLUSION drawn from it was wrong. 2.31x is weak, not absent.

**Config that captured it** (`nrue.passive_rx.nogate.conf`): `noise_gates = "0:1:500:0:0"` (energy,
persistence and SNR all off) plus `ISAC_PDCCH_NO_MISMATCH_GATE=1`, and the AL ladder narrowed to
**AL4 only** (`ss = "1:0:1:0:-1:-1:0:-1"`) -- the gNB puts BOTH SIB1 and RA at `cce=0 al=4`, so this
keeps the positive control while cutting 15 candidates/occasion to 2, which is what makes a
gate-off run affordable on the RT thread (656,000 decodes over 328,000 occasions, no instability).

**Cost of running that wide open**: 1353 accepts of which ~1306 are real (SI 1281 + RA 11 + 14
genuine 1_0 to 0x461e/0x461f) -> ~96.5 % precision even with the mismatch gate DISABLED. The ~46
false accepts are one-offs spread over 46 distinct RNTIs; the real ones repeat (9x, 5x, 11x), which
is precisely what the mismatch and persistence gates key on. PDSCH CRC 1307/1316 = **99.3 %**.

**Recommended standing config for CORESET#0**: keep the mismatch and persistence gates ON (they
reject the one-off noise correctly) but drop `energy_adapt_factor` from 3.0 to **1.5** -- above the
measured 1.40 median floor, below the 2.31x real signal. NOT yet re-measured with that exact value;
the 3.0 default is what must not be used for CORESET#0.

**Note on persistence**: `rnti_persist_k` must be 1 (off) to catch Msg2 -- an RA-RNTI is sent once
per attach, so K=2 drops it by construction.

### 10.4 Row rate and catch rate (2026-08-21) — **read this whole section before quoting a number**

Three of my own figures were retracted inside this section. The sequence matters more than any one
number, so it is preserved.

**RETRACTED #1: "gate off takes catch 3.2 % -> 12 %".** That counted RAW ACCEPTS with the mismatch
gate disabled, and those are overwhelmingly noise. Measured properly on `run_diag`: 11,178 accepts
carried **9,948 DISTINCT RNTIs**, and only **472 carried the live C-RNTI**. `crc_ok` (434) tracks the
real count, because a false accept cannot pass a PDSCH CRC. **True catch is 2-3 % with the gate ON or
OFF** (`run_uss` 722/24,773 = 2.9 %; `run_ussng` 4,045/129,941 = 3.1 %). The energy gate buys nothing
on the dedicated CORESET. The 4.9 -> 23 rows/s gain came from iperf offering 738 grants/s instead of
170/s, not from the gate.

**RETRACTED #2: "no energy above the floor, so AL2 is unreceivable".** Measured on a run that
happened to decode ZERO, which is self-consistent but describes a dead run, not the system.

**RETRACTED #3 (nearly asserted, caught in time): "efloor predicts yield".** After three runs it
looked clean (0.54 -> 0 rows/s, 0.55 -> 0.67, 0.75 -> 31.9). Run 5 broke it: efloor 0.58 -> 46.4
rows/s. It is not a predictor.

**What the 6-run repeatability set actually shows** (identical config, 90 s each, back to back,
ENERGY probe alternating ON/OFF):

| run | probe | crc_ok | rows/s | efloor |
|---|---|---|---|---|
| 1 | off | 0 | 0.00 | 0.54 |
| 2 | on | 57 | 0.67 | 0.55 |
| 3 | off | 2,673 | 31.92 | 0.75 |
| 4 | on | 3,452 | 40.02 | 0.77 |
| 5 | off | 3,970 | 46.36 | 0.58 |
| 6 | on | 5,192 | **61.08** | 0.79 |

- **Strictly monotonic across six sequential runs.** Not variance -- a trend.
- **The ENERGY probe's own logging does NOT degrade reception**: it alternates ON/OFF independently
  of the trend (31.9 off, 40.0 on, 46.4 off, 61.1 on). That confound is dissolved; the earlier
  probe-on run that decoded 0 was position 1 in the trend, not a probe artefact.
- **Best measured row rate to date: 61.1 rows/s data-aided (comb-1) -> vel_max +/-1.34 m/s**, and
  157 accepts/s usable for DM-RS CFR -> **+/-3.4 m/s**. Both far above the 4.9 rows/s that §10.4
  originally reported as the gated figure.

**UNRESOLVED, and it is a flaw in the experiment I designed**: the monotonic trend could be the
receiver stabilising across runs OR the offered iperf load ramping, and the script did NOT bracket
the gNB log per run, so the two cannot be separated. Offered load is known to vary a lot -- measured
170/s, 543/s and 738/s at different points the same day. **Fix before re-running: bracket
`stat -c%s` on the gNB log around EACH run and report rows-per-offered-grant, not rows/s.** Until
that is done, no row-rate number here should be quoted as a property of the receiver.

### 10.1 What is NOT done

1. ~~No live run.~~ **Done — §10.0, §10.2, §10.3.** SI-RNTI, RA-RNTI, TC-RNTI, C-RNTI fallback,
   the data-aided invariant and the both-formats negative control are ALL validated on air against
   the gNB's own log. **P-RNTI is the only class still untested** (this cell paged nothing). Measured over a 6 s window, this cell's DL PDCCH is 100 % `type=c-rnti` plus SI-RNTI —
   no RACH, no paging — so those three cannot appear without forcing a UE re-attach, which
   interrupts whatever traffic the active UE is carrying. TC-RNTI/Msg4 is the one that matters,
   since §7.2 (RRCSetup parsing) depends on it. Procedure: scan CORESET#0 with
   `dci10 = "2:1:0:-1:0:0:1:0"` (sib1 = 0, so the SIB1-derived TDRA list applies) and restart the
   active UE mid-capture. Two `TC=1` accepts HAVE been seen across runs; both were false decodes
   caught by the existing mismatch gate, not real Msg4.
2. **The `mod_idx == G/(Qm·Nl)` invariant cannot fire on this branch.** It lives in
   `nr_isac_pdsch_data_aided_submit()`, reached only when `want_data` is true, which needs
   `nr_isac_enabled()` — and `passive-rx-only` links `nr_isac_stub.c` (returns 0). To exercise it:
   `cmake . -DENABLE_ISAC_SENSING=ON`, `sensing.enable = 1`, `sources` including `pdsch_data`, and
   `pdcch_blind_monitor_pdsch = "2:..."`. Worth doing precisely because 1_0 uses the other CDM-group
   branch (see above).
3. **One search space at a time.** SI/RA/TC-RNTI live in CORESET#0's common search spaces, so
   reaching RRCSetup means re-pointing the whole monitor there (`coreset_type = 1`,
   `dci10_scan = 2`, `dci10_ss_type = 1`) — it cannot run alongside the dedicated USS scan in one
   process. Making `nr_pdcch_blind_monitor_process()` loop over two configured spaces is a
   contained refactor (~14 `cfg->coreset_*`/`cfg->ss_*` references to parameterise) and is the
   natural next step; §7.2's RRCSetup parsing depends on it.
4. **C-RNTI vs TC-RNTI is not decidable from the air** and is not claimed to be: same range, same
   field list (TS 38.212 gives TC's DAI as 2 reserved bits in the same position). The label follows
   the search space. Nothing downstream depends on the distinction except the TDRA-list choice,
   which is correct under that labelling.

## 8. Retracted this session — do not cite

- ~~"MCS 9 fails 100 %, deterministic decoder bug."~~ 33 % with gates off; same MCS+TBS+PRB gives
  both outcomes. Not a deterministic parameter bug.
- ~~"The rank-4 failure is caused by the four-RX MRC skip / energy gate / time-tracking flags."~~
  All three tested and eliminated.
- ~~"The passive receiver has a range limit; accepts collapsed as the UE moved away."~~ The gNB→X410
  path never changed — neither endpoint moved. The downlink was simply idle.
- ~~"iperf is uplink."~~ Asserted from a 27 s window never verified to be inside the test.
- ~~"The RXDISCONT storm is sensing-pipeline CPU load."~~ It was a false positive introduced by the
  x410 merge dropping `rx_samples_consumed` accounting for the end-of-frame read
  (`delta=4448 = 4096+352`). Fixed; 11,582 → 0.
- CLAUDE.md §4's *"busiest dedicated slots 14/15 show flat energy"* — that grant count came from
  **all** slot-decision events (dominated by UL slots 8/9/18/19). Real DL grants are near-flat
  across slots 0-7/10-17. There was never a correlation to explain.
- CLAUDE.md §10's *"cell sends 99.997 % at AL1"* — srsRAN's FAPI `dci_aggregation_level` is
  **log2(L)**, verified 3071/3071 against the scheduler's own `al=`. Real grants are AL2 and AL4,
  never AL1.

---

## 9. Files

- `nrue.passive_rx.conf` — the passive receiver config (see §4).
- `openair1/PHY/NR_UE_ISAC/nr_isac_stub.c` — no-op sensing shim; `ENABLE_ISAC_SENSING` selects it.
- `passive_5g_receiver.pptx` / `.pdf` — 15-slide deck describing the system, limits and roadmap.
- Branches: `passive-rx-only` (this work), `merge-x410-passive` (same + sensing available),
  `nrsniffer-pdcch-pdsch-migration` and `total-passive-ue` (untouched originals),
  `x410-100MHz` (the active-UE baseline — do not lose it).
