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

## 2. Status — what works, measured (2026-08-21 snapshot; CURRENT baseline is §15.2)

**The numbers in this section are the 2026-08-21 state and are superseded for PDSCH CRC and catch
rate — see §15.2. They are kept because the DCI-level validation against the gNB's own log (below)
still stands and has not been re-run.**

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

### 7.5 Re-integrate sensing and read the real vel_max — **DONE 2026-08-24/25. See §15.1.**
`-DENABLE_ISAC_SENSING=ON`, `pdsch_decode=2`, `sources="pdsch_data"`. Real CPIs now measured:
`vel_max` up to 23.7 m/s once the signed-slot-delta bug was fixed. Sub-slot sampling still UNUSED.

### 7.6 Resolve MCS 9 — **MOOT.** Decoder health at mcs 9/10 is now measured at ~86 %
(§15.2); the "MCS 9 fails 100 %" observation was an energy-gate sampling artefact, already retracted
in §8. The live question is no longer MCS 9 but MCS ADAPTIVITY across the whole range -- see §42.

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

### 10.5 "2-3 % catch rate on the dedicated CORESET" — SOLVED, see §11
**Superseded and removed.** The ranked causes recorded here (frequency-dependent channel estimate /
residual sub-bin STO, LLR autoscaling, AL2-vs-AL4, the energy gate) were ALL closed by §11: the
"2-3 % catch rate" was an artefact of the X410 silently stalling mid-run, and healthy catch on that
CORESET is ~70-85 %. Nothing in the original ranking survived; §11 is the record.

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

---

## 11. §10.5 SOLVED (2026-08-23) — it was never the CORESET. Read this before touching §10.5.

**The dedicated CORESET's "2-3 % catch rate" is an artefact of the X410 silently stalling
mid-run.** Healthy catch on that CORESET is **~70 %**, sustained (one 200 s run, 100 % healthy,
flat end to end). §10.5's ranked causes are closed: **cause #1 (frequency-dependent channel
estimate / residual sub-bin STO) is REFUTED**, cause #2 (LLR autoscale) was never needed, cause #3
(AL2 vs AL4) is irrelevant, and cause #4 (energy gate) was already correctly excluded.

### 11.1 The fault
The host loses its UHD/MPM claim on the X410. UHD logs ONE warning -- `mpmd device reclaiming loop
failed!` or `rpc::timeout: Timeout of 2000ms` -- its task loop exits, and it neither stops the RX
stream nor tells the application. The X410 stops delivering real IQ while DPDK keeps returning
**full-length reads with perfectly continuous timestamps** carrying a constant near-zero level
(mean |I|+|Q| pinned **4.88-4.93** vs **70-81** healthy).

Nothing could see it: `RFTSDISC` (uncapped) and `RXDISCONT` both key on timestamp continuity, and
`--passive-rx` has no RRC/T310 to declare radio-link failure. Downstream everything keeps
"working" -- the blind PDCCH monitor still emits noise decodes (**5129 distinct RNTIs over 5317
accepts**) -- while real DCI accepts for the live C-RNTI go to **EXACTLY zero** and never return.

**Trigger, measured:** the X410 mgmt link (1 GbE, `128.178.122.3`) RTT spikes from 0.17 ms to
**2151-2566 ms**, past UHD's 2000 ms RPC timeout.

### 11.2 The evidence
Catch rate is predicted by the healthy fraction alone, at ~0.70 x healthy:

| run | healthy windows | permanent? | predicted | measured CATCH |
|---|---|---|---|---|
| run5 | 6/84 (7 %) | yes | 4.9 % | **4.60 %** |
| run6 | 29/84 (35 %) | yes | 24.5 % | **22.85 %** |
| run1 | 56/85 (66 %) | yes | 46 % | **49.14 %** |
| run2 / run3 / 200 s | 100 % | -- | ~70 % | **69.6 / 75.0 / ~70 %** |

All 4 healthy runs carry **0** reclaim/RPC errors; all 5 stalled runs carry them. In one run the
UHD error is at log line 25445, the first collapsed census window at 25561, last real DCI at 25268
-- correct causal order.

**Per-CCE catch is dead flat** in every run (5.1-5.5 %, 63.9-66.2 %, 29.1-30.6 % ... across CCE
2->40 = RB 12->251). By `score_cce.py`'s own criterion that refutes the timing-ramp hypothesis.
Note `score_cce.py` has two flaws for this data: it never filters DCIGT to the live C-RNTI, and it
aggregates all runs -- use `captures/score_cce_perrun.py`.

### 11.3 What landed
- **`RFCENSUS`** (`nr-ue.c`): uncapped once-per-second census -- RF power off the raw time-domain
  slot buffer, SSB slots, `shiftForNextFrame`, `max_pos_acc`. Power is measured before any
  alignment or demodulation, so it separates "signal went away" from "looking in the wrong place",
  which no other instrument here could do.
- **`RFSTALL` watchdog**: fires when window power drops below 0.25x a reference that is updated
  ONLY from healthy windows, so it can never follow the signal down into the fault.
- **Real recovery**: `trx_usrp_stop()` was a **no-op** -- that is why OAI could never restart a
  stalled stream. It now stops+drains; `nrue_ru_restart_rx()` pairs it with a start and clears the
  stale `firstTS` origin; X410 GPIO routing is now first-start-only (a second `set_gpio_src()`
  throws and aborted the process); `trx_usrp_start` is exception-safe.
- **On unrecoverable stall: `exit(3)`** with a clear reason rather than silent corruption.

**Validated live:** one run took **3 stalls, recovered from all 3** with the real-accept timeline
flat throughout (3059 3056 3061 3031 3052 3062 3053 3100 3067 2740 per decile), then exited
cleanly on a 4th where the claim was permanently gone.

### 11.4 Consequences
- **Every passive-RX measurement taken before this may be silently contaminated.** Re-measure
  before quoting any of them, including the 90 %-vs-2-3 % CORESET#0/dedicated comparison in §10.5,
  whose dedicated number is now known to be a stall artefact.
