/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pusch_ul_disc.h"
#include "nr_pdcch_blind_monitor.h"
#include "PHY/NR_REFSIG/nr_refsig.h"
#include "PHY/gold.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UL_DISC_TRACKS 128
#define UL_DISC_SCAN_PER_GRANT 64
#define UL_DISC_FAMILY (65536.0 * 2.0 * 2.0 * 32.0)
#define UL_DISC_ALPHA 1.0e-6
typedef struct {
  uint16_t rnti;
  bool used, timed;
  long last_slot;
  double correction;
  uint32_t scan_cursor;
  nr_pusch_ul_disc_result_t found;
} ul_disc_track_t;
static ul_disc_track_t tracks[UL_DISC_TRACKS];
static pthread_mutex_t track_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned evict_next;
static uint32_t current_epoch;

static ul_disc_track_t *track(uint16_t rnti)
{
  for (int i = 0; i < UL_DISC_TRACKS; ++i)
    if (tracks[i].used && tracks[i].rnti == rnti) return &tracks[i];
  for (int i = 0; i < UL_DISC_TRACKS; ++i)
    if (!tracks[i].used) {
      tracks[i] = (ul_disc_track_t){.rnti=rnti, .used=true};
      return &tracks[i];
    }
  ul_disc_track_t *t = &tracks[evict_next++ % UL_DISC_TRACKS];
  *t = (ul_disc_track_t){.rnti=rnti, .used=true};
  return t;
}

nr_pusch_ul_disc_result_t nr_pusch_ul_disc_cached(uint16_t rnti)
{
  nr_pusch_ul_disc_result_t result = {.id=-1, .false_alarm_bound=1};
  pthread_mutex_lock(&track_lock);
  for (int i = 0; i < UL_DISC_TRACKS; ++i)
    if (tracks[i].used && tracks[i].rnti == rnti) {
      result = tracks[i].found;
      result.fresh = false;
      break;
    }
  pthread_mutex_unlock(&track_lock);
  return result;
}

void nr_pusch_ul_disc_reset(void)
{
  pthread_mutex_lock(&track_lock);
  memset(tracks, 0, sizeof(tracks));
  evict_next = 0;
  current_epoch = 0;
  pthread_mutex_unlock(&track_lock);
}

void nr_pusch_ul_disc_epoch(uint32_t dedicated_epoch)
{
  pthread_mutex_lock(&track_lock);
  if (current_epoch != dedicated_epoch) {
    memset(tracks, 0, sizeof(tracks));
    evict_next = 0;
    current_epoch = dedicated_epoch;
  }
  pthread_mutex_unlock(&track_lock);
}

int nr_pusch_ul_disc_timing_seed(uint16_t rnti, int nominal_advance, long abs_slot, bool *tracked)
{
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  if (t->timed && (abs_slot <= t->last_slot || abs_slot - t->last_slot > 2000)) t->timed = false;
  if (tracked) *tracked = t->timed;
  const int correction = t->timed ? (int)lrint(t->correction) : 0;
  pthread_mutex_unlock(&track_lock);
  return nominal_advance + correction;
}

void nr_pusch_ul_disc_timing_update(uint16_t rnti, int nominal_advance,
                                    int measured_advance, double score, long abs_slot)
{
  if (score < 0.35 || abs(measured_advance - nominal_advance) > 2048) return;
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  const double correction = measured_advance - nominal_advance;
  if (!t->timed || abs_slot <= t->last_slot || abs_slot - t->last_slot > 2000) {
    t->correction = correction;
    t->timed = true;
  } else if (fabs(correction - t->correction) <= 256.0) {
    t->correction += 0.25 * (correction - t->correction);
  }
  t->last_slot = abs_slot;
  pthread_mutex_unlock(&track_lock);
}

static void gold_for(uint32_t *seq, int words, int n_sym, int slot, int sym, int id, int nscid)
{
  const uint64_t a = ((uint64_t)n_sym * slot + sym + 1) * (((uint64_t)id << 1) + 1);
  uint32_t x2 = (uint32_t)(((a << 17) + ((uint64_t)id << 1) + nscid) % (1ULL << 31));
  uint32_t x1 = 0;
  seq[0] = gold_generic(&x1, &x2, 1);
  for (int i = 1; i < words; ++i) seq[i] = gold_generic(&x1, &x2, 0);
}

