/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdcch_ul_config_disc.h"
#include "nr_pdcch_ul_interp_sweep.h"
#include "nr_pdsch_prb_set.h"
#include "nr_passive_cfg_epoch.h"
#include "common/utils/LOG/log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* This is deliberately a small producer-side catalogue. No polar decode, allocation,
 * or search over RNTIs happens here. The common candidate scheduler has already
 * established the CRC, payload length, bank nID and C-RNTI. The DL BWP is a
 * prior, never a hard constraint. A failed TB does not delete a candidate. */
#define UL_CFG_UE NR_PDCCH_BLIND_MAX_UE
#define UL_CFG_HYP 64
#define UL_CFG_TICKET 128
#define UL_CFG_TAG (UINT64_C(1) << 63)
typedef struct {
  nr_pdcch_blind_ul_opts_t opts;
  int16_t prior_score;
  uint16_t proposed, crc_try, crc_ok;
  uint32_t last_harq_slot[16];
  uint8_t last_ndi[16];
  uint8_t seen_harq[16];
  uint32_t last_hash;
  uint8_t distinct_ok;
} hypothesis_t;
typedef struct {
  uint16_t rnti, length;
  uint32_t serial;
  uint64_t bank_key, incarnation;
  uint16_t dl_start, dl_size, carrier;
  uint8_t n;
  int8_t locked;
  hypothesis_t h[UL_CFG_HYP];
} ue_t;
typedef struct {
  uint64_t token;
  uint64_t incarnation;
  uint32_t hash;
  uint16_t rnti, length;
  uint8_t ue, hyp;
  bool consumed;
} ticket_t;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static ue_t ue[UL_CFG_UE];
static ticket_t tickets[UL_CFG_TICKET];
static uint64_t serial = 1;
static nr_pdcch_ul_config_stats_t stats;
static _Atomic uint64_t proposal_busy_drops, bank_busy_drops;
static bool config_dirty;
static uint8_t dl_hits[20];
static uint16_t dl_total;
static uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
}
static void proposal_cost(uint64_t start)
{
  const uint64_t ns = now_ns() - start;
  ++stats.proposal_calls;
  stats.proposal_ns += ns;
  if (ns > stats.proposal_max_ns) stats.proposal_max_ns = ns;
}
typedef struct {
  uint64_t bank_key;
  uint16_t rnti, length;
  uint32_t first_slot, lock_slot, last_slot, count;
} bank_note_t;
static bank_note_t bank_note[64];
static uint32_t bank_note_clock;

bool nr_pdcch_ul_bank_enabled(void)
{
  static _Atomic int enabled = -1;
  int on = atomic_load_explicit(&enabled, memory_order_relaxed);
  if (on < 0) {
    const char *s = getenv("ISAC_UL_DCI_BANK");
    const char *chain = getenv("ISAC_UL_PUSCH_CHAIN");
    const char *solver = getenv("ISAC_UL_PUSCH_SOLVER");
    on = (s && strcmp(s, "1") == 0) || (chain && strcmp(chain, "1") == 0) ||
         (solver && strcmp(solver, "1") == 0);
    atomic_store_explicit(&enabled, on, memory_order_relaxed);
  }
  return on != 0;
}

void nr_pdcch_ul_bank_note(uint64_t bank_key, uint16_t rnti, uint16_t length,
                           uint32_t abs_slot, bool length_lock)
{
  if (!nr_pdcch_ul_bank_enabled() || !rnti || !length) return;
  if (pthread_mutex_trylock(&mu) != 0) {
    atomic_fetch_add_explicit(&bank_busy_drops, 1, memory_order_relaxed);
    return;
  }
  config_dirty = true;
  bank_note_t *e = NULL, *empty = NULL;
  for (int i = 0; i < 64; ++i) {
    if (bank_note[i].rnti == rnti && bank_note[i].bank_key == bank_key) { e = &bank_note[i]; break; }
    if (!empty && !bank_note[i].rnti) empty = &bank_note[i];
  }
  if (!e) e = empty ? empty : &bank_note[++bank_note_clock % 64];
  if (e->rnti != rnti || e->bank_key != bank_key)
    *e = (bank_note_t){.bank_key=bank_key,.rnti=rnti};
  if (length_lock) {
    if (!e->lock_slot || e->length != length) {
      e->length = length; e->lock_slot = abs_slot;
      LOG_A(PHY, "SENSING: UL_BANK_LENGTH_LOCK rnti=0x%04x len=%u abs_slot=%u bank=%llu first_01_slot=%u count=%u\n",
            rnti, length, abs_slot, (unsigned long long)bank_key, e->first_slot, e->count);
    }
  } else {
    if (e->count && e->last_slot == abs_slot) {
      pthread_mutex_unlock(&mu);
      return;
    }
    e->last_slot = abs_slot;
    if (!e->count) {
      e->first_slot = abs_slot;
      LOG_A(PHY, "SENSING: UL_BANK_FIRST_01 rnti=0x%04x len=%u abs_slot=%u bank=%llu\n",
            rnti, length, abs_slot, (unsigned long long)bank_key);
    }
    ++e->count;
    if ((e->count % 64) == 0)
      LOG_A(PHY, "SENSING: UL_BANK_COUNT rnti=0x%04x len=%u first_01_slot=%u lock_slot=%u count=%u\n",
            rnti, length, e->first_slot, e->lock_slot, e->count);
  }
  pthread_mutex_unlock(&mu);
}

