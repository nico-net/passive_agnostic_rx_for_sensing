#include "nr_pdcch_uss_prune.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include <pthread.h>
#include <string.h>

#define USSH_SAMPLES 256
#define USSH_GEOMS 8
#define USSH_DEMOTED 16
#define USSH_REPROMOTE_HITS 2 /* consecutive in-set DCIs that return a demoted RNTI to pruning */
#define USSH_RESET_MISSES 8   /* misses (no in-set DCI between) after which the geometry's fit is thrown away and re-learned */

typedef struct {
  uint64_t geom;
  int n, nh;
  int notes_since_fit; /* the table rotates once full, so n alone cannot pace the refit */
  int miss_run;
  uint16_t rnti[USSH_SAMPLES];
  uint8_t slot[USSH_SAMPLES], cce[USSH_SAMPLES], L[USSH_SAMPLES];
  struct { uint8_t N, a, M[5]; } h[4]; /* fitting hypotheses */
  struct { uint16_t rnti; uint8_t streak; } dem[USSH_DEMOTED];
  int next_dem;
} ussh_t;
typedef ussh_t g_ussh_cell_t[USSH_GEOMS];
NR_CELL_STATIC(g_ussh_cell_t, g_ussh);
#define g_ussh NR_CELL_REF(g_ussh)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_ussh_mu, PTHREAD_MUTEX_INITIALIZER);
#define g_ussh_mu NR_CELL_REF(g_ussh_mu)
NR_CELL_STATIC(nr_uss_prune_stats_t, g_ussh_st);
#define g_ussh_st NR_CELL_REF(g_ussh_st)
NR_CELL_STATIC(int, s_rot);
#define s_rot NR_CELL_REF(s_rot)
static const uint32_t kUsshA[3] = {39827, 39829, 39839};

uint32_t nr_uss_hash_Y(uint16_t rnti, int slot, uint32_t A)
{
  uint32_t y = rnti;
  for (int k = 0; k <= slot; k++)
    y = (uint32_t)(((uint64_t)A * y) % 65537u);
  return y;
}

int nr_uss_hash_positions(uint16_t rnti, int slot, int L, int N, uint32_t A, int M, uint16_t *cce_out)
{
  const int nl = N / L;
  if (nl <= 0 || M <= 0)
    return 0;
  const uint32_t y = nr_uss_hash_Y(rnti, slot, A);
  int n = 0;
  for (int m = 0; m < M; m++) {
    const uint16_t c = (uint16_t)(L * (int)((y + (uint32_t)((m * N) / (L * M))) % (uint32_t)nl));
    bool dup = false;
    for (int k = 0; k < n; k++)
      dup |= cce_out[k] == c;
    if (!dup && n < NR_USS_PRUNE_MAX_POS)
      cce_out[n++] = c;
  }
  return n;
}

bool nr_uss_hash_in(uint16_t rnti, int slot, int cce, int L, int N, uint32_t A, int M)
{
  const int nl = N / L;
  if (nl <= 0 || M <= 0)
    return false;
  const uint32_t y = nr_uss_hash_Y(rnti, slot, A);
  for (int m = 0; m < M; m++)
    if (L * (int)((y + (uint32_t)((m * N) / (L * M))) % (uint32_t)nl) == cce)
      return true;
  return false;
}

static int al_idx(int L) { return L == 1 ? 0 : L == 2 ? 1 : L == 4 ? 2 : L == 8 ? 3 : 4; }

