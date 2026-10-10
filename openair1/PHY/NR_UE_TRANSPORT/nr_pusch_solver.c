/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pusch_solver.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "common/utils/LOG/log.h"
#include "PHY/NR_REFSIG/ul_ref_seq_nr.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UL_SOLVER_UES 64
#define UL_SOLVER_BUCKET 4
#define UL_SOLVER_REFILL_SLOTS 40 /* 50 probes/s at 30 kHz SCS */
#define UL_SOLVER_CPU_CAP_US 200000 /* burst cap; refill is 0.5 core in signal time */

typedef struct {
  nr_ue_ul_ticket_t ticket;
  bool used, candidate, solved;
  uint64_t first_slot, last_probe_slot, cand_slot, cand_layouts, solved_layouts;
  uint32_t cand_hash, cursor, refine_cursor;
  uint16_t cand_tries;
  uint16_t id_cursor;
  uint8_t cand_S, cand_L, cand_add, cand_table, cand_type, cand_max_len;
  uint8_t cand_tp, cand_cdm, cand_mapping;
  uint16_t cand_data_id, cand_dmrs_id, cand_ports;
  uint8_t cand_tda;
  int16_t cand_ta_delta;
  int8_t S_by_tda[16], L_by_tda[16];
  uint8_t full_fail_streak;
  nr_pusch_solver_stats_t stats;
} ul_solver_ue_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static ul_solver_ue_t ues[UL_SOLVER_UES];
static nr_pusch_solver_stats_t totals;
static uint8_t global_tokens = UL_SOLVER_BUCKET;
static uint64_t global_refill_slot;
static uint64_t global_cpu_slot;
static int64_t global_cpu_tokens_us = UL_SOLVER_CPU_CAP_US;
static uint32_t probe_cpu_ema_us = 20000, full_cpu_ema_us = 30000;
static bool prior_valid;
static uint8_t prior_S, prior_L, prior_add, prior_table, prior_type, prior_max_len;
static uint8_t prior_tp, prior_cdm, prior_mapping;
static uint16_t prior_data_id, prior_dmrs_id, prior_ports;
static uint64_t prior_layouts;

bool nr_pusch_solver_enabled(void)
{
  static int enabled = -1;
  if (enabled < 0) {
    const char *v = getenv("ISAC_UL_PUSCH_SOLVER");
    enabled = v && !strcmp(v, "1");
  }
  return enabled == 1;
}

void nr_pusch_solver_reset(void)
{
  pthread_mutex_lock(&mu);
  memset(ues, 0, sizeof(ues));
  memset(&totals, 0, sizeof(totals));
  global_tokens = UL_SOLVER_BUCKET;
  global_refill_slot = 0;
  global_cpu_slot = 0;
  global_cpu_tokens_us = UL_SOLVER_CPU_CAP_US;
  probe_cpu_ema_us = 20000;
  full_cpu_ema_us = 30000;
  prior_valid = false;
  pthread_mutex_unlock(&mu);
}

static bool same_ticket(const nr_ue_ul_ticket_t *a, const nr_ue_ul_ticket_t *b)
{
  return a->rnti == b->rnti && a->identity_gen == b->identity_gen &&
         a->incarnation == b->incarnation && a->epoch == b->epoch;
}

static ul_solver_ue_t *ue_for(const nr_ue_ul_ticket_t *t, bool create)
{
  ul_solver_ue_t *empty = NULL;
  for (int i = 0; i < UL_SOLVER_UES; ++i) {
    if (ues[i].used && same_ticket(&ues[i].ticket, t)) return &ues[i];
    if (!ues[i].used && !empty) empty = &ues[i];
  }
  if (!create) return NULL;
  if (!empty) {
    empty = &ues[0];
    for (int i = 1; i < UL_SOLVER_UES; ++i)
      if (ues[i].last_probe_slot < empty->last_probe_slot) empty = &ues[i];
  }
  *empty = (ul_solver_ue_t){.ticket=*t, .used=true};
  memset(empty->S_by_tda,-1,sizeof(empty->S_by_tda));
  memset(empty->L_by_tda,-1,sizeof(empty->L_by_tda));
  return empty;
}

