/* Blind UCI-on-PUSCH demultiplexing -- see nr_pusch_uci_blind.h.
 *
 * The mapping is TS 38.212 6.2.7 (steps 1, 3, 4, 5, 7) as written: every step walks the OFDM symbols from its first
 * symbol, takes the REs still eligible for it in increasing subcarrier order (phi), and selects phi(j * d) with
 * d = floor(M / R) when the R REs still to place are fewer than the M eligible ones (all of them otherwise):
 *   step 1  REs reserved for HARQ-ACK of 0..2 bits (computed with O_ACK = 2), from the first symbol after the first DM-RS
 *           group; they stay eligible for CSI part 2 and the UL-SCH;
 *   step 3  rate-matched HARQ-ACK (> 2 bits), same first symbol;
 *   step 4  CSI part 1 from the first non-DM-RS symbol, on REs neither used nor reserved;
 *   step 5  CSI part 2 from the first non-DM-RS symbol, on REs not used (reserved ones included);
 *   step 7  HARQ-ACK of 1..2 bits punctures the reserved REs (an UL-SCH RE there keeps its place, its LLRs are erased).
 * The counts are 6.3.2.4.1.1-3 with L the UCI CRC length (6.3.1.2.1) and sum K_r = C * K. */
#include "nr_pusch_uci_blind.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "PHY/CODING/coding_defs.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "nr_pdsch_passive_decode.h" /* nr_pdsch_gw_cb0_t, nr_pdsch_passive_gw_cb_decode_iter */
#include "nr_td_grantwork.h"         /* nr_td_gw_cb0_extract */
#include "common/utils/nr_cell.h"

/* TS 38.213 Table 9.3-1 (HARQ-ACK) and 9.3-2 (CSI) beta offsets; TS 38.331 alpha (scaling). */
static const double k_beta_ack[16] = {1.000, 2.000, 2.500, 3.125, 4.000, 5.000, 6.250, 8.000,
                                      10.000, 12.625, 15.875, 20.000, 31.000, 50.000, 80.000, 126.000};
static const double k_beta_csi[19] = {1.125, 1.250, 1.375, 1.625, 1.750, 2.000, 2.250, 2.500, 2.875, 3.125,
                                      3.500, 4.000, 5.000, 6.250, 8.000, 10.000, 12.625, 15.875, 20.000};
static const double k_alpha[4] = {0.5, 0.65, 0.8, 1.0};

/* Search space bounds. HARQ-ACK of a dynamic codebook on one carrier is one bit per PDSCH (two per two-codeword PDSCH):
 * the slots a UL slot acknowledges bound it; 40 covers a semi-static codebook of the largest K1 sets as well. CSI part 1
 * (CRI/RI/LI/wideband CQI) stays below 32 bits, part 2 (PMI, subband CQI) below 64 for the type-I reports. */
#define UCI_MAX_O_ACK 40
#define UCI_MAX_O_CSI1 32
#define UCI_MAX_O_CSI2 64
#define UCI_SCREEN_ITER 6 /* LDPC iterations of a single-block screen: a right hypothesis at the SNR a TB needs converges in 1-3 */

#define T_DATA 0
#define T_ACK 1
#define T_CSI1 2
#define T_CSI2 3
#define T_PUNCT 4
#define T_RVD 0x80 /* flag: reserved for HARQ-ACK of <= 2 bits */

/* ------------------------------------------------------------------ geometry and counts */
static inline bool is_dmrs(const nr_uci_geom_t *g, int l)
{
  return (g->dmrs_pos >> l) & 1;
}

static inline uint32_t m_uci(const nr_uci_geom_t *g, int l)
{
  return is_dmrs(g, l) ? 0 : (uint32_t)g->rb * 12u;
}

static inline uint32_t m_data(const nr_uci_geom_t *g, int l)
{
  return is_dmrs(g, l) ? (uint32_t)g->rb * g->data_re_dmrs_rb : (uint32_t)g->rb * 12u;
}

uint32_t nr_uci_data_re(const nr_uci_geom_t *g)
{
  uint32_t n = 0;
  for (int l = g->start_sym; l < g->start_sym + g->nsym; l++)
    n += m_data(g, l);
  return n;
}

/* first non-DM-RS symbol (CSI), first symbol after the first DM-RS group (HARQ-ACK); -1 if none */
static void first_symbols(const nr_uci_geom_t *g, int *l_csi, int *l_ack)
{
  const int end = g->start_sym + g->nsym;
  *l_csi = -1;
  *l_ack = -1;
  for (int l = g->start_sym; l < end; l++)
    if (!is_dmrs(g, l)) {
      *l_csi = l;
      break;
    }
  int l = g->start_sym;
  while (l < end && !is_dmrs(g, l))
    l++;
  while (l < end && is_dmrs(g, l))
    l++;
  if (l < end)
    *l_ack = l;
}

static void sums(const nr_uci_geom_t *g, uint32_t *s1, uint32_t *s2)
{
  int lc, la;
  first_symbols(g, &lc, &la);
  *s1 = 0;
  *s2 = 0;
  for (int l = g->start_sym; l < g->start_sym + g->nsym; l++) {
    *s1 += m_uci(g, l);
    if (la >= 0 && l >= la)
      *s2 += m_uci(g, l);
  }
}

static inline int crc_len(unsigned o)
{
  return o > 19 ? 11 : (o > 11 ? 6 : 0);
}

