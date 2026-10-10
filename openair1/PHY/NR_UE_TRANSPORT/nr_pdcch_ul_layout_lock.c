/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* UL-LAYOUT: blind UL BWP / DCI 0_1 layout lock. See the header for the method. */
#include "nr_pdcch_ul_layout_lock.h"
#include "nr_pdsch_prb_set.h" /* NR_FDRA_TYPE1 */
#include "common/utils/LOG/log.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VOTE_DB 6.0f       /* a probe votes when its best hypothesis has >= this DM-RS MRC SNR */
#define LOCK_MIN 6         /* votes for the leader */
#define LOCK_RATIO 2.0f    /* leader >= ratio x runner-up */
#define LOCK_RNTIS 2       /* distinct RNTIs among the leader's votes */
#define PROBE_MAX_RB 16    /* probes only allocations this narrow (the detectable kind; bounded cost) */
#define FIT_MIN 256        /* bank size before a length is fitted (small banks leave the RIV MSBs constant) */
#define CDM_MIN 3
#define PENDING_MAX 24

/* ------------------------------------------------------------------ pure helpers */
int nr_ul_layout_riv_bits(int n)
{
  if (n < 1) return 0;
  const double span = (double)n * (double)(n + 1) / 2.0;
  int b = 0;
  while ((double)(UINT64_C(1) << b) < span) b++;
  return b;
}

bool nr_ul_layout_riv(uint32_t riv, int n, uint16_t *start, uint16_t *len)
{
  if (n < 1 || (uint64_t)riv >= (uint64_t)n * (uint64_t)(n + 1) / 2) return false;
  const uint32_t a = riv / (uint32_t)n, b = riv % (uint32_t)n;
  uint32_t s, l;
  if (a + b + 1 <= (uint32_t)n) { l = a + 1; s = b; }
  else { l = (uint32_t)n - a + 1; s = (uint32_t)n - 1 - b; }
  if (l < 1 || s + l > (uint32_t)n) return false;
  *start = (uint16_t)s;
  *len = (uint16_t)l;
  return true;
}

static inline uint32_t fld(const nr_dci_bits_t *p, int len, int msb, int w)
{
  return (uint32_t)nr_dci_bits_field(p, len, msb, w);
}

/* HARQ statistics at a candidate MCS offset: the pid of consecutive grants of one RNTI advances by 1..3 (cyclic,
 * mod 16) and RV is mostly 0. A constant region gives step 0, a random one ~3/16. */
static float mcs_score(const nr_ul_layout_dci_t *b, int n, uint16_t len, int o, int *pairs_out)
{
  uint16_t rn[64];
  int8_t last[64];
  long last_slot[64];
  int nr = 0, ok = 0, pairs = 0, rv0 = 0;
  for (int i = 0; i < n; i++) {
    const uint32_t pid = fld(&b[i].p, len, o + 8, 4), rv = fld(&b[i].p, len, o + 6, 2);
    rv0 += rv == 0;
    int k = 0;
    while (k < nr && rn[k] != b[i].rnti) k++;
    if (k == nr) {
      if (nr == 64) continue;
      rn[nr] = b[i].rnti;
      last[nr] = -1;
      nr++;
    }
    if (last[k] >= 0 && last_slot[k] != b[i].slot) {
      const int d = ((int)pid - last[k] + 16) % 16;
      pairs++;
      ok += d >= 1 && d <= 3;
    }
    last[k] = (int8_t)pid;
    last_slot[k] = b[i].slot;
  }
  if (pairs_out) *pairs_out = pairs;
  return pairs ? (float)ok / pairs + (float)rv0 / n : 0.f;
}

int nr_ul_layout_fit_mcs(const nr_ul_layout_dci_t *b, int n, uint16_t len, float *score, float *second)
{
  float best = -1.f, sec = -1.f;
  int bo = -1, best_pairs = 0;
  for (int o = 2; o + 12 <= len; o++) {
    int pairs = 0;
    const float s = mcs_score(b, n, len, o, &pairs);
    if (s > best) { sec = best; best = s; bo = o; best_pairs = pairs; }
    else if (s > sec) sec = s;
  }
  if (score) *score = best;
  if (second) *second = sec;
  /* >= 64 HARQ pairs; the true offset scores ~1.8 (step 1..3 ~0.9 + RV0 ~0.9) on PCI 415, a one-bit shift ~1.3 */
  if (bo < 0 || best_pairs < 64 || best < 1.2f || best - sec < 0.3f) return -1;
  return bo;
}

static void p1_stats(const nr_ul_layout_dci_t *b, int n, uint16_t len, float *p1)
{
  for (int i = 0; i < len; i++) p1[i] = 0.f;
  for (int k = 0; k < n; k++)
    for (int i = 0; i < len; i++) p1[i] += (float)fld(&b[k].p, len, i, 1);
  for (int i = 0; i < len; i++) p1[i] /= (float)(n ? n : 1);
}

int nr_ul_layout_fit_prefix(const nr_ul_layout_dci_t *b, int n, uint16_t len, int mcs_off, const uint16_t *cand_n,
                            int n_cand, nr_ul_layout_prefix_t *out, int cap)
{
  if (!b || n <= 0 || mcs_off < 2 || len > NR_DCI_MAX_PAYLOAD) return 0;
  float p1[NR_DCI_MAX_PAYLOAD];
  p1_stats(b, n, len, p1);
  int nconst = 0;
  while (nconst < 2 && 1 + nconst < mcs_off && (p1[1 + nconst] <= 0.02f || p1[1 + nconst] >= 0.98f)) nconst++;
  const int p = mcs_off - 1;
  int cnt = 0;
  for (int pass = 0; pass < 2; pass++) /* pass 0: b == leading constant bits; pass 1: fewer */
    for (int bw = (pass ? nconst - 1 : nconst); bw >= 0 && (pass || bw == nconst); bw--)
      for (int w = 1; w <= 16; w++) {
        uint16_t nmax = 0;
        for (int i = 0; i < n_cand; i++)
          if (nr_ul_layout_riv_bits(cand_n[i]) == w && cand_n[i] > nmax) nmax = cand_n[i];
        if (!nmax) continue;
        const int rest = p - bw - w;
        if (rest < 0 || rest > 5) continue;
        const float lsb = p1[1 + bw + w - 1];
        if (lsb < 0.05f || lsb > 0.95f) continue; /* RIV LSB carries entropy */
        int valid = 0;
        const uint64_t lim = (uint64_t)nmax * (nmax + 1) / 2;
        for (int k = 0; k < n; k++) valid += fld(&b[k].p, len, 1 + bw, w) < lim;
        if ((float)valid < 0.98f * n) continue;
        if (cnt < cap)
          out[cnt++] = (nr_ul_layout_prefix_t){.bwp_ind = (uint8_t)bw, .riv = (uint8_t)w,
                                               .tda = (uint8_t)(rest > 4 ? 4 : rest),
                                               .fh = (uint8_t)(rest > 4 ? rest - 4 : 0), .n_max = nmax};
      }
  /* Every surviving split stays a hypothesis (the DM-RS stage tells their RIV widths apart). Order: no frequency
   * hopping first (a constant-0 hopping bit is indistinguishable from a TDRA bit), stable otherwise. The first one
   * fixes the TDRA position used for k2. */
  for (int i = 1; i < cnt; i++)
    for (int j = i; j > 0 && out[j].fh < out[j - 1].fh; j--) {
      const nr_ul_layout_prefix_t x = out[j]; out[j] = out[j - 1]; out[j - 1] = x;
    }
  return cnt;
}