static double score_impl(const nr_pusch_ul_disc_grid_t *g, uint16_t mask,
                         int type, int nscid, int id, int *pairs, bool phase_sensitive)
{
  if (pairs) *pairs = 0;
  if (!g || !mask || g->fft <= 0 || g->fft > 8192 || g->n_rx < 1 || g->n_rx > 4 ||
      g->n_rb_carrier < 1 || g->n_rb_carrier > 275 || g->rb_count < 1 ||
      g->rb_start < 0 || g->rb_start + g->rb_count > g->n_rb_carrier ||
      g->symbols_per_slot < 1 || g->symbols_per_slot > 14 ||
      (mask >> g->symbols_per_slot) || type < 0 || type > 1 || nscid < 0 || nscid > 1 ||
      id < 0 || id > 65535) return 0;
  for (int a = 0; a < g->n_rx; ++a) if (!g->rx[a]) return 0;
  const int nprb = type ? 4 : 6;
  const int ntot = nprb * (g->rb_start + g->rb_count);
  const int words = ((g->n_rb_carrier * 24) >> 5) + 1;
  uint32_t seq[208];
  c16_t pilot[1650];
  if (words > 208 || ntot > 1650) return 0;
  double nr = 0, ni = 0, den = 0;
  int count = 0;
  for (int sym = 0; sym < g->symbols_per_slot; ++sym) {
    if (!(mask & (1u << sym))) continue;
    gold_for(seq, words, g->symbols_per_slot, g->slot, sym, id, nscid);
    nr_pdsch_dmrs_rx(g->normal_cp ? NR_NORMAL : NR_EXTENDED, seq, pilot, 1000, 0,
                     (unsigned short)(g->rb_start + g->rb_count), type, 16384);
    const c16_t *p = pilot + nprb * g->rb_start;
    for (int a = 0; a < g->n_rx; ++a) {
      const c16_t *row = g->rx[a] + sym * g->fft;
      double prev_r = 0, prev_i = 0;
      for (int m = 0; m < nprb * g->rb_count; ++m) {
        const int off = type ? 6 * (m / 2) + m % 2 : 4 * (m / 2) + 2 * (m % 2);
        const int bin = (g->first_carrier + 12 * g->rb_start + off) % g->fft;
        const c16_t y = row[bin];
        const double hr = (double)p[m].r * y.r - (double)p[m].i * y.i;
        const double hi = (double)p[m].r * y.i + (double)p[m].i * y.r;
        den += hr * hr + hi * hi;
        if (m) {
          nr += hr * prev_r + hi * prev_i;
          ni += hi * prev_r - hr * prev_i;
          ++count;
        }
        prev_r = hr; prev_i = hi;
      }
    }
  }
  if (pairs) *pairs = count;
  return den > 0 ? (phase_sensitive ? nr : hypot(nr, ni)) / den : 0;
}

double nr_pusch_ul_disc_score(const nr_pusch_ul_disc_grid_t *g, uint16_t mask,
                              int type, int nscid, int id, int *pairs)
{
  return score_impl(g, mask, type, nscid, id, pairs, false);
}

double nr_pusch_ul_disc_delay_score(const nr_pusch_ul_disc_grid_t *g, uint16_t mask,
                                    int type, int nscid, int id, int *pairs)
{
  return score_impl(g, mask, type, nscid, id, pairs, true);
}

void nr_pusch_ul_disc_score_batch(const nr_pusch_ul_disc_grid_t *g,
                                  const nr_pusch_ul_disc_hyp_t *h, int count,
                                  nr_pusch_ul_disc_metric_t *m)
{
  if (!h || !m || count < 0) return;
  for (int i = 0; i < count; ++i)
    m[i].score = score_impl(g, h[i].mask, h[i].dmrs_type, h[i].nscid,
                            h[i].id, &m[i].pairs, false);
}