/* Q' = min(ceil((O + L) beta s1 / sumK), ceil(alpha s2) - sub), 0 for O = 0 */
static uint32_t q_count(unsigned o, double beta, double alpha, uint32_t sum_k, uint32_t s1, uint32_t s2, uint32_t sub)
{
  if (o == 0 || sum_k == 0)
    return 0;
  const double first = ceil(((double)o + crc_len(o)) * beta * (double)s1 / (double)sum_k);
  const double cap = ceil(alpha * (double)s2) - (double)sub;
  const double q = first < cap ? first : cap;
  return q > 0 ? (uint32_t)q : 0;
}

bool nr_uci_counts(const nr_uci_geom_t *g, const nr_uci_hyp_t *h, nr_uci_counts_t *c)
{
  memset(c, 0, sizeof(*c));
  if ((h->o_ack == 0 && h->o_csi1 == 0) || h->b_ack > 15 || h->b_csi1 > 18 || h->b_csi2 > 18 || h->alpha > 3 || g->sum_k == 0)
    return false;
  uint32_t s1, s2;
  sums(g, &s1, &s2);
  const double a = k_alpha[h->alpha];
  const uint32_t qa = q_count(h->o_ack <= 2 ? 2 : h->o_ack, k_beta_ack[h->b_ack], a, g->sum_k, s1, s2, 0);
  if (h->o_ack <= 2) {
    c->q_rvd = qa;
    c->q_ack = q_count(h->o_ack, k_beta_ack[h->b_ack], a, g->sum_k, s1, s2, 0);
  } else {
    c->q_ack = qa;
  }
  c->q_csi1 = q_count(h->o_csi1, k_beta_csi[h->b_csi1], a, g->sum_k, s1, s1, qa);
  c->q_csi2 = h->o_csi1 ? q_count(h->o_csi2, k_beta_csi[h->b_csi2], a, g->sum_k, s1, s1, qa + c->q_csi1) : 0;
  c->u_re = (h->o_ack > 2 ? c->q_ack : 0) + c->q_csi1 + c->q_csi2;
  return c->u_re < nr_uci_data_re(g);
}

/* ------------------------------------------------------------------ mapping (TS 38.212 6.2.7) */
typedef struct {
  uint8_t *t[14];  /* per non-DM-RS symbol: RE type (T_*) | T_RVD, rb * 12 entries; NULL = untouched */
  uint16_t touched;
} uci_map_t;

static __thread uint8_t *t_mapbuf;
static __thread uint32_t t_mapcap;
static __thread uint16_t *t_phi;

static bool map_alloc(const nr_uci_geom_t *g, uci_map_t *m)
{
  const uint32_t M = (uint32_t)g->rb * 12u, need = 14u * M;
  if (t_mapcap < need) {
    free(t_mapbuf);
    free(t_phi);
    t_mapbuf = malloc(need);
    t_phi = malloc((size_t)M * sizeof(*t_phi));
    t_mapcap = (t_mapbuf && t_phi) ? need : 0;
    if (!t_mapcap)
      return false;
  }
  memset(m, 0, sizeof(*m));
  for (int l = 0; l < 14; l++)
    m->t[l] = t_mapbuf + (size_t)l * M;
  return true;
}

static inline void touch(uci_map_t *m, uint32_t M, int l)
{
  if (!(m->touched >> l & 1)) {
    memset(m->t[l], T_DATA, M);
    m->touched |= (uint16_t)(1u << l);
  }
}

enum { ELIG_RVD_STEP, ELIG_ACK, ELIG_CSI1, ELIG_CSI2, ELIG_PUNCT, ELIG_CSI2_RAW };

/* One placement step: R REs from symbol l0 on. Returns false if they do not fit. */
static bool place(const nr_uci_geom_t *g, uci_map_t *m, int l0, uint32_t R, int kind)
{
  if (R == 0)
    return true;
  if (l0 < 0)
    return false;
  const uint32_t M = (uint32_t)g->rb * 12u;
  for (int l = l0; l < g->start_sym + g->nsym && R > 0; l++) {
    if (m_uci(g, l) == 0)
      continue;
    if (kind == ELIG_PUNCT && !(m->touched >> l & 1))
      continue; /* no reserved RE here */
    touch(m, M, l);
    uint8_t *t = m->t[l];
    if (kind == ELIG_CSI2_RAW) {
      /* the alternative CSI part 2 placement (nr_uci_hyp_t.csi2_map = 1): d from the full symbol, raw RE walk */
      const uint32_t d = R >= M ? 1 : M / R;
      for (uint32_t k = 0; k < M && R > 0;) {
        if ((t[k] & 0x7f) != T_DATA) {
          k++;
          continue;
        }
        t[k] = (uint8_t)(T_CSI2 | (t[k] & T_RVD));
        R--;
        k += d;
      }
      continue;
    }
    uint32_t n = 0;
    for (uint32_t k = 0; k < M; k++) {
      const uint8_t ty = t[k] & 0x7f, rv = t[k] & T_RVD;
      bool e;
      switch (kind) {
        case ELIG_RVD_STEP:
        case ELIG_ACK:
          e = ty == T_DATA && !rv;
          break;
        case ELIG_CSI1:
          e = ty == T_DATA && !rv;
          break;
        case ELIG_CSI2:
          e = ty == T_DATA;
          break;
        default: /* ELIG_PUNCT: the reserved set, whatever CSI part 2 put there */
          e = rv != 0;
          break;
      }
      if (e)
        t_phi[n++] = (uint16_t)k;
    }
    if (n == 0)
      continue;
    const uint32_t cnt = R >= n ? n : R, d = R >= n ? 1 : n / R;
    for (uint32_t j = 0; j < cnt; j++) {
      uint8_t *x = &t[t_phi[j * d]];
      switch (kind) {
        case ELIG_RVD_STEP:
          *x |= T_RVD;
          break;
        case ELIG_ACK:
          *x = T_ACK;
          break;
        case ELIG_CSI1:
          *x = T_CSI1;
          break;
        case ELIG_CSI2:
          *x = (uint8_t)(T_CSI2 | (*x & T_RVD));
          break;
        default:
          if ((*x & 0x7f) == T_DATA)
            *x = (uint8_t)(T_PUNCT | T_RVD);
          break;
      }
    }
    R -= cnt;
  }
  return R == 0;
}

