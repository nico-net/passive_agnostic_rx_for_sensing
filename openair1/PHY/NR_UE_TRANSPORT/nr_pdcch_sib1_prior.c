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
          "offset=%u dur=%u] ra_ss=%d sib1_ss=%d paging_ss=%d\n",
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
          g_prior.paging_ss_valid ? (int)g_prior.paging_ss_id : -1);
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
