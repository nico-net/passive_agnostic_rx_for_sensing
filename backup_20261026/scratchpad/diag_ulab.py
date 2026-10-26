# TEMPORARY diagnostic (never committed): per-grant A/B of the UL SFO correction on identical IQ.
p='/home/sens/NICOLA/agn-wt/ul-fit/openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c'
s=open(p).read()
anchor="""    if (out.status != NR_PUSCH_PASSIVE_UNSUPPORTED && out.status != NR_PUSCH_PASSIVE_ERROR) {
      atomic_fetch_add_explicit(&g_decoded, 1, memory_order_relaxed);"""
diag="""    /* TEMP-DIAG ULAB */
    if (!job.cfr_only && !ulq_is_probe(&job.grant) && (out.status == NR_PUSCH_PASSIVE_OK || out.status == NR_PUSCH_PASSIVE_CRC_FAIL)
        && job.grant.mcs >= 20 && !job.grant.transform_precoding && nr_pusch_passive_rescue_allowed() && nr_pusch_passive_optional_share_left()) {
      static _Atomic unsigned ab_ctr;
      const int64_t e0 = nr_sfo_eps_q();
      if (e0 && (out.status == NR_PUSCH_PASSIVE_CRC_FAIL || atomic_fetch_add(&ab_ctr, 1) % 8 == 0)) {
        static const char *nm[5] = {"zero", "neg", "half", "dbl", "same"};
        const int64_t q[5] = {0, -e0, e0 / 2, e0 * 2, e0};
        int okv[5];
        nr_pusch_passive_harq_arm(false);
        for (int v = 0; v < 5; v++) {
          nr_pusch_passive_out_t o2;
          nr_sfo_pin(q[v]);
          nr_pusch_passive_decode(ue, idx, (uint32_t)job.frame_rx, (uint8_t)job.nr_slot_rx, &job.grant, job.ta_offset_samples,
                                  (uint64_t)job.absolute_slot, false, job.fo_hz, &o2);
          nr_sfo_pin(INT64_MIN);
          okv[v] = o2.status == NR_PUSCH_PASSIVE_OK;
        }
        LOG_A(PHY, "SENSING: ULAB rnti=0x%04x prb=%u+%u mcs=%u snr_db=%.1f main=%d %s=%d %s=%d %s=%d %s=%d %s=%d eps_ppm=%.3f\\n", job.grant.rnti,
              job.grant.bwp_start + job.grant.start_rb, job.grant.num_rb, job.grant.mcs, (double)out.snr_db, out.status == NR_PUSCH_PASSIVE_OK,
              nm[0], okv[0], nm[1], okv[1], nm[2], okv[2], nm[3], okv[3], nm[4], okv[4], (double)e0 * 1e-6);
      }
    }
"""
assert anchor in s
s=s.replace(anchor,diag+anchor,1)
if '#include "PHY/MODULATION/nr_sfo_state.h"' not in s:
    s=s.replace('#include "nr_pusch_passive_decode.h"\n','#include "nr_pusch_passive_decode.h"\n#include "PHY/MODULATION/nr_sfo_state.h"\n',1)
open(p,'w').write(s)