bool nr_pdcch_ul_config_disc_enabled(void)
{
  static _Atomic int enabled = -1;
  int on = atomic_load_explicit(&enabled, memory_order_relaxed);
  if (on < 0) {
    const char *s = getenv("ISAC_UL_CONFIG_DISC");
    const char *chain = getenv("ISAC_UL_PUSCH_CHAIN");
    const char *solver = getenv("ISAC_UL_PUSCH_SOLVER");
    on = (s && strcmp(s, "1") == 0) || (chain && strcmp(chain, "1") == 0) ||
         (solver && strcmp(solver, "1") == 0);
    atomic_store_explicit(&enabled, on, memory_order_relaxed);
  }
  return on != 0;
}

void nr_pdcch_ul_config_disc_note_dl_slot(uint32_t abs_slot)
{
  if (!nr_pdcch_ul_config_disc_enabled() || pthread_mutex_trylock(&mu) != 0) return;
  config_dirty = true;
  uint8_t *h = &dl_hits[abs_slot % 20];
  if (*h < 15) ++*h;
  if (dl_total < UINT16_MAX) ++dl_total;
  pthread_mutex_unlock(&mu);
}

void nr_pdcch_ul_config_disc_reset(void)
{
  pthread_mutex_lock(&mu);
  if (!config_dirty) { pthread_mutex_unlock(&mu); return; }
  memset(ue, 0, sizeof(ue));
  memset(tickets, 0, sizeof(tickets));
  memset(bank_note, 0, sizeof(bank_note));
  memset(dl_hits, 0, sizeof(dl_hits));
  dl_total = 0;
  memset(&stats, 0, sizeof(stats));
  atomic_store_explicit(&proposal_busy_drops, 0, memory_order_relaxed);
  atomic_store_explicit(&bank_busy_drops, 0, memory_order_relaxed);
  ++serial;
  config_dirty = false;
  pthread_mutex_unlock(&mu);
}

/* Put the DL solution first, then whole-carrier and nearby BWPs. Deduplicate
 * geometries. All candidates stay inside Point A..carrier_rb. */
