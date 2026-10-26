/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* UL-LAYOUT: blind UL BWP / DCI 0_1 layout lock. See the header for the method. */
#include "nr_pdcch_ul_layout_lock.h"
#include "nr_ul_trial_variant.h"
#include "nr_pdsch_prb_set.h" /* NR_FDRA_TYPE1 */
#include "nr_blind_stats.h"
#include "nr_ul_layout_lookalike.h"
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
static float mcs_score(const nr_ul_layout_dci_t *b, int n, uint16_t len, int o, int *pairs_out, int *ok_out, int *rv0_out)
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
  if (ok_out) *ok_out = ok;
  if (rv0_out) *rv0_out = rv0;
  return pairs ? (float)ok / pairs + (float)rv0 / n : 0.f;
}

/* BLINDFIX F5: the offset is accepted on significance, not on scores fitted to PCI 415 traffic.
 * Null (random bits): a pid step lands in 1..3 with p = 3/16, RV is 0 with p = 1/4. The best offset must have BOTH counts
 * significant at alpha / (number of offsets) (a constant field has RV0 100 % but no pid steps), and must beat the runner-up
 * offset on the pooled success count (a one-bit shift carries part of the signal and is not the field). */
int nr_ul_layout_fit_mcs(const nr_ul_layout_dci_t *b, int n, uint16_t len, float *score, float *second)
{
  float best = -1.f, sec = -1.f;
  int bo = -1, best_pairs = 0, best_ok = 0, best_rv = 0, sec_pairs = 0, sec_ok = 0, sec_rv = 0, nofs = 0;
  for (int o = 2; o + 12 <= len; o++) {
    int pairs = 0, ok = 0, rv0 = 0;
    const float s = mcs_score(b, n, len, o, &pairs, &ok, &rv0);
    nofs++;
    if (s > best) {
      sec = best; sec_pairs = best_pairs; sec_ok = best_ok; sec_rv = best_rv;
      best = s; bo = o; best_pairs = pairs; best_ok = ok; best_rv = rv0;
    } else if (s > sec) { sec = s; sec_pairs = pairs; sec_ok = ok; sec_rv = rv0; }
  }
  if (score) *score = best;
  if (second) *second = sec;
  if (bo < 0 || best_pairs < 8) return -1;
  const double a = 1e-3 / (double)(nofs > 0 ? nofs : 1);
  if (!(nbs_binom_sf((uint64_t)best_ok, (uint64_t)best_pairs, 3.0 / 16.0) < a && nbs_binom_sf((uint64_t)best_rv, (uint64_t)n, 0.25) < a))
    return -1;
  if (sec >= 0.f && !nbs_beats_runner((uint64_t)(best_ok + best_rv), (uint64_t)(best_pairs + n), (uint64_t)(sec_ok + sec_rv),
                                      (uint64_t)(sec_pairs + n), a))
    return -1;
  return bo;
}

/* Ranking prior for the MCS block offset (used only when the significance test above leaves no unique winner; the CRC then decides).
 * Score = pid-step fraction + RV==0 fraction + NDI/RV consistency fraction. The pid-step term (1..3) is a habit of some schedulers, not
 * a rule (TS 38.212 / 38.321 let the gNB use any HARQ process id), so it is only one of three terms and carries no requirement here.
 * The consistency term is the 38.321 NDI rule itself: when a HARQ process id comes back for the same RNTI, the NDI is toggled for new
 * data (RV 0 first) and kept for a retransmission (RV != 0). A constant field fails it (same NDI, RV 0), random bits meet it half of
 * the time, the true block almost always. */
static float mcs_rank_score(const nr_ul_layout_dci_t *b, int n, uint16_t len, int o)
{
  uint16_t rn[64];
  int8_t last[64];
  long last_slot[64];
  int8_t ndi_of[64][16];
  int nr = 0, ok = 0, pairs = 0, rv0 = 0, cons = 0, cpairs = 0;
  uint32_t pid_seen = 0;
  for (int i = 0; i < n; i++) {
    const uint32_t pid = fld(&b[i].p, len, o + 8, 4), rv = fld(&b[i].p, len, o + 6, 2), ndi = fld(&b[i].p, len, o + 5, 1);
    pid_seen |= 1u << pid;
    rv0 += rv == 0;
    int k = 0;
    while (k < nr && rn[k] != b[i].rnti) k++;
    if (k == nr) {
      if (nr == 64) continue;
      rn[nr] = b[i].rnti;
      last[nr] = -1;
      memset(ndi_of[nr], -1, sizeof(ndi_of[nr]));
      nr++;
    }
    if (last[k] >= 0 && last_slot[k] != b[i].slot) {
      const int d = ((int)pid - last[k] + 16) % 16;
      pairs++;
      ok += d >= 1 && d <= 3;
    }
    if (ndi_of[k][pid] >= 0 && last_slot[k] != b[i].slot) {
      cpairs++;
      cons += (ndi != (uint32_t)ndi_of[k][pid]) == (rv == 0);
    }
    ndi_of[k][pid] = (int8_t)ndi;
    last[k] = (int8_t)pid;
    last_slot[k] = b[i].slot;
  }
  /* RV == 0 everywhere is also what a block of constant-zero bits shows (a reserved or unused DCI region scored 1.02 / 1.50, tied, ahead of the
   * true block in i7R4_lab_20261026_021625): it counts only where the HARQ process id field takes at least two values, as a real HARQ id does. */
  const bool pid_varies = (pid_seen & (pid_seen - 1u)) != 0;
  return (pairs ? (float)ok / pairs : 0.f) + (pid_varies ? (float)rv0 / (n ? n : 1) : 0.f) + (cpairs ? (float)cons / cpairs : 0.f);
}

int nr_ul_layout_rank_mcs(const nr_ul_layout_dci_t *b, int n, uint16_t len, int *offs, float *score, int cap)
{
  if (!b || n <= 0 || !offs || !score || cap < 1) return 0;
  int m = 0;
  for (int o = 2; o + 12 <= len; o++) {
    const float s = mcs_rank_score(b, n, len, o);
    if (m == cap && s <= score[cap - 1]) continue;
    int j = m < cap ? m++ : cap - 1;
    for (; j > 0 && score[j - 1] < s; j--) { score[j] = score[j - 1]; offs[j] = offs[j - 1]; }
    score[j] = s;
    offs[j] = o;
  }
  return m;
}

static float bit_entropy(float p)
{
  return (p <= 0.f || p >= 1.f) ? 0.f : -(p * log2f(p) + (1.f - p) * log2f(1.f - p));
}
#define NR_UL_PREFIX_ENT_MARGIN 0.5f

static void p1_stats(const nr_ul_layout_dci_t *b, int n, uint16_t len, float *p1)
{
  for (int i = 0; i < len; i++) p1[i] = 0.f;
  for (int k = 0; k < n; k++)
    for (int i = 0; i < len; i++) p1[i] += (float)fld(&b[k].p, len, i, 1);
  for (int i = 0; i < len; i++) p1[i] /= (float)(n ? n : 1);
}

static int fit_prefix_stat(const nr_ul_layout_dci_t *b, int n, uint16_t len, int mcs_off, const uint16_t *cand_n,
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
        if (cnt < cap) {
          /* entropy (bits) of the TDRA + hopping bits this split leaves: a TDRA / hopping field is near-constant, a RIV LSB
           * is uniform, so the split that absorbed a RIV LSB into TDRA (riv one bit too short) carries ~1 bit more */
          float ent = 0.f;
          for (int i = 1 + bw + w; i < mcs_off; i++) ent += bit_entropy(p1[i]);
          out[cnt++] = (nr_ul_layout_prefix_t){.bwp_ind = (uint8_t)bw, .riv = (uint8_t)w,
                                               .tda = (uint8_t)(rest > 4 ? 4 : rest),
                                               .fh = (uint8_t)(rest > 4 ? rest - 4 : 0), .n_max = nmax, .ent = ent};
        }
      }
  /* ULDCI-20261014: rank the splits by that entropy and drop those more than
   * NR_UL_PREFIX_ENT_MARGIN bits above the best. Measured on TIM PCI 923 / Vodafone PCI 415 (replay + OTA 10-14): the
   * order used to be RIV width ascending, so {riv=14, tda=4} (N <= 162: 107@x, 162@x, 178@39 locks) beat the true
   * {riv=15, tda=3}; those false widths also seeded the BWP hypotheses that win the thin-evidence votes. */
  if (cnt > 1) {
    for (int i = 1; i < cnt; i++)
      for (int j = i; j > 0 && (out[j].ent < out[j - 1].ent - 0.05f
                                || (out[j].ent <= out[j - 1].ent + 0.05f && out[j].riv > out[j - 1].riv)); j--) {
        /* equal entropy: the wider RIV (a leading RIV MSB that is 0 in a short bank is not a constant BWP indicator bit) */
        const nr_ul_layout_prefix_t x = out[j]; out[j] = out[j - 1]; out[j - 1] = x;
      }
    int keep = 1;
    while (keep < cnt && out[keep].ent <= out[0].ent + NR_UL_PREFIX_ENT_MARGIN) keep++;
    return keep;
  }
  return cnt;
}