void nr_ul_layout_build_opts(const nr_ul_layout_prefix_t *pre, const nr_ul_layout_tail_t *t, uint16_t bwp_start,
                             uint16_t bwp_size, const uint8_t *k2_per_row, int add_pos, uint16_t pci, uint8_t mu,
                             uint8_t typeA_pos, nr_pdcch_blind_ul_opts_t *o)
{
  memset(o, 0, sizeof(*o));
  o->bwp_start = bwp_start;
  o->bwp_size = bwp_size;
  o->fdra_bwp_start = bwp_start;
  o->fdra_mode = NR_FDRA_TYPE1;
  o->numerology = mu;
  o->dmrs_typeA_position = typeA_pos;
  o->tda_count = 1 << pre->tda;
  for (int i = 0; i < o->tda_count && i < 16; i++) {
    o->tda_start[i] = 0;
    o->tda_length[i] = 14;
    o->tda_mapping[i] = 0;
    o->tda_k2[i] = (k2_per_row && k2_per_row[i]) ? k2_per_row[i] : (k2_per_row && k2_per_row[0] ? k2_per_row[0] : 2);
  }
  o->dmrs_config_type = 0;
  o->dmrs_add_pos = add_pos;
  o->dmrs_max_length = 1;
  o->transform_precoding = t->tp;
  o->mcs_table = t->tp ? 3 : 0;
  o->data_scrambling_id = -1;
  o->ul_dmrs_scrambling_id = -1;
  o->phy_cell_id = pci;
  o->carrier_indicator_bits = 0;
  o->ul_sul_bits = 0;
  o->bwp_indicator_bits = pre->bwp_ind;
  o->freq_hopping_bits = pre->fh;
  o->harq_pid_bits = 4;
  o->dai1_bits = t->dai1;
  o->dai2_bits = t->dai2;
  o->sri_bits = t->sri;
  o->precoding_info_bits = 0;
  o->antenna_ports_bits = t->ant;
  o->srs_request_bits = 2;
  o->csi_request_bits = t->csi;
  o->cbg_bits = 0;
  o->ptrs_dmrs_bits = 0;
  o->beta_offset_bits = t->beta;
  o->dmrs_seq_init_bits = t->init;
}

int nr_ul_layout_fit_tail(const nr_ul_layout_dci_t *b, int n, uint16_t len, const nr_ul_layout_prefix_t *pre,
                          nr_ul_layout_tail_t *out, int cap)
{
  const int sample = n < 256 ? n : 256;
  nr_ul_layout_tail_t all[64];
  int na = 0;
  static const uint8_t k2s[16] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
  for (int tp = 0; tp <= 1; tp++)
    for (int dai1 = 2; dai1 >= 1; dai1--)
      for (int dai2 = 0; dai2 <= 2; dai2 += 2)
        for (int sri = 0; sri <= 2; sri++)
          for (int ai = 0; ai < 3; ai++) {
            const int ant = tp ? (ai == 0 ? 2 : ai == 1 ? 3 : -1) : (ai == 0 ? 3 : ai == 1 ? 4 : 2);
            if (ant < 0) continue;
            const bool nonspec = !tp && ant == 2; /* CP-OFDM type 1 / maxLength 1 needs 3 bits (Table 7.3.1.1.2-8) */
            for (int csi = 0; csi <= 6; csi++)
              for (int beta = 0; beta <= 2; beta += 2) {
                nr_ul_layout_tail_t t = {.dai1 = (int8_t)dai1, .dai2 = (int8_t)dai2, .sri = (int8_t)sri,
                                         .ant = (int8_t)ant, .csi = (int8_t)csi, .beta = (int8_t)beta,
                                         .init = (int8_t)(tp ? 0 : 1), .tp = (uint8_t)tp};
                nr_pdcch_blind_ul_opts_t o;
                nr_ul_layout_build_opts(pre, &t, 0, pre->n_max, k2s, 1, 0, 1, 0, &o);
                if (nr_pdcch_blind_dci01_size(&o) != len) continue;
                int ok = 0, cdm[3] = {0};
                for (int k = 0; k < sample; k++) {
                  nr_pdcch_blind_ul_result_t g;
                  if (nr_pdcch_blind_extract_01(b[k].p, len, b[k].rnti, &o, &g)) {
                    ok++;
                    if (g.n_dmrs_cdm_groups >= 1 && g.n_dmrs_cdm_groups <= 2) cdm[g.n_dmrs_cdm_groups]++;
                  }
                }
                t.plaus = sample ? (float)ok / sample : 0.f;
                if (t.plaus < 0.97f) continue;
                t.cdm = cdm[2] > cdm[1] ? 2 : 1;
                t.cost = (int16_t)((dai1 == 1) + dai2 + 2 * sri + tp + (nonspec ? 3 : 0) + (ant == 4) + csi + beta);
                if (na < 64) all[na++] = t;
              }
          }
  /* best first: prior cost, then plausibility */
  for (int i = 1; i < na; i++)
    for (int j = i; j > 0 && (all[j].cost < all[j - 1].cost ||
                             (all[j].cost == all[j - 1].cost && all[j].plaus > all[j - 1].plaus)); j--) {
      const nr_ul_layout_tail_t x = all[j]; all[j] = all[j - 1]; all[j - 1] = x;
    }
  const int m = na < cap ? na : cap;
  memcpy(out, all, sizeof(*out) * m);
  return m;
}

int nr_ul_layout_infer_k2(const uint32_t *occ, const uint32_t *hist, int spf)
{
  if (spf <= 0 || spf > 160) return 0;
  uint32_t mx = 0, total = 0;
  for (int s = 0; s < spf; s++) { if (occ[s] > mx) mx = occ[s]; total += hist[s]; }
  if (mx < 50 || total < 32) return 0;
  /* A slot is "PDCCH" (has DL symbols at its start) when it carried at least 3 accepted DCIs and 0.2 % of the busiest
   * slot: a special slot can carry only a few UL DCIs against tens of thousands of DL accepts elsewhere (raw_pci415_12s
   * 20261008: slot 7 with 173 DCI 0_1 -- a 2 % threshold called it free and inferred k2 = 1). */
  for (int k = 1; k <= 16; k++) {
    uint32_t bad = 0;
    for (int s = 0; s < spf; s++)
      if (occ[(s + k) % spf] >= 3u && occ[(s + k) % spf] * 500u > mx) bad += hist[s];
    if (bad * 20u <= total) return k;
  }
  return 0;
}