- Treat `exit(3)` as VOID-the-run in any harness.
- Next lever if stalls persist: get the X410 mgmt port off the shared LAN, or raise UHD's RPC/claim
  timeout. In-process recovery is impossible once the claim is permanently lost -- every device RPC
  throws.

### 11.5 A trap worth adding to §1 (call it R9)
**Nearly every PHY diagnostic in this tree is CENSORED, and its absence proves nothing.**
`PBCHFAIL` stops after 6 prints, `TRACKLOCK` after 25, and `nr_ue_timing_mutation_log` (TIMEMUT)
caps at 200 **and** suppresses any event where the value did not change. "TIMEMUT went to zero"
means "shiftForNextFrame stopped moving", NOT "PBCH died" -- reading it as the latter produced a
wrong root cause and a watchdog keyed on a signal that could never fire. Check the print limit in
source before inferring anything from a diagnostic going quiet. Same class as R7.

### 11.6 Update (2026-08-23, later): committed as `da5412d4cf`, plus two corrections

**Measured healthy catch: 76.9 %** (11603 crc_ok / 15082 offered; PDCCH catch 84-85 %).
Confirms §11.2 -- the 2-3 % figure was the stall averaged over a capture.

**A SECOND failure mode exists and is NOT the RF stall: a TIMING RUNAWAY.** RF power stays
perfectly healthy (75-87) while `max_pos_acc` diverges monotonically (439 -> 1394, with
`shiftForNextFrame` -4 -> -14) and the FFT window walks off the signal. Real accepts die exactly
as in the stall case. **A power-based detector cannot see it.** The shipped watchdog is therefore
keyed on PBCH DECODE COUNT, which is common to both modes (a healthy receiver decodes every SSB,
so `pbch_ok=0` is unambiguous); power is kept as a second, corroborating signal.

**CORRECTION to §11.3's "recovery" claim.** In-process recovery is now **OFF by default** and the
default action on a stall is a clean `exit(3)`. Re-init does restore the RF stream and PDCCH, but
the earlier statement that it also restores full operation is **withdrawn as unconfirmed** -- see
the PMI finding below, which produces an identical signature and was not excluded. Opt in with
`ISAC_RF_STALL_MAX_REINIT`, and re-test with the PMI held constant before trusting it.

**NEW, and it confounds this metric: PDSCH decodability is hostage to the SERVED UE's PMI.**
PDSCH is precoded toward that UE; **PDCCH carries no `pm_index` at all**. After a UE re-attach
moved `pm_index` 3 -> 15/19, one run measured **84 % PDCCH catch and 0 % PDSCH CRC** -- the new
beam simply nulls this receiver. Nothing was wrong with the receiver. Consequences:
- `crc_ok`-based catch is NOT a pure receiver metric. Report `real_accepts/offered` (PDCCH catch)
  alongside it, and check the gNB's `pm_index` before attributing a PDSCH change to code.
- This is §5's limitation 1 resurfacing: `max_rank: 1` removes the RANK problem, not the BEAM.

**Operational facts worth keeping.**
- **Root SSH to the X410 works** (`ssh root@128.178.122.3`, needs `StrictHostKeyChecking=no`), and
  `systemctl restart usrp-hwd` is the ONLY reliable unwedge. `mpm_reboot=1` and clearing
  `/var/run/dpdk` + `/dev/hugepages` did NOT fix a wedged device; that did.
- **Any SIGKILL of the UE wedges the X410** (`rx xport timed out getting a response from
  mgmt_portal`, `RFNOC::GRAPH ... timed out during flush`). Stop with SIGTERM.
- **MPM holds a stale claim for ~20 s** after a process exits (refused at t=10 s, succeeds by
  t=20 s). `_run_catchrate.sh` now waits for the device to be claimable between runs; the old
  `sleep 3` was killing alternate runs at `dur=1s occ=0`.
- **`pgrep -x nr-uesoftmodem` matches ZOMBIES.** A shutdown hang left one unreaped and the runner's
  wait loop spun for ~28 min, silently inflating that run's gNB-log bracket to `offered=1266/s`.
  Distrust any run whose `offered` rate is far from the cell's real ~190/s.
- R5 is real: during a capture sens6's sshd is starved and unreachable for minutes.

---

## 12. Rank > 1 PDSCH: NOT a decoder bug, it is SINR margin (2026-08-23)

**Bottom line: the passive receiver's rank-2 PDSCH failure is a link-margin shortfall, not a defect.
Rank 1 sits ~1 dB above the 16QAM R=0.64 waterfall; rank 2 costs exactly the 3 dB power split and
falls off the cliff.** `max_rank: 1` is therefore the right standing setting for a capture rig --
and it is a **PDSCH-only** knob (`cell_cfg.pdsch:` in `gnb.yaml`), so it does not touch UL.

### 12.1 The measurement chain

Same rig, same link, same cell, mcs 10 / 16QAM / R=0.64, `--ue-nb-ant-rx 4`:

| | median EVM | PDSCH CRC |
|---|---|---|
| **rank 1** (passive) | **29.7 %** (n=3773) | **3463/3817 = 90.7 %** |
| **rank 2** (passive) | **42.6 %** (n=3060) | ~0 % (1/23101) |
| **rank 1, ACTIVE UE** | -- | **0 errors** (DL harq 23/0 … 29/0) |

EVM 29.7 % -> 42.6 % is a ratio of 1.434 = **3.1 dB in error power -- exactly the two-layer power
split**. LDPC's waterfall is 1-2 dB wide, so 90.7 % -> ~0 % is the expected cliff, not a bug.

### 12.2 Eliminated by measurement -- do NOT re-investigate these

Each was tested, not argued:
- **Receive gain**: +15 dB (40 -> 55) changed nothing (1/23101 vs 1/25390). Gain is not SNR.
- **Layer separability**: CHESTDIAG `orth` 0.59-0.95 at nl=2 -- only ~1.2 dB of ZF enhancement,
  nowhere near the gap. (At nl=4: `orth` 0.035-0.25, `resid=[1.00,0.73,0.61,0.55]`, ~2.5 effective
  layers -- rank 4 is genuinely unreachable with this array.)
