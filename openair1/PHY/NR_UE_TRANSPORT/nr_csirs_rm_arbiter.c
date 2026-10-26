/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_rm_arbiter.h"
#include <pthread.h>
#include <string.h>
#include "common/utils/LOG/log.h"

#define ARB_MAX 64

typedef struct {
  bool used;
  uint8_t csi_type, row, cdm_type, freq_density, symb_l0, symb_l1, hole_only, variant;
  uint16_t freq_domain, start_rb, nr_of_rbs, scramb_id;
  uint32_t with_wins, without_wins, trials;
  nr_csi_arb_verdict_t verdict;
} arb_t;

static arb_t g_arb[ARB_MAX];
static pthread_mutex_t g_arb_lock = PTHREAD_MUTEX_INITIALIZER;

static bool same(const arb_t *a, const fapi_nr_dl_config_csirs_pdu_rel15_t *p)
{
  return a->used && a->csi_type == p->csi_type && a->variant == p->last_trs_slot && a->row == p->row && a->cdm_type == p->cdm_type && a->freq_density == p->freq_density
         && a->symb_l0 == p->symb_l0 && a->symb_l1 == p->symb_l1 && a->freq_domain == p->freq_domain && a->start_rb == p->start_rb
         && a->nr_of_rbs == p->nr_of_rbs && a->scramb_id == p->scramb_id;
}

/* find: the entry of a pattern, or NULL (create = allocate one; a full table recycles the least-evidence UNDECIDED entry). Caller holds g_arb_lock. */
static arb_t *find(const fapi_nr_dl_config_csirs_pdu_rel15_t *p, bool create)
{
  arb_t *free_slot = NULL, *weak = NULL;
  for (int i = 0; i < ARB_MAX; ++i) {
    if (same(&g_arb[i], p))
      return &g_arb[i];
    if (!g_arb[i].used)
      free_slot = free_slot ? free_slot : &g_arb[i];
    else if (g_arb[i].verdict == NR_CSI_ARB_UNDECIDED && (!weak || g_arb[i].trials < weak->trials))
      weak = &g_arb[i];
  }
  if (!create)
    return NULL;
  arb_t *a = free_slot ? free_slot : weak;
  if (!a)
    return NULL;
  memset(a, 0, sizeof(*a));
  a->used = true;
  a->csi_type = p->csi_type;
  a->row = p->row;
  a->cdm_type = p->cdm_type;
  a->freq_density = p->freq_density;
  a->symb_l0 = p->symb_l0;
  a->symb_l1 = p->symb_l1;
  a->freq_domain = p->freq_domain;
  a->start_rb = p->start_rb;
  a->nr_of_rbs = p->nr_of_rbs;
  a->scramb_id = p->scramb_id;
  a->variant = p->last_trs_slot;
  a->hole_only = (p->measurement_bitmap & NR_CSIRS_PDU_HOLE_ONLY) != 0;
  return a;
}

static nr_csi_arb_verdict_t decide(const arb_t *a)
{
  if (a->with_wins >= NR_CSI_ARB_MIN_WINS && a->with_wins > 2 * a->without_wins)
    return NR_CSI_ARB_KEEP;
  if (a->without_wins >= NR_CSI_ARB_MIN_WINS && a->without_wins > 2 * a->with_wins)
    return NR_CSI_ARB_DROP;
  if (a->hole_only && a->trials >= NR_CSI_ARB_MAX_TRIALS && a->with_wins == 0)
    return NR_CSI_ARB_DROP;
  return NR_CSI_ARB_UNDECIDED;
}

void nr_csi_arb_reset(void)
{
  pthread_mutex_lock(&g_arb_lock);
  memset(g_arb, 0, sizeof(g_arb));
  pthread_mutex_unlock(&g_arb_lock);
}