/* ------------------------------------------------------------------ live state */
typedef struct {
  uint16_t len;
  uint32_t count; /* DCIs seen (ring index = count % BANK) */
  nr_ul_layout_dci_t bank[NR_UL_LAYOUT_BANK];
  long first_slot, fit_slot, lock_slot;
  uint32_t last_eval_count;
  bool fitted;
  int mcs_off;
  float mcs_score, mcs_second;
  int npre;
  nr_ul_layout_prefix_t pre[NR_UL_LAYOUT_MAX_PRE];
  int ntail;
  nr_ul_layout_tail_t tail[NR_UL_LAYOUT_MAX_TAIL];
  uint8_t k2row[16];
  uint32_t gold, gctl, cdm1, cdm2, probes;
  uint32_t crc_ok, crc_fail_run, trials, crc_locks;
  bool locked;
  nr_pdcch_blind_ul_opts_t opts;
  int chosen_tail;
} len_state_t;

typedef struct { uint16_t n, start; uint32_t votes; uint16_t rntis[8]; uint8_t nrnti; } hyp_state_t;
typedef struct {
  long target;
  uint16_t rnti;
  int li;
  int n;
  float snr[NR_UL_LAYOUT_MAX_HYP];
  uint16_t r0[NR_UL_LAYOUT_MAX_HYP], r1[NR_UL_LAYOUT_MAX_HYP];
  int8_t set[NR_UL_LAYOUT_MAX_HYP];
  float ctl, comb1;
} pending_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static len_state_t g_len[NR_UL_LAYOUT_MAX_LEN];
static int g_nlen;
static nr_ul_layout_cell_t g_cell;
static bool g_have_cell;
static uint32_t g_occ[160];
static uint64_t g_gen = 1;
static hyp_state_t g_hyp[NR_UL_LAYOUT_MAX_HYP];
static int g_nhyp;
static int g_locked_hyp = -1;
static uint32_t g_addpos[3], g_ctl_votes, g_discarded, g_tied, g_votes_total, g_probes_fed, g_probes_booked;
static pending_t g_pend[PENDING_MAX];
static int g_npend;
static long g_first_slot = -1, g_lock_slot = -1, g_trial_slot = -1;

#define RELOCK_FAILS 8    /* consecutive CRC failures of a locked layout before alternatives are tried again */
#define TRIAL_MCS_MAX 10  /* trials only on robust (small-MCS) grants */
static bool env_on(const char *name)
{
  const char *s = getenv(name);
  return s && !strcmp(s, "1");
}
bool nr_ul_layout_sib1_first_enabled(void) { return nr_ul_layout_lock_enabled() && env_on("ISAC_UL_LOCK_SIB1_FIRST"); }
bool nr_ul_layout_crc_first_enabled(void) { return nr_ul_layout_lock_enabled() && env_on("ISAC_UL_LOCK_CRC_FIRST"); }

bool nr_ul_layout_lock_enabled(void)
{
  static _Atomic int on = -1;
  int v = atomic_load_explicit(&on, memory_order_relaxed);
  if (v < 0) {
    const char *s = getenv("ISAC_UL_LAYOUT_LOCK");
    v = (s && !strcmp(s, "1")) ? 1 : 0;
    atomic_store_explicit(&on, v, memory_order_relaxed);
  }
  return v == 1;
}

static void reset_locked(void)
{
  memset(g_len, 0, sizeof(g_len));
  g_nlen = 0;
  memset(g_occ, 0, sizeof(g_occ));
  memset(g_hyp, 0, sizeof(g_hyp));
  g_nhyp = 0;
  g_locked_hyp = -1;
  memset(g_addpos, 0, sizeof(g_addpos));
  g_ctl_votes = g_discarded = g_tied = g_votes_total = g_probes_fed = g_probes_booked = 0;
  g_npend = 0;
  g_first_slot = g_lock_slot = g_trial_slot = -1;
  g_have_cell = false;
  g_gen++;
}

void nr_ul_layout_lock_reset(void)
{
  pthread_mutex_lock(&mu);
  reset_locked();
  pthread_mutex_unlock(&mu);
}

void nr_ul_layout_note_pdcch_slot(long abs_slot)
{
  if (!nr_ul_layout_lock_enabled() || abs_slot < 0) return;
  pthread_mutex_lock(&mu);
  const int spf = g_have_cell ? g_cell.slots_per_frame : 0;
  if (spf > 0 && spf <= 160) g_occ[abs_slot % spf]++;
  pthread_mutex_unlock(&mu);
}

static int cand_list(uint16_t *c, int cap)
{
  static const uint16_t std_n[] = {11, 18, 24, 25, 31, 32, 38, 51, 52, 65, 66, 78, 79, 106, 107, 133, 135,
                                   160, 162, 189, 216, 217, 245, 270, 273};
  int n = 0;
  const int car = g_cell.carrier_rb;
  if (nr_ul_layout_sib1_first_enabled() && g_cell.sib1_ul_size > 0 && g_cell.sib1_ul_size <= car && n < cap)
    c[n++] = g_cell.sib1_ul_size;
  for (int k = 0; k <= 5; k++) {
    if (car - k > 0 && n < cap) c[n++] = (uint16_t)(car - k);
    if (g_cell.dl_bwp_size > k && g_cell.dl_bwp_size - k <= car && n < cap) c[n++] = (uint16_t)(g_cell.dl_bwp_size - k);
  }
  for (unsigned i = 0; i < sizeof(std_n) / sizeof(std_n[0]); i++)
    if (std_n[i] <= car && n < cap) c[n++] = std_n[i];
  /* dedupe */
  int m = 0;
  for (int i = 0; i < n; i++) {
    bool dup = false;
    for (int j = 0; j < m; j++) dup |= c[j] == c[i];
    if (!dup) c[m++] = c[i];
  }
  return m;
}

/* caller holds mu. Adds the (N, start) hypotheses for every RIV width used by a fitted length. */
static void build_hyps_locked(void)
{
  uint16_t cn[64];
  const int nc = cand_list(cn, 64);
  /* order: N closest to the carrier first, then start */
  for (int i = 1; i < nc; i++)
    for (int j = i; j > 0 && abs((int)cn[j] - g_cell.carrier_rb) < abs((int)cn[j - 1] - g_cell.carrier_rb); j--) {
      const uint16_t x = cn[j]; cn[j] = cn[j - 1]; cn[j - 1] = x;
    }
  if (nr_ul_layout_sib1_first_enabled() && g_cell.sib1_ul_size > 0
      && g_cell.sib1_ul_start + g_cell.sib1_ul_size <= g_cell.carrier_rb && g_nhyp < NR_UL_LAYOUT_MAX_HYP) {
    bool used = false, have = false;
    for (int l = 0; l < g_nlen; l++)
      for (int p = 0; g_len[l].fitted && p < g_len[l].npre; p++)
        used |= g_len[l].pre[p].riv == nr_ul_layout_riv_bits(g_cell.sib1_ul_size);
    for (int h = 0; h < g_nhyp; h++) have |= g_hyp[h].n == g_cell.sib1_ul_size && g_hyp[h].start == g_cell.sib1_ul_start;
    if (used && !have) { /* hypothesis 0 when the list is still empty (later: ranked first by trial_rank) */
      g_hyp[g_nhyp++] = (hyp_state_t){.n = g_cell.sib1_ul_size, .start = g_cell.sib1_ul_start};
    }
  }
  for (int i = 0; i < nc; i++) {
    bool width_used = false;
    for (int l = 0; l < g_nlen; l++)
      for (int p = 0; g_len[l].fitted && p < g_len[l].npre; p++) width_used |= g_len[l].pre[p].riv == nr_ul_layout_riv_bits(cn[i]);
    if (!width_used) continue;
    for (int s = 0; s <= 5 && s + cn[i] <= g_cell.carrier_rb; s++) {
      bool have = false;
      for (int h = 0; h < g_nhyp; h++) have |= g_hyp[h].n == cn[i] && g_hyp[h].start == s;
      if (!have && g_nhyp < NR_UL_LAYOUT_MAX_HYP) g_hyp[g_nhyp++] = (hyp_state_t){.n = cn[i], .start = (uint16_t)s};
    }
  }
}

