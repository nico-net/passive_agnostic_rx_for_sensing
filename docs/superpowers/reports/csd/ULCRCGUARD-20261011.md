# ISAC_UL_LOCK_CRC_GUARD (2026-10-11)

Problem: ISAC_UL_LOCK_CRC_FIRST locked the UL layout on one PUSCH TB CRC pass. TBs <= 3824 bits use CRC16 (TS 38.212 7.2.1), so with
thousands of hypothesis trials false passes are expected (about trials/65536). OTA TIM ab_Tq1_1_230208 / ab_Tq2_1_231945 showed passes on
TBS 32/63/201 bits.

Flag ISAC_UL_LOCK_CRC_GUARD=1 (default off = old behaviour; needs ISAC_UL_LOCK_CRC_FIRST). Applies to lock and re-lock alike.
- Decisive single pass: TBS > 3824 bits (CRC24A) or n_segments > 1 with all CB CRCs ok.
- CRC16 pass = evidence per (BWP hyp, prefix, tail) per DCI length. Lock when passes m >= 2, from >= 2 distinct absolute slots, and
  H * P(Poisson(n_h / 65536) >= m) < 0.01 (n_h = trials of that hypothesis, H = hypotheses tracked, Bonferroni).
- DM-RS-SNR plausibility rule NOT implemented: the verdict only carries the decoder's own post-estimation SNR (different scale).
- Logs: `ULLAYOUT_CRC_EVIDENCE len= hyp=h/k/t tbs= nseg= decisive= passes= trials= rntis= slots= p_false= -> LOCK|SUSPECT`;
  STATS per length: susp= dec= evlock=.
- API: nr_ul_layout_crc_verdict(g, ok, tbs_bits, nseg, seg_ok, slot); nr_ul_layout_crc_result() kept (unknown TB, CRC16 under guard).
  Hook: nr_pusch_passive_queue.c verdict call.
- Tests: UlLayoutLock.CrcGuardNeedsEvidenceForCrc16AndLocksOnCrc24 (single CRC16 no lock, same-slot repeat no lock, other hypothesis
  does not add, 2 independent passes lock, re-lock same rule, CRC24A locks). Evidence: DGX unit test only, no replay/OTA.
- Limit: evidence table 16 entries per length (fewest-passes evicted); trials are only counted for the K trial hypotheses issued.