/* Fits (N_CCE, A, M_L per AL) to the samples: N from ncce_max down to 6, the three A values, up to 4 hypotheses whose M sums stay <= 12. */
static void ussh_fit(ussh_t *u, int ncce_max)
{
  u->nh = 0;
  for (int N = ncce_max; N >= 6 && u->nh < 4; N--)
    for (int a = 0; a < 3 && u->nh < 4; a++) {
      uint8_t M[5] = {0};
      bool ok = true;
      for (int i = 0; i < u->n && ok; i++) {
        const int ai = al_idx(u->L[i]);
        int m = M[ai] ? M[ai] : 1;
        while (m <= 8 && !nr_uss_hash_in(u->rnti[i], u->slot[i], u->cce[i], u->L[i], N, kUsshA[a], m))
          m++;
        if (m > 8)
          ok = false;
        else if (m > M[ai]) {
          /* a larger M moves earlier samples' positions: re-check all of this AL */
          M[ai] = (uint8_t)m;
          for (int j = 0; j < i && ok; j++)
            if (u->L[j] == u->L[i] && !nr_uss_hash_in(u->rnti[j], u->slot[j], u->cce[j], u->L[j], N, kUsshA[a], m))
              ok = false;
        }
      }
      int cover = 0;
      for (int k = 0; k < 5; k++)
        cover += M[k];
      if (ok && cover > 0 && cover <= 12) {
        u->h[u->nh].N = (uint8_t)N;
        u->h[u->nh].a = (uint8_t)a;
        memcpy(u->h[u->nh].M, M, 5);
        u->nh++;
      }
    }
  u->notes_since_fit = 0;
  if (u->nh > 0) {
    g_ussh_st.fits++;
    LOG_A(PHY, "SENSING: USS_HASH fitted geom=%llu on %d DCIs: %d hypotheses, first N_CCE=%u p%%3=%u M=[%u %u %u %u %u]\n",
          (unsigned long long)u->geom, u->n, u->nh, u->h[0].N, u->h[0].a, u->h[0].M[0], u->h[0].M[1], u->h[0].M[2],
          u->h[0].M[3], u->h[0].M[4]);
  }
}

/* True when (rnti, slot, cce, L) lies on the hashed set of some current hypothesis. An AL never seen in the fit (M = 0) may still be
 * configured and its m = 0 position does not depend on M_L, so it is admitted at M = 1. Needs u->nh > 0. */
static bool ussh_fit_has(const ussh_t *u, uint16_t rnti, int slot, int cce, int L)
{
  for (int h = 0; h < u->nh; h++) {
    const int M = u->h[h].M[al_idx(L)];
    if (nr_uss_hash_in(rnti, slot, cce, L, u->h[h].N, kUsshA[u->h[h].a], M > 0 ? M : 1))
      return true;
  }
  return false;
}

static ussh_t *ussh_find(uint64_t geom)
{
  for (int i = 0; i < USSH_GEOMS; i++)
    if (g_ussh[i].geom == geom && g_ussh[i].n)
      return &g_ussh[i];
  return NULL;
}

static int dem_find(const ussh_t *u, uint16_t rnti)
{
  for (int i = 0; i < USSH_DEMOTED; i++)
    if (u->dem[i].rnti == rnti)
      return i;
  return -1;
}

bool nr_uss_prune_audit_slot(uint32_t abs_slot)
{
  return ((((uint32_t)abs_slot * 0x9E3779B1u) >> 20) % 16u) == 3u;
}

static bool admit_locked(uint64_t geom, uint16_t rnti, int slot, int cce, int L)
{
  const ussh_t *u = ussh_find(geom);
  if (!u || u->nh == 0 || dem_find(u, rnti) >= 0)
    return true;
  return ussh_fit_has(u, rnti, slot, cce, L);
}

bool nr_uss_prune_allows(uint64_t geom, uint16_t rnti, int slot, int cce, int L)
{
  pthread_mutex_lock(&g_ussh_mu);
  const bool allow = admit_locked(geom, rnti, slot, cce, L);
  pthread_mutex_unlock(&g_ussh_mu);
  return allow;
}

bool nr_uss_prune_admit(uint64_t geom, uint16_t rnti, int slot, int cce, int L, bool audit)
{
  pthread_mutex_lock(&g_ussh_mu);
  bool allow = admit_locked(geom, rnti, slot, cce, L);
  if (!allow) {
    if (audit) {
      g_ussh_st.audit_tasks++;
      allow = true;
    } else {
      g_ussh_st.pruned++;
    }
  }
  pthread_mutex_unlock(&g_ussh_mu);
  return allow;
}