bool nr_pusch_ul_disc_accept(double score, int pairs, double *bound)
{
  /* Conservative complex random-walk tail and union over the search family.
   * This is a design bound for independent circular Gaussian noise, not a
   * guarantee for correlated interference or a wrong grant. */
  const double p = pairs >= 128 && isfinite(score) && score > 0
      ? fmin(1.0, 2.0 * UL_DISC_FAMILY * exp(-0.25 * pairs * score * score)) : 1.0;
  if (bound) *bound = p;
  return p <= UL_DISC_ALPHA;
}

static uint16_t observed_symbols(const nr_pusch_ul_disc_grid_t *g, int id, int nscid, int type)
{
  uint16_t mask = 0;
  nr_pusch_ul_disc_hyp_t h[14];
  nr_pusch_ul_disc_metric_t m[14];
  int n = 0;
  for (int sym = g->start_symbol; sym < g->start_symbol + g->num_symbols; ++sym)
    h[n++] = (nr_pusch_ul_disc_hyp_t){.mask=1u << sym, .id=id,
                                      .dmrs_type=type, .nscid=nscid};
  if (!n) return 0;
  nr_pusch_ul_disc_score_batch(g, h, n, m);
  for (int i = 0; i < n; ++i)
    if (nr_pusch_ul_disc_accept(m[i].score, m[i].pairs, NULL)) mask |= h[i].mask;
  return mask;
}

static void consider(const nr_pusch_ul_disc_grid_t *g, nr_pusch_ul_disc_result_t *best,
                     uint16_t observed, int id, int nscid, int type,
                     int mapping, int add, int len, int front_load, int type_a_pos)
{
  if (len != front_load) return;
  const int mask = nr_pdcch_blind_ul_dmrs_mask(g->num_symbols, g->start_symbol,
                                               mapping, add, len, type_a_pos);
  if (mask <= 0 || (uint16_t)mask != observed) return;
  int pairs = 0;
  const double score = nr_pusch_ul_disc_score(g, mask, type, nscid, id, &pairs);
  double p = 1;
  if (!nr_pusch_ul_disc_accept(score, pairs, &p)) return;
  if (!best->accepted || p < best->false_alarm_bound) {
    *best = (nr_pusch_ul_disc_result_t){.id=id, .nscid=nscid, .dmrs_type=type,
      .mapping_type=mapping, .add_pos=add, .front_load=len,
      .max_length=len == 2 ? 2 : 0, .mask=(uint16_t)mask,
      .coherence=score, .false_alarm_bound=p, .accepted=true, .fresh=true};
  } else if (best->id == id && best->nscid == nscid && best->dmrs_type == type &&
             best->mask == (uint16_t)mask && fabs(p - best->false_alarm_bound) < 1e-12) {
    /* Multiple RRC layouts can produce the same observed symbol mask. The
     * waveform then proves the mask, not the distinct ASN.1 configuration. */
    if (best->mapping_type != mapping) best->mapping_type = -1;
    if (best->add_pos != add) best->add_pos = -1;
  }
}

