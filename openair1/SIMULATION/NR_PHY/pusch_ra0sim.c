/* Offline regression fixture. Reuse ulsim's real UE encoder, OFDM and LDPC setup;
 * only replace the receive call. No radio harness or production test knob. */
#define main ulsim_main
#define phy_procedures_gNB_uespec_RX ra0_test_rx
#include "ulsim.c"
#undef phy_procedures_gNB_uespec_RX
#undef main

#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h"

extern int nr_ulsch_decoding(PHY_VARS_gNB *, NR_DL_FRAME_PARMS *, uint32_t, uint8_t, int *, int);
extern int phy_procedures_gNB_uespec_RX(PHY_VARS_gNB *, int, int, NR_UL_IND_t *);
static unsigned ra0_failures, ra0_trials;
/* The live UE executable owns these; the simulator has no capture producer. */
_Atomic long nr_ue_diag_producer_absolute_slot = -1;
_Atomic long nr_ue_diag_producer_wall_ns = 0;
_Atomic long nr_ue_pending_rebase_delta = 0;
_Atomic int nr_ue_pending_rebase_valid = 0;

typedef struct {
  PHY_VARS_gNB *gnb;
  nfapi_nr_pusch_pdu_t pdu;
  int frame, slot, calls;
  bool first_rejected;
} collision_trial_t;

static bool collision_decode(void *opaque, const nr_pdcch_blind_ul_result_t *grant)
{
  collision_trial_t *trial = opaque;
  PHY_VARS_gNB *gnb = trial->gnb;
  nfapi_nr_pusch_pdu_t pdu = trial->pdu;
  pdu.rb_start = grant->start_rb;
  pdu.rb_size = grant->num_rb;
  const int dmrs = count_bits64_with_mask(pdu.ul_dmrs_symb_pos, pdu.start_symbol_index, pdu.nr_of_symbols);
  const int tbs = nr_compute_tbs(pdu.qam_mod_order, pdu.target_code_rate, pdu.rb_size, pdu.nr_of_symbols,
                                 6 * pdu.num_dmrs_cdm_grps_no_data * dmrs, 0, 0, 1);
  pdu.pusch_data.tb_size = tbs / 8;
  pdu.maintenance_parms_v3.ldpcBaseGraph = get_BG(tbs, pdu.target_code_rate);
  NR_gNB_ULSCH_t *ulsch = &gnb->ulsch[0];
  ulsch->harq_process->ulsch_pdu = pdu;
  uint16_t prb[NR_PRB_SET_MAX];
  const int n = nr_ra_type0_prbs(grant->rbg_bitmap, grant->rbg_bwp_start, pdu.bwp_size,
                                grant->rbg_size, prb, NR_PRB_SET_MAX);
  AssertFatal(n == pdu.rb_size, "collision fixture bitmap/count mismatch\n");
  nr_rx_pusch_prb_list_tp(gnb, &gnb->pusch_vars[0], &pdu, &ulsch->unav_res,
                         trial->frame, trial->slot, prb, n);
  ulsch->harq_process->harq_to_be_cleared = true;
  int id = 0;
  const int rc = nr_ulsch_decoding(gnb, &gnb->frame_parms, trial->frame, trial->slot, &id, 1);
  bool nonzero = false;
  for (unsigned i = 0; i < pdu.pusch_data.tb_size; ++i) nonzero |= ulsch->harq_process->b[i] != 0;
  const bool ok = rc == 0 && nonzero && ulsch->harq_process->C > 0
                  && ulsch->harq_process->processedSegments == ulsch->harq_process->C;
  if (++trial->calls == 1) trial->first_rejected = !ok;
  return ok;
}