static bool build_map(const nr_uci_geom_t *g, const nr_uci_hyp_t *h, const nr_uci_counts_t *c, uci_map_t *m)
{
  if (!map_alloc(g, m))
    return false;
  int lc, la;
  first_symbols(g, &lc, &la);
  if (c->q_rvd && !place(g, m, la, c->q_rvd, ELIG_RVD_STEP))
    return false;
  if (h->o_ack > 2 && !place(g, m, la, c->q_ack, ELIG_ACK))
    return false;
  if (!place(g, m, lc, c->q_csi1, ELIG_CSI1) || !place(g, m, lc, c->q_csi2, h->csi2_map ? ELIG_CSI2_RAW : ELIG_CSI2))
    return false;
  if (h->o_ack <= 2 && c->q_ack && !place(g, m, la, c->q_ack, ELIG_PUNCT))
    return false;
  return true;
}

/* UL-SCH stream window [j0, j0 + n) (in LLRs) from the codeword through map m. Returns G_ulsch. */
static uint32_t extract(const nr_uci_geom_t *g, const uci_map_t *m, const int16_t *llr, uint32_t j0, uint32_t n, int16_t *out)
{
  const uint32_t q = (uint32_t)g->qm * g->nl, M = (uint32_t)g->rb * 12u;
  uint32_t j = 0, i = 0; /* j: UL-SCH stream index, i: codeword index (LLRs) */
  const uint32_t j1 = n ? j0 + n : UINT32_MAX;
  for (int l = g->start_sym; l < g->start_sym + g->nsym; l++) {
    const uint32_t md = m_data(g, l);
    if (!(m->touched >> l & 1)) {
      const uint32_t len = md * q;
      /* overlap of [j, j + len) with [j0, j1) */
      const uint32_t a = j > j0 ? j : j0, b = (j + len) < j1 ? (j + len) : j1;
      if (b > a)
        memcpy(out + (a - j0), llr + i + (a - j), (size_t)(b - a) * sizeof(int16_t));
      j += len;
      i += len;
      continue;
    }
    const uint8_t *t = m->t[l];
    for (uint32_t k = 0; k < M; k++, i += q) {
      const uint8_t ty = t[k] & 0x7f;
      if (ty != T_DATA && ty != T_PUNCT)
        continue;
      if (j + q > j0 && j < j1)
        for (uint32_t b = 0; b < q; b++) {
          const uint32_t jj = j + b;
          if (jj >= j0 && jj < j1)
            out[jj - j0] = ty == T_DATA ? llr[i + b] : 0;
        }
      j += q;
    }
  }
  return j;
}

uint32_t nr_uci_demux(const nr_uci_geom_t *g, const nr_uci_hyp_t *h, const int16_t *llr, uint32_t j0, uint32_t n, int16_t *out)
{
  nr_uci_counts_t c;
  uci_map_t m;
  if (!nr_uci_counts(g, h, &c) || !build_map(g, h, &c, &m))
    return 0;
  const uint32_t G_ulsch = (nr_uci_data_re(g) - c.u_re) * g->qm * g->nl;
  if (n && j0 + n > G_ulsch)
    return 0;
  return extract(g, &m, llr, j0, n, out) == G_ulsch ? G_ulsch : 0;
}

/* ------------------------------------------------------------------ learned state (per RNTI) */
#define LRN_RNTI 64
#define LRN_CSI 4
typedef struct {
  double x1_lo, x1_hi, x2_lo, x2_hi; /* (O + L) * beta of CSI part 1 / 2, as intervals (0, 0: no part 2) */
  uint8_t o1, b1, o2, b2;            /* a representative hypothesis inside the intervals */
  uint8_t csi2_map;                  /* the CSI part 2 placement this UE uses (nr_uci_hyp_t.csi2_map) */
  uint32_t hits;
} lrn_csi_t;
typedef struct {
  uint16_t rnti;
  uint32_t used;
  lrn_csi_t csi[LRN_CSI];
  int n_csi;
  uint16_t ack_mask;    /* beta indices (table 9.3-1) consistent with every rate-matched HARQ-ACK win; 0 = none yet */
  uint8_t o_ack_seen[8]; /* recent winning rate-matched O_ACK values */
  uint8_t alpha;        /* 3 until a cap-bound win says otherwise */
  uint32_t explore_seq; /* exploration rotation */
  uint32_t wins;
} lrn_t;

typedef lrn_t g_lrn_cell_t[LRN_RNTI];
NR_CELL_STATIC(g_lrn_cell_t, g_lrn);
#define g_lrn NR_CELL_REF(g_lrn)
NR_CELL_STATIC(uint32_t, g_lrn_clock);
#define g_lrn_clock NR_CELL_REF(g_lrn_clock)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_lrn_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_lrn_lock NR_CELL_REF(g_lrn_lock)
typedef _Atomic uint64_t g_st_cell_t[6];
NR_CELL_STATIC(g_st_cell_t, g_st);
#define g_st NR_CELL_REF(g_st)