/* caller holds mu: PRB range of grant p (length state ls) under hypothesis h. */
static bool hyp_range(const len_state_t *ls, const nr_dci_bits_t *p, int h, uint16_t *r0, uint16_t *nprb)
{
  const int w = nr_ul_layout_riv_bits(g_hyp[h].n);
  for (int k = 0; k < ls->npre; k++) {
    if (ls->pre[k].riv != w) continue;
    uint16_t s, l;
    if (!nr_ul_layout_riv(fld(p, ls->len, 1 + ls->pre[k].bwp_ind, w), g_hyp[h].n, &s, &l)) return false;
    *r0 = (uint16_t)(g_hyp[h].start + s);
    *nprb = l;
    return true;
  }
  return false;
}

static void tail_str(const nr_ul_layout_tail_t *t, char *s, size_t n)
{
  snprintf(s, n, "dai1=%d dai2=%d sri=%d ant=%d srs=2 csi=%d beta=%d init=%d %s cdm=%u cost=%d plaus=%.3f", t->dai1,
           t->dai2, t->sri, t->ant, t->csi, t->beta, t->init, t->tp ? "TP" : "CP", t->cdm, t->cost, t->plaus);
}

/* caller holds mu: build the locked opts of one fitted length once the BWP is locked */
static void lock_length_locked(len_state_t *ls, long now_slot)
{
  if (!ls->fitted || ls->locked || g_locked_hyp < 0) return;
  const hyp_state_t *H = &g_hyp[g_locked_hyp];
  const int w = nr_ul_layout_riv_bits(H->n);
  const nr_ul_layout_prefix_t *pre = NULL;
  for (int k = 0; k < ls->npre; k++) if (ls->pre[k].riv == w) { pre = &ls->pre[k]; break; }
  if (!pre || ls->ntail == 0) return;
  /* tail order: physical CDM evidence, then waveform evidence, then the configuration prior */
  const int cdm_ev = (ls->cdm1 + ls->cdm2 >= CDM_MIN) ? (ls->cdm1 >= 3 * ls->cdm2 ? 1 : ls->cdm2 >= 3 * ls->cdm1 ? 2 : 0) : 0;
  /* CP-OFDM evidence = Gold DM-RS detections beyond the false-detection rate (the control measures one hypothesis per
   * probe; the argmax runs over all of them) */
  const bool cp_ev = ls->gold >= 5 && ls->gold >= 3 + 2 * ls->gctl * (uint32_t)g_nhyp;
  int best = 0, best_key = 1 << 30;
  for (int i = 0; i < ls->ntail; i++) {
    const nr_ul_layout_tail_t *t = &ls->tail[i];
    const int key = (cdm_ev && t->cdm != cdm_ev ? 1000 : 0) + (cp_ev && t->tp ? 100 : 0) + t->cost;
    if (key < best_key) { best_key = key; best = i; }
  }
  int addpos = 1, mx = (int)g_addpos[1]; /* dmrs-AdditionalPosition 1 unless the probes say otherwise */
  for (int i = 0; i < 3; i++) if ((int)g_addpos[i] > mx) { mx = (int)g_addpos[i]; addpos = i; }
  nr_ul_layout_build_opts(pre, &ls->tail[best], H->start, H->n, ls->k2row, addpos, g_cell.pci, g_cell.mu,
                          g_cell.typeA_pos, &ls->opts);
  if (nr_pdcch_blind_dci01_size(&ls->opts) != ls->len) return;
  ls->chosen_tail = best;
  ls->locked = true;
  ls->lock_slot = now_slot;
  char a[160], b[160] = "-";
  tail_str(&ls->tail[best], a, sizeof(a));
  int runner = -1;
  for (int i = 0; i < ls->ntail; i++) if (i != best) { runner = i; break; }
  if (runner >= 0) tail_str(&ls->tail[runner], b, sizeof(b));
  const int spf = g_cell.slots_per_frame ? g_cell.slots_per_frame : 20;
  LOG_A(PHY, "SENSING: ULLAYOUT LOCK len=%u bwp=%u@%u riv=%u bwp_ind=%u tda=%u(k2=%u S0 L14 typeA) fh=%u dmrs_add_pos=%d"
             " tail[%s] runner[%s] cdm_ev[cdm1=%u cdm2=%u] gold=%u mcs_fit=%.2f/%.2f dcis=%u first_abs=%ld lock_abs=%ld"
             " t_conv_ms=%.0f\n",
        ls->len, H->n, H->start, pre->riv, pre->bwp_ind, pre->tda, ls->k2row[0], pre->fh, addpos, a, b, ls->cdm1,
        ls->cdm2, ls->gold, ls->mcs_score, ls->mcs_second, ls->count, ls->first_slot, now_slot,
        10.0 * (double)(now_slot - ls->first_slot) / spf);
}