static bool one_cb_qpsk(const nr_pdcch_blind_ul_result_t *g)
{
  if (!g->plausible || g->rv || g->nrOfLayers != 1 ||
      g->num_rb < 1 || g->num_rb > 48 || g->mcs > 28 || g->num_symbols < 1 ||
      g->start_symbol + g->num_symbols > 14) return false;
  if (g->transform_precoding &&
      (g->dmrs_config_type || g->ra_type0 || g->frequency_hopping ||
       get_index_for_dmrs_lowpapr_seq(g->num_rb * 6) < 0)) return false;
  const int qm = nr_get_Qm_ul(g->mcs, g->mcs_table);
  const int rate = nr_get_code_rate_ul(g->mcs, g->mcs_table);
  if (qm != 2 || rate <= 0) return false;
  const int dmrs = __builtin_popcount((unsigned)g->ul_dmrs_symb_pos);
  if (!dmrs) return false;
  const int re = (g->dmrs_config_type ? 4 : 6) * g->n_dmrs_cdm_groups * dmrs;
  const uint32_t a = nr_compute_tbs(qm, rate, g->num_rb, g->num_symbols, re, 0, 0, 1);
  return a >= 24 && a <= 3824; /* either base graph has exactly one code block */
}

static void apply_physical(nr_pdcch_blind_ul_result_t *g, int physical, int l0,
                           const uint16_t *ids, int nids)
{
  /* Interleave inexpensive high-probability changes before the Cartesian tail.
   * The reading's own TDRA and DM-RS form variant zero. */
  static const struct { uint8_t S,L,mapping; } time_rank[] = {
    {1,13,0}, {0,14,0}, {2,12,0}, {3,11,1},
    {1,12,0}, {0,12,0}, {2,10,1}, {0,7,1}
  };
  static const uint8_t add_rank[] = {1, 2, 0, 3};
  static const uint8_t table_rank[] = {1, 0, 2, 3, 4};
  const bool dmrs_override=physical != 0;
  int id_index=0, dmrs_index=0;
  /* First isolate dedicated DM-RS and data scrambling identities from the
   * time/frequency geometry. A correct pilot ID can be needed before any
   * later physical variant has a chance of passing the CRC. */
  if (physical && nids > 1) {
    if (physical < nids) {
      dmrs_index=physical;
      physical=0;
    } else if (physical < 2*nids-1) {
      id_index=physical-nids+1;
      physical=0;
    } else {
      physical-=2*nids-2;
    }
  }
  if (physical) {
    int shape=physical-1;
    if (nids>0) {
      id_index=shape%nids;
      shape/=nids;
      dmrs_index=(shape/2)%nids;
    }
    const int ti=(shape / 4) % 8;
    const int add = add_rank[shape % 4];
    const int table = table_rank[(shape / 32) % 5];
    g->start_symbol = time_rank[ti].S;
    g->num_symbols = time_rank[ti].L;
    g->mapping_type = time_rank[ti].mapping;
    g->mcs_table = table;
    g->transform_precoding = table >= 3;
    g->dmrs_config_type = (shape / 160) % 2;
    if (g->transform_precoding) g->dmrs_config_type = 0;
    const int max_len = 1 + (shape / 320) % 2;
    const long type2=1;
    const long *type_arg=g->dmrs_config_type ? &type2 : NULL;
    const long tp=g->transform_precoding ? NR_PUSCH_Config__transformPrecoder_enabled
                                         : NR_PUSCH_Config__transformPrecoder_disabled;
    uint8_t cdm=0;
    uint16_t ports=0;
    int front=0;
    if (decode_dci_antenna_ports_val(1,type_arg,tp,g->antenna_ports_field,
                                     &cdm,&ports,&front)!=0 || front>max_len) {
      g->plausible=false;
      return;
    }
    g->n_dmrs_cdm_groups=cdm;
    g->dmrs_ports=ports;
    const int mask = nr_pdcch_blind_ul_dmrs_mask(g->num_symbols, g->start_symbol,
                                                 g->mapping_type, add, max_len, l0);
    if (mask <= 0) { g->plausible=false; return; }
    g->ul_dmrs_symb_pos = (uint16_t)mask;
  }
  if (nids > 0) {
    g->data_scrambling_id = ids[id_index];
    if (dmrs_override) g->ul_dmrs_scrambling_id = ids[dmrs_index];
  }
  g->data_id_advance = false;
  g->fdra_candidate_count = 0;
  g->hyp_generation = 0; /* probe feedback belongs to this solver, not U3 */
}