- **DM-RS ports / CDM groups / nscid / TDA / DM-RS mask / MCS / mcs_table**: all decode EXACTLY as
  the gNB logs them (`cdm=1 ports=0x3 nscid=0` vs `num_dmrs_cdm_grps_no_data=1
  dmrs_ports=000000000011 nscid=0`).
- **TBS / G arithmetic**: self-consistent (143 PRB x 138 RE x Qm4 x Nl2 = G 157872).
- **ML detector**: on/off identical (~0 either way).
- **Grant size / base graph**: fails at every size; even the 2-PRB `bg=2` shape is 1/8093.
- **Layer<->DM-RS-port order**: reversed via a probe -- no change.
- **`nrOfLayers`**: UL-only, never read in the DL receive path.
- **`chest_time`**, **RX antenna port selection** (all four channels on RX1).
- **CDM OCC de-spreading**: `NFAPI_NR_DMRS_TYPE1_linear_interp` DOES average the OCC pair
  (`c16addShift(ch_l, ch_r, 1)`), so port 0 does not leak into port 1. This was the best structural
  hypothesis and it is wrong.
- **`nr_rx_pdsch` itself**: `dlsim -x2 -y4 -z4 -R273 -e10` decodes rank 2 with **0 errors**. The
  shared demodulator is correct; only the link is short.

### 12.3 Receive-branch imbalance -- real, and the place the missing dB actually are

Per-(layer,antenna) channel power, median relative to the strongest branch:

```
ant0  0 dB   ant1 -26.1 dB   ant2 -18.2 dB   ant3 -7.4 dB      (varies: also seen -6.2/-17.2/-7.9/0)
```

Two branches contribute essentially nothing. **This is why rank 1 works at all**: `nr_rx_pdsch`'s
four-RX rank-one compatibility mode decodes **branch 0 alone**
(`mrc_nb_rx = (nl == 1 && nbRx == 4) ? 1 : nbRx`, `mrc_rx_index = 0`), and branch 0 is a live one.

Quantified: four *balanced* branches would give up to **+6 dB** of MRC gain -- comfortably more than
the 3 dB rank 2 costs. With the imbalance as measured, combining all four yields only **+0.8 dB**.
So the ranked fix is (1) fix the branch imbalance, (2) then remove the branch-0-only hack, (3)
failing both, lower the gNB MCS. Cabling was checked by the user and is good, so this is the
antennas/placement themselves.

**Trap**: `orth`/`resid` CANNOT see this -- CHESTDIAG normalises each branch by its own RMS before
forming the Gram matrix, deliberately, so per-chain gain cannot bias the geometry. It reported
0.73-0.95 while two branches were noise. Never judge branches by `orth`.

### 12.4 PDSCH is precoded toward the SERVED UE; PDCCH is not (corrects an earlier claim)

Verified in the gNB source, not inferred:
- `lib/fapi_adaptor/mac/p7/pdu_translators/pdcch.cpp`: `mac_pdcch_precoding_info` is an **empty
  struct**; PDCCH gets `pdcch_codebook_offsets[0] + get_pdcch_precoding_matrix_index()` -- a FIXED,
  non-UE-specific precoder.
- `.../pdsch.cpp`: PDSCH uses the served UE's **PMI report** when present, omni only as fallback.
- Both are applied for real (`precoding_configuration::make_wideband`, then the grid mapper).

**An earlier statement in this document's §11 that "PDCCH carries no pm_index at all" is WRONG** --
it does; the log formatter just does not print it. The substantive consequence is unchanged and
was measured: after a UE re-attach moved `pm_index` 3 -> 15/19, one run recorded **84 % PDCCH catch
with 0 % PDSCH CRC**. `crc_ok`-based catch is therefore NOT a pure receiver metric -- report
`real_accepts/offered` alongside it, and check the gNB's `pm_index` before blaming code.

### 12.5 The X410 as an ACTIVE UE -- works, and does not have this problem

Full attach (RAR -> Msg3 -> RRCSetup -> RRCSetupComplete -> **Registration Accept**) and **0 DL
errors** on the same four antennas with the same imbalance. Two structural advantages a passive
receiver can never have:
1. the gNB steers the beam using **that UE's own PMI**, and
2. **closed-loop link adaptation** (`olla_target_bler: 0.1`) lowers MCS until it decodes.

A passive receiver has neither: it must decode whatever MCS was chosen for someone else's channel.
That is the whole reason 3 dB is fatal for us and invisible to them.

**The verified active-UE invocation is NOT guessable -- recovered from sens6 shell history:**
```bash
cd /home/sens/NICOLA/openairinterface5g-x410-100MHz/cmake_targets/ran_build/build
sudo ./nr-uesoftmodem --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3,use_dpdk=1234 \
  -O /home/sens/NICOLA/ue_100_4x4.nicola.conf -C 3414990000 --ue-fo-compensation --ssb 165 \
  --ue-rxgain 40 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --thread-pool 0,1,4,5,6,7 \
  --time-sync-I 0.01 --ntn-initial-time-drift -4.25 --usrp-tx-thread-config 1
```
Traps paid for here: it must run from the **x410-100MHz tree** (the passive-rx-only tree lacks the
X410 active fixes); **`--usrp-tx-thread-config 1` is mandatory** -- without it PRACH never leaves
the box and the gNB logs ZERO preamble detections, which looks exactly like the old
"PRACH-detection blocker"; and do NOT add `-A 90` or `-r/--band/--numerology` (the conf carries
them). Note `ue_100_4x4.conf`'s comment calls `uecap_ports2.xml` "rank-one" -- the XML actually
declares up to four/two layers, so capability is not the limiter.

**Untested: the active UE at rank 2.** At `max_rank: 2` it loops (8x RRCSetup, no Registration
Accept, no PDU session, no `oaitun`), so it never carried data. n=2 against one clean rank-1 run and
confounded by a gNB restart -- do NOT read it as caused by max_rank. The gNB logged `ri=2` 500x
while assigning `num_layers=1` throughout, consistent with there being no bulk DL traffic to justify
a second layer. To settle it: fix the registration loop, then drive iperf DL to the UE's `oaitun`.