/* caller holds mu */
static void finalize_pending_locked(void)
{
  if (g_npend == 0) return;
  /* current leader (by votes) for the collision test */
  int lead = -1;
  for (int h = 0; h < g_nhyp; h++) if (g_hyp[h].votes && (lead < 0 || g_hyp[h].votes > g_hyp[lead].votes)) lead = h;
  for (int i = 0; i < g_npend; i++) {
    pending_t *p = &g_pend[i];
    int k = -1;
    for (int h = 0; h < p->n; h++) if (p->snr[h] > -90.f && (k < 0 || p->snr[h] > p->snr[k])) k = h;
    if (k < 0) continue;
    len_state_t *ls = &g_len[p->li];
    if (p->ctl >= VOTE_DB) { g_ctl_votes++; ls->gctl++; }
    if (p->snr[k] < VOTE_DB) continue;
    ls->gold++;
    /* Degenerate mapping: another hypothesis predicts exactly the same PRBs for this grant (for an L-PRB grant
     * (N, s) and (N -+ 1, s -+ (L-1)) coincide). Such a detection cannot tell them apart, so it votes for none
     * (voting for the first in list order handed whole families of wrong hypotheses systematic votes). */
    bool twin = false;
    for (int h = 0; h < p->n && !twin; h++)
      twin = h != k && p->r1[h] != 0 && p->r0[h] == p->r0[k] && p->r1[h] == p->r1[k];
    if (twin) { g_tied++; continue; }
    /* Collision guard: every UE's DM-RS uses the same (PCI) scrambling, so a wrong hypothesis that moves this grant
     * onto another decoded grant of the same slot finds that grant's real DM-RS. Such a detection is ambiguous. */
    bool amb = false;
    for (int j = 0; j < g_npend && !amb; j++) {
      if (j == i) continue;
      /* the other grant's own best hypothesis too: two strong detections of one energy blob under different
       * hypotheses are the same signal */
      int kj = -1;
      for (int h = 0; h < g_pend[j].n; h++)
        if (g_pend[j].snr[h] > -90.f && (kj < 0 || g_pend[j].snr[h] > g_pend[j].snr[kj])) kj = h;
      if (kj >= 0 && g_pend[j].snr[kj] < VOTE_DB) kj = -1;
      const int hs[3] = {k, lead, kj};
      for (int q = 0; q < 3 && !amb; q++) {
        const int h = hs[q];
        if (h < 0 || h >= g_pend[j].n || g_pend[j].r1[h] == 0) continue;
        amb = p->r0[k] < g_pend[j].r1[h] && g_pend[j].r0[h] < p->r1[k];
      }
    }
    if (amb) { g_discarded++; continue; }
    hyp_state_t *H = &g_hyp[k];
    H->votes++;
    g_votes_total++;
    bool seen = false;
    for (int r = 0; r < H->nrnti; r++) seen |= H->rntis[r] == p->rnti;
    if (!seen && H->nrnti < 8) H->rntis[H->nrnti++] = p->rnti;
    if (p->set[k] >= 0 && p->set[k] < 3) g_addpos[p->set[k]]++;
    if (!isnan(p->comb1)) {
      if (p->comb1 > -1.5f) ls->cdm1++;
      else if (p->comb1 < -3.f) ls->cdm2++;
    }
  }
  const long now = g_pend[g_npend - 1].target;
  g_npend = 0;
  if (g_locked_hyp >= 0) return;
  int a = -1, b = -1;
  for (int h = 0; h < g_nhyp; h++) {
    if (a < 0 || g_hyp[h].votes > g_hyp[a].votes) { b = a; a = h; }
    else if (b < 0 || g_hyp[h].votes > g_hyp[b].votes) b = h;
  }
  if (a < 0) return;
  const uint32_t va = g_hyp[a].votes, vb = b >= 0 ? g_hyp[b].votes : 0;
  if (va >= LOCK_MIN && (float)va >= LOCK_RATIO * (float)vb && g_hyp[a].nrnti >= LOCK_RNTIS) {
    g_locked_hyp = a;
    g_lock_slot = now;
    const int spf = g_cell.slots_per_frame ? g_cell.slots_per_frame : 20;
    char rest[256];
    int u = 0;
    for (int h = 0; h < g_nhyp && u < (int)sizeof(rest) - 24; h++)
      if (g_hyp[h].votes && h != a) u += snprintf(rest + u, sizeof(rest) - u, " %u@%u:%u", g_hyp[h].n, g_hyp[h].start, g_hyp[h].votes);
    if (!u) snprintf(rest, sizeof(rest), " -");
    LOG_A(PHY, "SENSING: ULLAYOUT BWP_LOCK bwp=%u@%u votes=%u runner=%u@%u:%u rntis=%u others[%s ] ctl_votes=%u"
               " discarded=%u tied=%u probes=%u hyps=%d addpos_votes=%u/%u/%u first_abs=%ld lock_abs=%ld t_conv_ms=%.0f"
               " confidence=%.2f\n",
          g_hyp[a].n, g_hyp[a].start, va, b >= 0 ? g_hyp[b].n : 0, b >= 0 ? g_hyp[b].start : 0, vb, g_hyp[a].nrnti,
          rest, g_ctl_votes, g_discarded, g_tied, g_probes_fed, g_nhyp, g_addpos[0], g_addpos[1], g_addpos[2], g_first_slot,
          now, 10.0 * (double)(now - g_first_slot) / spf, (float)va / (float)(va + vb + g_ctl_votes));
    for (int l = 0; l < g_nlen; l++) lock_length_locked(&g_len[l], now);
  }
}

static pthread_mutex_t eval_mu = PTHREAD_MUTEX_INITIALIZER; /* serializes eval passes (static scratch below) */
void nr_ul_layout_eval_once(void)
{
  pthread_mutex_lock(&eval_mu);
  static nr_ul_layout_dci_t snap[NR_UL_LAYOUT_BANK];
  for (int l = 0; l < NR_UL_LAYOUT_MAX_LEN; l++) {
    pthread_mutex_lock(&mu);
    if (l >= g_nlen || g_len[l].fitted || g_len[l].count < FIT_MIN || g_len[l].count - g_len[l].last_eval_count < 64
        || !g_have_cell) {
      pthread_mutex_unlock(&mu);
      continue;
    }
    len_state_t *ls = &g_len[l];
    const uint64_t gen = g_gen;
    const uint16_t len = ls->len;
    ls->last_eval_count = ls->count;
    const int n = ls->count < NR_UL_LAYOUT_BANK ? (int)ls->count : NR_UL_LAYOUT_BANK;
    /* chronological copy of the ring */
    const uint32_t first = ls->count < NR_UL_LAYOUT_BANK ? 0 : ls->count % NR_UL_LAYOUT_BANK;
    for (int i = 0; i < n; i++) snap[i] = ls->bank[(first + i) % NR_UL_LAYOUT_BANK];
    uint32_t occ[160];
    memcpy(occ, g_occ, sizeof(occ));
    const int spf = g_cell.slots_per_frame;
    uint16_t cn[64];
    const int nc = cand_list(cn, 64);
    pthread_mutex_unlock(&mu);

    float sc = 0.f, sec = 0.f;
    const int mo = nr_ul_layout_fit_mcs(snap, n, len, &sc, &sec);
    nr_ul_layout_prefix_t pre[NR_UL_LAYOUT_MAX_PRE];
    const int npre = mo > 0 ? nr_ul_layout_fit_prefix(snap, n, len, mo, cn, nc, pre, NR_UL_LAYOUT_MAX_PRE) : 0;
    nr_ul_layout_tail_t tail[NR_UL_LAYOUT_MAX_TAIL];
    const int ntail = npre > 0 ? nr_ul_layout_fit_tail(snap, n, len, &pre[0], tail, NR_UL_LAYOUT_MAX_TAIL) : 0;
    /* k2 per TDRA index (indices read with the first prefix; all prefixes share the TDRA position only when their
     * bwp_ind + riv agree, so the first one is the reference) */
    uint8_t k2row[16] = {0};
    bool k2ok = npre > 0;
    if (npre > 0) {
      static uint32_t hist[16][160];
      memset(hist, 0, sizeof(hist));
      for (int i = 0; i < n; i++) {
        const uint32_t idx = fld(&snap[i].p, len, 1 + pre[0].bwp_ind + pre[0].riv, pre[0].tda);
        if (idx < 16 && spf > 0) hist[idx][snap[i].slot % spf]++;
      }
      for (int r = 0; r < 16; r++) {
        uint32_t tot = 0;
        for (int s = 0; s < spf; s++) tot += hist[r][s];
        if (tot < 32) continue;
        k2row[r] = (uint8_t)nr_ul_layout_infer_k2(occ, hist[r], spf);
        if (!k2row[r]) k2ok = false;
      }
      if (!k2row[0]) for (int r = 0; r < 16 && !k2row[0]; r++) k2row[0] = k2row[r];
      if (!k2row[0]) k2ok = false;
    }
    pthread_mutex_lock(&mu);
    if (gen == g_gen && !ls->fitted) {
      ls->mcs_off = mo;
      ls->mcs_score = sc;
      ls->mcs_second = sec;
      if (mo > 0 && npre > 0 && ntail > 0 && k2ok) {
        ls->npre = npre;
        memcpy(ls->pre, pre, sizeof(pre));
        ls->ntail = ntail;
        memcpy(ls->tail, tail, sizeof(tail));
        memcpy(ls->k2row, k2row, sizeof(k2row));
        ls->fitted = true;
        ls->fit_slot = snap[n - 1].slot;
        build_hyps_locked();
        char pr[160];
        int u = 0;
        for (int k = 0; k < npre; k++)
          u += snprintf(pr + u, sizeof(pr) - u, "%s{bwp_ind=%u riv=%u tda=%u fh=%u Nmax=%u}", k ? "," : "",
                        pre[k].bwp_ind, pre[k].riv, pre[k].tda, pre[k].fh, pre[k].n_max);
        char t0[160], t1[160] = "-";
        tail_str(&tail[0], t0, sizeof(t0));
        if (ntail > 1) tail_str(&tail[1], t1, sizeof(t1));
        LOG_A(PHY, "SENSING: ULLAYOUT FIT len=%u dcis=%d mcs_off=%d score=%.2f second=%.2f prefixes=[%s] k2_row0=%u"
                   " tails=%d best[%s] next[%s] bwp_hyps=%d first_abs=%ld fit_abs=%ld\n",
              len, n, mo, sc, sec, pr, k2row[0], ntail, t0, t1, g_nhyp, ls->first_slot, ls->fit_slot);
        lock_length_locked(ls, ls->fit_slot);
      } else {
        LOG_I(PHY, "SENSING: ULLAYOUT FIT len=%u dcis=%d not yet: mcs_off=%d (%.2f/%.2f) prefixes=%d tails=%d k2=%d\n", len,
              n, mo, sc, sec, npre, ntail, k2ok);
      }
    }
    pthread_mutex_unlock(&mu);
  }
  pthread_mutex_unlock(&eval_mu);
}