static void geometry(ue_t *u, const nr_pdcch_blind_ul_opts_t *base,
                     const nr_pdcch_ul_config_prior_t *p, uint16_t length)
{
  const int carrier = p->carrier_rb;
  const int ds = p->dl_bwp_size ? p->dl_bwp_size : carrier;
  const int db = p->dl_bwp_size ? p->dl_bwp_start : 0;
  const int starts[] = {db, 0, db - 6, db + 6, db - 12, db + 12, db, db, 0, (carrier - 52) / 2};
  const int sizes[] = {ds, carrier, ds, ds, ds, ds, ds - 12, ds + 12, 52, 52};
  /* The DCI length constrains the sum of these three widths, not their split.
   * Retaining only the first split makes every later grant use the same wrong
   * field offsets when RRC chose, for example, one BWP bit instead of one CSI
   * bit. Give each geometry its first split before spending slots on more. */
  static const uint8_t width_pairs[][2] = {
    {0,0}, {1,0}, {0,1}, {1,1}, {0,2}, {2,0}, {1,2}, {2,1}, {2,2}
  };
  static int solver_cached=-1; /* read once (review fix: no per-proposal getenv) */
  if (__atomic_load_n(&solver_cached,__ATOMIC_RELAXED)<0) {
    const char *solver_flag=getenv("ISAC_UL_PUSCH_SOLVER");
    __atomic_store_n(&solver_cached,(solver_flag && strcmp(solver_flag,"1")==0)?1:0,__ATOMIC_RELAXED);
  }
  const bool solver=__atomic_load_n(&solver_cached,__ATOMIC_RELAXED)==1;
  for (int pass = 0; pass < (solver ? 5 : 4) && u->n < UL_CFG_HYP; ++pass)
  for (int g = 0; g < 10 && u->n < UL_CFG_HYP; ++g) {
    const int start = starts[g], size = sizes[g];
    if (start < 0 || size < 1 || start + size > carrier || size > 275) continue;
    for (int mode = 0; mode < 3 && u->n < UL_CFG_HYP; ++mode)
    for (int td = 0; td < (g < 4 && base->tda_count == 0 ? 3 : 1) && u->n < UL_CFG_HYP; ++td) {
      nr_pdcch_blind_ul_opts_t o = *base;
      o.bwp_start = start; o.bwp_size = size;
      o.fdra_bwp_start = start;
      o.fdra_mode = mode == 0 ? NR_FDRA_TYPE1 : mode == 1 ? NR_FDRA_TYPE0_CFG1 : NR_FDRA_DYN_CFG1;
      /* A configured hopping flag consumes one DCI bit. Give its first
       * equal-length width split an early slot in the catalogue. */
      if (solver && pass == 1) o.freq_hopping_bits = 1;
      if (td) {
        /* Unknown RRC list: search two small legal tables as alternatives
         * to TS 38.214's 16-row default. The row is intentionally a
         * hypothesis; only TB CRC can identify the actual k2/S/L. */
        o.tda_count = td == 1 ? 2 : 4;
        for (int row = 0; row < o.tda_count; ++row) {
          o.tda_start[row] = td == 1 ? 0 : 2;
          o.tda_length[row] = td == 1 ? 14 : 12;
          o.tda_mapping[row] = td == 1 ? 0 : 1;
          o.tda_k2[row] = td == 1 ? 4 : 2;
        }
      }
      /* Search only the small field-width neighborhood here. The existing
       * full UL width engine takes over once a BWP has CRC support. */
      o.bwp_indicator_bits = o.sri_bits = o.csi_request_bits = 0;
      const int csi_total = (int)length - nr_pdcch_blind_dci01_size(&o);
      int nth = 0, width_cost = 0;
      bool fit = false;
      for (unsigned w = 0; w < sizeof(width_pairs)/sizeof(width_pairs[0]); ++w) {
        const int bw = width_pairs[w][0], sri = width_pairs[w][1];
        const int csi = csi_total - bw - sri;
        if (csi < 0 || csi > 6) continue;
        if (nth++ != (solver ? (pass <= 1 ? 0 : pass - 1) : pass)) continue;
        o.bwp_indicator_bits = bw;
        o.sri_bits = sri;
        o.csi_request_bits = csi;
        fit = nr_pdcch_blind_dci01_size(&o) == length;
        width_cost = bw + sri + csi;
        break;
      }
      if (!fit) continue;
      bool dup = false;
      for (int k = 0; k < u->n; ++k) {
        const nr_pdcch_blind_ul_opts_t *x = &u->h[k].opts;
        dup |= x->bwp_start == start && x->bwp_size == size && x->fdra_mode == o.fdra_mode
               && x->freq_hopping_bits == o.freq_hopping_bits
               && x->tda_count == o.tda_count
               && x->bwp_indicator_bits == o.bwp_indicator_bits
               && x->sri_bits == o.sri_bits && x->csi_request_bits == o.csi_request_bits;
      }
      if (dup) continue;
      hypothesis_t *h = &u->h[u->n++];
      h->opts = o;
      h->prior_score = (start == db && size == ds ? 12 : 0)
                     + (size == ds ? 3 : 0) + (mode == 0 ? 2 : 0)
                     - width_cost - (td ? 2 : 0);
    }
  }
}

