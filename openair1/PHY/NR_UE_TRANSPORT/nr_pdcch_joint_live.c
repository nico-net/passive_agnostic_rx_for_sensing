#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "common/utils/LOG/log.h"
#include "nr_pdcch_joint_live.h"
#include "nr_pdcch_joint_solve.h"

typedef struct { uint16_t A; uint8_t L; } joint_enc_ctx_t;
static int joint_encode_cb(void *vctx, uint64_t payload, uint16_t crc_mask, uint8_t *coded, int E)
{
  const joint_enc_ctx_t *c = (const joint_enc_ctx_t *)vctx;
  uint32_t out[(NR_PDCCH_JOINT_MAX_E + 31) / 32 + 1] = {0};
  polar_encoder_fast(&payload, out, (int32_t)crc_mask, /*ones_flag=*/1, NR_POLAR_DCI_MESSAGE_TYPE, c->A, c->L);
  nr_bit2byte_uint32_8(out, E, coded);
  return 0;
}

#define JOINT_CACHE_N 32
static struct { uint16_t A, nid; uint8_t L; int pre; nr_pdcch_joint_model_t *m; } s_cache[JOINT_CACHE_N];
static int s_cache_n;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long long s_attempts, s_screen_pass, s_accepted;

bool nr_pdcch_joint_live_enabled(void)
{
  static int en = -1;
  if (en < 0) {
    const char *e = getenv("ISAC_PDCCH_JOINT");
    en = (e != NULL && atoi(e) != 0) ? 1 : 0;
  }
  return en == 1;
}

void nr_pdcch_joint_live_counts(unsigned long long *a, unsigned long long *p, unsigned long long *k)
{
  if (a) *a = atomic_load(&s_attempts);
  if (p) *p = atomic_load(&s_screen_pass);
  if (k) *k = atomic_load(&s_accepted);
}

/* One model per (DCI length, AL, nID, pre-descrambling RNTI); a failed build is cached as NULL so it is not retried
 * on every candidate. Models are read-only once built, so solving needs no lock. */
static nr_pdcch_joint_model_t *model_get(uint16_t A, uint8_t L, uint16_t nid, int pre, int E)
{
  pthread_mutex_lock(&s_mu);
  for (int i = 0; i < s_cache_n; i++)
    if (s_cache[i].A == A && s_cache[i].L == L && s_cache[i].nid == nid && s_cache[i].pre == pre) {
      nr_pdcch_joint_model_t *m = s_cache[i].m;
      pthread_mutex_unlock(&s_mu);
      return m;
    }
  if (s_cache_n >= JOINT_CACHE_N) { /* full: the fallback is off for new shapes rather than rebuilding per candidate */
    pthread_mutex_unlock(&s_mu);
    return NULL;
  }
  joint_enc_ctx_t ctx = {A, L};
  nr_pdcch_joint_model_t *m = nr_pdcch_joint_model_new_ex(joint_encode_cb, &ctx, A, E, nid, 1, pre);
  s_cache[s_cache_n].A = A;
  s_cache[s_cache_n].L = L;
  s_cache[s_cache_n].nid = nid;
  s_cache[s_cache_n].pre = pre;
  s_cache[s_cache_n++].m = m;
  if (m == NULL)
    LOG_W(PHY, "SENSING: JOINT_RNTI model build failed (len=%u AL=%u nid=%u) -- fallback off for this shape\n", A, L, nid);
  pthread_mutex_unlock(&s_mu);
  return m;
}

bool nr_pdcch_joint_live_decode_11(const int16_t *llr, uint8_t aggregation_level, uint16_t dci_length, uint16_t nid,
                                   int pre_descrambled_rnti, uint16_t rnti_min, uint16_t rnti_max,
                                   nr_pdcch_joint_live_result_t *out)
{
  return nr_pdcch_joint_live_decode(llr, aggregation_level, dci_length, nid, pre_descrambled_rnti, rnti_min, rnti_max,
                                    1, out);
}

bool nr_pdcch_joint_live_decode(const int16_t *llr, uint8_t aggregation_level, uint16_t dci_length, uint16_t nid,
                                int pre_descrambled_rnti, uint16_t rnti_min, uint16_t rnti_max, int indicator,
                                nr_pdcch_joint_live_result_t *out)
{
  if (!nr_pdcch_joint_live_enabled() || out == NULL || dci_length == 0 || dci_length > NR_PDCCH_JOINT_MAX_A
      || (aggregation_level != 1 && aggregation_level != 2 && aggregation_level != 4 && aggregation_level != 8
          && aggregation_level != 16))
    return false;
  const int E = aggregation_level * 108;
  out->reject_reason = NULL;
  atomic_fetch_add(&s_attempts, 1);
  if (!nr_pdcch_joint_prescreen(llr, E))
    return false;
  atomic_fetch_add(&s_screen_pass, 1);
  nr_pdcch_joint_model_t *m = model_get(dci_length, aggregation_level, nid, pre_descrambled_rnti, E);
  if (m == NULL)
    return false;
  nr_pdcch_joint_result_t r;
  if (!nr_pdcch_joint_solve(m, llr, /*order=*/0, &r)) /* order 0: cheapest, and measured no worse at AL1 */
    return false;
  out->payload = r.payload;
  out->rnti = r.rnti;
  out->mismatched_bits = (uint16_t)r.mismatched_bits;
  if (r.rnti < rnti_min || r.rnti > rnti_max) {
    out->reject_reason = "joint solve: RNTI outside plausible range";
    return false;
  }
  if ((int)((r.payload >> (dci_length - 1)) & 1) != (indicator ? 1 : 0)) { /* the admission raw_11 / raw_01 apply */
    out->reject_reason = indicator ? "joint solve: format indicator=0 (UL grant, not DL)"
                                   : "joint solve: format indicator=1 (DL assignment, not an UL grant)";
    return false;
  }
  atomic_fetch_add(&s_accepted, 1);
  return true;
}