### 12.6 UL capture -- pursue it, on the PASSIVE path

`max_rank` is PDSCH-only, so keeping it at 1 costs UL nothing, and UL is single-layer in practice.
The active-UE path does NOT help: during UL slots it is transmitting, and it never sees other UEs'
UL at all.
- **Favourable**: no PMI/beam failure mode (UEs do not beamform at the gNB the way the gNB does at
  them); TDD separates UL in time (7 DL / 2 UL); and **the UL grants are already decoded** -- DCI
  format 0_1 shares the CORESET/CCE space and is currently thrown away by the format-indicator
  check. Keeping them is a small change, not new machinery.
- **The real work**: (a) **timing advance** -- UL is pre-advanced to land aligned at the *gNB*, so
  the offset is per-UE and the DL sync cannot be reused directly; (b) **link budget** -- UE TX is far
  below the gNB's and power control keeps it at the minimum that reaches the gNB, so proximity to
  the UE now matters. Fix the branch imbalance first: DL rank 1 has ~1 dB margin today and UL starts
  below that.

### 12.7 Diagnostics added, and one that was silently broken

- **`CHESTDIAG`'s `rep` string was declared, PRINTED, and never written** -- an uninitialised buffer.
  The per-(layer,antenna) powers were computed into `pw[][]` and thrown away, which is why the
  branch imbalance stayed invisible for so long. Now filled (`pw=L0[...] L1[...]`).
- **`EQDIAG`** (new, `ISAC_PDSCH_TBPARM=1`): post-equalisation EVM per layer against the ideal QAM
  grid. This is what separates "equaliser/channel estimate wrong" from "everything downstream
  wrong", and it is what identified this as margin. It samples the symbol with the MOST valid data
  REs (the last symbol is often DM-RS with none) and tags the RNTI (false accepts otherwise consume
  the sample budget and swamp the live UE).
- **Dead-branch exclusion** (`ISAC_RX_BRANCH_MIN_DB`, default 12 dB, 0 disables): zeroes channel
  estimates of branches far below the strongest, so they stop injecting noise into a combiner that
  uses ONE scalar noise variance for all branches. Always keeps >= Nl branches. Mitigation only --
  it did not recover rank 2, because the deficit is margin, not noise injection.
- **Windowed least-squares MIMO CFR** (`nr_pdsch_data_aided.c`): Nl > 1 grants can now contribute a
  CFR row. H = Y/X is a scalar division and invalid for a superposition; this solves
  `G h_a = b_a` over a window where the channel is flat, emits **layer 0 always** (switching
  projections would inject a phase discontinuity that mimics target motion), and drops
  ill-conditioned windows whole. At Nl == 1 the window is W = 1 and the arithmetic is bit-identical.
- **ML detector exposed** (`ISAC_PDSCH_ML`, default on): `ue->do_ml` was set only by dlsim, so the
  ML path was dead code on a real receiver. Enabling it exposed a latent **NULL dereference**: this
  call site passed `NULL /* rho_dl: single layer */` while `nr_rx_pdsch` dereferences
  `rho_dl[symbol]` whenever ML engages -- it segfaulted on the first rank-2 grant. `rho_dl` is now
  allocated, only when ML can actually engage.

**Status: all of the above is BUILT and UNCOMMITTED** on `passive-rx-only`
(`nr_pdsch_data_aided.c`, `nr_pdsch_passive_decode.c`).

### 12.8 Wrong turns this session, recorded so they are not repeated

- **"It's SNR"** -- asserted before testing, then falsified by my own gain sweep. The right framing
  is SINR *margin*; receive gain does not change it.
- **"PDCCH is not precoded"** -- wrong, see §12.4.
- **"Re-init breaks PDSCH"** (§11) -- withdrawn as unconfirmed: a PMI change produces an identical
  signature and was not excluded.
- **Reading `orth ~ 1` as "layers separated"** -- pure noise is independent too, so a dead layer
  scores resid ~1. It took the per-antenna powers to see the truth.
- **Inventing the active-UE invocation** instead of recovering it -- cost two runs and a false
  "PRACH-detection blocker" conclusion. Recover commands from shell history; do not reconstruct them.

---

## 13. THE PMI EXPLANATION IS DISCARDED (2026-08-23, late) -- read this before citing §11.4 or §12.4