nr_pusch_ul_disc_result_t nr_pusch_ul_disc_search(const nr_pusch_ul_disc_grid_t *g,
                                                  uint16_t rnti, uint8_t type_a_pos,
                                                  const int *prior_ids, int npriors,
                                                  int preferred_type, int preferred_nscid,
                                                  uint8_t antenna_ports_field)
{
  nr_pusch_ul_disc_result_t best = {.id=-1, .false_alarm_bound=1};
  if (!g || g->symbols_per_slot < 1 || g->symbols_per_slot > 14 ||
      g->num_symbols <= 0 || g->num_symbols > 14 || g->start_symbol < 0 ||
      g->start_symbol + g->num_symbols > g->symbols_per_slot) return best;
  pthread_mutex_lock(&track_lock);
  ul_disc_track_t *t = track(rnti);
  const nr_pusch_ul_disc_result_t cached = t->found;
  const uint32_t cursor = t->scan_cursor;
  t->scan_cursor = cursor + UL_DISC_SCAN_PER_GRANT;
  pthread_mutex_unlock(&track_lock);
  int front_load[2] = {0,0};
  for (int type = 0; type < 2; ++type) {
    uint8_t cdm = 0;
    uint16_t ports = 0;
    const long one = 1;
    if (decode_dci_antenna_ports_val(1, type ? &one : NULL,
                                     NR_PUSCH_Config__transformPrecoder_disabled,
                                     antenna_ports_field, &cdm, &ports, &front_load[type]) != 0)
      front_load[type] = 0;
  }

  /* DL-known physical IDs first, full legal symbol-layout family. */
  for (int i = -1; i < npriors && i < 8; ++i) {
    if (i == -1 && !cached.accepted) continue;
    const int id = i == -1 ? cached.id : prior_ids[i];
    if (id < 0 || id > 65535) continue;
    for (int ns = 0; ns < 2; ++ns)
      for (int type = 0; type < 2; ++type) {
        const int first_ns = i == -1 ? cached.nscid : preferred_nscid;
        const int first_type = i == -1 ? cached.dmrs_type : preferred_type;
        const int nscid = ns == 0 ? first_ns : 1-first_ns;
        const int dmrs_type = type == 0 ? first_type : 1-first_type;
        if (!front_load[dmrs_type]) continue;
        const uint16_t observed = observed_symbols(g, id, nscid, dmrs_type);
        if (!observed) continue;
        for (int mapping = 0; mapping < 2; ++mapping)
          for (int add = 0; add < 4; ++add)
            for (int len = 1; len <= 2; ++len)
              consider(g, &best, observed, id, nscid, dmrs_type, mapping, add, len,
                       front_load[dmrs_type], type_a_pos);
        if (best.accepted) goto publish;
      }
  }
  if (!best.accepted) {
    /* A first DM-RS symbol is enough to reject almost every wrong ID, and it
     * avoids a 16-bit Cartesian product with all additional-position masks.
     * Type A begins at l0=2/3; type B begins at the allocation start. Once a
     * physical ID crosses the familywise gate, rank its complete layouts. */
    const int firsts[3] = {g->start_symbol, 2, 3};
    nr_pusch_ul_disc_hyp_t h[UL_DISC_SCAN_PER_GRANT * 12];
    nr_pusch_ul_disc_metric_t m[UL_DISC_SCAN_PER_GRANT * 12];
    int n = 0;
    for (int k = 0; k < UL_DISC_SCAN_PER_GRANT; ++k)
      for (int ns = 0; ns < 2; ++ns)
        for (int type = 0; type < 2; ++type)
          for (int s = 0; s < 3; ++s) {
            if (!front_load[type]) continue;
            const int sym = firsts[s];
            if (sym < g->start_symbol || sym >= g->start_symbol + g->num_symbols ||
                (s && sym == firsts[0]) || (s == 2 && sym == firsts[1])) continue;
            h[n++] = (nr_pusch_ul_disc_hyp_t){.mask=1u << sym,
              .id=(cursor+k)&65535, .dmrs_type=type, .nscid=ns};
          }
    nr_pusch_ul_disc_score_batch(g, h, n, m);
    for (int i = 0; i < n; ++i) {
      if (!nr_pusch_ul_disc_accept(m[i].score, m[i].pairs, NULL)) continue;
      const int id=h[i].id, ns=h[i].nscid, type=h[i].dmrs_type;
      const uint16_t observed = observed_symbols(g, id, ns, type);
      for (int mapping = 0; mapping < 2; ++mapping)
        for (int add = 0; add < 4; ++add)
          for (int len = 1; len <= 2; ++len)
            consider(g, &best, observed, id, ns, type, mapping, add, len,
                     front_load[type], type_a_pos);
    }
  }
publish:
  if (best.accepted) {
    if (cached.accepted && best.id == cached.id && best.nscid == cached.nscid &&
        best.dmrs_type == cached.dmrs_type) best.fresh = false;
    pthread_mutex_lock(&track_lock);
    t = track(rnti);
    if (!t->found.accepted || best.id == t->found.id ||
        best.false_alarm_bound < t->found.false_alarm_bound) t->found = best;
    pthread_mutex_unlock(&track_lock);
  }
  return best;
}