/* Independent scalar 38.211 Gold reference, deliberately not nr_gold_pusch(). */
static void reference_gold(uint32_t seed, uint8_t *bits, int count)
{
  uint32_t x1 = 1, x2 = seed;
  for (int n = 0; n < 1600 + count; ++n) {
    if (n >= 1600) bits[n - 1600] = (x1 ^ x2) & 1;
    const unsigned b1 = ((x1 >> 3) ^ x1) & 1;
    const unsigned b2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
    x1 = (x1 >> 1) | (b1 << 30);
    x2 = (x2 >> 1) | (b2 << 30);
  }
}

int ra0_test_rx(PHY_VARS_gNB *gnb, int frame, int slot, NR_UL_IND_t *ind)
{
  /* This also translates the simulator's queued FAPI request into the HARQ PDU. */
  const int initial_rc = phy_procedures_gNB_uespec_RX(gnb, frame, slot, ind);
  AssertFatal(initial_rc == 0, "VOID: simulator reference receive failed\n");
  /* Exercise the same delay estimator and receive-branch policy as the passive
   * caller. Restore the mode before ulsim next transmits its active UE waveform. */
  const int saved_passive = get_softmodem_params()->passive_rx;
  get_softmodem_params()->passive_rx = 1;
  NR_DL_FRAME_PARMS *fp = &gnb->frame_parms;
  NR_gNB_ULSCH_t *ulsch = &gnb->ulsch[0];
  NR_gNB_PUSCH *pv = &gnb->pusch_vars[0];
  const nfapi_nr_pusch_pdu_t *p = &ulsch->harq_process->ulsch_pdu;
  AssertFatal(p->nrOfLayers == 1 && p->transform_precoding == transformPrecoder_disabled
                  && p->dmrs_config_type <= 1 && p->dmrs_ports == 1
                  && !(p->pdu_bit_map & PUSCH_PDU_BITMAP_PUSCH_PTRS),
              "RA0 fixture scope: rank1 CP-OFDM DMRS type1/2 port0 no PTRS\n");
  const bool collision = getenv("RA0_FDRA_COLLISION") != NULL;
  const int n = p->rb_size, split = collision ? 16 : n / 3, gap = collision ? 48 : 5;
  AssertFatal(!collision || (n == 32 && p->rb_start == 16 && p->bwp_size == 106 && p->bwp_start == 0),
              "collision fixture requires the BWP106, RBG1/RBG5 allocation\n");
  AssertFatal(split > 0 && p->rb_start + n + gap <= p->bwp_size, "fixture allocation outside BWP\n");
  const int ndmrs = count_bits64_with_mask(p->ul_dmrs_symb_pos, p->start_symbol_index, p->nr_of_symbols);
  const int pilots_per_rb = p->dmrs_config_type == 0 ? 6 : 4;
  const int G = nr_get_G(n, p->nr_of_symbols, pilots_per_rb * p->num_dmrs_cdm_grps_no_data, ndmrs, 0,
                        p->qam_mod_order, 1);
  int16_t *baseline = malloc(G * sizeof(*baseline));
  uint8_t *payload = malloc(p->pusch_data.tb_size);
  AssertFatal(baseline && payload, "fixture allocation failed\n");
  uint32_t *unav = &ulsch->unav_res;
  int id = 0;
  nr_rx_pusch_group_tp(gnb, &pv, &p, &unav, 1, frame, slot);
  memcpy(baseline, pv->llr, G * sizeof(*baseline));
  ulsch->harq_process->harq_to_be_cleared = true;
  const int base_rc = nr_ulsch_decoding(gnb, fp, frame, slot, &id, 1);
  AssertFatal(base_rc == 0 && ulsch->harq_process->C > 0
                  && ulsch->harq_process->processedSegments == ulsch->harq_process->C,
              "VOID: contiguous reference TB failed CRC\n");
  memcpy(payload, ulsch->harq_process->b, p->pusch_data.tb_size);

  uint16_t contiguous[n], mapped[n];
  for (int i = 0; i < n; ++i) {
    contiguous[i] = p->rb_start + i;
    mapped[i] = contiguous[i] + (i >= split ? gap : 0);
  }
  nr_rx_pusch_prb_list_tp(gnb, pv, p, unav, frame, slot, contiguous, n);
  const bool contiguous_equal = !memcmp(baseline, pv->llr, G * sizeof(*baseline));

  unsigned invalid_accepted = 0;
  invalid_accepted += nr_rx_pusch_prb_list_tp(gnb, pv, p, unav, frame, slot, NULL, n) != -1;
  invalid_accepted += nr_rx_pusch_prb_list_tp(gnb, pv, p, unav, frame, slot, contiguous, n - 1) != -1;
  uint16_t malformed[n];
  memcpy(malformed, contiguous, sizeof(malformed));
  malformed[1] = malformed[0];
  invalid_accepted += nr_rx_pusch_prb_list_tp(gnb, pv, p, unav, frame, slot, malformed, n) != -1;
  malformed[1] = p->bwp_size;
  invalid_accepted += nr_rx_pusch_prb_list_tp(gnb, pv, p, unav, frame, slot, malformed, n) != -1;
  const bool rejected_unchanged = !memcmp(baseline, pv->llr, G * sizeof(*baseline));
  /* The second trial also moves the BWP origin, independently of the PRB list. */
  nfapi_nr_pusch_pdu_t mapped_pdu = *p;
  const int bwp_shift = !collision && ra0_trials % 2 ? 3 : 0;
  mapped_pdu.bwp_start += bwp_shift;
  mapped_pdu.bwp_size -= bwp_shift;

  /* Move data into two unequal physical segments AFTER OFDM reception. Regenerate
   * the pilot phases at their physical PRB indices using an independent sequence.
   * Give the second segment a 90-degree channel phase: stale/overwritten H fails.
   * This is a frequency-grid unit fixture, not a claimed over-air transmission. */
  const int fft = fp->ofdm_symbol_size;
  const int off = (slot % RU_RX_SLOT_DEPTH) * fp->symbols_per_slot * fft;
  for (int sym = p->start_symbol_index; sym < p->start_symbol_index + p->nr_of_symbols; ++sym) {
    uint8_t gold[2 * 6 * 275];
    uint64_t seed = (UINT64_C(1) << 17) * (fp->symbols_per_slot * slot + sym + 1)
                    * (2 * p->ul_dmrs_scrambling_id + 1) + 2 * p->ul_dmrs_scrambling_id + p->scid;
    reference_gold(seed & 0x7fffffff, gold, sizeof(gold));
    for (int a = 0; a < p->param_v4.numSpatialStreamIndices; ++a) {
      c16_t *row = &gnb->common_vars.rxdataF[a][off + sym * fft];
      c16_t saved[fft];
      memcpy(saved, row, sizeof(saved));
      memset(row, 0, sizeof(saved));
      if (collision)
        for (int k = 0; k < fft; ++k)
          row[k] = (c16_t){.r = ((k * 73 + sym * 113) % 2001) - 1000,
                          .i = ((k * 137 + sym * 211) % 2003) - 1001};
      for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 12; ++k) {
          c16_t y = saved[(fp->first_carrier_offset + 12 * (p->bwp_start + contiguous[i]) + k) % fft];
          const bool pilot = p->dmrs_config_type == 0 ? !(k & 1) : k % 6 < 2;
          const int pilot_index = p->dmrs_config_type == 0 ? k / 2 : 2 * (k / 6) + k % 6;
          if ((p->ul_dmrs_symb_pos & (1 << sym)) && pilot) {
            const int old = 2 * (pilots_per_rb * (p->bwp_start + contiguous[i]) + pilot_index);
            const int now = 2 * (pilots_per_rb * (mapped_pdu.bwp_start + mapped[i]) + pilot_index);
            int ar = 1 - 2 * gold[old], ai = 1 - 2 * gold[old + 1];
            int br = 1 - 2 * gold[now], bi = 1 - 2 * gold[now + 1];
            int re = (ar * br + ai * bi) / 2, im = (ar * bi - ai * br) / 2;
            y = (c16_t){.r = y.r * re - y.i * im, .i = y.r * im + y.i * re};
          }
          if (i >= split) y = (c16_t){.r = -y.i, .i = y.r};
          row[(fp->first_carrier_offset + 12 * (mapped_pdu.bwp_start + mapped[i]) + k) % fft] = y;
        }
      }
    }
  }
  nr_rx_pusch_prb_list_tp(gnb, pv, &mapped_pdu, unav, frame, slot, mapped, n);
  unsigned sign_errors = 0;
  for (int i = 0; i < G; ++i)
    if (baseline[i] != 0 && (pv->llr[i] == 0 || ((baseline[i] < 0) != (pv->llr[i] < 0)))) ++sign_errors;
  ulsch->harq_process->harq_to_be_cleared = true;
  const int rc = nr_ulsch_decoding(gnb, fp, frame, slot, &id, 1);
  const bool crc_ok = rc == 0 && ulsch->harq_process->C > 0
                      && ulsch->harq_process->processedSegments == ulsch->harq_process->C;
  const bool bytes_ok = crc_ok && !memcmp(payload, ulsch->harq_process->b, p->pusch_data.tb_size);
  bool collision_ok = true;
  if (collision) {
    /* Same two allocations produced by the BWP106 / 14-bit FDRA parser test.
     * Here the callback returns actual LDPC/TB validity, never a geometric oracle. */
    nr_pdcch_blind_ul_result_t bundle = {.fdra_candidate_count = 2};
    bundle.fdra_candidates[0] = (nr_pusch_fdra_allocation_t){.start_rb=64, .num_rb=16,
      .ra_type0=1, .rbg_size=8, .mode=NR_FDRA_TYPE0_CFG1, .rbg_bitmap=0x22};
    bundle.fdra_candidates[1] = (nr_pusch_fdra_allocation_t){.start_rb=16, .num_rb=32,
      .ra_type0=1, .rbg_size=16, .mode=NR_FDRA_DYN_CFG2, .rbg_bitmap=0x22};
    collision_trial_t trial = {.gnb=gnb, .pdu=*p, .frame=frame, .slot=slot};
    const int winner = nr_pdcch_blind_ul_fdra_try(&bundle, collision_decode, &trial);
    ulsch->harq_process->ulsch_pdu = trial.pdu;
    collision_ok = winner == 1 && trial.calls == 2 && trial.first_rejected
                    && !memcmp(payload, ulsch->harq_process->b, trial.pdu.pusch_data.tb_size);
    printf("RA0_FDRA_COLLISION attempts=%d first_rejected=%d winner=%d payload=%d\n",
           trial.calls, trial.first_rejected, winner, collision_ok);
  }
  const bool pass = contiguous_equal && !invalid_accepted && rejected_unchanged && !sign_errors && bytes_ok && collision_ok;
  ++ra0_trials;
  if (!pass) ++ra0_failures;
  printf("RA0_GRID trial=%u bwp_shift=%d contiguous_equal=%d invalid_accepted=%u rejected_unchanged=%d sign_errors=%u/%d crc=%d payload=%d %s\n",
         ra0_trials, bwp_shift, contiguous_equal, invalid_accepted, rejected_unchanged, sign_errors, G, crc_ok, bytes_ok,
         pass ? "PASS" : "FAIL");
  free(baseline);
  free(payload);
  get_softmodem_params()->passive_rx = saved_passive;
  return crc_ok ? 0 : 1;
}

int main(int argc, char **argv)
{
  const int rc = ulsim_main(argc, argv);
  printf("RA0_GRID trials=%u failures=%u\n", ra0_trials, ra0_failures);
  return rc || !ra0_trials || ra0_failures ? 1 : 0;
}