bool nr_uss_prune_note(uint64_t geom, uint16_t rnti, int slot, int cce, int L, int ncce_max, bool force)
{
  bool kept = true;
  pthread_mutex_lock(&g_ussh_mu);
  ussh_t *u = ussh_find(geom);
  for (int i = 0; i < USSH_GEOMS && !u; i++)
    if (!g_ussh[i].n) {
      u = &g_ussh[i];
      u->geom = geom;
    }
  if (u) {
    const ussh_t before = *u;
    /* A DL DCI of an RNTI outside its hashed set falsifies the hypothesis for that RNTI. If a refit that includes it still has a hypothesis
     * (M was under-fitted: audited DCIs at m > 0 raise it) the fit widens; otherwise the sample is not fitted and the RNTI goes back to full
     * search. Misses with no in-set DCI between them eventually mean the cell's search space changed: the fit is dropped and re-learned. */
    const bool miss = !force && u->nh > 0 && !ussh_fit_has(u, rnti, slot, cce, L);
    if (miss)
      g_ussh_st.audit_miss++;
    const int di = dem_find(u, rnti);
    if (!force && u->nh > 0 && !miss) {
      u->miss_run = 0;
      if (di >= 0 && ++u->dem[di].streak >= USSH_REPROMOTE_HITS) {
        u->dem[di].rnti = 0;
        g_ussh_st.rnti_repromotions++;
      }
    } else if (di >= 0) {
      u->dem[di].streak = 0;
    }
    /* force (UL DCI sharing the USS) or a miss: enter the sample and refit now; a full table overwrites a rotating entry. */
    const bool full = u->n >= USSH_SAMPLES;
    const int i = full ? (s_rot++ % USSH_SAMPLES) : u->n;
    if (!full)
      u->n++;
    u->notes_since_fit++;
    u->rnti[i] = rnti;
    u->slot[i] = (uint8_t)slot;
    u->cce[i] = (uint8_t)cce;
    u->L[i] = (uint8_t)L;
    /* The fit only lower-bounds M_L (the smallest M that places every sample): a full table keeps rotating and refitting, so audited DCIs at
     * m > 0 can still raise M. */
    if (force || miss || (u->n >= 24 && (u->notes_since_fit >= 16 || (u->nh == 0 && u->notes_since_fit >= 8)))) {
      ussh_fit(u, ncce_max);
      if (miss && u->nh > 0)
        u->miss_run = 0; /* the fit widened to explain it */
      if ((force || miss) && u->nh == 0 && before.nh > 0) {
        const int keep_miss = before.miss_run;
        *u = before;
        kept = false;
        if (miss) {
          u->miss_run = keep_miss + 1;
          if (dem_find(u, rnti) < 0) {
            u->dem[u->next_dem % USSH_DEMOTED].rnti = rnti;
            u->dem[u->next_dem % USSH_DEMOTED].streak = 0;
            u->next_dem = (u->next_dem + 1) % USSH_DEMOTED;
            g_ussh_st.rnti_demotions++;
          }
          if (u->miss_run >= USSH_RESET_MISSES) {
            const uint64_t g = u->geom;
            memset(u, 0, sizeof(*u));
            u->geom = g;
            g_ussh_st.geom_resets++;
            LOG_A(PHY, "SENSING: USS_HASH reset geom=%llu after %d out-of-set DCIs\n", (unsigned long long)g, USSH_RESET_MISSES);
          }
        }
      }
    }
  }
  pthread_mutex_unlock(&g_ussh_mu);
  return kept;
}

void nr_uss_prune_stats(nr_uss_prune_stats_t *out)
{
  pthread_mutex_lock(&g_ussh_mu);
  *out = g_ussh_st;
  pthread_mutex_unlock(&g_ussh_mu);
}

int nr_uss_prune_hypotheses(uint64_t geom)
{
  pthread_mutex_lock(&g_ussh_mu);
  const ussh_t *u = ussh_find(geom);
  const int n = u ? u->nh : 0;
  pthread_mutex_unlock(&g_ussh_mu);
  return n;
}

void nr_uss_prune_reset(void)
{
  pthread_mutex_lock(&g_ussh_mu);
  memset(g_ussh, 0, sizeof(g_ussh));
  memset(&g_ussh_st, 0, sizeof(g_ussh_st));
  s_rot = 0;
  pthread_mutex_unlock(&g_ussh_mu);
}