static void ul_layout_start_thread(void);
static void *eval_thread(void *arg)
{
  (void)arg;
  for (;;) {
    usleep(100000);
    nr_ul_layout_eval_once();
  }
  return NULL;
}

int nr_ul_layout_on_dci(uint16_t rnti, uint16_t len, long abs_slot, nr_dci_bits_t payload,
                        const nr_ul_layout_cell_t *cell, nr_pdcch_blind_ul_result_t *out)
{
  if (!nr_ul_layout_lock_enabled() || !cell || !out || !rnti || !len || len > NR_DCI_MAX_PAYLOAD || abs_slot < 0
      || cell->carrier_rb < 1 || cell->carrier_rb > 275 || cell->slots_per_frame == 0 || cell->slots_per_frame > 160)
    return 0;
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, ul_layout_start_thread);
  pthread_mutex_lock(&mu);
  if (g_have_cell && (cell->pci != g_cell.pci || cell->carrier_rb != g_cell.carrier_rb
                      || cell->slots_per_frame != g_cell.slots_per_frame)) {
    LOG_A(PHY, "SENSING: ULLAYOUT reset: cell changed (PCI %u->%u, carrier %u->%u)\n", g_cell.pci, cell->pci,
          g_cell.carrier_rb, cell->carrier_rb);
    reset_locked();
  }
  if (!g_have_cell) { g_cell = *cell; g_have_cell = true; }
  if (g_first_slot < 0) g_first_slot = abs_slot;
  g_occ[abs_slot % g_cell.slots_per_frame]++; /* a DCI 0_1 was here: the slot has DL symbols */
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == len) { li = l; break; }
  if (li < 0 && g_nlen < NR_UL_LAYOUT_MAX_LEN) {
    li = g_nlen++;
    memset(&g_len[li], 0, sizeof(g_len[li]));
    g_len[li].len = len;
    g_len[li].first_slot = abs_slot;
    g_len[li].chosen_tail = -1;
  }
  if (li < 0) { pthread_mutex_unlock(&mu); return 0; }
  len_state_t *ls = &g_len[li];
  ls->bank[ls->count % NR_UL_LAYOUT_BANK] = (nr_ul_layout_dci_t){.rnti = rnti, .slot = abs_slot, .p = payload};
  ls->count++;
  int ret = 0;
  if (ls->locked) {
    const nr_pdcch_blind_ul_opts_t o = ls->opts;
    const uint64_t gen = g_gen;
    pthread_mutex_unlock(&mu);
    if (!nr_pdcch_blind_extract_01(payload, len, rnti, &o, out)) return 0;
    out->hyp_generation = NR_UL_LAYOUT_TAG | gen;
    return 1;
  }
  if (ls->fitted && g_locked_hyp < 0 && g_nhyp > 0) {
    const nr_ul_layout_prefix_t *p0 = &ls->pre[0];
    const uint32_t idx = fld(&payload, len, 1 + p0->bwp_ind + p0->riv, p0->tda);
    const uint8_t k2 = idx < 16 && ls->k2row[idx] ? ls->k2row[idx] : ls->k2row[0];
    int testable = 0;
    uint16_t r0 = 0, nr = 0;
    for (int h = 0; h < g_nhyp; h++) {
      uint16_t a, b;
      if (hyp_range(ls, &payload, h, &a, &b) && b <= PROBE_MAX_RB) { if (!testable) { r0 = a; nr = b; } testable++; }
    }
    if (k2 && testable) {
      memset(out, 0, sizeof(*out));
      out->raw_payload = payload;
      out->dci_length = len;
      out->rnti = out->crc_rnti = rnti;
      out->ul_dci_format = NR_BLIND_UL_DCI_FORMAT_0_1;
      out->width_hyp_class = out->interp_hyp_class = -1;
      out->plausible = true;
      out->k2 = k2;
      out->bwp_start = 0;
      out->bwp_size = g_cell.carrier_rb;
      out->start_rb = r0;
      out->num_rb = nr;
      out->start_symbol = 0;
      out->num_symbols = 14;
      out->nrOfLayers = 1;
      out->n_dmrs_cdm_groups = 1;
      out->dmrs_ports = 1;
      out->ul_dmrs_scrambling_id = out->data_scrambling_id = g_cell.pci;
      out->hyp_generation = NR_UL_LAYOUT_TAG | NR_UL_LAYOUT_PROBE | g_gen;
      ls->probes++;
      g_probes_booked++;
      ret = 2;
    }
  }
  pthread_mutex_unlock(&mu);
  return ret;
}

static void ul_layout_start_thread(void)
{
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &at, eval_thread, NULL) != 0)
    LOG_E(PHY, "SENSING: ULLAYOUT eval thread could not start: the lock will never fit\n");
  pthread_attr_destroy(&at);
}