static bool same_physical(const nr_pdcch_blind_ul_result_t *a, const nr_pdcch_blind_ul_result_t *b)
{
  return a->bwp_start == b->bwp_start && a->bwp_size == b->bwp_size &&
         a->start_rb == b->start_rb && a->num_rb == b->num_rb &&
         a->ra_type0 == b->ra_type0 && a->rbg_bitmap == b->rbg_bitmap &&
         a->start_symbol == b->start_symbol && a->num_symbols == b->num_symbols &&
         a->ul_dmrs_symb_pos == b->ul_dmrs_symb_pos && a->mcs == b->mcs &&
         a->mcs_table == b->mcs_table && a->dmrs_config_type == b->dmrs_config_type &&
         a->n_dmrs_cdm_groups == b->n_dmrs_cdm_groups && a->dmrs_ports == b->dmrs_ports &&
         a->transform_precoding == b->transform_precoding &&
         a->frequency_hopping == b->frequency_hopping && a->mapping_type == b->mapping_type &&
         a->data_scrambling_id == b->data_scrambling_id && a->ul_dmrs_scrambling_id == b->ul_dmrs_scrambling_id;
}

static bool emit(nr_pusch_solver_var_t *out, int *n, int cap,
                 const nr_pusch_solver_reading_t *r, const nr_pdcch_blind_ul_result_t *g,
                 uint64_t source_slot, int l0, bool probe)
{
  if (*n >= cap) return false;
  for (int i = 0; i < *n; ++i)
    if (same_physical(&out[i].grant, g)) return false;
  out[*n] = (nr_pusch_solver_var_t){.grant=*g, .layouts=r->layouts,
    .dci_hash=nr_dci_bits_hash(&r->grant.raw_payload, r->grant.dci_length),
    .dci_slot=source_slot, .type_a_pos=(uint8_t)l0, .probe=probe};
  ++*n;
  return true;
}