**The "PDSCH fails because the gNB's beam is steered at the served UE and we sit in its null" idea
is REFUTED. Do not cite it as the cause of a PDSCH failure.** §11.4 and §12.4 record the
measurement that motivated it (after a UE re-attach moved `pm_index` 3 -> 15/19, one run measured
84 % PDCCH catch with 0 % PDSCH CRC) and the gNB-source fact behind it (PDSCH is precoded with the
served UE's PMI report; PDCCH uses a fixed non-UE-specific precoder). **Both of those remain true.
The INFERENCE drawn from them does not.**

**The test that killed it.** The commercial UE was physically moved to sit next to the X410's
antennas, so the beam steered at that UE necessarily illuminates us and the gNB's link adaptation
is targeting essentially our own channel. PDSCH still failed completely:

- **72 s capture**, live C-RNTI `0x460b`
- **8172 real DCI accepts**, **all `rv = 0`** (noise gives random rv, so these are genuine grants),
  every field correct: `mcs=10/tbl1 cdm=1 ports=0x1 nscid=0 sym=1+13 dmrsmask=0x884 prb=0+273`
- **`pdsch_decode[try=8722 crc_ok=0 (0.0%)]`**
- `rf_pow` median 58.2, no saturation -- so the UE's proximity was not desensitising the front end
  either (a separate hypothesis, also eliminated by the same run)

**And the sharper form of it: the SAME C-RNTI `0x460b` measured 90.7 % CRC earlier the same day**
(3463/3817, EVM 29.7 %). Same receiver, same cell, same MCS, same rank. So this receiver
demonstrably can decode this UE, and whatever changed is not the beam.

**What this leaves open.** The cause of the bimodal PDSCH behaviour -- 0 % or ~90 %, nothing in
between, across otherwise identical runs -- is now UNKNOWN. Candidates NOT yet tested:
- Something about the UE's new position/proximity other than beam or front-end desense.
- A receiver-side state that differs between runs (the same run-to-run bimodality that produced the
  original "2-3 % catch rate" before the stall watchdog existed).

**The measurement to take next, and why it was not taken:** post-equalisation EVM per layer at the
moment PDSCH fails. That splits "the signal reaching the equaliser is degraded" from "the signal is
fine and something downstream is wrong", and nothing measured so far distinguishes them. Three
attempts died to the X410 wedging (UHD `AssertionError: xport_list->empty()` in
`dpdk_io_service.cpp:437`, then a genuine timing runaway at `max_pos_acc=1144`). NOTE: the EQDIAG
probe that produced those EVM numbers was REMOVED by the revert below -- it would have to be
reinstated.

**Code state.** The rank > 1 work was reverted on request (`812f6b4af6` reverts `4bde92cfda`);
`max_rank` stays at 1, which is a PDSCH-only knob and costs UL nothing. The stall watchdog
(`da5412d4cf`) and its false-positive fix (`51d7fc8bd0`) are RETAINED -- they are what took catch
from 2-3 % to ~85 % PDCCH and are unrelated to rank.

**Two facts from the reverted work that still apply to the tree as it now stands:**
- The receive-branch imbalance is REAL and UNFIXED: 0 / -26.1 / -18.2 / -7.4 dB. Rank 1 only works
  because `nr_rx_pdsch` decodes branch 0 alone at `nl == 1 && nbRx == 4`, and branch 0 is live.
  Four balanced branches are worth +6 dB of MRC; the current imbalance yields +0.8 dB.
- `nr_pdsch_passive_decode.c` passes `NULL` for `rho_dl`. That is safe ONLY while `ue->do_ml` is
  false. Anything that sets it segfaults on the first rank-2 grant.

---

## 14. ROW RATE vs DWELL: the receiver cannot hold time sync at high DL load (2026-08-23, late)

**The blocking item for high-rate sensing is now identified and it is NOT the X410: at ~1550
grants/s the receiver loses PBCH lock within ~2 s, because ~1550 blind PDSCH decodes/s run on the
PHY RECEIVE THREAD and starve the timing loop.** `PASSIVE_RX_ONLY_HANDOVER.md` §7.4 already listed
"move the PDSCH decode off the PHY receive thread" as future work; it is no longer future work, it
is what caps dwell time.

### 14.1 The load / dwell measurement

Identical binary, conf, gNB and receiver; only the offered DL grant rate differs (iperf DL rate
raised from ~15 Mbps to 100 Mbps):

| offered | dwell before losing lock | PDSCH rows/s | data rows per CPI |
|---|---|---|---|
| **149 grants/s** | **90 s, 0 stalls** | **136** | 144 |
| **1552 grants/s** | **~2 s** (both of 2 runs) | -- (too short) | **215** |

Offered rate is the gNB's own `DL PDCCH ... type=c-rnti` count over a bracketed window (1552/s
measured directly, close to the ~1400 DL-slot ceiling for this 7DL+1special/10 TDD pattern at mu=1).

### 14.2 It is the TIMING LOOP, not the RF path -- the numbers separate them cleanly

```
run1: pbch_ok=0 pbch_fail=50   rf_pow=513   ref=520   max_pos_acc=967
run2: pbch_ok=0 pbch_fail=50   rf_pow=543   ref=543   max_pos_acc=1373      (healthy ~500)
```

`rf_pow` ~520 is the HIGHEST of the whole session (57-70 at low load), so the RF path is delivering
more signal than ever. What collapses is PBCH, with `max_pos_acc` diverging to 967/1373 against a
healthy ~500 -- the FFT window walks off. Both runs report "PBCH lock lost (timing runaway)", NOT
"RF stream stalled". Contrast the genuine RF stalls elsewhere in this document, which look like
`rf_pow` 62 -> 4.91 with `pbch_ok=0`: there the signal disappears; here it is abundant.

**This reinstates the load correlation.** §12/§13 withdrew an earlier "stall rate tracks DL load"
claim -- correctly, because that version was measured with the RFSTALL power test that
false-positived (fixed in `51d7fc8bd0`). Re-measured with the corrected trigger, the correlation is
real and large: 149 grants/s -> 90 s, 1552 grants/s -> 2 s.

### 14.3 Consequences

- **Row rate scales with GRANT rate, not bit rate.** A full-band 273 PRB mcs-10 grant carries
  ~12 kB but yields exactly ONE CFR row. At 15 Mbps the scheduler needed only ~149 of ~1400
  available DL slots/s; at 100 Mbps it used ~1550. Per-CPI data rows rose 144 -> 215 as predicted,
  so the lever works -- the receiver simply cannot survive long enough to exploit it.
- **There is currently a hard trade-off between rows/s and dwell time**, and both are needed:
  Doppler resolution comes from dwell, Doppler RANGE from row rate.
- **The ~1400 grants/s ceiling would give ~1200 rows/s -> vel_max ~ +/-30 m/s**, i.e. drone-capable
  WITHOUT sub-slot sampling. An earlier claim in this document that sub-slot sampling is "the only
  lever of the right order" was made against a lightly loaded cell and is wrong at full load.
- Do NOT read the 2 s runs' rows/s (46 and 86/s): they are dominated by sync time and are not a
  measurement of anything.

### 14.4 What to do

1. **Move the blind PDSCH decode off the PHY receive thread** (the `isac_capture.c`/`isac_buffer.c`
   consumer-thread shape this project already uses). This is the fix; everything else is a
   workaround.
2. Until then, pick an operating point deliberately: low load for long coherent dwells, high load
   for short high-PRF captures. A load sweep to find where lock breaks (~40 Mbps?) was proposed and
   NOT run.