static int score(const hypothesis_t *h, const nr_pdcch_blind_ul_result_t *g,
                 const nr_pdcch_ul_config_prior_t *p)
{
  int s = h->prior_score + 40 * h->crc_ok - 2 * (h->crc_try - h->crc_ok)
        - (int)h->proposed / 2;
  if (g->rv == 0) s += 2;
  if (g->harq_pid < 16 && h->seen_harq[g->harq_pid]) {
    const int age = (int)p->source_abs_slot - (int)h->last_harq_slot[g->harq_pid];
    if (age > 0 && age < 64) s += (h->last_ndi[g->harq_pid] != g->ndi) ? 2 : 1;
  }
  if (p->tdd_period > 0 && p->tdd_period <= 64) {
    const int slot = (p->source_abs_slot + g->k2) % p->tdd_period;
    s += (p->ul_slot_mask & (UINT64_C(1) << slot)) ? 4 : -5;
  } else if (dl_total >= 10) {
    /* Occupancy is incomplete: an empty DL phase is only a weak UL hint.
     * Several repeated DL accepts in a phase are evidence against k2 there. */
    s += dl_hits[(p->source_abs_slot + g->k2) % 20] >= 3 ? -3 : 1;
  }
  return s;
}

bool nr_pdcch_ul_config_disc_grant(uint16_t rnti, uint16_t length, nr_dci_bits_t payload,
                                   const nr_pdcch_blind_ul_opts_t *base,
                                   const nr_pdcch_ul_config_prior_t *p,
                                   nr_pdcch_blind_ul_result_t *out)
{
  if (!nr_pdcch_ul_config_disc_enabled() || !base || !p || !out || !rnti || !length
      || length > NR_DCI_MAX_PAYLOAD || p->carrier_rb < 1 || p->carrier_rb > 275)
    return false;
  if (pthread_mutex_trylock(&mu) != 0) {
    atomic_fetch_add_explicit(&proposal_busy_drops, 1, memory_order_relaxed);
    return false;
  }
  const uint64_t start_ns = now_ns();
  config_dirty = true;
  int ui = -1, oldest = 0;
  for (int i = 0; i < UL_CFG_UE; ++i) {
    if (ue[i].rnti == rnti && ue[i].length == length
        && ue[i].bank_key == p->bank_key
        && ue[i].dl_start == p->dl_bwp_start && ue[i].dl_size == p->dl_bwp_size
        && ue[i].carrier == p->carrier_rb) { ui = i; break; }
    if (ue[i].serial < ue[oldest].serial) oldest = i;
  }
  if (ui < 0) {
    ui = oldest;
    memset(&ue[ui], 0, sizeof(ue[ui]));
    ue[ui].rnti = rnti; ue[ui].length = length;
    ue[ui].bank_key = p->bank_key;
    ue[ui].incarnation = ++serial;
    ue[ui].dl_start = p->dl_bwp_start; ue[ui].dl_size = p->dl_bwp_size;
    ue[ui].carrier = p->carrier_rb;
    ue[ui].locked = -1;
    geometry(&ue[ui], base, p, length);
  }
  ue_t *u = &ue[ui];
  u->serial = (uint32_t)++serial;
  int best = -1, best_score = -100000;
  nr_pdcch_blind_ul_result_t grant = {0}, chosen = {0};
  for (int i = 0; i < u->n; ++i) {
    hypothesis_t *h = &u->h[i];
    if (!nr_pdcch_blind_extract_01(payload, length, rnti, &h->opts, &grant)
        || grant.carrier_indicator || grant.ul_sul_indicator || grant.start_rb + grant.num_rb > h->opts.bwp_size)
      continue;
    const int s = score(h, &grant, p) + (u->locked == i ? 1000 : 0);
    if (s > best_score) { best = i; best_score = s; chosen = grant; }
  }
  if (best < 0) {
    ++stats.dropped_capacity;
    proposal_cost(start_ns);
    pthread_mutex_unlock(&mu);
    return false;
  }
  hypothesis_t *h = &u->h[best];
  ++h->proposed;
  if (chosen.harq_pid < 16) {
    h->last_harq_slot[chosen.harq_pid] = p->source_abs_slot;
    h->last_ndi[chosen.harq_pid] = chosen.ndi;
    h->seen_harq[chosen.harq_pid] = 1;
  }
  const uint64_t token = UL_CFG_TAG | ++serial;
  ticket_t *t = &tickets[token % UL_CFG_TICKET];
  *t = (ticket_t){.token=token, .hash=nr_dci_bits_hash(&payload,length), .rnti=rnti,
                  .length=length, .incarnation=u->incarnation, .ue=ui, .hyp=best};
  chosen.hyp_generation = token;
  chosen.width_hyp_class = chosen.interp_hyp_class = -1;
  *out = chosen;
  ++stats.proposed;
  proposal_cost(start_ns);
  pthread_mutex_unlock(&mu);
  return true;
}