/* Statistics rank the splits; they must not be able to remove the BWP the receiver already expects. A bank dominated by a few full-band
 * grants has a constant RIV (S = 0, L = 273 is RIV 545), so the RIV-LSB entropy gate drops the cell's own split [MEASURED, sens6 OTA lab,
 * i7R4_lab_20261026_021625: mcs_off 20 found with prefixes {162,107,79}, the 273 split (riv=16) missing in every candidate, PUSCH
 * try=7036 crc_ok=0]. Every width a prior BWP size needs (the first candidate N: the SIB1 UL BWP, else the carrier; and the largest
 * candidate, the carrier) therefore stays a hypothesis for every legal BWP-indicator width (the leading constant bits), whatever the
 * entropy/validity gates said; the PUSCH CRC decides. */
int nr_ul_layout_fit_prefix(const nr_ul_layout_dci_t *b, int n, uint16_t len, int mcs_off, const uint16_t *cand_n,
                            int n_cand, nr_ul_layout_prefix_t *out, int cap)
{
  int cnt = fit_prefix_stat(b, n, len, mcs_off, cand_n, n_cand, out, cap);
  if (!b || n <= 0 || mcs_off < 2 || len > NR_DCI_MAX_PAYLOAD || n_cand <= 0 || !cand_n) return cnt;
  float p1[NR_DCI_MAX_PAYLOAD];
  p1_stats(b, n, len, p1);
  int nconst = 0;
  while (nconst < 2 && 1 + nconst < mcs_off && (p1[1 + nconst] <= 0.02f || p1[1 + nconst] >= 0.98f)) nconst++;
  uint16_t pri[2] = {cand_n[0], 0};
  for (int i = 0; i < n_cand; i++) if (cand_n[i] > pri[1]) pri[1] = cand_n[i];
  for (int q = 0; q < 2; q++) {
    const int w = nr_ul_layout_riv_bits(pri[q]);
    uint16_t nmax = 0;
    for (int i = 0; i < n_cand; i++) if (nr_ul_layout_riv_bits(cand_n[i]) == w && cand_n[i] > nmax) nmax = cand_n[i];
    for (int bw = 0; bw <= nconst; bw++) { /* no BWP indicator first: the common configuration */
      const int rest = mcs_off - 1 - bw - w;
      if (!nmax || rest < 0 || rest > 5 || cnt >= cap) continue;
      bool have = false;
      for (int k = 0; k < cnt; k++) have |= out[k].bwp_ind == bw && out[k].riv == w;
      if (have) continue;
      float ent = 0.f;
      for (int i = 1 + bw + w; i < mcs_off; i++) ent += bit_entropy(p1[i]);
      out[cnt++] = (nr_ul_layout_prefix_t){.bwp_ind = (uint8_t)bw, .riv = (uint8_t)w, .tda = (uint8_t)(rest > 4 ? 4 : rest),
                                           .fh = (uint8_t)(rest > 4 ? rest - 4 : 0), .n_max = nmax, .ent = ent};
    }
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
#define CRC_EV_MAX 64
typedef struct {
  uint16_t len;
  uint32_t count; /* DCIs seen (ring index = count % BANK) */
  nr_ul_layout_dci_t bank[NR_UL_LAYOUT_BANK];
  long first_slot, fit_slot, lock_slot;
  uint32_t last_eval_count;
  bool fitted;
  bool prov;      /* the fit rests on a ranking prior (no significant MCS offset): only a CRC pass may lock it; probes, votes and fit-reads stay off */
  int cand[NR_UL_LAYOUT_MAX_CAND], ncand, cand_i; /* ranked MCS offsets of a provisional fit, the one under CRC test */
  uint32_t prov_ctr0, prov_budget;  /* g_var_ctr when the candidate went under test; trial verdicts (CRC pass or fail of a trial decode) it gets */
  uint32_t prov_done;               /* trial verdicts the candidate under test has received */
  uint32_t prov_cycle;              /* completed passes over the ranked list: the per-candidate slice doubles every pass (iterative deepening) */
  uint32_t prov_rot;                /* candidates abandoned */
  int mcs_off;
  float mcs_score, mcs_second;
  int npre;
  nr_ul_layout_prefix_t pre[NR_UL_LAYOUT_MAX_PRE];
  int ntail;
  nr_ul_layout_tail_t tail[NR_UL_LAYOUT_MAX_TAIL];
  uint8_t k2row[16];
  uint32_t gold, gctl, cdm1, cdm2, probes;
  uint32_t crc_ok, crc_fail_run, trials, crc_locks;
  uint32_t lock_decisive; /* decisive PUSCH CRC passes (CRC24 / all CB CRCs) of the CURRENT lock: 0 = the lock rests on votes or chance-level CRC16 passes only */
  bool locked;
  nr_pdcch_blind_ul_opts_t opts;
  int chosen_tail;
  int chosen_var; /* trial variant (vt | k2d << 3) of the CRC-locked layout; 0 = default table / k2 */
  struct { uint32_t e; uint32_t trials, passes; uint16_t rnti[4]; long slot[4]; uint8_t nr, ns; } ev[CRC_EV_MAX];
  uint32_t ev_susp, ev_dec, ev_lock; /* CRC guard stats: suppressed passes, decisive passes, evidence locks */
} len_state_t;

typedef struct { uint16_t n, start; uint32_t votes; uint16_t rntis[8]; uint8_t nrnti; uint32_t det; } hyp_state_t;
/* g_only[a][b] = probes where hypothesis a detected a DM-RS (>= VOTE_DB) and b (which had PRBs) did not */
static uint16_t g_only[NR_UL_LAYOUT_MAX_HYP][NR_UL_LAYOUT_MAX_HYP];
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
static int g_lat_var;          /* latched trial variant (vt | k2d << 3), applied to every length's locked opts */
static uint32_t g_var_ctr;     /* trial-slot counter: the variant rotates per trial slot */
static uint32_t g_probes_skipped, g_addpos[3], g_ctl_votes, g_discarded, g_tied, g_votes_total, g_probes_fed, g_probes_booked;
static pending_t g_pend[PENDING_MAX];
static int g_npend;
static long g_first_slot = -1, g_lock_slot = -1, g_trial_slot = -1;
static void lock_length_locked(len_state_t *ls, long now_slot);
static int pre_for_hyp(const len_state_t *ls, int h);

#define RELOCK_FAILS 8    /* consecutive CRC failures of a locked layout before alternatives are tried again */
#define TRIAL_MCS_MAX 10  /* trials only on robust (small-MCS) grants */
/* slot-level disjointness: PUSCH grants of different UEs in one slot do not overlap in frequency under the true BWP reading */
/* The SPRT / edge / coherent lock statistics are not used by the receiver; the predicates stay for nr_pusch_passive_decode.c. */
bool nr_ul_layout_edge_enabled(void) { return false; }
bool nr_ul_layout_coherent_enabled(void) { return false; }
bool nr_ul_layout_sprt_enabled(void) { return false; }
/* The configured / SIB1 / DL-derived UL BWP is a hypothesis (hypothesis 0, first in the CRC trials) */
bool nr_ul_layout_bwp_seed_enabled(void) { return true; }
bool nr_ul_layout_dl_prior_enabled(void) { return true; }
bool nr_ul_layout_sib1_first_enabled(void) { return true; }
bool nr_ul_layout_crc_guard_enabled(void) { return true; }
bool nr_ul_layout_crc_first_enabled(void) { return true; }
bool nr_ul_layout_lock_enabled(void) { return true; }

static void reset_locked(void)
{
  memset(g_len, 0, sizeof(g_len));
  g_nlen = 0;
  memset(g_occ, 0, sizeof(g_occ));
  memset(g_hyp, 0, sizeof(g_hyp));
  memset(g_only, 0, sizeof(g_only));
  g_nhyp = 0;
  g_locked_hyp = -1;
  g_lat_var = 0;
  g_var_ctr = 0;
  memset(g_addpos, 0, sizeof(g_addpos));
  g_ctl_votes = g_discarded = g_tied = g_votes_total = g_probes_fed = g_probes_booked = g_probes_skipped = 0;
  g_npend = 0;
  g_first_slot = g_lock_slot = g_trial_slot = -1;
  g_have_cell = false;
}

void nr_ul_layout_lock_reset(void)
{
  pthread_mutex_lock(&mu);
  reset_locked();
  pthread_mutex_unlock(&mu);
}

void nr_ul_layout_note_pdcch_slot(long abs_slot)
{
  if (abs_slot < 0) return;
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

uint32_t nr_ul_layout_enum_count(int car, int w, uint32_t maxriv)
{
  uint32_t c = 0;
  for (int n = car; n >= 1; n--)
    if (nr_ul_layout_riv_bits(n) == w && (uint64_t)n * (n + 1) / 2 > maxriv) c += (uint32_t)(car - n + 1);
  return c;
}

bool nr_ul_layout_enum_at(int car, int w, uint32_t maxriv, uint32_t idx, uint16_t *n_out, uint16_t *start_out)
{
  for (int n = car; n >= 1; n--) {
    if (nr_ul_layout_riv_bits(n) != w || (uint64_t)n * (n + 1) / 2 <= maxriv) continue;
    const uint32_t m = (uint32_t)(car - n + 1);
    if (idx < m) { *n_out = (uint16_t)n; *start_out = (uint16_t)idx; return true; }
    idx -= m;
  }
  return false;
}

/* F6: after the legacy hypotheses, every (N, start) of the carrier that the RIV field can express
 * (riv_bits(N) == the fitted width) and that is consistent with the DCI bank: N(N+1)/2 must exceed the largest RIV observed
 * at that field (a smaller N would make an observed RIV illegal). The observed maximum only grows, so the filter never excludes
 * the true N. Appended in priority order (N descending, start ascending) up to the table size. */
static void append_all_locked(void)
{
  for (int l = 0; l < g_nlen; l++)
    for (int p = 0; g_len[l].fitted && p < g_len[l].npre; p++) {
      const len_state_t *ls = &g_len[l];
      const int w = ls->pre[p].riv;
      uint32_t maxriv = 0;
      const int nb = ls->count < NR_UL_LAYOUT_BANK ? (int)ls->count : NR_UL_LAYOUT_BANK;
      for (int i = 0; i < nb; i++) {
        const uint32_t r = fld(&ls->bank[i].p, ls->len, 1 + ls->pre[p].bwp_ind, w);
        if (r > maxriv) maxriv = r;
      }
      uint16_t n, st;
      for (uint32_t i = 0; g_nhyp < NR_UL_LAYOUT_MAX_HYP && nr_ul_layout_enum_at(g_cell.carrier_rb, w, maxriv, i, &n, &st); i++) {
        bool have = false;
        for (int h = 0; h < g_nhyp && !have; h++) have = g_hyp[h].n == n && g_hyp[h].start == st;
        if (!have) g_hyp[g_nhyp++] = (hyp_state_t){.n = n, .start = st};
      }
    }
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
  append_all_locked();
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

/* variant = vt | k2d << 3 | tda << 4: vt = forced MCS table + 1 (0 = keep), k2d = k2 + 1 on every TDRA row,
 * tda = 0: every row S0 L14 type A; 1: every row S0 L13 type B (UL slot with the last symbol left to SRS) */
static void apply_variant(nr_pdcch_blind_ul_opts_t *o, int var)
{
  if ((var & 7) > 0) o->mcs_table = (uint8_t)((var & 7) - 1);
  for (int i = 0; i < o->tda_count && i < 16; i++) {
    if (var & 8) o->tda_k2[i]++;
    if (var & 16) { o->tda_start[i] = 0; o->tda_length[i] = 13; o->tda_mapping[i] = 1; }
  }
}

/* caller holds mu: build the locked opts of one fitted length once the BWP is locked */
static void lock_length_locked(len_state_t *ls, long now_slot)
{
  if (!ls->fitted || ls->prov || ls->locked || g_locked_hyp < 0) return; /* a provisional fit locks only through a CRC pass */
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
  apply_variant(&ls->opts, g_lat_var);
  if (nr_pdcch_blind_dci01_size(&ls->opts) != ls->len) return;
  ls->chosen_tail = best;
  ls->chosen_var = g_lat_var;
  ls->locked = true;
  ls->lock_decisive = 0;
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


/* Log-odds (nr_ul_layout_llr_decide; unit-tested, not used for the lock decision): lock rule on the vote tally instead of "leader >= 6 votes and >= 2x the runner-up".
 * Each vote is worth W = ln 3 (a vote for h is 3x likelier when h is true than the look-alike leakage of a neighbour hypothesis, measured
 * 0.2-0.5 in OTA data); the DL BWP / SIB1 UL BWP carry a prior of one vote-equivalent (prior[h] = 1) that shifts the margin but never
 * locks by itself (the leader still needs min votes of its own). Lock when the leader beats EVERY other hypothesis by LLR_MARGIN. */
#define LLR_W 1.0986f   /* ln 3 */
#define LLR_MARGIN 4.3f
#define LLR_MIN_VOTES_PRIOR 4
bool nr_ul_layout_llr_decide(const uint32_t *votes, const uint8_t *prior, const uint8_t *nrnti, int n, int *lead)
{
  int a = -1;
  for (int h = 0; h < n; h++) if (votes[h] && (a < 0 || votes[h] > votes[a])) a = h;
  if (a < 0 || nrnti[a] < LOCK_RNTIS) return false;
  if (votes[a] < (prior[a] ? LLR_MIN_VOTES_PRIOR : LOCK_MIN)) return false;
  const float sa = LLR_W * (float)(votes[a] + prior[a]);
  for (int h = 0; h < n; h++)
    if (h != a && (sa - LLR_W * (float)(votes[h] + prior[h]) < LLR_MARGIN
                   || (float)(votes[a] + prior[a]) < 1.5f * (float)(votes[h] + prior[h]))) /* 62:45 look-alikes (OTA Vy1) never lock */
      return false;
  if (lead) *lead = a;
  return true;
}
/* Pairwise sign test on DM-RS detections (nr_ul_layout_sign_decide; not used for the lock decision, only unit-tested).
 * A probe (one UL grant, scored under every hypothesis) is a DETECTION for every hypothesis whose DM-RS SNR is >= VOTE_DB, not only
 * the argmax. For a pair (a, b) only the probes where exactly one of them detects are informative:
 *   a-only = a detects, b (which has PRBs for this grant) does not;  b-only = the reverse.
 * Why: a look-alike (216@1 next to the true 217@0) reads the PRBs next to the true grant. A neighbouring UE's DM-RS, scrambled with
 * the same PCI, can make it detect (and, being stronger, win the argmax and take the vote), but b-only needs the TRUE UE's own
 * DM-RS to be absent, which it is not: so b-only stays near 0 for a look-alike while a-only grows with every grant the true UE sends.
 * Under "indistinguishable" the discordant probes split 50/50, so a leads when P(X >= A | n = A + B, 1/2) <= SIGN_ALPHA against EVERY
 * rival that was ever detected. The prior (DL BWP / SIB1 UL BWP) counts one discordant probe for the hypothesis that carries it; a
 * leader without it needs SIGN_MIN_ONLY, with it SIGN_MIN_ONLY - 1. A rival that is never discordant but equally detected (a twin
 * for every grant seen) cannot be split by evidence and blocks the lock unless only the leader carries the prior. */
#define SIGN_ALPHA 0.02
#define SIGN_MIN_ONLY 6
#define SIGN_DMAX 6      /* a probe with more detecting hypotheses than this is wide-band energy, not a grant */
bool nr_ul_layout_sign_decide(const uint16_t *only, int stride, const uint32_t *det, const uint8_t *prior, const uint8_t *nrnti, int n, int *lead)
{
  int best = -1;
  double best_p = 2.0;
  for (int a = 0; a < n; a++) {
    if (det[a] < (uint32_t)(SIGN_MIN_ONLY - 1) || nrnti[a] < LOCK_RNTIS) continue;
    double worst = 0.0; /* the weakest pairwise p-value of a against its rivals */
    bool ok = true;
    for (int h = 0; h < n && ok; h++) {
      if (h == a || det[h] == 0) continue;
      const uint32_t A = (uint32_t)only[(size_t)a * stride + h] + prior[a], B = (uint32_t)only[(size_t)h * stride + a] + prior[h];
      if (A < (uint32_t)(SIGN_MIN_ONLY - (prior[a] ? 1 : 0)) || A <= B) { ok = false; break; }
      const double pv = nbs_binom_sf(A, A + B, 0.5);
      if (pv > SIGN_ALPHA) { ok = false; break; }
      if (pv > worst) worst = pv;
    }
    if (ok && worst < best_p) { best = a; best_p = worst; }
  }
  if (best < 0) return false;
  if (lead) *lead = best;
  return true;
}
/* caller holds mu: book the detections of one probe */
static void sign_book_locked(const pending_t *p)
{
  int D[SIGN_DMAX + 1], nd = 0;
  for (int h = 0; h < p->n; h++)
    if (p->snr[h] >= VOTE_DB) { if (nd == SIGN_DMAX) return; D[nd++] = h; }
  if (nd == 0 || p->ctl >= VOTE_DB) return;
  for (int i = 0; i < nd; i++) {
    hyp_state_t *H = &g_hyp[D[i]];
    H->det++;
    bool seen = false;
    for (int r = 0; r < H->nrnti; r++) seen |= H->rntis[r] == p->rnti;
    if (!seen && H->nrnti < 8) H->rntis[H->nrnti++] = p->rnti;
    for (int g = 0; g < p->n; g++) {
      if (p->snr[g] <= -90.f || p->snr[g] >= VOTE_DB) continue;
      if (g_only[D[i]][g] < 65535) g_only[D[i]][g]++;
    }
  }
}
bool nr_ul_layout_searching(void) { return g_have_cell && g_locked_hyp < 0; }

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
    sign_book_locked(p);
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
  uint32_t va = g_hyp[a].votes, vb = b >= 0 ? g_hyp[b].votes : 0;
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

/* An operator-CONFIGURED UL BWP (pdcch_blind_monitor_ul_bwp) is the BWP: it short-circuits the DM-RS vote, which
 * cannot tell N@s from N+1@s. CRC-first trials still re-lock it after a run of failures. A SIB1-derived BWP is only a hypothesis. */
static void seed_lock_locked(long slot)
{
  if (!(g_cell.sib1_ul_cfg || g_cell.sib1_ul_dl) || g_locked_hyp >= 0) return;
  for (int h = 0; h < g_nhyp; h++)
    if (g_hyp[h].n == g_cell.sib1_ul_size && g_hyp[h].start == g_cell.sib1_ul_start) {
      g_locked_hyp = h;
      g_lock_slot = slot;
      LOG_A(PHY, "SENSING: ULLAYOUT SEED_LOCK bwp=%u@%u (%s) abs=%ld\n", g_hyp[h].n, g_hyp[h].start, g_cell.sib1_ul_cfg ? "configured" : "derived prior (SIB1 / DL BWP)", slot);
      return;
    }
}
static pthread_mutex_t eval_mu = PTHREAD_MUTEX_INITIALIZER; /* serializes eval passes (static scratch below) */

typedef struct {
  nr_ul_layout_prefix_t pre[NR_UL_LAYOUT_MAX_PRE];
  int npre;
  nr_ul_layout_tail_t tail[NR_UL_LAYOUT_MAX_TAIL];
  int ntail;
  uint8_t k2row[16];
  bool k2ok;
} fit_chain_t;

/* prefix, tail and k2 of one MCS offset hypothesis (no lock held; eval_mu serializes the static scratch) */
static void fit_chain(const nr_ul_layout_dci_t *snap, int n, uint16_t len, int mo, const uint32_t *occ, int spf,
                      const uint16_t *cn, int nc, fit_chain_t *c)
{
  memset(c, 0, sizeof(*c));
  c->npre = mo > 0 ? nr_ul_layout_fit_prefix(snap, n, len, mo, cn, nc, c->pre, NR_UL_LAYOUT_MAX_PRE) : 0;
  c->ntail = c->npre > 0 ? nr_ul_layout_fit_tail(snap, n, len, &c->pre[0], c->tail, NR_UL_LAYOUT_MAX_TAIL) : 0;
  /* k2 per TDRA index (indices read with the first prefix; all prefixes share the TDRA position only when their
   * bwp_ind + riv agree, so the first one is the reference) */
  c->k2ok = c->npre > 0;
  if (c->npre > 0) {
    static uint32_t hist[16][160];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++) {
      const uint32_t idx = fld(&snap[i].p, len, 1 + c->pre[0].bwp_ind + c->pre[0].riv, c->pre[0].tda);
      if (idx < 16 && spf > 0) hist[idx][snap[i].slot % spf]++;
    }
    for (int r = 0; r < 16; r++) {
      uint32_t tot = 0;
      for (int s = 0; s < spf; s++) tot += hist[r][s];
      if (tot < 32) continue;
      c->k2row[r] = (uint8_t)nr_ul_layout_infer_k2(occ, hist[r], spf);
      if (!c->k2row[r]) c->k2ok = false;
    }
    if (!c->k2row[0]) for (int r = 0; r < 16 && !c->k2row[0]; r++) c->k2row[0] = c->k2row[r];
    if (!c->k2row[0]) c->k2ok = false;
  }
}

/* caller holds mu: the k2 inference of this DCI length found no free slot; the TDRA table is the cell's, so another length's fitted k2 applies */
static void k2_borrow_locked(len_state_t *ls, fit_chain_t *c)
{
  if (c->npre <= 0 || c->k2ok) return;
  for (int o = 0; o < g_nlen && !c->k2ok; o++)
    if (&g_len[o] != ls && g_len[o].fitted && g_len[o].k2row[0]) {
      for (int r = 0; r < 16; r++) if (!c->k2row[r]) c->k2row[r] = g_len[o].k2row[r] ? g_len[o].k2row[r] : g_len[o].k2row[0];
      c->k2ok = true;
      LOG_I(PHY, "SENSING: ULLAYOUT K2_BORROW len=%u from len=%u k2_row0=%u\n", ls->len, g_len[o].len, c->k2row[0]);
    }
}

/* caller holds mu: trial verdicts a provisional candidate gets: twice the (BWP hypothesis x tail x variant) combinations its prefixes open, each
 * tested by one real PUSCH decode, so the budget follows the hypothesis space and counts decodes that happened (a round whose jobs the
 * queue dropped or could not read is no evidence against the candidate) */
static uint32_t prov_budget_locked(const len_state_t *ls)
{
  int nr = 0;
  for (int h = 0; h < g_nhyp; h++) nr += pre_for_hyp(ls, h) >= 0;
  const uint32_t nh = g_cell.sib1_ul_cfg ? 1u : (uint32_t)(nr ? nr : 1);
  const uint32_t nt = (uint32_t)(ls->ntail < 2 ? (ls->ntail ? ls->ntail : 1) : 2);
  const uint32_t vc = (uint32_t)nr_ul_variant_count(true, true, true, ls->ntail > 0 && ls->tail[0].tp);
  uint32_t nk = 1; /* splits of one RIV width: each gets its own variant cycle */
  for (int k = 0; k < ls->npre; k++) {
    uint32_t c = 0;
    for (int k2 = 0; k2 < ls->npre; k2++) c += ls->pre[k2].riv == ls->pre[k].riv;
    if (c > nk) nk = c;
  }
  const uint32_t full = 2u * nh * nt * vc * nk;
  /* iterative deepening: a slice is two passes over the tail x variant space of the prior-first BWP hypothesis, doubled after every pass over
   * the ranked list, up to the whole space. A list of eight candidates is thus cycled in seconds, the right one meets its CRC pass early, and a
   * wide hypothesis space is still covered by the later, longer passes [MEASURED, sens6 OTA lab, r2in1/r2in2/i7R4: a first candidate from a
   * junk-dominated bank held the whole space's budget (1536 verdicts) while the true offset waited in the list]. */
  const uint32_t base = 2u * nt * vc * nk;
  const uint32_t sh = ls->prov_cycle < 16 ? ls->prov_cycle : 16;
  const uint64_t sl = (uint64_t)base << sh;
  return sl < full ? (uint32_t)sl : full;
}

bool nr_ul_layout_provisional(uint16_t len)
{
  bool r = false;
  pthread_mutex_lock(&mu);
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == len && g_len[l].fitted && g_len[l].prov && !g_len[l].locked) r = true;
  pthread_mutex_unlock(&mu);
  return r;
}

/* caller holds mu: adopt `c` as the layout of ls (strict: significant offset; prov: ranked candidate under CRC test) */
static void commit_fit_locked(len_state_t *ls, const fit_chain_t *c, int mo, float sc, float sec, int n, long fit_slot, bool prov)
{
  ls->mcs_off = mo;
  ls->mcs_score = sc;
  ls->mcs_second = sec;
  ls->npre = c->npre;
  memcpy(ls->pre, c->pre, sizeof(ls->pre));
  ls->ntail = c->ntail;
  memcpy(ls->tail, c->tail, sizeof(ls->tail));
  memcpy(ls->k2row, c->k2row, sizeof(ls->k2row));
  ls->fitted = true;
  ls->prov = prov;
  ls->chosen_tail = -1;
  memset(ls->ev, 0, sizeof(ls->ev)); /* evidence of an abandoned candidate does not carry over */
  ls->fit_slot = fit_slot;
  build_hyps_locked();
  if (prov) {
    ls->prov_ctr0 = g_var_ctr;
    ls->prov_done = 0;
    ls->prov_budget = prov_budget_locked(ls);
  }
  char pr[512];
  int u = 0;
  for (int k = 0; k < c->npre && u < (int)sizeof(pr); k++) /* snprintf returns the untruncated length: stop at full */
    u += snprintf(pr + u, sizeof(pr) - u, "%s{bwp_ind=%u riv=%u tda=%u fh=%u Nmax=%u}", k ? "," : "", c->pre[k].bwp_ind,
                  c->pre[k].riv, c->pre[k].tda, c->pre[k].fh, c->pre[k].n_max);
  char t0[160], t1[160] = "-";
  tail_str(&c->tail[0], t0, sizeof(t0));
  if (c->ntail > 1) tail_str(&c->tail[1], t1, sizeof(t1));
  LOG_A(PHY, "SENSING: ULLAYOUT FIT len=%u dcis=%d mcs_off=%d score=%.2f second=%.2f prefixes=[%s] k2_row0=%u"
             " tails=%d best[%s] next[%s] bwp_hyps=%d first_abs=%ld fit_abs=%ld%s\n",
        ls->len, n, mo, sc, sec, pr, c->k2row[0], c->ntail, t0, t1, g_nhyp, ls->first_slot, ls->fit_slot,
        prov ? " PROVISIONAL(CRC decides)" : "");
  if (prov) {
    LOG_A(PHY, "SENSING: ULLAYOUT FIT_PROV len=%u cand=%d/%d mcs_off=%d budget=%u rotations=%u\n", ls->len, ls->cand_i + 1,
          ls->ncand, mo, ls->prov_budget, ls->prov_rot);
    return; /* no probe vote, fit-read or vote lock on a ranking prior: the CRC trials lock it (crc_lock_locked) */
  }
  seed_lock_locked(ls->fit_slot);
  lock_length_locked(ls, ls->fit_slot);
}

void nr_ul_layout_eval_once(void)
{
  pthread_mutex_lock(&eval_mu);
  static nr_ul_layout_dci_t snap[NR_UL_LAYOUT_BANK];
  for (int l = 0; l < NR_UL_LAYOUT_MAX_LEN; l++) {
    pthread_mutex_lock(&mu);
    const bool in = l < g_nlen && g_have_cell;
    const bool want_fit = in && !g_len[l].fitted && g_len[l].count >= FIT_MIN && g_len[l].count - g_len[l].last_eval_count >= 64;
    /* a provisional fit rests on a ranking prior from the bank of its time; the strict (significance) fit keeps being evaluated on the growing
     * bank and replaces it the moment it is significant -- as it did before the provisional path existed [MEASURED, sens6 OTA lab: the good runs
     * lock 273@0 through the strict fit once the traffic has made it significant; r2in1/r2in2/i7R4 stayed on a provisional candidate from the first
     * 261-532 DCIs and never re-evaluated it] */
    const bool want_strict = in && g_len[l].fitted && g_len[l].prov && !g_len[l].locked && g_len[l].count >= FIT_MIN
                             && g_len[l].count - g_len[l].last_eval_count >= 64;
    /* a provisional candidate whose trial budget is spent without a CRC lock gives way to the next ranked one */
    /* rotate when the candidate has had its verdicts; or, should the queue lose the trials (rounds without verdicts), after twice that many rounds */
    const bool want_rot = in && g_len[l].fitted && g_len[l].prov && !g_len[l].locked
                          && (g_len[l].prov_done >= g_len[l].prov_budget || g_var_ctr - g_len[l].prov_ctr0 >= 2u * g_len[l].prov_budget);
    if (!want_fit && !want_strict && !want_rot) {
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
    int cand[NR_UL_LAYOUT_MAX_CAND], ncand = 0, from = 0;
    if (want_rot) {
      ncand = ls->ncand;
      memcpy(cand, ls->cand, sizeof(cand));
      from = ls->cand_i + 1;
    }
    pthread_mutex_unlock(&mu);

    float sc = 0.f, sec = 0.f, rs[NR_UL_LAYOUT_MAX_CAND] = {0};
    int mo = -1;
    bool strict = false;
    if (want_fit || want_strict || want_rot) {
      mo = nr_ul_layout_fit_mcs(snap, n, len, &sc, &sec);
      strict = mo > 0;
    }
    if (strict) {
      fit_chain_t c;
      fit_chain(snap, n, len, mo, occ, spf, cn, nc, &c);
      pthread_mutex_lock(&mu);
      if (gen == g_gen && (!ls->fitted || (ls->prov && !ls->locked))) {
        k2_borrow_locked(ls, &c);
        if (c.npre > 0 && c.ntail > 0 && c.k2ok) {
          if (ls->prov) LOG_A(PHY, "SENSING: ULLAYOUT FIT_STRICT len=%u replaces the provisional mcs_off=%d (rotations=%u): mcs_off=%d significant\n", len, ls->mcs_off, ls->prov_rot, mo);
          commit_fit_locked(ls, &c, mo, sc, sec, n, snap[n - 1].slot, false);
        } else if (!ls->fitted) {
          ls->mcs_off = mo;
          ls->mcs_score = sc;
          ls->mcs_second = sec;
          LOG_I(PHY, "SENSING: ULLAYOUT FIT len=%u dcis=%d not yet: mcs_off=%d (%.2f/%.2f) prefixes=%d tails=%d k2=%d\n", len, n, mo,
                sc, sec, c.npre, c.ntail, c.k2ok);
        }
      }
      pthread_mutex_unlock(&mu);
      continue;
    }
    if (!want_fit && !want_rot) continue; /* the strict retry found nothing significant: the provisional candidate keeps its trials */
    /* No unique significant offset (traffic-dependent statistics): the ranked offsets become CRC-tested hypotheses. */
    if (!want_rot || from >= ncand) { /* first ranking, or the list is exhausted: rank again on the current bank */
      ncand = nr_ul_layout_rank_mcs(snap, n, len, cand, rs, NR_UL_LAYOUT_MAX_CAND);
      from = 0;
      pthread_mutex_lock(&mu);
      if (want_rot) { if (ls->prov_cycle < 1000) ls->prov_cycle++; } else ls->prov_cycle = 0;
      pthread_mutex_unlock(&mu);
    } else {
      for (int i = 0; i < ncand; i++) rs[i] = 0.f;
    }
    pthread_mutex_lock(&mu);
    if (gen != g_gen || (want_fit && ls->fitted) || (want_rot && (!ls->fitted || ls->locked || !ls->prov))) {
      pthread_mutex_unlock(&mu);
      continue;
    }
    pthread_mutex_unlock(&mu);
    bool done = false;
    for (int i = from; i < ncand && !done; i++) {
      fit_chain_t c;
      fit_chain(snap, n, len, cand[i], occ, spf, cn, nc, &c);
      if (c.npre <= 0 || c.ntail <= 0) continue;
      pthread_mutex_lock(&mu);
      if (gen == g_gen && !ls->locked && (want_fit ? !ls->fitted : ls->prov)) {
        k2_borrow_locked(ls, &c);
        if (c.k2ok) {
          if (want_rot) ls->prov_rot++;
          memcpy(ls->cand, cand, sizeof(ls->cand));
          ls->ncand = ncand;
          ls->cand_i = i;
          commit_fit_locked(ls, &c, cand[i], rs[i], i + 1 < ncand ? rs[i + 1] : 0.f, n, snap[n - 1].slot, true);
          done = true;
        }
      }
      pthread_mutex_unlock(&mu);
    }
    if (!done && want_fit) {
      pthread_mutex_lock(&mu);
      LOG_I(PHY, "SENSING: ULLAYOUT FIT len=%u dcis=%d not yet: mcs_off=%d (%.2f/%.2f) ranked=%d, none with prefix+tail+k2\n", len, n, mo, sc, sec, ncand);
      pthread_mutex_unlock(&mu);
    } else if (!done && want_rot) { /* every ranked offset failed its chain: rank again at the next budget */
      pthread_mutex_lock(&mu);
      ls->prov_ctr0 = g_var_ctr;
      ls->prov_done = 0;
      ls->ncand = 0;
      ls->cand_i = -1;
      pthread_mutex_unlock(&mu);
    }
  }
  pthread_mutex_unlock(&eval_mu);
}

static void ul_layout_start_thread(void);
bool nr_ul_layout_fit_read_enabled(void) { return true; }

/* ULDCI-20261014: the bank fit (MCS block, prefix, tail) is known ~1 s after the first DCIs and does
 * not depend on the BWP lock, but the fields of every grant of an UNLOCKED length were read by the width sweeps, whose
 * default has no BWP indicator: on a cell that sends one (Vodafone 415, replay b: 2466 of 3370 grants) MCS/NDI/RV/HARQ
 * were read one bit early, so the fields of one HARQ process jumped. This reads such a grant with the fitted layout; the
 * RIV/TDRA stay provisional (BWP = widest candidate of the prefix at the SIB1 start when it matches, else at PRB 0). */
bool nr_ul_layout_read_fitted(uint16_t rnti, uint16_t len, nr_dci_bits_t payload, nr_pdcch_blind_ul_result_t *out)
{
  if (!nr_ul_layout_fit_read_enabled() || !out || !len) return false;
  nr_pdcch_blind_ul_opts_t o;
  pthread_mutex_lock(&mu);
  const len_state_t *ls = NULL;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == len && g_len[l].fitted && !g_len[l].locked && !g_len[l].prov) ls = &g_len[l];
  if (!ls || ls->npre < 1 || ls->ntail < 1) { pthread_mutex_unlock(&mu); return false; }
  const nr_ul_layout_prefix_t *p0 = &ls->pre[0];
  const uint16_t start = (g_cell.sib1_ul_size == p0->n_max && g_cell.sib1_ul_start + p0->n_max <= g_cell.carrier_rb)
                             ? g_cell.sib1_ul_start : 0;
  nr_ul_layout_build_opts(p0, &ls->tail[ls->chosen_tail >= 0 && ls->chosen_tail < ls->ntail ? ls->chosen_tail : 0], start,
                          p0->n_max, ls->k2row, 1, g_cell.pci, g_cell.mu, g_cell.typeA_pos, &o);
  pthread_mutex_unlock(&mu);
  if (!nr_pdcch_blind_extract_01(payload, len, rnti, &o, out)) return false;
  out->hyp_generation = 1; /* explicit-layout sentinel (as a forced layout): admitted by the chain, no width-class ticket */
  return true;
}

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
  if (!cell || !out || !rnti || !len || len > NR_DCI_MAX_PAYLOAD || abs_slot < 0
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
  else { /* priors that were not known at the first DCI */
    if (!g_cell.sib1_ul_size && cell->sib1_ul_size) { g_cell.sib1_ul_start = cell->sib1_ul_start; g_cell.sib1_ul_size = cell->sib1_ul_size; g_cell.sib1_ul_cfg = cell->sib1_ul_cfg; g_cell.sib1_ul_dl = cell->sib1_ul_dl; }
    if (!g_cell.dl_bwp_size && cell->dl_bwp_size) { g_cell.dl_bwp_start = cell->dl_bwp_start; g_cell.dl_bwp_size = cell->dl_bwp_size; }
  }
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
  if (ls->fitted && !ls->prov && g_locked_hyp < 0 && g_nhyp > 0) {
    const nr_ul_layout_prefix_t *p0 = &ls->pre[0];
    const uint32_t idx = fld(&payload, len, 1 + p0->bwp_ind + p0->riv, p0->tda);
    const uint8_t k2 = idx < 16 && ls->k2row[idx] ? ls->k2row[idx] : ls->k2row[0];
    int testable = 0;
    uint16_t r0 = 0, nr = 0;
    for (int h = 0; h < g_nhyp; h++) {
      uint16_t a, b;
      if (hyp_range(ls, &payload, h, &a, &b) && b <= PROBE_MAX_RB && a + b <= 275) { if (!testable) { r0 = a; nr = b; } testable++; }
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
        if (!hyp_range(&g_len[l], &g->raw_payload, h, &a, &b) || b > PROBE_MAX_RB || a + b > 275) b = 0;
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
  { /* one line per probe, the three best hypotheses */
    {
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


/* ------------------------------------------------------------------ CRC-first */
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

/* caller holds mu: the sel-th (cyclic) prefix of ls whose RIV width matches BWP hypothesis h, or -1. Several splits of one width (BWP indicator
 * 0, 1 or 2 bits, the rest to the TDRA) cannot be told apart by the statistics of a bank with constant leading RIV bits: the trials cycle them. */
static int pre_for_hyp_sel(const len_state_t *ls, int h, int sel)
{
  const int w = nr_ul_layout_riv_bits(g_hyp[h].n);
  int cnt = 0;
  for (int k = 0; k < ls->npre; k++) cnt += ls->pre[k].riv == w;
  if (!cnt) return -1;
  sel %= cnt;
  for (int k = 0; k < ls->npre; k++) if (ls->pre[k].riv == w && sel-- == 0) return k;
  return -1;
}

/* caller holds mu: build the opts of hypothesis (h, prefix k, tail t); false when it does not read `ls->len` */
static bool trial_opts_ap_locked(const len_state_t *ls, int h, int k, int t, int var, int addpos, nr_pdcch_blind_ul_opts_t *o)
{
  if (h < 0 || h >= g_nhyp || k < 0 || k >= ls->npre || t < 0 || t >= ls->ntail) return false;
  nr_ul_layout_build_opts(&ls->pre[k], &ls->tail[t], g_hyp[h].start, g_hyp[h].n, ls->k2row, addpos, g_cell.pci,
                          g_cell.mu, g_cell.typeA_pos, o);
  apply_variant(o, var);
  return nr_pdcch_blind_dci01_size(o) == ls->len;
}
static bool trial_opts_locked(const len_state_t *ls, int h, int k, int t, int var, nr_pdcch_blind_ul_opts_t *o)
{
  return trial_opts_ap_locked(ls, h, k, t, var, cur_addpos_locked(), o);
}

/* caller holds mu: evidence slot of hypothesis e (created; the entry with the fewest passes is evicted when full) */
static typeof(((len_state_t *)0)->ev[0]) *ev_slot_locked(len_state_t *ls, uint32_t e)
{
  int v = 0;
  for (int i = 0; i < CRC_EV_MAX; i++) {
    if (ls->ev[i].e == e + 1u) return &ls->ev[i];
    if (ls->ev[i].passes < ls->ev[v].passes) v = i;
  }
  memset(&ls->ev[v], 0, sizeof(ls->ev[v]));
  ls->ev[v].e = e + 1u; /* 0 = empty */
  return &ls->ev[v];
}

/* P(Poisson(lam) >= m) */
static double pois_tail(double lam, int m)
{
  double term = exp(-lam), cdf = 0;
  for (int i = 0; i < m; i++) { cdf += term; term *= lam / (i + 1); }
  return cdf >= 1 ? 0 : 1 - cdf;
}

int nr_ul_layout_crc_trials(uint16_t rnti, uint16_t len, long abs_slot, nr_dci_bits_t payload,
                            const nr_ul_layout_cell_t *cell, nr_pdcch_blind_ul_result_t *out, int cap)
{
  if (!cell || !out || cap < 1 || abs_slot < 0) return 0;
  int K = 3; /* CRC-first jobs per slot */
  if (K > NR_UL_LAYOUT_MAX_TRIALS) K = NR_UL_LAYOUT_MAX_TRIALS;
  if (K > cap) K = cap;
  int n = 0;
  pthread_mutex_lock(&mu);
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == len) li = l;
  len_state_t *ls = li >= 0 ? &g_len[li] : NULL;
  /* one DCI per slot (<= K jobs), small MCS only; locked layouts are re-tried only after a run of CRC failures */
  if (!g_have_cell || !ls || !ls->fitted || g_nhyp == 0 || g_trial_slot == abs_slot
      || (ls->locked && ls->crc_fail_run < RELOCK_FAILS && ls->lock_decisive) || fld(&payload, len, ls->mcs_off, 5) > TRIAL_MCS_MAX) {
    pthread_mutex_unlock(&mu);
    return 0;
  }
  /* rank BWP hypotheses: SIB1 UL BWP first, then DM-RS votes, then list order */
  int rk[NR_UL_LAYOUT_MAX_HYP], nr = 0;
  for (int h = 0; h < g_nhyp; h++) if (pre_for_hyp(ls, h) >= 0) rk[nr++] = h;
  const bool sib1 = g_cell.sib1_ul_size > 0;
  for (int i = 1; i < nr; i++)
    for (int j = i; j > 0; j--) {
      const int a = rk[j], b = rk[j - 1];
      const bool sa = sib1 && g_hyp[a].n == g_cell.sib1_ul_size && g_hyp[a].start == g_cell.sib1_ul_start;
      const bool sb = sib1 && g_hyp[b].n == g_cell.sib1_ul_size && g_hyp[b].start == g_cell.sib1_ul_start;
      if (!(sa > sb || (sa == sb && g_hyp[a].votes > g_hyp[b].votes))) break;
      rk[j] = b; rk[j - 1] = a;
    }
  if (nr > 0 && !(g_cell.sib1_ul_cfg && g_hyp[rk[0]].n == g_cell.sib1_ul_size && g_hyp[rk[0]].start == g_cell.sib1_ul_start)) { /* the leader's look-alike partners (N+1@s, shift variants) are tried right behind it: CRC decides, not votes */
    uint16_t pt[4][2];
    const int np = nr_ul_lookalike_partners(g_hyp[rk[0]].n, g_hyp[rk[0]].start, g_cell.carrier_rb, pt);
    int pos = 1;
    for (int q = 0; q < np && pos < K; q++) {
      int hp = -1;
      for (int h = 0; h < g_nhyp; h++) if (g_hyp[h].n == pt[q][0] && g_hyp[h].start == pt[q][1]) hp = h;
      if (hp < 0 && g_nhyp < NR_UL_LAYOUT_MAX_HYP) { g_hyp[g_nhyp] = (hyp_state_t){.n = pt[q][0], .start = pt[q][1]}; hp = g_nhyp++; }
      if (hp < 0 || pre_for_hyp(ls, hp) < 0) continue;
      int idx = 0;
      while (idx < nr && rk[idx] != hp) idx++;
      if (idx == nr) rk[nr++] = hp;
      for (; idx > pos; idx--) rk[idx] = rk[idx - 1];
      rk[pos++] = hp;
    }
  }
  /* a CONFIGURED UL BWP that leads the ranking is certain: the K jobs of the slot then test K variants of it instead of K BWP hypotheses */
  const bool certain = nr > 0 && g_cell.sib1_ul_cfg && g_hyp[rk[0]].n == g_cell.sib1_ul_size
                       && g_hyp[rk[0]].start == g_cell.sib1_ul_start;
  const int nvj = certain ? K : 1;
  if (certain) nr = 1;
  const uint32_t vctr = g_var_ctr;
  g_var_ctr++;
  if (!certain && nr > K) { /* uncertain BWP: the leader (the BWP the receiver already expects: SIB1 UL BWP, then votes) is in every round; the other K - 1 slots walk the rest of the ranking, one window per two variant rounds (votes alone cannot be trusted).
                             * A walk that moved the leader out with the window left it a lottery whether a candidate under test ever met the cell's own BWP. */
    const int others = nr - 1, per = K - 1 > 0 ? K - 1 : 1;
    const int nwin = (others + per - 1) / per, rot = (int)((vctr / (2u * (uint32_t)nr_ul_variant_count(true, true, true, false))) % (uint32_t)nwin) * per;
    int tmp[NR_UL_LAYOUT_MAX_HYP];
    tmp[0] = rk[0];
    for (int i = 1; i < nr; i++) tmp[i] = rk[1 + (i - 1 + rot) % others];
    memcpy(rk, tmp, sizeof(int) * (size_t)nr);
  }
  const uint64_t gen = g_gen & NR_UL_LAYOUT_GEN_MASK;
  const int vcmax = nr_ul_variant_count(true, true, true, false) > 0 ? nr_ul_variant_count(true, true, true, false) : 1; /* one full variant cycle per prefix split */
  /* K distinct hypotheses per round, the leader first; the tail and the prefix split change once per full variant cycle (every tail and every
   * split of a hypothesis gets its own cycle), so the leader meets all of them in 2 x splits x vc rounds without costing the others a slot */
  const int ntl = ls->ntail < 2 ? ls->ntail : 2;
  int seq[NR_UL_LAYOUT_MAX_HYP][2], ns = 0;
  for (int i = 0; i < nr && ntl > 0; i++) { seq[ns][0] = i; seq[ns++][1] = (int)((vctr / (uint32_t)vcmax) % (uint32_t)ntl); }
  for (int sq = 0; sq < ns && n < K; sq++) {
    const int i = seq[sq][0], t = seq[sq][1];
    {
      const int cnt = nr_ul_variant_count(true, true, true, ls->tail[t].tp);
      for (int j = 0; j < nvj && n < K; j++) {
        const uint32_t ix = vctr * (uint32_t)nvj + (uint32_t)j;
        int tbl, k2d, tda;
        nr_ul_variant_get((int)(ix % (uint32_t)cnt), true, true, true, ls->tail[t].tp, &tbl, &k2d, &tda);
        const int var = (tbl + 1) | k2d << 3 | tda << 4;
        const int h = rk[i], k = pre_for_hyp_sel(ls, h, (int)(vctr / ((uint32_t)vcmax * (uint32_t)(ntl ? ntl : 1))));
        if (ls->locked && g_locked_hyp == h && ls->chosen_tail == t && ls->chosen_var == var) continue; /* the failing layout itself */
        nr_pdcch_blind_ul_opts_t o;
        if (!trial_opts_locked(ls, h, k, t, var, &o) || !nr_pdcch_blind_extract_01(payload, len, rnti, &o, &out[n])) continue;
        /* the data-scrambling-ID walk moves on every failed decode, so a wrong table would never meet the right ID: every other round of
         * the variant rotation pins the walking trials to the PCI default (and keeps them out of the walk) */
        if (out[n].data_id_advance && ((ix / (uint32_t)cnt) & 1u) == 0) {
          out[n].data_scrambling_id = g_cell.pci;
          out[n].data_id_advance = false;
        }
        const uint32_t e = (uint32_t)(h | k << 8 | t << 11 | var << 13);
        out[n].hyp_generation = NR_UL_LAYOUT_TAG | NR_UL_LAYOUT_TRIAL | ((uint64_t)e << 40) | gen;
        ev_slot_locked(ls, e)->trials++;
        n++;
      }
    }
  }
  g_trial_slot = abs_slot;
  if (n) ls->trials += (uint32_t)n;
  pthread_mutex_unlock(&mu);
  return n;
}

/* ISAC_UL_NID_BG stage B: the CRC-first hypotheses of one DCI (BWP hypothesis x tail x variant), enumerated WITHOUT booking anything. Same ranking and same
 * grant builder as nr_ul_layout_crc_trials(), so a CRC pass found in the background carries the same tag (h, k, t, var) and is fed to the lock through
 * nr_ul_layout_crc_verdict(). The locked BWP hypothesis leads the ranking; at most `nhyp` BWP hypotheses are walked. `first` is a cursor over the
 * `*total` combinations; the cursor wraps. Returns the number of grants written (<= cap). */
int nr_ul_layout_hyp_list(uint16_t rnti, uint16_t len, nr_dci_bits_t payload, uint32_t first, int nhyp, int ap_n, nr_pdcch_blind_ul_result_t *out, int cap,
                          uint32_t *total, uint32_t *next)
{
  if (total) *total = 0;
  if (next) *next = first;
  if (!out || cap < 1) return 0;
  pthread_mutex_lock(&mu);
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == len) li = l;
  len_state_t *ls = li >= 0 ? &g_len[li] : NULL;
  if (!g_have_cell || !ls || !ls->fitted || g_nhyp == 0) {
    pthread_mutex_unlock(&mu);
    return 0;
  }
  int rk[NR_UL_LAYOUT_MAX_HYP], nr = 0;
  for (int h = 0; h < g_nhyp; h++) if (pre_for_hyp(ls, h) >= 0) rk[nr++] = h;
  const bool sib1 = g_cell.sib1_ul_size > 0;
  for (int i = 1; i < nr; i++)
    for (int j = i; j > 0; j--) {
      const int a = rk[j], b = rk[j - 1];
      const bool sa = sib1 && g_hyp[a].n == g_cell.sib1_ul_size && g_hyp[a].start == g_cell.sib1_ul_start;
      const bool sb = sib1 && g_hyp[b].n == g_cell.sib1_ul_size && g_hyp[b].start == g_cell.sib1_ul_start;
      if (!(sa > sb || (sa == sb && g_hyp[a].votes > g_hyp[b].votes))) break;
      rk[j] = b; rk[j - 1] = a;
    }
  for (int i = 1; i < nr && g_locked_hyp >= 0; i++) /* the locked hypothesis leads */
    if (rk[i] == g_locked_hyp) { for (int j = i; j > 0; j--) rk[j] = rk[j - 1]; rk[0] = g_locked_hyp; break; }
  if (nhyp > 0 && nr > nhyp) nr = nhyp;
  if (nr > 0 && g_cell.sib1_ul_cfg && g_hyp[rk[0]].n == g_cell.sib1_ul_size && g_hyp[rk[0]].start == g_cell.sib1_ul_start) nr = 1; /* a CONFIGURED UL BWP is certain (as in the CRC trials) */
  uint32_t tot = 0, base[2] = {0, 0};
  int nt = ls->ntail < 2 ? ls->ntail : 2;
  for (int t = 0; t < nt; t++) {
    base[t] = tot;
    tot += (uint32_t)nr * (uint32_t)nr_ul_variant_count(true, true, true, ls->tail[t].tp) * (uint32_t)ap_n;
  }
  if (total) *total = tot;
  const uint64_t gen = g_gen & NR_UL_LAYOUT_GEN_MASK;
  int n = 0;
  uint32_t c = 0;
  for (; c < tot && n < cap; c++) {
    const uint32_t ix = (first + c) % tot;
    int t = nt - 1;
    while (t > 0 && ix < base[t]) t--;
    const int cnt = nr_ul_variant_count(true, true, true, ls->tail[t].tp);
    const uint32_t r = ix - base[t];
    const int i = (int)(r / (uint32_t)(cnt * ap_n)), v = (int)((r / (uint32_t)ap_n) % (uint32_t)cnt), ai = (int)(r % (uint32_t)ap_n);
    const int cur_ap = cur_addpos_locked(), addpos = ai == 0 ? cur_ap : (cur_ap + ai) % 3; /* the current reading first, then the other dmrs-AdditionalPosition values */
    int tbl, k2d, tda;
    nr_ul_variant_get(v, true, true, true, ls->tail[t].tp, &tbl, &k2d, &tda);
    const int var = (tbl + 1) | k2d << 3 | tda << 4;
    const int h = rk[i], k = pre_for_hyp(ls, h);
    nr_pdcch_blind_ul_opts_t o;
    if (!trial_opts_ap_locked(ls, h, k, t, var, addpos, &o) || !nr_pdcch_blind_extract_01(payload, len, rnti, &o, &out[n])) continue;
    out[n].data_scrambling_id = g_cell.pci;
    out[n].data_id_advance = false;
    out[n].hyp_generation = NR_UL_LAYOUT_TAG | NR_UL_LAYOUT_TRIAL | ((uint64_t)(uint32_t)(h | k << 8 | t << 11 | var << 13) << 40) | gen;
    n++;
  }
  if (next && tot) *next = (first + c) % tot;
  pthread_mutex_unlock(&mu);
  return n;
}

/* the background search tried `n` more candidates (data ids) under the hypothesis of this tagged grant: the CRC16 false-alarm accounting of the evidence rule counts them */
void nr_ul_layout_trial_add(const nr_pdcch_blind_ul_result_t *g, uint32_t n)
{
  if (!nr_ul_layout_is_tagged(g) || !nr_ul_layout_is_trial(g)) return;
  pthread_mutex_lock(&mu);
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == g->dci_length) li = l;
  if (li >= 0 && (g->hyp_generation & NR_UL_LAYOUT_GEN_MASK) == (g_gen & NR_UL_LAYOUT_GEN_MASK))
    ev_slot_locked(&g_len[li], (uint32_t)((g->hyp_generation >> 40) & 0x7ffff))->trials += n;
  pthread_mutex_unlock(&mu);
}

/* caller holds mu: a TB CRC passed under (h, k, t): this layout is the lock (BWP, prefix, tail, k2/TDRA) */
static void crc_lock_locked(int li, int h, int k, int t, int var, long slot)
{
  len_state_t *ls = &g_len[li];
  nr_pdcch_blind_ul_opts_t o;
  if (!trial_opts_locked(ls, h, k, t, var, &o)) return;
  const bool relock = ls->locked;
  const bool vchg = g_lat_var != var;
  g_lat_var = var;
  ls->chosen_var = var;
  ls->opts = o;
  ls->chosen_tail = t;
  ls->locked = true;
  ls->prov = false; /* the CRC evidence rule (decisive pass, or CRC16 in >= 2 slots) resolved the ranked candidate */
  ls->lock_decisive = 0;
  ls->lock_slot = slot;
  ls->crc_fail_run = 0;
  ls->crc_locks++;
  if (g_locked_hyp != h || vchg) {
    g_locked_hyp = h;
    for (int l = 0; l < g_nlen; l++) if (l != li) g_len[l].locked = false; /* re-derived under the new BWP */
    for (int l = 0; l < g_nlen; l++) lock_length_locked(&g_len[l], slot);
  }
  g_lock_slot = slot;
  char tl[160];
  tail_str(&ls->tail[t], tl, sizeof(tl));
  LOG_A(PHY, "SENSING: ULLAYOUT_CRC_LOCK%s len=%u bwp=%u@%u riv=%u bwp_ind=%u tda=%u(k2=%u) fh=%u tail[%s] crc_ok=%u"
             " trials=%u mcs_table=%u var=%d abs=%ld\n",
        relock ? " RELOCK" : "", ls->len, g_hyp[h].n, g_hyp[h].start, ls->pre[k].riv, ls->pre[k].bwp_ind, ls->pre[k].tda,
        o.tda_k2[0], ls->pre[k].fh, tl, ls->crc_ok, ls->trials, (unsigned)o.mcs_table, var, slot);
}

/* May this TB CRC pass lock layout (h,k,t)? Caller holds mu.
 * Decisive: CRC24A (TBS > 3824 bits, TS 38.212 7.2.1) or a multi-segment TB whose CB CRCs all pass.
 * Otherwise CRC16: a trial passes by chance with p = 2^-16. With n_h trials of hypothesis h and H hypotheses tracked,
 * the pass count m is accepted when H * P(Poisson(n_h * 2^-16) >= m) < ALPHA (Bonferroni) and the passes come from
 * >= 2 distinct slots. The DM-RS-SNR plausibility rule is skipped: the verdict only carries the decoder's own SNR scale. */
#define CRC_GUARD_ALPHA 0.01
static bool crc_guard_accept_locked(len_state_t *ls, int h, int k, int t, int var, uint16_t rnti, long slot, uint32_t tbs_bits,
                                    int nseg, int seg_ok)
{
  const uint32_t e = (uint32_t)(h | k << 8 | t << 11 | var << 13);
  const bool decisive = tbs_bits > 3824 || (nseg > 1 && seg_ok == nseg);
  typeof(ls->ev[0]) *v = ev_slot_locked(ls, e);
  v->passes++;
  int i;
  for (i = 0; i < v->nr; i++) if (v->rnti[i] == rnti) break;
  if (i == v->nr && v->nr < 4) v->rnti[v->nr++] = rnti;
  for (i = 0; i < v->ns; i++) if (v->slot[i] == slot) break;
  if (i == v->ns && v->ns < 4) v->slot[v->ns++] = slot;
  int H = 0;
  for (i = 0; i < CRC_EV_MAX; i++) if (ls->ev[i].e) H++;
  const double pf = H * pois_tail(v->trials / 65536.0, (int)v->passes);
  const bool acc = decisive || (v->passes >= 2 && v->ns >= 2 && pf < CRC_GUARD_ALPHA);
  if (decisive) ls->ev_dec++; else if (!acc) ls->ev_susp++; else ls->ev_lock++;
  LOG_A(PHY, "SENSING: ULLAYOUT_CRC_EVIDENCE len=%u hyp=%u/%u/%u tbs=%u nseg=%d decisive=%d passes=%u trials=%u rntis=%u slots=%u"
             " p_false=%.3g -> %s\n",
        ls->len, h, k, t, tbs_bits, nseg, decisive, v->passes, v->trials, v->nr, v->ns, pf, acc ? "LOCK" : "SUSPECT");
  return acc;
}

void nr_ul_layout_crc_result(const nr_pdcch_blind_ul_result_t *g, bool ok)
{
  nr_ul_layout_crc_verdict(g, ok, 0, 0, 0, 0);
}

void nr_ul_layout_crc_verdict(const nr_pdcch_blind_ul_result_t *g, bool ok, uint32_t tbs_bits, int nseg, int seg_ok, long slot)
{
  if (!nr_ul_layout_is_tagged(g) || nr_ul_layout_is_probe(g)) return;
  pthread_mutex_lock(&mu);
  int li = -1;
  for (int l = 0; l < g_nlen; l++) if (g_len[l].len == g->dci_length) li = l;
  if (li >= 0 && (g->hyp_generation & NR_UL_LAYOUT_GEN_MASK) == (g_gen & NR_UL_LAYOUT_GEN_MASK)) {
    len_state_t *ls = &g_len[li];
    if (nr_ul_layout_is_trial(g)) {
      if (ls->fitted && ls->prov && !ls->locked) ls->prov_done++;
      const unsigned e = (unsigned)((g->hyp_generation >> 40) & 0x7ffff);
      const int h = e & 255, k = (e >> 8) & 7, t = (e >> 11) & 3, var = (e >> 13) & 63;
      if (ok) {
        const bool decisive = tbs_bits > 3824 || (nseg > 1 && seg_ok == nseg);
        if (ls->locked && g_locked_hyp == h && ls->chosen_tail == t && ls->chosen_var == var) { ls->crc_ok++; ls->crc_fail_run = 0; ls->lock_decisive += decisive; }
        /* a non-default table / k2 latches only on two independent CRC passes (distinct slots), guard or not */
        else if (crc_guard_accept_locked(ls, h, k, t, var, g->rnti, slot, tbs_bits, nseg, seg_ok))
        { crc_lock_locked(li, h, k, t, var, ls->bank[(ls->count - 1) % NR_UL_LAYOUT_BANK].slot);
          ls->lock_decisive += decisive; }
      }
    } else if (ls->locked) {
      if (ok) { ls->crc_ok++; ls->crc_fail_run = 0; ls->lock_decisive += tbs_bits > 3824 || (nseg > 1 && seg_ok == nseg); }
      else if (ls->crc_fail_run < 1000000) ls->crc_fail_run++;
    }
  }
  pthread_mutex_unlock(&mu);
}

/* ISAC_UL_NID_BG: a hit of the background n_ID search that already passed its own confirmation rule (CRC24, or CRC16 with the same id in >= 2 slots) is a decisive
 * CRC pass of the tagged hypothesis. keep_table: the hit does not tell the MCS tables apart (same modulation and code rate), so the verdict carries the lock's
 * current table instead of the hypothesis's. */
void nr_ul_layout_nid_verdict(const nr_pdcch_blind_ul_result_t *g, bool keep_table, uint32_t tbs_bits, long slot)
{
  if (!nr_ul_layout_is_tagged(g) || !nr_ul_layout_is_trial(g)) return;
  nr_pdcch_blind_ul_result_t t = *g;
  if (keep_table) {
    pthread_mutex_lock(&mu);
    int li = -1;
    for (int l = 0; l < g_nlen; l++) if (g_len[l].len == g->dci_length) li = l;
    if (li >= 0) {
      const len_state_t *ls = &g_len[li];
      uint64_t e = (g->hyp_generation >> 40) & 0x7ffff;
      const uint64_t var = (e >> 13) & 63, cur = ls->locked ? (uint64_t)(ls->chosen_var & 7) : (uint64_t)(g_lat_var & 7);
      e = (e & ~(63ull << 13)) | (((var & ~7ull) | cur) << 13);
      t.hyp_generation = (g->hyp_generation & ~(0x7ffffull << 40)) | (e << 40);
    }
    pthread_mutex_unlock(&mu);
  }
  nr_ul_layout_crc_verdict(&t, true, tbs_bits, 2, 2, slot);
}

/* The prior-ranked provisional BWP search (ULPRIOR) was removed; prior_note stays a no-op until nr_pusch_passive_queue.c stops calling it. */
bool nr_ul_layout_prior_enabled(void) { return false; }
void nr_ul_layout_prior_note(const nr_pdcch_blind_ul_result_t *g, bool ok, uint32_t tbs_bits, int nseg, int seg_ok, long slot,
                             float snr_db, float se)
{
  (void)g; (void)ok; (void)tbs_bits; (void)nseg; (void)seg_ok; (void)slot; (void)snr_db; (void)se;
}

void nr_ul_layout_stats_dump(void)
{
  pthread_mutex_lock(&mu);
  char v[512];
  int u = 0;
  for (int h = 0; h < g_nhyp && u < (int)sizeof(v) - 24; h++)
    if (g_hyp[h].votes) u += snprintf(v + u, sizeof(v) - u, " %u@%u:%u", g_hyp[h].n, g_hyp[h].start, g_hyp[h].votes);
  if (!u) snprintf(v, sizeof(v), " -");
  char ls[256];
  int w = 0;
  for (int l = 0; l < g_nlen && w < (int)sizeof(ls) - 64; l++)
    w += snprintf(ls + w, sizeof(ls) - w, " len%u[dcis=%u fit=%d prov=%d cand=%d/%d rot=%u lock=%d probes=%u gold=%u cdm1=%u cdm2=%u trials=%u crc_ok=%u crc_locks=%u susp=%u dec=%u evlock=%u]",
                  g_len[l].len, g_len[l].count, g_len[l].fitted, g_len[l].prov, g_len[l].cand_i + 1, g_len[l].ncand, g_len[l].prov_rot, g_len[l].locked, g_len[l].probes, g_len[l].gold,
                  g_len[l].cdm1, g_len[l].cdm2, g_len[l].trials, g_len[l].crc_ok, g_len[l].crc_locks, g_len[l].ev_susp, g_len[l].ev_dec, g_len[l].ev_lock);
  char sgn[400] = "";
  { /* top-4 hypotheses by detections; "x>y" = a-only(row, leader) / b-only(leader, row) discordant probes */
    int top[4] = {-1, -1, -1, -1}, q = 0;
    for (int h = 0; h < g_nhyp; h++) {
      if (!g_hyp[h].det) continue;
      int j = 3;
      if (top[3] >= 0 && g_hyp[h].det <= g_hyp[top[3]].det) continue;
      for (; j > 0 && (top[j - 1] < 0 || g_hyp[h].det > g_hyp[top[j - 1]].det); j--) top[j] = top[j - 1];
      top[j] = h;
    }
    q += snprintf(sgn, sizeof(sgn), " signt[");
    for (int i = 0; i < 4 && top[i] >= 0 && q < (int)sizeof(sgn) - 70; i++)
      q += snprintf(sgn + q, sizeof(sgn) - q, " %u@%u:det=%u,rntis=%u,vs_lead=%u>%u", g_hyp[top[i]].n, g_hyp[top[i]].start, g_hyp[top[i]].det,
                    g_hyp[top[i]].nrnti, i ? g_only[top[i]][top[0]] : 0, i ? g_only[top[0]][top[i]] : 0);
    snprintf(sgn + q, sizeof(sgn) - q, " ]");
  }
  LOG_A(PHY, "SENSING: ULLAYOUT STATS bwp=%s%u@%u probes[booked=%u fed=%u skipped=%u] votes=%u ctl_votes=%u discarded=%u tied=%u hyps=%d"
             " tally[%s ]%s%s\n",
        g_locked_hyp >= 0 ? "LOCKED " : "searching ", g_locked_hyp >= 0 ? g_hyp[g_locked_hyp].n : 0,
        g_locked_hyp >= 0 ? g_hyp[g_locked_hyp].start : 0, g_probes_booked, g_probes_fed, g_probes_skipped, g_votes_total, g_ctl_votes,
        g_discarded, g_tied, g_nhyp, v, w ? ls : "", sgn);
  pthread_mutex_unlock(&mu);
}