nr_csi_arb_verdict_t nr_csi_arb_verdict(const fapi_nr_dl_config_csirs_pdu_rel15_t *p)
{
  pthread_mutex_lock(&g_arb_lock);
  const arb_t *a = find(p, false);
  const nr_csi_arb_verdict_t v = a ? a->verdict : NR_CSI_ARB_UNDECIDED;
  pthread_mutex_unlock(&g_arb_lock);
  return v;
}

bool nr_csi_arb_applies(const fapi_nr_dl_config_csirs_pdu_rel15_t *p)
{
  const nr_csi_arb_verdict_t v = nr_csi_arb_verdict(p);
  return (p->measurement_bitmap & NR_CSIRS_PDU_HOLE_ONLY) ? v == NR_CSI_ARB_KEEP : v != NR_CSI_ARB_DROP;
}

bool nr_csi_arb_trial_add(const fapi_nr_dl_config_csirs_pdu_rel15_t *p)
{
  return (p->measurement_bitmap & NR_CSIRS_PDU_HOLE_ONLY) && nr_csi_arb_verdict(p) == NR_CSI_ARB_UNDECIDED;
}

bool nr_csi_arb_trial_remove(const fapi_nr_dl_config_csirs_pdu_rel15_t *p)
{
  return nr_csi_arb_verdict(p) != NR_CSI_ARB_KEEP;
}

nr_csi_arb_verdict_t nr_csi_arb_note(const fapi_nr_dl_config_csirs_pdu_rel15_t *p, nr_csi_arb_outcome_t o)
{
  pthread_mutex_lock(&g_arb_lock);
  arb_t *a = find(p, true);
  nr_csi_arb_verdict_t v = NR_CSI_ARB_UNDECIDED;
  if (a) {
    const nr_csi_arb_verdict_t before = a->verdict;
    a->trials++;
    if (o == NR_CSI_ARB_WITH_WINS && a->with_wins < UINT32_MAX)
      a->with_wins++;
    else if (o == NR_CSI_ARB_WITHOUT_WINS && a->without_wins < UINT32_MAX)
      a->without_wins++;
    a->verdict = decide(a);
    v = a->verdict;
    if (o != NR_CSI_ARB_NO_EVIDENCE || v != before || (a->trials & (a->trials - 1)) == 0)
      LOG_A(PHY, "SENSING: CSIRS_ARBITER %s lattice_variant=%u row=%u fd=%u l0=%u l1=%u cdm=%u dens=%u rb=%u:%u id=%u type=%s outcome=%s with_wins=%u without_wins=%u trials=%u verdict=%s%s\n",
            a->hole_only ? "HOLES" : "SEQ", a->variant, a->row, a->freq_domain, a->symb_l0, a->symb_l1, a->cdm_type, a->freq_density, a->start_rb, a->nr_of_rbs,
            a->scramb_id, a->csi_type == 2 ? "ZP" : "NZP",
            o == NR_CSI_ARB_WITH_WINS ? "with-passes-without-fails" : o == NR_CSI_ARB_WITHOUT_WINS ? "without-passes-with-fails" : "both-fail",
            a->with_wins, a->without_wins, a->trials, v == NR_CSI_ARB_KEEP ? "KEEP" : v == NR_CSI_ARB_DROP ? "DROP" : "UNDECIDED",
            v != before ? " CHANGED" : "");
  }
  pthread_mutex_unlock(&g_arb_lock);
  return v;
}

void nr_csi_arb_counts(const fapi_nr_dl_config_csirs_pdu_rel15_t *p, uint32_t *with_wins, uint32_t *without_wins, uint32_t *trials)
{
  pthread_mutex_lock(&g_arb_lock);
  const arb_t *a = find(p, false);
  if (with_wins) *with_wins = a ? a->with_wins : 0;
  if (without_wins) *without_wins = a ? a->without_wins : 0;
  if (trials) *trials = a ? a->trials : 0;
  pthread_mutex_unlock(&g_arb_lock);
}