int nr_ul_layout_probe_plan(const nr_pdcch_blind_ul_result_t *g, uint16_t *prb0, uint16_t *nprb, int cap,
                            uint16_t *nid, uint8_t *l0)
{
  if (!nr_ul_layout_is_probe(g)) return 0;
  pthread_mutex_lock(&mu);
  int n = 0;
  if ((g->hyp_generation & ~(NR_UL_LAYOUT_TAG | NR_UL_LAYOUT_PROBE)) == g_gen && g_locked_hyp < 0) {
    for (int l = 0; l < g_nlen; l++) {
      if (g_len[l].len != g->dci_length || !g_len[l].fitted) continue;
      n = g_nhyp < cap ? g_nhyp : cap;
      for (int h = 0; h < n; h++) {
        uint16_t a = 0, b = 0;
        if (!hyp_range(&g_len[l], &g->raw_payload, h, &a, &b) || b > PROBE_MAX_RB) b = 0;
        prb0[h] = a;
        nprb[h] = b;
      }
      break;
    }
    if (nid) *nid = g_cell.pci;
    if (l0) *l0 = g_cell.typeA_pos ? 3 : 2;
  }
  pthread_mutex_unlock(&mu);
  return n;
}

void nr_ul_layout_probe_feed(const nr_pdcch_blind_ul_result_t *g, long target, const nr_ul_layout_probe_res_t *r)
{
  if (!nr_ul_layout_is_probe(g) || !r) return;
  pthread_mutex_lock(&mu);
  if ((g->hyp_generation & ~(NR_UL_LAYOUT_TAG | NR_UL_LAYOUT_PROBE)) != g_gen || g_locked_hyp >= 0) {
    pthread_mutex_unlock(&mu);
    return;
  }
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == g->dci_length) li = l;
  if (li < 0) { pthread_mutex_unlock(&mu); return; }
  g_probes_fed++;
  { /* ISAC_UL_LAYOUT_LOG=1 (diagnostic, default off): one line per probe, the three best hypotheses */
    static int lg = -1;
    if (lg < 0) lg = getenv("ISAC_UL_LAYOUT_LOG") && atoi(getenv("ISAC_UL_LAYOUT_LOG")) ? 1 : 0;
    if (lg) {
      int b0 = -1, b1 = -1, b2 = -1;
      for (int h = 0; h < r->n && h < g_nhyp; h++) {
        if (b0 < 0 || r->snr_db[h] > r->snr_db[b0]) { b2 = b1; b1 = b0; b0 = h; }
        else if (b1 < 0 || r->snr_db[h] > r->snr_db[b1]) { b2 = b1; b1 = h; }
        else if (b2 < 0 || r->snr_db[h] > r->snr_db[b2]) b2 = h;
      }
      if (b0 >= 0)
        LOG_A(PHY, "SENSING: ULLAYOUT_PROBE rnti=0x%04x len=%u target=%ld best=%u@%u:%.1f/s%d second=%u@%u:%.1f third=%u@%u:%.1f"
                   " ctl=%.1f comb1=%.1f\n",
              g->rnti, g->dci_length, target, g_hyp[b0].n, g_hyp[b0].start, r->snr_db[b0], r->set[b0],
              b1 >= 0 ? g_hyp[b1].n : 0, b1 >= 0 ? g_hyp[b1].start : 0, b1 >= 0 ? r->snr_db[b1] : -99.f,
              b2 >= 0 ? g_hyp[b2].n : 0, b2 >= 0 ? g_hyp[b2].start : 0, b2 >= 0 ? r->snr_db[b2] : -99.f, r->ctl_db,
              r->comb1_db);
    }
  }
  if (g_npend > 0 && (target != g_pend[0].target || g_npend == PENDING_MAX)) finalize_pending_locked();
  if (g_locked_hyp < 0) {
    pending_t *p = &g_pend[g_npend++];
    memset(p, 0, sizeof(*p));
    p->target = target;
    p->rnti = g->rnti;
    p->li = li;
    p->n = r->n < g_nhyp ? r->n : g_nhyp;
    for (int h = 0; h < p->n; h++) {
      p->snr[h] = r->snr_db[h];
      p->set[h] = r->set[h];
      uint16_t a = 0, b = 0;
      if (hyp_range(&g_len[li], &g->raw_payload, h, &a, &b)) { p->r0[h] = a; p->r1[h] = (uint16_t)(a + b); }
    }
    p->ctl = r->ctl_db;
    p->comb1 = r->comb1_db;
  }
  pthread_mutex_unlock(&mu);
}


/* ------------------------------------------------------------------ CRC-first (ISAC_UL_LOCK_CRC_FIRST=1) */
static int cur_addpos_locked(void)
{
  int addpos = 1, mx = (int)g_addpos[1];
  for (int i = 0; i < 3; i++) if ((int)g_addpos[i] > mx) { mx = (int)g_addpos[i]; addpos = i; }
  return addpos;
}

/* caller holds mu: first prefix of ls whose RIV width matches BWP hypothesis h, or -1 */
static int pre_for_hyp(const len_state_t *ls, int h)
{
  const int w = nr_ul_layout_riv_bits(g_hyp[h].n);
  for (int k = 0; k < ls->npre; k++) if (ls->pre[k].riv == w) return k;
  return -1;
}

/* caller holds mu: build the opts of hypothesis (h, prefix k, tail t); false when it does not read `ls->len` */
static bool trial_opts_locked(const len_state_t *ls, int h, int k, int t, nr_pdcch_blind_ul_opts_t *o)
{
  if (h < 0 || h >= g_nhyp || k < 0 || k >= ls->npre || t < 0 || t >= ls->ntail) return false;
  nr_ul_layout_build_opts(&ls->pre[k], &ls->tail[t], g_hyp[h].start, g_hyp[h].n, ls->k2row, cur_addpos_locked(), g_cell.pci,
                          g_cell.mu, g_cell.typeA_pos, o);
  return nr_pdcch_blind_dci01_size(o) == ls->len;
}

