# Prompt for the precoder / true-channel work (PUCCH session on sens6; paste as a message)

You already run the PUCCH work on sens6 (`pucch/sens6`). This adds a second goal to the same session:
**recover the actual (unprecoded) downlink channel for sensing**, using the precoder W where it can be proven.
Read `CLAUDE.md` and `PROJECT_MEMORY.md` again if your context was reset. They are binding.

**Sync first:** fetch `twomodes/design` at the commit named in the operator's message and merge it into `pucch/sens6`.
It brings `docs/superpowers/reports/twomodes/CSIRS-CLASS-20261105.md` and `tools/csiclass/`. Read that report completely.

## 1. Goal, stated precisely

The goal is the physical channel H(f, t) from the gNB antenna ports to our RX antennas, per subcarrier, every slot it can be
observed. It feeds the sensing algorithms (delay, Doppler, angle). W is a means to that end, not an output in itself.
Everything below is per cell and per bandwidth; results are never merged.

What each signal gives us (TS 38.211 / 38.214 Rel-17; quote the clause next to every formula in code and docs):
- **Non-precoded NZP CSI-RS** (CSI acquisition, "class A"; 38.211 §7.4.1.5, 38.214 §5.2.2):
  - It gives H directly, Nrx × P per RE, at the CSI-RS REs and period.
  - This is the primary true-channel source. W is not needed for it.
- **Beamformed CSI-RS** (38.214 §5.2.1.4; CRI, port selection): it gives H·B, with B the unknown beamformer.
- **PDSCH DM-RS** (38.211 §7.4.1.1; 38.214 §5.1.2.3, PRG bundling, `prb-BundlingType` static/dynamic, nominal PRG 2/4/wideband,
  and the DCI 1_1 "PRB bundling size indicator"):
  - It gives H_eff = H·W per PRG and per layer.
  - DM-RS ports and CDM groups come from the DCI 1_1 "Antenna port(s)" field (38.212 Tables 7.3.1.2.2-1…-4), and the layers from
    those ports.
  - S11 already exports H_eff per decoded grant (`pdsch_dmrs` records).
- **Fundamental identifiability limit (state it in the design; never try to beat it):**
  - From our RX only the component of W in the row space of H affects what we observe. That is at most Nrx = 4 dimensions at our
    position, and 1 on 923 (see §2).
  - Hence no method can recover a full P × L W from our observations alone, unless the gNB uses a finite codebook and we identify
    the entry, or the UE reports it (PMI) and the gNB follows it.

## 2. Findings on the DEIB NSA cells (DGX GB10, 4 RX, one 4 s capture per cell; full numbers in CSIRS-CLASS-20261105.md)

- **PCI 923 (TIM), 217 PRB, n78 TDD:**
  - **CSI acquisition:** a 32-port NZP CSI-RS at symbols 8, 9, 12, 13, subcarriers 4..11, density 1, period 40 slots, n_s = 8,
    full carrier, ID 923. It is row 16 (fd-CDM2) or row 17 (cdm4); the two cannot be separated blind.
  - **TRS:** 1-port, comb 4, symbols 4, 5, 8, 9, two consecutive slots every 80. IDs 997, 576 and 921.
  - **Class:** A (non-precoded). It fits a dual-polarised UPA (8, 2) at 0.947, against a permutation-null max of 0.357. Port power
    spread is 2.4 dB, and the dominant beam is stable in 19 of 20 blocks.
  - **Type I codebook:** NO signature. The best codeword scores 0.955 against 0.934 for the best random vector (rank-1 ceiling).
    The on-grid codebook beats a half-step-shifted grid in only 1 of 30 grants.
  - **Rank:** H at our position is rank 1 (one path carries 66 % of the energy), so per-PRG least squares is infeasible (3–6 % of
    W identifiable).
- **PCI 415 (Vodafone), 217 PRB, n78 TDD:**
  - **CSI acquisition:** a 16-RE/RB resource at symbols 10–13, subcarriers 4..7, abs % 160 == 105, ID 415. It is FDM'd in the same
    slot with neighbour-sector IDs 418, 249, 252 and 380. The port order and CDM row are guesses.
  - **Type I codebook:** no evidence; chance level (4 of 21 grants above random). The test is sensitive: a planted Type I codeword
    is recovered 11–12 times in 12 at 0 dB.
  - **Precoder granularity:** 16 RB. DM-RS direction coherence drops from 0.64 to 0.485 at the 16-RB boundary.
    [INFERRED] This fits subband or SRS-reciprocity precoding, not Type I.