bool nr_pdcch_ul_config_disc_feedback(const nr_pdcch_blind_ul_result_t *g, bool ok)
{
  if (!g || !(g->hyp_generation & UL_CFG_TAG)) return false;
  if (!nr_cfg_epoch_work_current()) return true;
  pthread_mutex_lock(&mu);
  ticket_t *t = &tickets[g->hyp_generation % UL_CFG_TICKET];
  if (t->token == g->hyp_generation && !t->consumed && t->rnti == g->rnti
      && t->length == g->dci_length && t->hash == nr_dci_bits_hash(&g->raw_payload,g->dci_length)
      && t->ue < UL_CFG_UE && t->hyp < ue[t->ue].n && ue[t->ue].rnti == t->rnti
      && ue[t->ue].incarnation == t->incarnation) {
    t->consumed = true;
    hypothesis_t *h = &ue[t->ue].h[t->hyp];
    if (h->crc_try < UINT16_MAX) ++h->crc_try;
    if (ok) {
      ++stats.feedback_ok;
      const uint32_t hash = t->hash;
      if (h->crc_ok < UINT16_MAX) ++h->crc_ok;
      if (hash != h->last_hash) { h->last_hash = hash; if (h->distinct_ok < 2) ++h->distinct_ok; }
      if (h->distinct_ok >= 2 && ue[t->ue].locked != t->hyp) {
        ue[t->ue].locked = t->hyp;
        LOG_A(PHY, "SENSING: UL_CONFIG_DISC TB-CRC lock rnti=0x%04x BWP=%u+%u FDRA=%d len=%u\n",
              g->rnti, h->opts.bwp_start, h->opts.bwp_size, h->opts.fdra_mode, g->dci_length);
      }
    } else ++stats.feedback_bad;
  }
  pthread_mutex_unlock(&mu);
  return true;
}

void nr_pdcch_ul_config_disc_stats(nr_pdcch_ul_config_stats_t *out)
{
  if (!out) return;
  pthread_mutex_lock(&mu);
  *out = stats;
  out->dropped_busy = atomic_load_explicit(&proposal_busy_drops, memory_order_relaxed);
  out->bank_note_dropped_busy = atomic_load_explicit(&bank_busy_drops, memory_order_relaxed);
  for (int i = 0; i < UL_CFG_UE; ++i) {
    out->live_hypotheses += ue[i].n;
    out->locked_hypotheses += ue[i].rnti != 0 && ue[i].locked >= 0;
  }
  pthread_mutex_unlock(&mu);
}

bool nr_pdcch_ul_config_disc_locked(uint16_t rnti, uint16_t length,
                                    const nr_pdcch_ul_config_prior_t *prior,
                                    nr_pdcch_blind_ul_opts_t *out)
{
  if (!out || !prior || !nr_pdcch_ul_config_disc_enabled()) return false;
  if (pthread_mutex_trylock(&mu) != 0) return false;
  bool found = false;
  for (int i = 0; i < UL_CFG_UE; ++i)
    if (ue[i].rnti == rnti && ue[i].length == length && ue[i].locked >= 0
        && ue[i].bank_key == prior->bank_key
        && ue[i].dl_start == prior->dl_bwp_start && ue[i].dl_size == prior->dl_bwp_size
        && ue[i].carrier == prior->carrier_rb) {
      *out = ue[i].h[ue[i].locked].opts;
      found = true;
      break;
    }
  pthread_mutex_unlock(&mu);
  return found;
}

int nr_pdcch_ul_config_disc_options(const nr_pdcch_blind_ul_result_t *g,
                                    nr_pdcch_blind_ul_opts_t *out, int capacity,
                                    int *selected)
{
  if (selected) *selected = -1;
  if (!g || !out || capacity <= 0 || !(g->hyp_generation & UL_CFG_TAG)) return 0;
  pthread_mutex_lock(&mu);
  const ticket_t *t = &tickets[g->hyp_generation % UL_CFG_TICKET];
  int n = 0;
  if (t->token == g->hyp_generation && t->rnti == g->rnti && t->length == g->dci_length
      && t->hash == nr_dci_bits_hash(&g->raw_payload, g->dci_length) && t->ue < UL_CFG_UE
      && ue[t->ue].incarnation == t->incarnation && ue[t->ue].rnti == g->rnti) {
    const ue_t *u = &ue[t->ue];
    n = u->n < capacity ? u->n : capacity;
    if (selected) *selected = t->hyp;
    for (int i = 0; i < n; ++i) out[i] = u->h[i].opts;
  }
  pthread_mutex_unlock(&mu);
  return n;
}