static lrn_t *lrn_get_locked(uint16_t rnti, bool create)
{
  lrn_t *lru = &g_lrn[0];
  for (int i = 0; i < LRN_RNTI; i++) {
    if (g_lrn[i].rnti == rnti && g_lrn[i].used) {
      g_lrn[i].used = ++g_lrn_clock;
      return &g_lrn[i];
    }
    if (g_lrn[i].used < lru->used)
      lru = &g_lrn[i];
  }
  if (!create)
    return NULL;
  memset(lru, 0, sizeof(*lru));
  lru->rnti = rnti;
  lru->alpha = 3;
  lru->used = ++g_lrn_clock;
  return lru;
}

int nr_pusch_uci_blind_learned(uint16_t rnti, nr_uci_hyp_t *h, uint32_t *hits, int max)
{
  pthread_mutex_lock(&g_lrn_lock);
  const lrn_t *e = lrn_get_locked(rnti, false);
  int n = 0;
  for (int i = 0; e && i < e->n_csi && n < max; i++, n++) {
    h[n] = (nr_uci_hyp_t){.o_csi1 = e->csi[i].o1, .b_csi1 = e->csi[i].b1, .o_csi2 = e->csi[i].o2, .b_csi2 = e->csi[i].b2, .alpha = e->alpha};
    if (hits)
      hits[n] = e->csi[i].hits;
  }
  pthread_mutex_unlock(&g_lrn_lock);
  return n;
}

void nr_pusch_uci_blind_forget(uint16_t rnti)
{
  pthread_mutex_lock(&g_lrn_lock);
  lrn_t *e = lrn_get_locked(rnti, false);
  if (e)
    memset(e, 0, sizeof(*e));
  pthread_mutex_unlock(&g_lrn_lock);
}

void nr_pusch_uci_blind_stats(uint64_t out[6])
{
  for (int i = 0; i < 6; i++)
    out[i] = atomic_load_explicit(&g_st[i], memory_order_relaxed);
}

/* Record a confirmed win: the CSI products as intervals consistent with the observed counts, the HARQ-ACK beta set. */
static void learn(uint16_t rnti, const nr_uci_geom_t *g, const nr_uci_hyp_t *h, const nr_uci_counts_t *c)
{
  uint32_t s1, s2;
  sums(g, &s1, &s2);
  const double rho = (double)s1 / (double)g->sum_k; /* Q' = ceil(x * rho) away from the alpha cap */
  pthread_mutex_lock(&g_lrn_lock);
  lrn_t *e = lrn_get_locked(rnti, true);
  e->wins++;
  if (h->o_csi1) {
    const double lo1 = ((double)c->q_csi1 - 1.0) / rho, hi1 = (double)c->q_csi1 / rho;
    const double lo2 = c->q_csi2 ? ((double)c->q_csi2 - 1.0) / rho : 0, hi2 = c->q_csi2 ? (double)c->q_csi2 / rho : 0;
    int k = -1;
    for (int i = 0; i < e->n_csi; i++) {
      lrn_csi_t *x = &e->csi[i];
      const bool ov1 = lo1 < x->x1_hi && hi1 > x->x1_lo;
      const bool ov2 = (c->q_csi2 == 0 && x->x2_hi == 0) || (c->q_csi2 && lo2 < x->x2_hi && hi2 > x->x2_lo && x->csi2_map == h->csi2_map);
      if (ov1 && ov2) {
        k = i;
        break;
      }
    }
    if (k < 0) {
      k = e->n_csi < LRN_CSI ? e->n_csi++ : LRN_CSI - 1;
      e->csi[k] = (lrn_csi_t){.x1_lo = lo1, .x1_hi = hi1, .x2_lo = lo2, .x2_hi = hi2};
    } else {
      lrn_csi_t *x = &e->csi[k];
      x->x1_lo = fmax(x->x1_lo, lo1);
      x->x1_hi = fmin(x->x1_hi, hi1);
      if (c->q_csi2) {
        x->x2_lo = fmax(x->x2_lo, lo2);
        x->x2_hi = fmin(x->x2_hi, hi2);
      }
    }
    lrn_csi_t *x = &e->csi[k];
    x->o1 = h->o_csi1;
    x->b1 = h->b_csi1;
    x->o2 = h->o_csi2;
    x->b2 = h->b_csi2;
    x->csi2_map = h->csi2_map;
    x->hits++;
    /* keep the most used first */
    while (k > 0 && e->csi[k].hits > e->csi[k - 1].hits) {
      const lrn_csi_t t = e->csi[k];
      e->csi[k] = e->csi[k - 1];
      e->csi[--k] = t;
    }
  }
  if (h->o_ack > 2) {
    /* every beta of the same O range that gives the same count for some O of the range is consistent */
    const unsigned olo = h->o_ack <= 11 ? 3 : 12, ohi = h->o_ack <= 11 ? 11 : UCI_MAX_O_ACK;
    uint16_t mask = 0;
    for (int b = 0; b < 16; b++)
      for (unsigned o = olo; o <= ohi; o++)
        if (q_count(o, k_beta_ack[b], k_alpha[h->alpha], g->sum_k, s1, s2, 0) == c->q_ack) {
          mask |= (uint16_t)(1u << b);
          break;
        }
    e->ack_mask = e->ack_mask ? (uint16_t)(e->ack_mask & mask) : mask;
    if (!e->ack_mask)
      e->ack_mask = mask; /* the UE's configuration changed: start over */
    memmove(e->o_ack_seen + 1, e->o_ack_seen, sizeof(e->o_ack_seen) - 1);
    e->o_ack_seen[0] = h->o_ack;
  }
  pthread_mutex_unlock(&g_lrn_lock);
}