- **PCI 508 (WindTre):** no multi-port CSI-RS found (all 1024 IDs × 20 slots), and almost no traffic.
- **PCI 403:** never syncs.
- **Not tested on DEIB:** rank-2 joint codebook fits, Nl 3–4 grants (415 has 89 that passed CRC), more than one capture per cell,
  and daytime load.

So on DEIB the true channel must come from the non-precoded CSI-RS, plus DM-RS fusion (§3 path 4). Codebook identification of W is
not available there. On sens6 (OCUDU, known config) every path can be validated against the gNB log, which is ground truth only and
never receiver input.

## 3. Cover every spec possibility (design must enumerate all; implement in this order)

For each path: the spec clause, what is observable, the evidence level that allows printing, and the test.

1. **H from non-precoded CSI-RS** (all rows of 38.211 Table 7.4.1.5.3-1: 1–32 ports, noCDM / fd-CDM2 / cdm4-FD2-TD2 / cdm8-FD2-TD4,
   densities 0.5 / 1 / 3):
   - Blind row and port mapping, using the existing CSI-RS discovery (`csirs_disc*`) and `tools/csiclass`.
   - Class A/B decision with the covariance-structure test of the report (null-calibrated).
   - Output H per RE with its quality (SNR, valid ports, class confidence).
   - Where row/CDM is ambiguous (923: rows 16 vs 17), output only quantities that are invariant to the ambiguity, or carry both
     hypotheses with a flag. Never pick one silently.
   - TRS gives single-port H for tracking/Doppler; ZP-CSI-RS and CSI-IM are not channel sources.
2. **W from the UE's own CSI report (CRC-proven PMI):**
   - **Channels:** PUCCH formats 2/3/4, periodic and semi-persistent (38.213 §9.2, 38.212 §6.3.1). PUSCH CSI is out of reach at DEIB
     (UL CRC-OK ≤ 0.007 %); on sens6, try it if the UL is decodable there.
   - **Coding:** CSI part 1 and part 2 (38.212 §6.3.1.1.2, §6.3.2.1.2). RI, CRI, PMI (i1, i2), CQI and LI, with sizes from 38.212
     Tables 6.3.1.1.2-x. Reed-Muller for ≤ 11 bits has no CRC and is printed only when consistent with other evidence. Polar for
     ≥ 12 bits with CRC6/CRC11 confirms both the RNTI and the payload length.
   - **Codebooks**, all of them, with their 38.214 clauses:
     - Type I single-panel (§5.2.2.2.1): modes 1 and 2, all (N1, N2, O1, O2) of Table 5.2.2.2.1-2, rank 1–8;
     - Type I multi-panel (§5.2.2.2.2);
     - Type II (§5.2.2.2.3) and Type II port selection (§5.2.2.2.4);
     - Rel-16 enhanced Type II (§5.2.2.2.5) and eType II port selection (§5.2.2.2.6);
     - Rel-17 further-enhanced Type II port selection (§5.2.2.2.7);
     - `codebookSubsetRestriction`.
   - **Unknown config:** enumerate the payload-size hypotheses from the CSI-RS port count found blind, and let the CRC decide. Never
     print a PMI that is not CRC-proven.
   - **What this gives:** the PMI is the UE's recommendation. Whether the gNB follows it is checked against H_eff (path 3) before it
     is used as W. Label it "reported", or "reported + confirmed".
3. **W by codebook identification from H_eff:**
   - With H from path 1, test every allowed codebook entry per PRG (and jointly across layers for rank ≥ 2). Use the significance
     test of the report: a random-W null and the on-grid against shifted-grid quantisation test.
   - Accept only with the significance level fixed by the existing alpha constant (`NBS_SIGNIF_ALPHA`), never tuned per cell.
   - Non-codebook gNBs (SVD, reciprocity) must come out "not identified", never as a forced best entry.