3. When re-testing, re-read the C-RNTI: it changed 0x460b -> 0x460c across these runs.

**Method note.** An earlier reading of "the cell is idle, no DL traffic" was taken from a window
measured AFTER iperf had been stopped, and was wrong about the run conditions. Bracket the gNB log
around the capture -- never characterise offered load from a window not verified to be inside the
test. Same class as R2/R7 in section 1.

---

## 15-41. PASSIVE PDSCH PIPELINE: WHAT WAS BUILT AND WHAT IS MEASURED (2026-08-24/25)

**This block replaces the original §§15-41, which were a session-by-session narrative containing
many hypotheses that were later refuted by their own follow-up measurements. Only conclusions that
SURVIVED are kept. Section numbers referenced from memory or elsewhere map here via §15.6.**

### 15.1 What was built (all on `passive-rx-only`, commit `1c2a7fb291` + later uncommitted work)

**Deferred PDSCH decode** (`nr_pdsch_passive_queue.{h,c}`). `nr_pdsch_passive_decode()` costs 775 us
mean / 2689 us max against a **500 us slot budget**, so an occasion that ran one always overran its
deadline. A three-arm ablation at ~1500 grants/s pinned it as the cause, not a correlate:

| arm | dwell | `max_pos_acc` |
|---|---|---|
| tap on, decode ON | **3 s** | runs away 375 -> 583 -> 794 |
| tap on, decode OFF | 88 s | flat |
| tap OFF | 105 s | flat |

It is NOT aggregate CPU -- the surviving arm ran at 17.9 % duty and the dying arm at 25 %. What
separates them is the TAIL: occasions over the 500 us deadline went **9.0 % -> 0.017 %**.
Config: `pdcch_blind_monitor_pdsch = "<decode>:<mcs_tbl>:<xoh>:<rv0only>:<max/slot>:<n_consumers>:<depth>:<base_core>"`,
default `n_consumers = 0` = the previous in-line behaviour, bit-identical.

Two design points measurement forced:
- **The consumer RE-FEPs from `rxdata`** rather than being handed transformed samples. The 775 us
  splits fep 216 / chest 235 / alloc 74 / demod 85 / ldpc 163 us, so handing over transformed data
  moves only 42 % and leaves 451 us -- still 90 % of the slot budget.
- **Admission is DROP-OLDEST.** A FIFO under overload hands the consumer the oldest job, i.e. the one
  closest to being overwritten. Measured under drop-newest: `decoded=58384, crc_ok=0`. For sensing a
  dropped grant costs one CFR row; a stale one costs a WRONG row.
- `rxdata`'s steady-state write region is ONE FRAME (`nr-ue.c:1121`), so a slot's IQ is overwritten
  exactly `slots_per_frame` later, SILENTLY. Every job is checked at dequeue against
  `nr_ue_diag_producer_absolute_slot` and discarded if stale.

**CPI slow-time fix (the `vel_max` defect).** `sensing_engine.cc` computed the slow-time delta as
UNSIGNED `(wrap + slot_idx - prev) % wrap`. Correct only for in-ORDER submissions -- an arrival 2
slots early yields `20478`, not `-2`, so every out-of-order arrival from the concurrent consumer pool
added ~20480 slots to `cpi_slot_span`. A 128-row CPI whose true span is ~256 slots reported ~190,000.
Fixed with a signed shortest-path delta (same fix on `abs_slot_run_`):
**T_slot 1500 -> 3.76, `vel_max` 0.1 -> up to 23.7 m/s measured on real CPIs.**

**Receive-thread load.** Two causes of PBCH lock loss removed: (a) the per-accept
`nr_slot_fep` + `nr_pdsch_channel_estimation` (96.7 us, ~14.5 % duty at 1500 accepts/s) was computed
and DISCARDED whenever `want_dmrs` is false and the SNR gate is off -- now skipped; (b) `PARMSET` /
`PDSCHQ` / `BTIM` prints sat OUTSIDE the periodic-summary block and fired every occasion --
**790,798 lines and 163 MB per 90 s run**, an instrument heavy enough to cause the timing runaway it
was added to diagnose. **Dwell 3-6 s -> 66-76 s; `max_lag_slots` 20-30/20 -> 8/20.**

**Three antenna-combining defects fixed** (`ISAC_RX_MRC_MODE` selects branch-0 / strongest / MRC-all /
MRC-live):
1. `nr_channel_compensation.c` accumulated with non-saturating `add_epi16`, which SIGN-FLIPS on
   overflow -- while the `rho` accumulator in the same function already used the saturating form.
2. MRC headroom `log2_approx(nb_rx >> 1)` gives 1 bit for a 4-branch sum needing 2.
3. `nr_dl_channel_estimation.c` ASSIGNED `*nvar` inside the per-antenna loop and divided by
   `nb_antennas_rx` in a helper that never summed across antennas -- so `nvar` described only the LAST
   antenna and was ~4x too small.

### 15.2 Current measured baseline (mcs 10 / 16QAM pinned, retx 4, ~1540 grants/s)

| run | `crc_ok/try` | decoder health `ok/(ok+seg_fail)` | empty `zero_tb/(ok+zero_tb)` |
|---|---|---|---|
| M11 | 59.8 % | **86.4 %** | 34.0 % |
| M12 | 57.6 % | **86.0 %** | 36.5 % |

**`crc_ok/try` is NOT decoder health.** It is `health x (1 - empty)` and the two factors must always
be reported separately. Also measured: dwell 66-76 s at full load; `vel_max` up to 23.7 m/s; PDCCH
catch 81.4 % (an upper bound -- it counts accepted candidates, and one grant can be accepted at two
aggregation levels).

### 15.3 RESOLVED: the all-zero transport blocks are gNB grants with no MAC PDU

