# Passive blind PUCCH extraction: design (2026-11-05)

Status: design for the operator's review. It is the third part of the two-modes work, after
`2026-11-03-two-modes-design.md` and `2026-11-04-param-extraction-addendum.md`. It will be implemented on sens6
(x86, RTX 4060 Ti) and must stay portable to the DGX.

## 1. Goal and non-goals

Goal: in traffic mode, extract the uplink control traffic of every UE of every PCI, at a cost that stays small
next to DCI extraction:
- the HARQ-ACK (ACK/NACK/DTX) that answers each DL grant, with its slot and PUCCH resource;
- scheduling requests;
- CSI reports: their presence, and their payload where it can be decoded and verified;
- the PUCCH resource map of each RNTI and of the cell.

Non-goals:
- No decoding of UCI multiplexed on PUSCH (that is a later item).
- No blind search of the whole UL grid.
- No calibration on the DEIB cells.
- No new env, CLI or -D flag.
- Sensing mode gets nothing in v1. The PUCCH DM-RS is narrowband (1-16 PRB, below the 128-PRB wideband floor), so it
  could at most be an optional low-priority CFR source later.

Anti-goals:
- A PUCCH field printed from a guessed resource (the operator's "never guess" rule applies, as in E1-E3).
- Any measurable increase in scan or decode drops in either mode.

## 2. Principle: look only where the DL says a PUCCH will be

The DL DCIs we already decode tell us when and where each HARQ-ACK must be sent:
- the PDSCH slot n;
- the PDSCH-to-HARQ timing K1 (the E1-proven field, or the 1_0 field);
- the PUCCH resource indicator PRI;
- the DAI.

`harq_fb_abs_slot` is already in the DCI record. So:
- PUCCH processing runs only on UL-capable slots that some decoded DCI points to.
- Inside those slots, only the PRBs that the candidate resources can occupy are processed.
- When no DL grant expects feedback, the cost is zero.
- Periodic resources (SR, periodic CSI) are scanned only at the periods learned for them, using the existing
  periodic tracker (`nr_periodic_track.h`, as used by blind SRS).

## 3. Evidence ladder (same rules as E1-E3)

### Level 0: labels from the DL alone (no IQ)
For each DL grant of an RNTI, the next DCI on the same HARQ process says what the gNB received:
- NDI toggled means ACK;
- the same NDI with a new RV means NACK;
- no further DCI is unknown.

These labels are the ground truth used to learn the resources and to measure detection accuracy on DEIB, where no
gNB log exists. A label is used only when that HARQ process has no ambiguity (the E1 pid field is proven).

### Level 1: formats 0 and 1, CRC-free
- Divide the received PRB by the base sequence. The group and sequence number come from `hoppingId` if known, else
  the PCI (TS 38.211 6.3.2.2). Then a 12-point IDFT gives every cyclic shift at once (TS 38.211 6.3.2.3/6.3.2.4).
- Format 1 adds the time-domain OCC (TS 38.211 Table 6.3.2.4.1-2), separated by a transform across the symbols.
- The output is an energy map over PRB × start symbol × number of symbols × cyclic shift × OCC, computed only at the
  predicted slots and PRBs.

### Level 2: learning the resources
- Per RNTI, the energy-map peaks at its predicted slots are correlated with its Level-0 labels. For format 0, ACK and
  NACK sit at fixed cyclic-shift offsets from m0 (TS 38.213 Table 9.2.3-3/4).
- A resource (format, PRB, m0, symbols, OCC, second-hop PRB) is LEARNED only when one hypothesis survives a
  significance test (alpha = NBS_SIGNIF_ALPHA). Otherwise it stays candidate and nothing is printed.
- The PRI field maps to a resource-set index. So one RNTI's resources are learned per PRI value.
- Cell families, as in E3: a resource set learned for one RNTI is the first hypothesis for others.
- SIB1 pucch-ConfigCommon gives the common resources (TS 38.213 Table 9.2.1-1, index 0-15). They are exact for
  Msg4 HARQ-ACK and for UEs before dedicated configuration.

### Level 3: decoding
- Formats 0/1 with a learned resource: HARQ-ACK bits. DTX is decided against the noise from empty resources with a
  derived threshold. SR is energy on the learned SR resource at its learned period.
- Format 2:
  - Locate it CRC-free from its DM-RS first. The DM-RS is on every third subcarrier; its PN c_init depends only on
    N_ID (the PCI unless configured), the slot and the symbol, not on the RNTI.
  - The payload is scrambled with c_init = n_RNTI·2^15 + n_ID. Test only the C-RNTIs known from the DCIs of that
    slot's PRI/K1 chain.
  - Payloads of 11 bits or fewer use Reed-Muller and carry no CRC, so they are printed only when consistent with the
    Level-0 HARQ-ACK labels.
  - Payloads of 12 bits or more use Polar with CRC6/CRC11. A CRC pass confirms both the RNTI and the payload
    length.
- Formats 3/4: DFT-s-OFDM with low-PAPR DM-RS, the same approach as format 2 (DM-RS first, then the RNTI hypotheses,
  then the Polar CRC).
- Reuse the gNB-side decoders in `openair1/PHY/NR_TRANSPORT/pucch_rx.c` (nr_decode_pucch0/1/2) for the maths. Wrap
  them behind UE-side pure functions; never link gNB state into the receiver.

## 4. Cost and integration
- Capture comes from the existing UL-slot raw IQ capture point (the blind-SRS capture at `executables/nr-ue.c` ~823).
  Only the predicted symbols and PRBs are FFT'd.
- The work runs on the existing background worker pattern (as ulNidBg / SRS), never on the RX or scan threads, and
  takes no lock that a scan consumer holds. The g_phase2_mu / UL-layout lock convoy fixed on 2026-11-04 must not
  come back.
- A per-cell CPU budget follows the E1/E3 pattern: a fixed share, oldest-due first, deferrals counted. The budget
  value is a general bound, for example 10 ms/s per cell; it is never tuned on a cell.
- Traffic mode only. Legacy and sensing outputs stay byte-identical (tested).
- Records: a new type "uci" with:
  - rnti, abs_slot, the dci_abs_slot it answers, format, resource;
  - harq_ack bits, sr, csi (presence; payload only if CRC-verified);
  - snr, evidence ("label" / "phys" / "crc"), cfg_ref.
  It joins the DCI record through dci_abs_slot and harq_fb_abs_slot.

## 5. Validation
- sens6 lab, with the gNB log as ground truth (validation only, never receiver input): the OCUDU ZMQ bed
  (`tests/passive_rx/ocudu/`) and the LiteOn + COTS UEs. Per format, measure:
  - detection,
  - false alarms,
  - ACK/NACK/DTX confusion,
  - resource correctness,
  - CSI payload correctness.
- An OAI rfsim gNB bed is an alternative where OCUDU is not runnable.
- DEIB OTA, daytime, A/B against the traffic mode without PUCCH:
  - agreement with the Level-0 labels;
  - CPU and scan/decode drops not worse (the no-loss rule);
  - never an absolute-recall claim.
- Evidence labels name the host: [MEASURED OTA sens6] or [MEASURED OTA DGX]. Results are never merged across hosts or
  cells.

## 6. Open points for the operator
- Whether the PUCCH DM-RS should become a sensing CFR source later (default: no in v1).
- Whether an RNTI's HARQ-ACK should be printed only once its resource is learned (default: yes, never guess).