int nr_pusch_solver_plan(const nr_ue_ul_ticket_t *ticket,
                         const nr_pusch_solver_reading_t *rd, int nrd,
                         uint64_t source_slot, uint64_t target_slot, int l0,
                         const uint16_t *ids, int nids,
                         nr_pusch_solver_var_t *out, int max_out)
{
  if (!nr_pusch_solver_enabled() || !ticket || !ticket->rnti || !rd || nrd <= 0 || !out || max_out <= 0)
    return 0;
  if (max_out > UL_SOLVER_BUCKET) max_out = UL_SOLVER_BUCKET;
  if (nids < 0) nids = 0;
  if (nids > 4) nids = 4;
  pthread_mutex_lock(&mu);
  ul_solver_ue_t *u = ue_for(ticket, true);
  uint16_t ranked_ids[5];
  for (int i=0;i<nids;++i) ranked_ids[i]=ids[i];
  if (u->stats.grants >= 64 && nids < 5) {
    const uint16_t sweep_id=u->id_cursor++ & 1023;
    bool known=false;
    for (int i=0;i<nids;++i) known |= ranked_ids[i]==sweep_id;
    if (!known) ranked_ids[nids++]=sweep_id;
  }
  ids=ranked_ids;
  if (!u->first_slot) u->first_slot = source_slot ? source_slot : 1;
  ++u->stats.grants; ++totals.grants;
  if (target_slot >= global_refill_slot) {
    uint64_t refill = (target_slot - global_refill_slot) / UL_SOLVER_REFILL_SLOTS;
    if (!global_refill_slot) refill = UL_SOLVER_BUCKET;
    if (refill) {
      global_tokens = global_tokens + refill > UL_SOLVER_BUCKET ? UL_SOLVER_BUCKET : global_tokens + refill;
      global_refill_slot = target_slot;
    }
  }
  if (!global_cpu_slot) global_cpu_slot=target_slot;
  if (target_slot > global_cpu_slot) {
    const uint64_t add=(target_slot-global_cpu_slot)*250u;
    global_cpu_tokens_us += add > UL_SOLVER_CPU_CAP_US ? UL_SOLVER_CPU_CAP_US : (int64_t)add;
    if (global_cpu_tokens_us > UL_SOLVER_CPU_CAP_US) global_cpu_tokens_us=UL_SOLVER_CPU_CAP_US;
    global_cpu_slot=target_slot;
  }
  u->last_probe_slot = target_slot;
  int n = 0;
  if (u->solved) {
    int options[64], nopt=0;
    for (int i = 0; i < nrd && nopt < 64; ++i)
      if (rd[i].layouts & u->solved_layouts) options[nopt++]=i;
    const int best=nopt ? options[u->full_fail_streak % nopt] : -1;
    if (best >= 0) {
      nr_pdcch_blind_ul_result_t g = rd[best].grant;
      if (g.tda_index < 16 && u->S_by_tda[g.tda_index] >= 0) {
        g.start_symbol = (uint8_t)u->S_by_tda[g.tda_index];
        g.num_symbols = (uint8_t)u->L_by_tda[g.tda_index];
      }
      g.mcs_table = u->cand_table; g.dmrs_config_type = u->cand_type;
      g.transform_precoding = u->cand_tp;
      g.n_dmrs_cdm_groups = u->cand_cdm;
      g.dmrs_ports = u->cand_ports;
      g.mapping_type = u->cand_mapping;
      g.data_scrambling_id = u->cand_data_id;
      g.ul_dmrs_scrambling_id = u->cand_dmrs_id;
      const int mask = nr_pdcch_blind_ul_dmrs_mask(g.num_symbols, g.start_symbol,
                          g.mapping_type, u->cand_add, u->cand_max_len, l0);
      if (mask > 0) g.ul_dmrs_symb_pos = (uint16_t)mask;
      g.fdra_candidate_count = 0;
      g.data_id_advance = false;
      g.hyp_generation = 0;
      emit(out, &n, max_out, &rd[best], &g, source_slot, l0, false);
      if (n) {
        if (global_cpu_tokens_us < full_cpu_ema_us) n=0;
        else {
          out[0].reserved_cpu_us=full_cpu_ema_us;
          global_cpu_tokens_us-=full_cpu_ema_us;
          ++u->stats.fulls; ++totals.fulls;
        }
      }
      if (!n) { ++u->stats.drops; ++totals.drops; }
    }
  } else {
    if (u->candidate && source_slot != u->cand_slot) {
      for (int i = 0; i < nrd && n < max_out; ++i) {
        if (!(rd[i].layouts & u->cand_layouts) || rd[i].grant.tda_index != u->cand_tda) continue;
        nr_pdcch_blind_ul_result_t g = rd[i].grant;
        g.start_symbol = u->cand_S; g.num_symbols = u->cand_L;
        g.mcs_table = u->cand_table; g.dmrs_config_type = u->cand_type;
        g.transform_precoding = u->cand_tp;
        g.n_dmrs_cdm_groups = u->cand_cdm;
        g.dmrs_ports = u->cand_ports;
        g.mapping_type = u->cand_mapping;
        g.data_scrambling_id = u->cand_data_id;
        g.ul_dmrs_scrambling_id = u->cand_dmrs_id;
        const int mask = nr_pdcch_blind_ul_dmrs_mask(g.num_symbols, g.start_symbol,
                          g.mapping_type, u->cand_add, u->cand_max_len, l0);
        if (mask > 0) g.ul_dmrs_symb_pos = (uint16_t)mask;
        g.fdra_candidate_count = 0; g.data_id_advance = false; g.hyp_generation = 0;
        if (one_cb_qpsk(&g) && emit(out, &n, max_out, &rd[i], &g, source_slot, l0, true))
          out[n-1].confirmation = true;
      }
    }
    /* First solved layout in the cell is an inexpensive prior, never a hard prune. */
    if (!n && prior_valid && global_tokens && source_slot != u->cand_slot) {
      for (int i = 0; i < nrd && n < max_out; ++i) {
        if (!(rd[i].layouts & prior_layouts)) continue;
        nr_pdcch_blind_ul_result_t g = rd[i].grant;
        g.start_symbol=prior_S; g.num_symbols=prior_L; g.mcs_table=prior_table;
        g.dmrs_config_type=prior_type; g.data_scrambling_id=prior_data_id;
        g.ul_dmrs_scrambling_id=prior_dmrs_id;
        g.transform_precoding=prior_tp; g.n_dmrs_cdm_groups=prior_cdm;
        g.dmrs_ports=prior_ports; g.mapping_type=prior_mapping;
        const int mask=nr_pdcch_blind_ul_dmrs_mask(g.num_symbols,g.start_symbol,
                          g.mapping_type,prior_add,prior_max_len,l0);
        if (mask>0) g.ul_dmrs_symb_pos=(uint16_t)mask;
        g.fdra_candidate_count=0; g.data_id_advance=false; g.hyp_generation=0;
        if (one_cb_qpsk(&g)) emit(out,&n,max_out,&rd[i],&g,source_slot,l0,true);
      }
    }
    /* Each DCI has a different population of valid layouts. Keep a separate
     * rotating pass over its base readings: a single cursor through the
     * Cartesian product otherwise races ahead of physical=0 as nrd changes
     * between DCIs, leaving later small grants entirely unprobed. Reserve
     * one place on alternating grants for time/DM-RS/table hypotheses. */
    const int base_quota=(u->stats.grants & 1) ? max_out : max_out-1;
    const uint32_t base_start=u->cursor++;
    for (int j=0;j<nrd && n<base_quota;++j) {
      const int ri=(base_start+j)%nrd;
      nr_pdcch_blind_ul_result_t g=rd[ri].grant;
      apply_physical(&g,0,l0,ids,nids);
      if (one_cb_qpsk(&g))
        emit(out,&n,max_out,&rd[ri],&g,source_slot,l0,true);
    }
    const int attempts = nrd < 64 ? nrd * 2 : 128;
    for (int j = 0; j < attempts && n < max_out; ++j) {
      const uint32_t k = u->refine_cursor++;
      const int ri = k % nrd;
      const int physical = 1 + k / nrd;
      nr_pdcch_blind_ul_result_t g = rd[ri].grant;
      apply_physical(&g, physical, l0, ids, nids);
      if (!one_cb_qpsk(&g)) continue;
      emit(out, &n, max_out, &rd[ri], &g, source_slot, l0, true);
    }
    if (n > global_tokens) n = global_tokens;
    int admitted=0;
    for (; admitted<n; ++admitted) {
      if (global_cpu_tokens_us < probe_cpu_ema_us) break;
      out[admitted].reserved_cpu_us=probe_cpu_ema_us;
      global_cpu_tokens_us-=probe_cpu_ema_us;
    }
    n=admitted;
    global_tokens -= n;
    u->stats.probes += n; totals.probes += n;
    if (!n) {
      ++u->stats.drops; ++totals.drops;
      if (u->stats.drops <= 8 || (u->stats.drops & 63) == 0) {
        int min_rb=999, min_mcs=99, rv0=0, small=0, qpsk=0, eligible=0;
        for (int i=0;i<nrd;++i) {
          const nr_pdcch_blind_ul_result_t *g=&rd[i].grant;
          if (g->num_rb<min_rb) min_rb=g->num_rb;
          if (g->mcs<min_mcs) min_mcs=g->mcs;
          rv0 += g->rv==0;
          small += g->num_rb<=48;
          qpsk += g->mcs<=28 && nr_get_Qm_ul(g->mcs,g->mcs_table)==2;
          eligible += one_cb_qpsk(g);
        }
        LOG_I(PHY, "SENSING: UL_PUSCH_NO_PROBE rnti=0x%04x src=%lu nrd=%d min_rb=%d min_mcs=%d rv0=%d small=%d qpsk=%d eligible=%d rb=%u mcs=%u/t%u rv=%u S=%u L=%u dmrs=0x%04x tp=%u hop=%u tokens=%u cpu_tokens=%ld cursor=%u\n",
              ticket->rnti,(unsigned long)source_slot,nrd,min_rb,min_mcs,rv0,small,qpsk,eligible,
              rd[0].grant.num_rb,rd[0].grant.mcs,rd[0].grant.mcs_table,
              rd[0].grant.rv,rd[0].grant.start_symbol,rd[0].grant.num_symbols,
              rd[0].grant.ul_dmrs_symb_pos,rd[0].grant.transform_precoding,
              rd[0].grant.frequency_hopping,global_tokens,(long)global_cpu_tokens_us,u->cursor);
      }
    }
  }
  pthread_mutex_unlock(&mu);
  return n;
}