4. **H by DM-RS fusion (the general path, no codebook assumption):**
   - Within the channel coherence time, stack grants whose W_k differ (different UEs and beams): H·[W_1 … W_k] = [H_eff,1 … H_eff,k].
   - With known W_k (paths 2 and 3) this recovers H on span{W_k}.
   - With unknown W_k, the observable is the projection: the joint subspace of H_eff across grants. Report its dimension and use it
     to densify H between CSI-RS occasions (path 1 anchors every 40 slots on 923). A tracking filter is allowed; state its model and
     validate it.
   - Report identifiable rank, not a fabricated full H.
5. **Reciprocity (TDD, n78):** the gNB may form W from the UE's SRS (38.214 §6.2.1).
   - We see the SRS at our RX (blind SRS export), but that is the UE→us channel, not the UE→gNB channel. It does not give W.
   - State this, and do not use it as W evidence.
   - It may still help to explain the per-16-RB structure.
6. **UL precoder (exact, cheap):**
   - **Codebook PUSCH:** DCI 0_1 "Precoding information and number of layers" (38.212 §7.3.1.1.2, Tables 7.3.1.1.2-2…-5). Its width
     depends on `maxRank`, `codebookSubset`, the number of SRS ports and `ul-FullPowerTransmission`, all unknown, so prove the width
     like the E1 field prover.
   - **The matrix:** from 38.211 §6.3.1.5 Tables 6.3.1.5-1…-7.
   - **Non-codebook PUSCH:** the SRI field, with no matrix.
   - Traffic-mode field only.
7. **Granularity and layers:**
   - PRG from the PRB-bundling field or blind from DM-RS coherence (the 16-RB result on 415).
   - Layers from the antenna-port field.
   - Up to 8 layers (2 CDM groups × up to 4 ports, dmrs-Type 1/2, single or double symbol).

## 4. Rules (in addition to CLAUDE.md)

- **Spec compliance:** every formula and table cites its TS 38.211/212/213/214 Rel-17 clause. Any deviation is a documented bug.
- **Never guess:**
  - W or PMI is printed only with its evidence label: crc / reported+confirmed / codebook-identified / projected / none.
  - Ambiguities are carried, never resolved silently.
- **Generality:**
  - No per-cell or per-operator constants or tuning.
  - Thresholds come from null distributions or existing alpha constants.
  - All port counts, rows, codebooks and ranks are supported; the code must not assume 32 ports or the 923 layout.
- **Placement:** sensing mode for the H outputs, traffic mode for the PMI, TPMI and SRI fields.
  - Legacy outputs stay byte-identical.
  - Sensing compute stays on the CPU (the GPU may only move data that already exists).
  - No locks on RX or scan threads, never `g_phase2_mu` or the UL-layout lock.
  - The worker pattern and CPU budget follow the PUCCH design.
- **Validation:**
  - On sens6 OCUDU, with the gNB log as ground truth, per path report: CSI-RS H against the configured ports/row; PMI against the
    gNB's received PMI; W against the gNB's applied precoder if logged; TPMI against the gNB's DCI.
  - On DEIB, only self-consistency and the null tests (no absolute claims).
  - Label everything `[MEASURED OTA sens6]` / `[SIM VERIFIED sens6]`; DGX numbers stay separate.

## 5. Process

1. Write the design first: `docs/superpowers/specs/2026-11-06-true-channel-precoder-design.md`. It covers §1–§4: the path table
   with clauses, the evidence ladder, the identifiability argument, the record formats (extend `pdsch_dmrs` with W evidence; a new
   `csirs_h` record), the cost and threads.
2. Send it to the operator for approval. Do not write production code before approval.
3. Then plan and execute with superpowers:writing-plans and superpowers:subagent-driven-development (Sonnet agents, a review per
   task, a final review).
4. Suggested order:
   1. path 1 (H from CSI-RS) in sensing mode;
   2. path 6 (TPMI);
   3. path 2 (PMI via PUCCH Level 3, shared with the PUCCH plan);
   4. path 3 (codebook identification);
   5. path 4 (fusion);
   6. path 5 documented only.
5. Report to the operator briefly: per path, per cell, what is extracted, with evidence labels and numbers.
