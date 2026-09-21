/* SIB1-derived PDCCH prior -- storage and the one derivation consumers need.
 * Rationale and scope: see nr_pdcch_sib1_prior.h. */

#include "PHY/NR_UE_TRANSPORT/nr_pdcch_sib1_prior.h"

#include <string.h>

#include "common/utils/LOG/log.h"

static nr_pdcch_sib1_prior_t g_prior; /* .valid == false until MAC publishes one */

void nr_pdcch_sib1_prior_set(const nr_pdcch_sib1_prior_t *p)
{
  if (p == NULL) {
    return;
  }
  g_prior = *p;
  g_prior.valid = true;

  /* Print it ONCE, in full. The blind search's own ladder log exists for exactly this reason: a
   * scan that silently covers the wrong geometry finds nothing and reports no error. If this line
   * disagrees with what the search is testing, that is the bug, and it should be visible at
   * start-up rather than as an empty accept census an hour later. */
  static bool logged = false;
  if (!logged) {
    logged = true;
    int first_w = -1, last_w = -1;
    (void)nr_pdcch_sib1_prior_window(&first_w, &last_w);
    LOG_A(PHY,
          "SENSING: SIB1 PRIOR: bwp=%u+%u coreset%s[id=%u dur=%u %s bundle=%u interleaver=%u "
          "shift=%u dmrs_id=%u win=%d..%d] ss%s[AL1=%u AL2=%u AL4=%u AL8=%u AL16=%u period=%u "
          "offset=%u dur=%u] ra_ss=%d sib1_ss=%d paging_ss=%d rach%s[prach_idx=%u msg1_fdm=%u msg1_fstart=%u sul=%d]\n",
          g_prior.dl_bwp_valid ? g_prior.dl_bwp_start : 0,
          g_prior.dl_bwp_valid ? g_prior.dl_bwp_size : 0,
          g_prior.coreset_valid ? "" : "(none)",
          g_prior.coreset_id,
          g_prior.duration,
          g_prior.interleaved ? "interleaved" : "non-interleaved",
          g_prior.reg_bundle_size,
          g_prior.interleaver_size,
          g_prior.shift_index,
          g_prior.pdcch_dmrs_scrambling_id,
          first_w,
          last_w,
          g_prior.ss_valid ? "" : "(none)",
          g_prior.al_candidates[0],
          g_prior.al_candidates[1],
          g_prior.al_candidates[2],
          g_prior.al_candidates[3],
          g_prior.al_candidates[4],
          g_prior.ss_period_slots,
          g_prior.ss_offset_slots,
          g_prior.ss_duration,
          g_prior.ra_ss_valid ? (int)g_prior.ra_ss_id : -1,
          g_prior.sib1_ss_valid ? (int)g_prior.sib1_ss_id : -1,
          g_prior.paging_ss_valid ? (int)g_prior.paging_ss_id : -1,
          g_prior.rach_valid ? "" : "(none)",
          g_prior.prach_config_index,
          g_prior.msg1_fdm,
          g_prior.msg1_frequency_start,
          g_prior.sul_present ? 1 : 0,
          g_prior.ra_ss_period,
          g_prior.ra_ss_offset,
          g_prior.ra_ss_duration);
  }
}

const nr_pdcch_sib1_prior_t *nr_pdcch_sib1_prior_get(void)
{
  return g_prior.valid ? &g_prior : NULL;
}

bool nr_pdcch_sib1_prior_window(int *first_w, int *last_w)
{
  if (first_w == NULL || last_w == NULL) {
    return false;
  }
  *first_w = -1;
  *last_w = -1;
  if (!g_prior.valid || !g_prior.coreset_valid) {
    return false;
  }
  /* frequencyDomainResources is a 45-bit bitmap, MSB first: bit i (from the MSB of byte 0) marks
   * the i-th group of 6 PRBs. That is the same "window" unit the blind extent search uses, which
   * is why this lives here rather than in the consumer. */
  for (int w = 0; w < 45; w++) {
    const int byte = w / 8;
    const int bit = 7 - (w % 8);
    if ((g_prior.frequency_domain_resource[byte] >> bit) & 1) {
      if (*first_w < 0) {
        *first_w = w;
      }
      *last_w = w;
    }
  }
  return (*first_w >= 0);
}

bool nr_pdcch_sib1_prior_ra_rnti_valid(uint16_t rnti)
{
  if (!g_prior.valid || !g_prior.rach_valid || rnti == 0) {
    return false;
  }
  /* Invert nr_mac_common.c:5118's construction:
   *     ra_rnti = 1 + s_id + 14*t_id + 1120*f_id + 8960*ul_carrier_id
   * s_id  = PRACH start symbol            0..13
   * t_id  = first slot of the PRACH occasion 0..79
   * f_id  = the FD occasion index         0..7
   * ul    = 0 (NUL) or 1 (SUL)
   * Every RA-RNTI has exactly ONE decomposition, so this is a decode, not a search. */
  const uint32_t v = (uint32_t)rnti - 1u;
  const uint32_t s_id = v % 14u;
  const uint32_t t_id = (v / 14u) % 80u;
  const uint32_t f_id = (v / 1120u) % 8u;
  const uint32_t ul_carrier_id = v / 8960u;

  if (ul_carrier_id > 1u) {
    return false; /* outside the constructible range entirely */
  }
  /* A cell with no supplementary uplink can never emit ul_carrier_id = 1. On its own this halves
   * the admissible set; msg1-FDM below is the far bigger cut. */
  if (ul_carrier_id == 1u && !g_prior.sul_present) {
    return false;
  }
  /* msg1-FDM is the number of frequency-multiplexed PRACH occasions: 1, 2, 4 or 8. The default in
   * every deployment this project has seen is ONE, which pins f_id to 0 and removes 7/8 of the
   * space by itself. */
  if (g_prior.msg1_fdm > 0 && f_id >= (uint32_t)g_prior.msg1_fdm) {
    return false;
  }
  /* s_id and t_id are bounded by construction (the modulo above), and tightening them further needs
   * the PRACH configuration table (TS 38.211 6.3.3.2-2/3) to say which slots and start symbols the
   * chosen prach-ConfigurationIndex actually uses. That table is NOT consulted here -- deliberately,
   * because a wrong table lookup would REJECT genuine RA-RNTIs, which is far worse than admitting a
   * few extra. Left as the obvious next tightening, with prach_config_index already captured. */
  (void)s_id;
  (void)t_id;
  return true;
}