/* ------------------------------------------------------------------ the search */
typedef struct {
  const nr_uci_search_in_t *in;
  nr_uci_search_out_t *out;
  unsigned C, K, Z, F;
  uint32_t N; /* UL-SCH REs without UCI */
  uint32_t s1, s2;
  int l_csi, l_ack;
  uint64_t t0_ns, budget_ns;
  int16_t *d;   /* single-block rate-dematching buffer */
  int16_t *win; /* single-block LLR window */
} srch_t;

static uint64_t cpu_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline bool over_budget(const srch_t *s)
{
  return cpu_ns() - s->t0_ns > s->budget_ns;
}

static inline uint32_t nlqm(const srch_t *s)
{
  return (uint32_t)s->in->geom.qm * s->in->geom.nl;
}

static uint32_t e_of(const srch_t *s, uint32_t u_re, unsigned r)
{
  return (uint32_t)nr_get_E((s->N - u_re) * nlqm(s), (uint8_t)s->C, s->in->geom.qm, s->in->geom.nl, (uint8_t)r);
}

/* LDPC on one code block r of an UL-SCH stream of g_ulsch bits whose E_r LLRs are in llr. */
static bool cb_test(srch_t *s, const int16_t *llr, uint32_t g_ulsch, unsigned r)
{
  const nr_uci_search_in_t *in = s->in;
  const uint32_t E = (uint32_t)nr_get_E(g_ulsch, (uint8_t)s->C, in->geom.qm, in->geom.nl, (uint8_t)r);
  nr_pdsch_gw_cb0_t cb;
  memset(&cb, 0, sizeof(cb));
  cb.p = (nr_td_cb0_params_t){.G = g_ulsch, .A = in->tbs_bits, .E = E, .tbslbrm = 0, .C = (int)s->C, .K = (int)s->K, .Z = (int)s->Z,
                              .F = (int)s->F, .BG = in->bg, .Qm = in->geom.qm, .Nl = in->geom.nl, .rv = in->rv};
  cb.d = s->d;
  s->out->cb_tests++;
  atomic_fetch_add_explicit(&g_st[4], 1, memory_order_relaxed);
  if (nr_td_gw_cb0_extract(llr, E, &cb.p, s->d) != NR_TD_GW_OK)
    return false;
  bool ok = false;
  return nr_pdsch_passive_gw_cb_decode_iter(&cb, UCI_SCREEN_ITER, &ok) && ok;
}

/* Code block r of hypothesis h through the mapping (window extraction only), for an UL-SCH stream of u_re removed REs
 * (u_re may exceed the hypothesis's own count when the rest is rate-matched HARQ-ACK placed after block r). */
static bool cb_test_hu(srch_t *s, const nr_uci_hyp_t *h, const nr_uci_counts_t *c, uint32_t u_re, unsigned r)
{
  if (u_re >= s->N)
    return false;
  const uint32_t g_ulsch = (s->N - u_re) * nlqm(s);
  uint32_t off = 0;
  for (unsigned i = 0; i < r; i++)
    off += (uint32_t)nr_get_E(g_ulsch, (uint8_t)s->C, s->in->geom.qm, s->in->geom.nl, (uint8_t)i);
  const uint32_t E = (uint32_t)nr_get_E(g_ulsch, (uint8_t)s->C, s->in->geom.qm, s->in->geom.nl, (uint8_t)r);
  uci_map_t m;
  if (!build_map(&s->in->geom, h, c, &m) || extract(&s->in->geom, &m, s->in->llr, off, E, s->win) < off + E)
    return false;
  return cb_test(s, s->win, g_ulsch, r);
}

static bool cb_test_h(srch_t *s, const nr_uci_hyp_t *h, const nr_uci_counts_t *c, unsigned r)
{
  return cb_test_hu(s, h, c, c->u_re, r);
}

/* The full TB at hypothesis h. On success the UL-SCH stream is in in->work. */
static bool tb_test(srch_t *s, const nr_uci_hyp_t *h, const nr_uci_counts_t *c)
{
  const nr_uci_search_in_t *in = s->in;
  uci_map_t m;
  if (!build_map(&in->geom, h, c, &m))
    return false;
  const uint32_t g_ulsch = extract(&in->geom, &m, in->llr, 0, 0, in->work);
  if (g_ulsch != (s->N - c->u_re) * nlqm(s))
    return false;
  s->out->tb_tests++;
  atomic_fetch_add_explicit(&g_st[5], 1, memory_order_relaxed);
  if (!in->tb(in->opaque, in->work, g_ulsch))
    return false;
  s->out->ok = true;
  s->out->h = *h;
  s->out->c = *c;
  s->out->g_ulsch = g_ulsch;
  return true;
}

/* The code block holding the first UL-SCH bits of the first HARQ-ACK symbol (where rate-matched ACK first moves data). */
static unsigned cb_of_ack(const srch_t *s, uint32_t u_re)
{
  if (s->l_ack < 0)
    return 0;
  uint32_t j = 0;
  for (int l = s->in->geom.start_sym; l < s->l_ack; l++)
    j += m_data(&s->in->geom, l) * nlqm(s);
  const uint32_t g_ulsch = (s->N - u_re) * nlqm(s);
  uint32_t off = 0;
  for (unsigned r = 0; r < s->C; r++) {
    off += (uint32_t)nr_get_E(g_ulsch, (uint8_t)s->C, s->in->geom.qm, s->in->geom.nl, (uint8_t)r);
    if (j < off)
      return r;
  }
  return s->C - 1;
}