void nr_pusch_solver_feedback(const nr_ue_ul_ticket_t *ticket,
                              const nr_pusch_solver_var_t *v, bool crc_ok,
                              bool credit, uint64_t cpu_us)
{
  if (!ticket || !v) return;
  pthread_mutex_lock(&mu);
  ul_solver_ue_t *u = ue_for(ticket, false);
  if (!u) { pthread_mutex_unlock(&mu); return; }
  global_cpu_tokens_us += (int64_t)v->reserved_cpu_us - (int64_t)cpu_us;
  if (global_cpu_tokens_us > UL_SOLVER_CPU_CAP_US) global_cpu_tokens_us=UL_SOLVER_CPU_CAP_US;
  uint32_t *ema=v->probe ? &probe_cpu_ema_us : &full_cpu_ema_us;
  const uint32_t observed=cpu_us>200000 ? 200000 : (uint32_t)cpu_us;
  *ema=(*ema*7u+observed)/8u;
  if (*ema<5000) *ema=5000;
  if (v->probe) {
    u->stats.probe_cpu_us += cpu_us;
    totals.probe_cpu_us += cpu_us;
  }
  if (!credit) { pthread_mutex_unlock(&mu); return; }
  if (v->probe) {
    if (u->candidate && v->confirmation && !crc_ok && v->dci_slot != u->cand_slot) {
      if (++u->cand_tries >= 96) {
        u->candidate=false;
        u->cand_tries=0;
      }
    }
    if (crc_ok) {
      ++u->stats.probe_ok; ++totals.probe_ok;
      const uint64_t overlap = v->layouts & u->cand_layouts;
      if (u->candidate && v->confirmation && v->dci_slot != u->cand_slot &&
          v->dci_hash != u->cand_hash && overlap) {
        u->solved = true;
        u->solved_layouts = overlap;
        u->S_by_tda[u->cand_tda & 15]=u->cand_S;
        u->L_by_tda[u->cand_tda & 15]=u->cand_L;
        ++u->stats.solved; ++totals.solved;
        prior_valid = true; prior_layouts = overlap;
        prior_S=u->cand_S; prior_L=u->cand_L; prior_add=u->cand_add;
        prior_table=u->cand_table; prior_type=u->cand_type;
        prior_max_len=u->cand_max_len; prior_data_id=u->cand_data_id;
        prior_dmrs_id=u->cand_dmrs_id;
        prior_tp=u->cand_tp; prior_cdm=u->cand_cdm;
        prior_ports=u->cand_ports; prior_mapping=u->cand_mapping;
        LOG_A(PHY, "SENSING: UL_PUSCH_SOLVER SOLVED rnti=0x%04x layouts=0x%016llx dci=%llu/%llu probes=%llu cpu_us=%llu\n",
              ticket->rnti, (unsigned long long)overlap,
              (unsigned long long)u->cand_slot, (unsigned long long)v->dci_slot,
              (unsigned long long)u->stats.probes, (unsigned long long)u->stats.probe_cpu_us);
      } else if (!u->candidate || !overlap) {
        u->candidate=true; u->cand_layouts=v->layouts;
        u->cand_hash=v->dci_hash; u->cand_slot=v->dci_slot;
        u->cand_tries=0;
        u->cand_S=v->grant.start_symbol; u->cand_L=v->grant.num_symbols;
        u->cand_tda=v->grant.tda_index; u->cand_table=v->grant.mcs_table;
        u->cand_type=v->grant.dmrs_config_type;
        u->cand_tp=v->grant.transform_precoding;
        u->cand_cdm=v->grant.n_dmrs_cdm_groups;
        u->cand_ports=v->grant.dmrs_ports;
        u->cand_mapping=v->grant.mapping_type;
        u->cand_data_id=v->grant.data_scrambling_id;
        u->cand_dmrs_id=v->grant.ul_dmrs_scrambling_id;
        u->cand_add=0; u->cand_max_len=1;
        for (int add=0; add<4; ++add) for (int len=1; len<=2; ++len)
          if (nr_pdcch_blind_ul_dmrs_mask(v->grant.num_symbols,v->grant.start_symbol,
                 v->grant.mapping_type,add,len,v->type_a_pos)==v->grant.ul_dmrs_symb_pos) {
            u->cand_add=add; u->cand_max_len=len; goto found_mask;
          }
found_mask:
        ++u->stats.candidates; ++totals.candidates;
        LOG_A(PHY, "SENSING: UL_PUSCH_SOLVER CANDIDATE rnti=0x%04x dci=%llu layouts=0x%016llx prb=%u+%u S=%u L=%u table=%u id=%u\n",
              ticket->rnti, (unsigned long long)v->dci_slot,
              (unsigned long long)v->layouts, v->grant.bwp_start+v->grant.start_rb,
              v->grant.num_rb, v->grant.start_symbol, v->grant.num_symbols,
              v->grant.mcs_table, v->grant.data_scrambling_id);
      }
    }
  } else if (crc_ok) {
    ++u->stats.full_ok; ++totals.full_ok;
    u->solved_layouts &= v->layouts; /* refinement on every later CRC pass */
    if (v->grant.tda_index < 16) {
      u->S_by_tda[v->grant.tda_index]=v->grant.start_symbol;
      u->L_by_tda[v->grant.tda_index]=v->grant.num_symbols;
    }
    u->full_fail_streak=0;
  } else if (!v->probe && u->full_fail_streak < 63) {
    ++u->full_fail_streak;
  }
  pthread_mutex_unlock(&mu);
}

void nr_pusch_solver_stats(nr_pusch_solver_stats_t *out)
{
  if (!out) return;
  pthread_mutex_lock(&mu);
  *out = totals;
  pthread_mutex_unlock(&mu);
}

bool nr_pusch_solver_is_solved(const nr_ue_ul_ticket_t *ticket)
{
  if (!ticket) return false;
  pthread_mutex_lock(&mu);
  const ul_solver_ue_t *u=ue_for(ticket,false);
  const bool solved=u && u->solved;
  pthread_mutex_unlock(&mu);
  return solved;
}