From the gNB's own log, alongside the grant that produced it:
```
[MAC] DL ue=0 rnti=0x461e h_id=12: Failed to assemble MAC PDU. Cause: No HARQ buffers available
[PHY] PDSCH: rnti=0x461e h_id=12 prb=[0,273) symb=[1,14) mod=16QAM rv=0 tbs=12033
```
The gNB schedules the grant, emits the DCI and transmits the PDSCH **with no MAC PDU in it**. The
receiver decodes it faithfully and recovers zeros; the all-zero guard then rejects it.
**Confirmed from both ends: gNB 30.4 % of DL grants, receiver 29.4 % all-zero on long-TDA grants.**
There is no decoder, LLR, descrambling, segmentation or slot bug behind this population.

It fooled several sessions because RLC has ~590 kB queued -- the data EXISTS, but MAC cannot build a
PDU without a free HARQ buffer, so the grant goes out empty anyway.

**It is GRANT-RATE driven, not retx driven.** At matched load: `max_nof_harq_retxs` 16 @ ~1500/s =
30.4 %; retx 4 @ ~1539/s = **30.7 %**. (An apparent 2.1 % was an artefact of a simultaneous
`max_ue_mcs` edit halving the grant rate.) Leave retx at 4 -- it wastes less airtime -- but it is not
the fix. Look for a HARQ process/buffer-pool knob or a shorter k1 (currently 4).

**High load still wins on rows despite the waste:** 1538/s x 0.69 = ~1077 usable/s against
666/s x 0.98 = ~653/s. Do not cut offered load to chase the empty-grant percentage.

### 15.4 REFUTED BY MEASUREMENT — do not re-investigate without new evidence

For the PDSCH CRC bimodality and the all-zero population:
decode parameters (identical `PARMSET` census across a 0 % and a 37 % run) · branch selection ·
constellation quality · `log2_maxh` · C-RNTI / re-attach (one RNTI gave 0 %, 48.9 %, 80.5 % AND
82.6 %) · rank · MCS · offered load · queue staleness · RT-thread starvation · **DM-RS sequence /
SFN-slot error** (chest SNR is a valid 9.0-18.6 dB in the failing runs; a wrong gold sequence gives
~0 dB) · **FFT-window / sub-bin STO** (`shiftForNextFrame=-5`, `max_pos_acc` 469-476, identical in
both modes) · **precoder / PMI** (per-run census: `pm_index=7` dominates BOTH a 43.4 % and a 0.0 %
run; 19/11/23 dominates BOTH a 40.7 % and two 0.0 % runs) · **signal degradation** (chest SNR is
ANTI-correlated with CRC: 18.6 dB -> 0 %, 9.0 dB -> 43.4 %) · **LLR saturation** (0.0000 % saturated)
· **missing soft information** (LLR magnitudes identical between decoded and failing TBs, 187 vs 188)
· **reassembly / TBS / CRC-type** (`tb_fail = 0` in every run; `payload_bytes` matches `TBS/8` to
<0.5 %) · **double descrambling** (`nr_dlsch_demodulation.c` has no scrambling code) · **segmentation
/ filler bits** (K and Z identical across outcomes) · **amplitude / `ch_mag` scaling** (sign and
magnitude bits equally biased, 86.97 % vs 86.72 %) · **k0** (PDCCH and PDSCH share the slot tag
`[303.4]` in one `DL_TTI.request`, so k0 = 0 on this cell).

**Still open:** `k0` is genuinely unhandled anywhere in the blind monitor (`parse_tda()` reads
`start:len:map` only, extraction always uses `proc->nr_slot_rx`). Real code debt that would break a
deployment using k0 != 0 -- but NOT a cause on this cell.

### 15.5 Traps this rig has charged for

- **Never change two gNB settings at once.** A "confirmed win" was retracted because a simultaneous
  MCS edit had halved the grant rate.
- **>= 5 runs per arm, alternated, per-run values reported.** Two runs at 0 % proved nothing twice;
  a 2x4 sweep that looked rigorous was ~1.5 runs per arm and produced a wrong hardware conclusion.
- **Check your own probe volume** before blaming the receiver (163 MB/run caused the fault it was
  measuring).
- **Verify the window is representative** -- an idle-cell sample was once read as confirmation.
  Bracket the gNB log AND check the offered rate is the regime under test.
- **Measure LLR sign statistics only AFTER descrambling.** Before it the scrambler randomises signs
  and every payload reads ~49-50 % positive, which proves only that the scrambler works.
- **`ISAC_RX_MRC_MODE` default must stay 0** until the branches are balanced; modes 1-3 are correct
  code losing to a hardware imbalance.
- **Read `ANTPOW`, not `RXBRANCH pw[]`, for antenna power.** `pw[]` is a channel-ESTIMATE magnitude
  and swings 10 dB between transport blocks; `ANTPOW` is raw time-domain power averaged over 2000
  slots. Note `ANTPOW` is an AMPLITUDE (20log10), `pw[]` a POWER (10log10).

### 15.6 Probes and harness (all default OFF except the census)

`ISAC_PDCCH_TIMING` (per-stage RT cost + per-occasion histogram + `over_slot`) ·
`ISAC_PDCCH_NO_PDSCH_DECODE` (ablation switch) · `ISAC_RX_MRC_MODE` 0-3 + `ISAC_RX_BRANCH` 1|2
(`RXBRANCH` per-branch |H|^2, `NVAR`, `L2MAXH`; =2 logs every TB) · `ISAC_PDSCH_EVM` (`EQDIAG`) ·
`ANTPOW` (raw per-antenna power, always on in the 1 s census) · `PARMSET` (distinct decode-parameter
census) · `LDPCDIAG` (ok / seg_fail / tb_fail / zero_tb split) · `LLRDIAG` + `BITPOS` (LLR magnitude
and sign, binned by outcome and by 16QAM bit role) · `SHAPE` / `NSYM` (outcome by grant shape and
symbol count).
Harness: `captures/btim/catch2.sh` (offered-rate-bracketed, hard-kills the shutdown hang),
`scoreboard.py`, `modestats.py`, `antstats.py`.

**Old section numbers** referenced elsewhere all map into this block: §§15-16 build, §§17-21 antenna
combining, §§22-28 dwell/queue/vel_max, §§29-38 the CRC investigation, §§39-41 the resolution.


---