/* Screen (C > 1: the blocks the UCI can move, then the TB) and decide. */
static bool try_hyp(srch_t *s, const nr_uci_hyp_t *h)
{
  nr_uci_counts_t c;
  if (!nr_uci_counts(&s->in->geom, h, &c))
    return false;
  if (s->C > 1) {
    if (!cb_test_h(s, h, &c, 0))
      return false;
    const unsigned ra = cb_of_ack(s, c.u_re);
    if (ra != 0 && (h->o_ack > 2 || c.q_ack) && !cb_test_h(s, h, &c, ra))
      return false;
  }
  return tb_test(s, h, &c);
}

/* beta orders: from the middle of each table outwards (no table index is privileged) */
static const uint8_t k_ack_order[16] = {9, 8, 10, 7, 11, 6, 12, 5, 13, 4, 14, 3, 15, 2, 1, 0};
static const uint8_t k_csi_order[19] = {9, 8, 10, 7, 11, 6, 12, 5, 13, 4, 14, 3, 15, 2, 16, 1, 17, 0, 18};

/* O_ACK prior: the dynamic-codebook total-DAI value first ((DAI + 1) + 4 j), then the values this UE used, then ascending. */
static int ack_o_order(const srch_t *s, const lrn_t *e, uint8_t *o, int max)
{
  int n = 0;
  bool seen[UCI_MAX_O_ACK + 1] = {false};
#define PUSH_O(v)                                         \
  do {                                                    \
    const int v_ = (v);                                   \
    if (v_ >= 3 && v_ <= UCI_MAX_O_ACK && !seen[v_] && n < max) { \
      seen[v_] = true;                                    \
      o[n++] = (uint8_t)v_;                               \
    }                                                     \
  } while (0)
  if (s->in->dai >= 0 && s->in->dai <= 3)
    for (int j = 0; j < 3; j++)
      PUSH_O(s->in->dai + 1 + 4 * j);
  if (e)
    for (int i = 0; i < 8; i++)
      PUSH_O(e->o_ack_seen[i]);
  for (int v = 3; v <= UCI_MAX_O_ACK; v++)
    PUSH_O(v);
#undef PUSH_O
  return n;
}

/* 1. the hypotheses learned for this RNTI: each learned CSI part (and none), with HARQ-ACK of <= 2 bits or rate-matched
 * HARQ-ACK of the O values this UE is likely to use under every beta still consistent with its earlier wins; each distinct
 * RE count once. */
static bool search_learned(srch_t *s, const lrn_t *e)
{
  if (!e || (!e->n_csi && !e->ack_mask))
    return false;
  uint8_t oo[UCI_MAX_O_ACK];
  const int no = ack_o_order(s, e, oo, 6);
  for (int ci = -1; ci < e->n_csi && !over_budget(s); ci++) {
    nr_uci_hyp_t h = {.alpha = e->alpha, .b_ack = k_ack_order[0]};
    if (ci >= 0) {
      h.o_csi1 = e->csi[ci].o1;
      h.b_csi1 = e->csi[ci].b1;
      h.o_csi2 = e->csi[ci].o2;
      h.b_csi2 = e->csi[ci].b2;
      h.csi2_map = e->csi[ci].csi2_map;
      h.o_ack = 0; /* HARQ-ACK of <= 2 bits (reserved REs only) */
      if (try_hyp(s, &h))
        return true;
    }
    if (!e->ack_mask)
      continue;
    uint32_t tried[UCI_MAX_O_ACK * 16];
    int nt = 0;
    for (int i = 0; i < no && !over_budget(s); i++)
      for (int b = 0; b < 16 && !over_budget(s); b++) {
        if (!(e->ack_mask >> k_ack_order[b] & 1))
          continue;
        h.o_ack = oo[i];
        h.b_ack = k_ack_order[b];
        nr_uci_counts_t c;
        if (!nr_uci_counts(&s->in->geom, &h, &c))
          continue;
        bool dup = false;
        for (int t = 0; t < nt && !dup; t++)
          dup = tried[t] == c.q_ack;
        if (dup)
          continue;
        tried[nt++] = c.q_ack;
        if (try_hyp(s, &h))
          return true;
      }
  }
  return false;
}

/* The U interval the LAST code block admits: with all UCI before its first bit, its LLRs are the codeword's last E_last
 * and only E_last depends on U. Scans U = 0, 1, ... testing once per distinct E_last. Returns true with [lo, hi]. */
static bool scan_u(srch_t *s, uint32_t u_max, uint32_t *lo, uint32_t *hi)
{
  const nr_uci_search_in_t *in = s->in;
  uint32_t last_e = 0;
  for (uint32_t u = 0; u <= u_max; u++) {
    const uint32_t E = e_of(s, u, s->C - 1);
    if (E == last_e)
      continue;
    last_e = E;
    if (over_budget(s))
      return false;
    /* the UCI must end before the last block's first codeword bit (conservative: every UCI RE in the first symbols) */
    if (E > in->G)
      return false;
    if (cb_test(s, in->llr + (in->G - E), (s->N - u) * nlqm(s), s->C - 1)) {
      *lo = u;
      uint32_t v = u;
      while (v + 1 <= u_max && e_of(s, v + 1, s->C - 1) == E)
        v++;
      *hi = v;
      return true;
    }
  }
  return false;
}