int nr_ul_layout_crc_trials(uint16_t rnti, uint16_t len, long abs_slot, nr_dci_bits_t payload,
                            const nr_ul_layout_cell_t *cell, nr_pdcch_blind_ul_result_t *out, int cap)
{
  if (!nr_ul_layout_crc_first_enabled() || !cell || !out || cap < 1 || abs_slot < 0) return 0;
  int K = getenv("ISAC_UL_LOCK_CRC_K") ? atoi(getenv("ISAC_UL_LOCK_CRC_K")) : 3;
  if (K < 1) K = 1;
  if (K > NR_UL_LAYOUT_MAX_TRIALS) K = NR_UL_LAYOUT_MAX_TRIALS;
  if (K > cap) K = cap;
  int n = 0;
  pthread_mutex_lock(&mu);
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == len) li = l;
  len_state_t *ls = li >= 0 ? &g_len[li] : NULL;
  /* one DCI per slot (<= K jobs), small MCS only; locked layouts are re-tried only after a run of CRC failures */
  if (!g_have_cell || !ls || !ls->fitted || g_nhyp == 0 || g_trial_slot == abs_slot
      || (ls->locked && ls->crc_fail_run < RELOCK_FAILS) || fld(&payload, len, ls->mcs_off, 5) > TRIAL_MCS_MAX) {
    pthread_mutex_unlock(&mu);
    return 0;
  }
  /* rank BWP hypotheses: SIB1 UL BWP first, then DM-RS votes, then list order */
  int rk[NR_UL_LAYOUT_MAX_HYP], nr = 0;
  for (int h = 0; h < g_nhyp; h++) if (pre_for_hyp(ls, h) >= 0) rk[nr++] = h;
  const bool sib1 = nr_ul_layout_sib1_first_enabled() && g_cell.sib1_ul_size > 0;
  for (int i = 1; i < nr; i++)
    for (int j = i; j > 0; j--) {
      const int a = rk[j], b = rk[j - 1];
      const bool sa = sib1 && g_hyp[a].n == g_cell.sib1_ul_size && g_hyp[a].start == g_cell.sib1_ul_start;
      const bool sb = sib1 && g_hyp[b].n == g_cell.sib1_ul_size && g_hyp[b].start == g_cell.sib1_ul_start;
      if (!(sa > sb || (sa == sb && g_hyp[a].votes > g_hyp[b].votes))) break;
      rk[j] = b; rk[j - 1] = a;
    }
  const uint64_t gen = g_gen & NR_UL_LAYOUT_GEN_MASK;
  for (int t = 0; t < ls->ntail && t < 2 && n < K; t++)
    for (int i = 0; i < nr && n < K; i++) {
      const int h = rk[i], k = pre_for_hyp(ls, h);
      if (ls->locked && g_locked_hyp == h && ls->chosen_tail == t) continue; /* the failing layout itself */
      nr_pdcch_blind_ul_opts_t o;
      if (!trial_opts_locked(ls, h, k, t, &o) || !nr_pdcch_blind_extract_01(payload, len, rnti, &o, &out[n])) continue;
      out[n].hyp_generation = NR_UL_LAYOUT_TAG | NR_UL_LAYOUT_TRIAL | ((uint64_t)(h | k << 6 | t << 9) << 40) | gen;
      n++;
    }
  if (n) { g_trial_slot = abs_slot; ls->trials += (uint32_t)n; }
  pthread_mutex_unlock(&mu);
  return n;
}

/* caller holds mu: a TB CRC passed under (h, k, t): this layout is the lock (BWP, prefix, tail, k2/TDRA) */
static void crc_lock_locked(int li, int h, int k, int t, long slot)
{
  len_state_t *ls = &g_len[li];
  nr_pdcch_blind_ul_opts_t o;
  if (!trial_opts_locked(ls, h, k, t, &o)) return;
  const bool relock = ls->locked;
  ls->opts = o;
  ls->chosen_tail = t;
  ls->locked = true;
  ls->lock_slot = slot;
  ls->crc_fail_run = 0;
  ls->crc_locks++;
  if (g_locked_hyp != h) {
    g_locked_hyp = h;
    for (int l = 0; l < g_nlen; l++) if (l != li) g_len[l].locked = false; /* re-derived under the new BWP */
    for (int l = 0; l < g_nlen; l++) lock_length_locked(&g_len[l], slot);
  }
  g_lock_slot = slot;
  char tl[160];
  tail_str(&ls->tail[t], tl, sizeof(tl));
  LOG_A(PHY, "SENSING: ULLAYOUT_CRC_LOCK%s len=%u bwp=%u@%u riv=%u bwp_ind=%u tda=%u(k2=%u) fh=%u tail[%s] crc_ok=%u"
             " trials=%u abs=%ld\n",
        relock ? " RELOCK" : "", ls->len, g_hyp[h].n, g_hyp[h].start, ls->pre[k].riv, ls->pre[k].bwp_ind, ls->pre[k].tda,
        ls->k2row[0], ls->pre[k].fh, tl, ls->crc_ok, ls->trials, slot);
}

void nr_ul_layout_crc_result(const nr_pdcch_blind_ul_result_t *g, bool ok)
{
  if (!nr_ul_layout_is_tagged(g) || nr_ul_layout_is_probe(g) || !nr_ul_layout_crc_first_enabled()) return;
  pthread_mutex_lock(&mu);
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == g->dci_length) li = l;
  if (li >= 0 && (g->hyp_generation & NR_UL_LAYOUT_GEN_MASK) == (g_gen & NR_UL_LAYOUT_GEN_MASK)) {
    len_state_t *ls = &g_len[li];
    if (nr_ul_layout_is_trial(g)) {
      const unsigned e = (unsigned)((g->hyp_generation >> 40) & 0xfff);
      const int h = e & 63, k = (e >> 6) & 7, t = (e >> 9) & 3;
      if (ok) {
        if (ls->locked && g_locked_hyp == h && ls->chosen_tail == t) { ls->crc_ok++; ls->crc_fail_run = 0; }
        else crc_lock_locked(li, h, k, t, ls->bank[(ls->count - 1) % NR_UL_LAYOUT_BANK].slot);
      }
    } else if (ls->locked) {
      if (ok) { ls->crc_ok++; ls->crc_fail_run = 0; }
      else if (ls->crc_fail_run < 1000000) ls->crc_fail_run++;
    }
  }
  pthread_mutex_unlock(&mu);
}

void nr_ul_layout_stats_dump(void)
{
  if (!nr_ul_layout_lock_enabled()) return;
  pthread_mutex_lock(&mu);
  char v[512];
  int u = 0;
  for (int h = 0; h < g_nhyp && u < (int)sizeof(v) - 24; h++)
    if (g_hyp[h].votes) u += snprintf(v + u, sizeof(v) - u, " %u@%u:%u", g_hyp[h].n, g_hyp[h].start, g_hyp[h].votes);
  if (!u) snprintf(v, sizeof(v), " -");
  char ls[256];
  int w = 0;
  for (int l = 0; l < g_nlen && w < (int)sizeof(ls) - 64; l++)
    w += snprintf(ls + w, sizeof(ls) - w, " len%u[dcis=%u fit=%d lock=%d probes=%u gold=%u cdm1=%u cdm2=%u trials=%u crc_ok=%u crc_locks=%u]",
                  g_len[l].len, g_len[l].count, g_len[l].fitted, g_len[l].locked, g_len[l].probes, g_len[l].gold,
                  g_len[l].cdm1, g_len[l].cdm2, g_len[l].trials, g_len[l].crc_ok, g_len[l].crc_locks);
  LOG_A(PHY, "SENSING: ULLAYOUT STATS bwp=%s%u@%u probes[booked=%u fed=%u] votes=%u ctl_votes=%u discarded=%u tied=%u hyps=%d"
             " tally[%s ]%s\n",
        g_locked_hyp >= 0 ? "LOCKED " : "searching ", g_locked_hyp >= 0 ? g_hyp[g_locked_hyp].n : 0,
        g_locked_hyp >= 0 ? g_hyp[g_locked_hyp].start : 0, g_probes_booked, g_probes_fed, g_votes_total, g_ctl_votes,
        g_discarded, g_tied, g_nhyp, v, w ? ls : "");
  pthread_mutex_unlock(&mu);
}