# 42. HANDOVER — MAKING THE PASSIVE RECEIVER MCS-ADAPTIVE (2026-08-25)

**Read §§39-41 first for the measured baseline, then this. Everything below is the next objective.**

## 42.1 The goal, stated precisely

The pipeline now works end to end: blind PDCCH -> DCI -> passive PDSCH decode -> data-aided CFR ->
CPI -> range-Doppler, at full load, with real measured `vel_max`. **What it cannot yet do is survive a
change of MCS.** Pinned at mcs 10 / 16QAM it decodes 86 % of non-empty transport blocks; at mcs 25 /
256QAM it decodes essentially none.

That matters because **the receiver does not choose the MCS and never will.** The gNB picks it by
closed-loop link adaptation against the SERVED UE's channel. Any change in that UE's position, the
cell's load, or the operator's config silently moves the modulation, and today that takes the sensing
pipeline from working to dead with no warning and no receiver-side recourse.

**Objective: decode robustly across the MCS range the gNB may plausibly choose, without pinning the
gNB.** Pinning mcs 10 is a laboratory crutch, not a deployment.

## 42.2 Why it fails today — the margin budget

| | required SNR (R ~ 0.64) |
|---|---|
| mcs 10, 16QAM | ~12 dB |
| mcs 25, 256QAM | ~23 dB |

§12.1 measured this receiver at EVM 29.7 % -> 90.7 % CRC, i.e. **~1 dB above the 16QAM waterfall**.
The gap to 256QAM is therefore ~11 dB, and there is ~1 dB in hand. Three structural reasons, and the
first two are the whole budget:

1. **No receive diversity.** Branches measure 0 / -29 / -17 / -6 dB (§21), so `nr_rx_pdsch`'s rank-1
   path decodes branch 0 ALONE. Four balanced branches are worth **up to +6 dB** of MRC gain; the
   current imbalance yields +0.8 dB, which is why the combining modes lose (§18, §19).
2. **No HARQ soft combining.** `pdsch_rv0_only = 1` skips every `rv != 0`, so each TB gets ONE shot
   while the served UE accumulates redundancy across retransmissions. This is the served UE's other
   structural advantage (§12.5) and the only SOFTWARE lever of comparable size.
3. **No link adaptation and no beam.** Cannot be fixed receiver-side; it is the definition of passive.

## 42.3 Direction, ranked by dB per unit of effort

**(1) Fix the receive-branch imbalance — up to +6 dB. Hardware. Biggest single lever.**
`ANTPOW` (`nr-ue.c`, `ISAC_RX_BRANCH=1`) reports RAW per-antenna power off the time-domain buffer,
which is the ONLY trustworthy antenna measurement here -- `RXBRANCH pw[]` is a channel-ESTIMATE
magnitude and swings 10 dB between transport blocks (§20, §21). Target: all four within ~3 dB.
Verify with a 5-run `ANTPOW` batch (§21's method); then re-test `ISAC_RX_MRC_MODE` 2 and 3, which are
already implemented and currently lose only because two branches are noise.

**(2) Implement HARQ soft combining — several dB, and it is the main software lever.**
Today `rv != 0` is counted and discarded. A passive receiver CAN combine: it sees every
retransmission of a TB (same `h_id`, same TBS, increasing `rv`), and `nr_pdsch_passive_decode()`
already has a private HARQ context (`g_harq`) that is namespaced per consumer. The work is to key
that context by `(rnti, h_id)` across slots instead of discarding it, accumulate LLRs before the LDPC
call, and decode when the accumulated redundancy suffices. **Traps:** `harq_unique_pid` must stay
disjoint from the attached-UE (1000+) and CSI (2000+) namespaces; with N consumers two threads can
hold the same `h_id` concurrently, so the context must be locked or sharded by `h_id`; and a stale
context from a previous TB with the same `h_id` must be invalidated on `ndi` toggle.

**(3) Lower MCS — up to ~+10 points of decoder health, untested, and only a diagnostic.**
Pinning mcs 6-8 should raise the 86 %. Useful to CALIBRATE the margin (it tells you how many dB you
actually have), but it is not a solution: it requires controlling the gNB, which §42.1 rules out.

**(4) Fix the gNB HARQ-pool exhaustion — +34 points on the headline metric, but not on adaptivity.**
34 % of grants carry no MAC PDU (§39, §41). Worth doing for row rate, orthogonal to MCS adaptivity.
Grant-RATE driven, not retx-driven (§41) -- look for a HARQ process/buffer-pool knob or a shorter k1.

## 42.4 How to know if you have succeeded

The acceptance test is a **gNB MCS sweep**, not a single-point number. With `min_ue_mcs == max_ue_mcs`
pinned in turn at 6, 10, 16, 20, 25, measure at each point:
- `ok/(ok+seg_fail)` -- decoder health, the metric that is NOT contaminated by empty grants;
- `zero_tb/(ok+zero_tb)` -- the gNB's empty-grant fraction, so it can be divided out.

**Today that curve collapses between mcs 10 and mcs 25. Success is it degrading gracefully instead.**
A useful target: >= 80 % decoder health at mcs 16, and non-zero at mcs 20.

## 42.5 Traps this rig has already charged for

- **Never change two gNB settings at once.** §41 retracts a "confirmed win" that was an artefact of a
  simultaneous MCS edit halving the grant rate. Change one thing, measure, then change the next.
- **>= 5 runs per arm, alternated, per-run values reported** (§19.3). Two runs at 0 % proved nothing
  twice today.
- **`crc_ok/try` is NOT decoder health.** It is `health x (1 - empty)`. Always report both factors.
- **Check your own probe volume** before blaming the receiver: an instrument firing every occasion
  cost 163 MB/run and caused the timing runaway it was added to diagnose (§27.3).
- **Verify the window is representative.** An idle-cell sample was read as confirmation once (§33).
  Bracket the gNB log AND check the offered rate is the regime under test.
- **`ISAC_RX_MRC_MODE` default must stay 0** until the branches are balanced; modes 1-3 are correct
  code losing to a hardware imbalance (§19).