/* q values reachable by (O, beta) for one UCI part on this grant, ordered by prior (small O, middle beta first). */
typedef struct {
  uint32_t q;
  uint8_t o, b;
} qcand_t;

static int csi_candidates(const srch_t *s, int part, uint32_t qa, uint32_t q1, double alpha, qcand_t *qc, int max)
{
  const int omax = part == 1 ? UCI_MAX_O_CSI1 : UCI_MAX_O_CSI2;
  int n = 0;
  for (int o = 1; o <= omax; o++)
    for (int bi = 0; bi < 19; bi++) {
      const int b = k_csi_order[bi];
      const uint32_t q = q_count((unsigned)o, k_beta_csi[b], alpha, s->in->geom.sum_k, s->s1, s->s1, qa + (part == 2 ? q1 : 0));
      if (q == 0)
        continue;
      bool dup = false;
      for (int i = 0; i < n && !dup; i++)
        dup = qc[i].q == q;
      if (!dup && n < max)
        qc[n++] = (qcand_t){.q = q, .o = (uint8_t)o, .b = (uint8_t)b};
    }
  return n;
}

/* 2. exploration */
static bool search_explore(srch_t *s, lrn_t *e_snapshot, uint32_t explore_seq)
{
  const nr_uci_search_in_t *in = s->in;
  nr_uci_search_out_t *out = s->out;
  uint8_t oo[UCI_MAX_O_ACK];
  const int no = ack_o_order(s, e_snapshot, oo, UCI_MAX_O_ACK);
  const double alpha = k_alpha[e_snapshot ? e_snapshot->alpha : 3];
  const uint8_t alpha_i = e_snapshot ? e_snapshot->alpha : 3;
  /* U bound: UCI in the first symbols up to a quarter of the UL-SCH REs */
  uint32_t lo = 0, hi = s->N / 4;
  bool have_u = false;
  if (s->C > 1) {
    if (!scan_u(s, s->N / 4, &lo, &hi))
      return false; /* no U explains the last code block: not a UCI problem this search can fix */
    out->u_lo = (int)lo;
    out->u_hi = (int)hi;
    have_u = true;
    /* lo == 0: the last block also decodes with no UCI RE removed -- U is small (CSI, few-bit HARQ-ACK) or zero; the
     * interval still bounds every hypothesis below */
    out->no_rm_uci = lo == 0;
  }
  /* (a) rate-matched HARQ-ACK only (no CSI): each distinct count once, in prior order */
  static __thread uint8_t *seen;
  static __thread uint32_t seen_cap;
  if (seen_cap < s->N + 1) {
    free(seen);
    seen = malloc(s->N + 1);
    seen_cap = seen ? s->N + 1 : 0;
    if (!seen)
      return false;
  }
  memset(seen, 0, s->N + 1);
  for (int i = 0; i < no && !over_budget(s); i++)
    for (int bi = 0; bi < 16 && !over_budget(s); bi++) {
      const nr_uci_hyp_t h = {.o_ack = oo[i], .b_ack = k_ack_order[bi], .alpha = alpha_i};
      nr_uci_counts_t c;
      if (!nr_uci_counts(&in->geom, &h, &c) || c.u_re > s->N || seen[c.u_re] || (have_u && (c.u_re < lo || c.u_re > hi)))
        continue;
      seen[c.u_re] = 1;
      if (try_hyp(s, &h))
        return true;
    }
  /* (b) CSI part 1 (+ part 2), with HARQ-ACK of <= 2 bits or rate-matched HARQ-ACK making up the rest of U */
  enum { QMAX = 640 };
  static __thread qcand_t q1c[QMAX], q2c[QMAX];
  nr_uci_hyp_t h0 = {.o_ack = 2, .b_ack = k_ack_order[0], .alpha = alpha_i};
  nr_uci_counts_t c0;
  const nr_uci_hyp_t hr = {.o_ack = 2, .b_ack = k_ack_order[0], .o_csi1 = 1, .alpha = alpha_i};
  if (!nr_uci_counts(&in->geom, &hr, &c0))
    return false;
  const uint32_t qa = c0.q_rvd;
  const int n1 = csi_candidates(s, 1, qa, 0, alpha, q1c, QMAX);
  /* pairs in a fixed order (part 1 prior, then part 2 prior), visited from a rotating start so successive exploring
   * grants cover different pairs */
  const int n2max = csi_candidates(s, 2, qa, 0, alpha, q2c, QMAX);
  const uint64_t npairs = (uint64_t)n1 * (uint64_t)(n2max + 1);
  if (npairs == 0)
    return false;
  const uint64_t start = ((uint64_t)explore_seq * 977u) % npairs;
  for (uint64_t t = 0; t < npairs && !over_budget(s); t++) {
    const uint64_t p = (start + t) % npairs;
    const int i1 = (int)(p / (uint64_t)(n2max + 1)), i2 = (int)(p % (uint64_t)(n2max + 1)) - 1;
    nr_uci_hyp_t h = h0;
    h.o_ack = 0;
    h.o_csi1 = q1c[i1].o;
    h.b_csi1 = q1c[i1].b;
    h.o_csi2 = i2 >= 0 ? q2c[i2].o : 0;
    h.b_csi2 = i2 >= 0 ? q2c[i2].b : 0;
    nr_uci_counts_t c;
    if (!nr_uci_counts(&in->geom, &h, &c))
      continue;
    if (have_u && c.u_re > hi)
      continue;
    const int nmap = i2 >= 0 ? 2 : 1; /* with CSI part 2: both placements (nr_uci_hyp_t.csi2_map) */
    if (!have_u || c.u_re >= lo) {
      /* HARQ-ACK of <= 2 bits: the CSI makes all of U */
      for (int mp = 0; mp < nmap; mp++) {
        h.csi2_map = (uint8_t)mp;
        if (try_hyp(s, &h))
          return true;
      }
      continue;
    }
    /* CSI below U: rate-matched HARQ-ACK makes up the rest; screen block 0 first (independent of the ACK when block 0
     * ends before the first HARQ-ACK symbol) with each E_0 the U interval allows */
    bool cb0 = false;
    for (int mp = 0; mp < nmap && !cb0; mp++) {
      h.csi2_map = (uint8_t)mp;
      for (uint32_t u = lo; u <= hi && !cb0; u++) {
        if (u > lo && e_of(s, u, 0) == e_of(s, u - 1, 0))
          continue;
        /* the CSI map with the reserved REs (ACK <= 2) puts the CSI where the rate-matched ACK case does as long as
         * block 0 precedes the first HARQ-ACK symbol; u sets block 0's length */
        if (s->C > 1)
          cb0 = cb_test_hu(s, &h, &c, u, 0);
      }
    }
    if (!cb0)
      continue;
    for (int i = 0; i < no && !over_budget(s); i++)
      for (int bi = 0; bi < 16; bi++) {
        nr_uci_hyp_t ha = h;
        ha.o_ack = oo[i];
        ha.b_ack = k_ack_order[bi];
        nr_uci_counts_t ca;
        if (!nr_uci_counts(&in->geom, &ha, &ca) || ca.u_re < lo || ca.u_re > hi)
          continue;
        if (try_hyp(s, &ha))
          return true;
      }
  }
  return false;
}

void nr_pusch_uci_blind_search(const nr_uci_search_in_t *in, nr_uci_search_out_t *out)
{
  memset(out, 0, sizeof(*out));
  out->u_lo = out->u_hi = -1;
  if (!in || !in->llr || !in->work || !in->tb || in->G == 0 || in->tbs_bits == 0 || in->geom.rb == 0 || in->geom.qm == 0 || in->geom.nl == 0)
    return;
  /* Only a TB with a 24-bit CRC (TBS > 3824 bits, TS 38.212 7.2.1) can decide among hypotheses: a 16-bit CRC passes 1 in
   * 65536 wrong ones, and every failing grant tries several. On air, searching CRC16 TBs too reported footprints of up to
   * 360 RE on 3-PRB grants -- chance passes -- after 1.1 M TB tests in 300 s ([MEASURED, sens6 OTA lab,
   * ulint1_lab_20261025_211640]). Such grants keep the plain decode's verdict. */
  if (in->tbs_bits <= 3824)
    return;
  srch_t s;
  memset(&s, 0, sizeof(s));
  s.in = in;
  s.out = out;
  s.N = nr_uci_data_re(&in->geom);
  if (s.N * nlqm(&s) != in->G)
    return;
  if (nr_segmentation(NULL, NULL, (unsigned)lenWithCrc(1, (int)in->tbs_bits), &s.C, &s.K, &s.Z, &s.F, in->bg) < 0 || s.C == 0 || s.C > 255)
    return;
  nr_uci_geom_t g = in->geom;
  g.sum_k = s.C * s.K;
  nr_uci_search_in_t in2 = *in;
  in2.geom = g;
  s.in = &in2;
  sums(&g, &s.s1, &s.s2);
  first_symbols(&g, &s.l_csi, &s.l_ack);
  static __thread int16_t *dbuf, *wbuf;
  static __thread uint32_t wcap;
  if (!dbuf)
    dbuf = malloc((size_t)68 * 384 * sizeof(int16_t));
  const uint32_t ecap = (s.N * nlqm(&s)) / s.C + 64 * nlqm(&s);
  if (wcap < ecap) {
    free(wbuf);
    wbuf = malloc((size_t)ecap * sizeof(int16_t));
    wcap = wbuf ? ecap : 0;
  }
  if (!dbuf || !wbuf)
    return;
  s.d = dbuf;
  s.win = wbuf;
  atomic_fetch_add_explicit(&g_st[0], 1, memory_order_relaxed);
  /* a copy of this RNTI's learned state (the search runs without the lock) */
  lrn_t snap;
  bool have = false;
  uint32_t seq = 0;
  pthread_mutex_lock(&g_lrn_lock);
  lrn_t *e = lrn_get_locked(in->rnti, in->explore);
  if (e) {
    snap = *e;
    have = true;
    if (in->explore)
      seq = e->explore_seq++;
  }
  pthread_mutex_unlock(&g_lrn_lock);
  s.t0_ns = cpu_ns();
  s.budget_ns = (uint64_t)(in->budget_us ? in->budget_us : 1) * 1000ull;
  /* the learned step tries a short, fixed list (each learned CSI part x its HARQ-ACK options): the grant's budget bounds it */
  const uint64_t keep = s.budget_ns;
  if (have && search_learned(&s, &snap)) {
    out->learned = true;
    atomic_fetch_add_explicit(&g_st[2], 1, memory_order_relaxed);
    learn(in->rnti, &g, &out->h, &out->c);
    return;
  }
  if (!in->explore)
    return;
  atomic_fetch_add_explicit(&g_st[1], 1, memory_order_relaxed);
  s.t0_ns = cpu_ns();
  s.budget_ns = keep;
  if (!search_explore(&s, have ? &snap : NULL, seq))
    return;
  atomic_fetch_add_explicit(&g_st[3], 1, memory_order_relaxed);
  learn(in->rnti, &g, &out->h, &out->c);
}
