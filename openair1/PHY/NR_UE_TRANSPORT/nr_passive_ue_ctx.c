/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "common/utils/nr_core_plan.h"
#include "common/utils/nr_cell.h"
#include "nr_passive_ue_ctx.h"
#include "nr_passive_reconf_rules.h"
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { EV_OBS, EV_PARAM, EV_ANCHOR, EV_EPOCH, EV_SIB1, EV_CSI, EV_TICK,
               EV_UL_GRANT, EV_UL_FEEDBACK, EV_UL_TA, EV_MSG3, EV_MSG4, EV_RRC } kind_t;
typedef struct {
  kind_t kind;
  uint64_t ns;
  int64_t slot, value, extra;
  uint16_t rnti;
  int p, cause;
  nr_ue_verif_t verif;
  nr_cfg_epoch_snapshot_t epoch;
  nr_passive_obs_t obs;
  nr_ue_ul_ticket_t ul_ticket;
  nr_ue_ul_update_t ul_update;
} event_t;
typedef struct { _Atomic uint64_t seq; event_t event; } ring_slot_t;
typedef struct {
  nr_ue_ctx_t ctx;
  bool used;
  uint64_t last_activity_ns, pending_ns;
  int64_t pending_slot;
  uint64_t pending_params;
  uint64_t inferred_hint;      /* params downgraded to HINT by an inferred reconfiguration: still count as trusted for the reconfiguration grouping */
  /* re-attachment evidence (nr_passive_reconf_rules.h) */
  uint32_t n_events;           /* events seen by this incarnation; 0 = freshly created by the event being handled */
  bool attach_open;            /* created by a RAR / Msg4: the next Msg4 completes it instead of replacing it */
  uint64_t attach_ns;
  bool msg3_valid;
  uint64_t msg3;
  struct { bool v; uint8_t ndi; int tbs; } hq[16];
  uint8_t hq_viol_n;
  uint16_t hq_viol_mask;
  uint64_t hq_viol_ns;
  uint64_t hq_armed_until;     /* violations only count right after a silence of >= NR_RECONF_REATTACH_GAP_NS (see hq_discontinuity) */
  int64_t rrc_eff_slot;        /* -1 none */
  int rrc_kind;
} entry_t;
NR_CELL_STATIC_INIT(int, g_cur_mu, 1); /* numerology of the observed cell, from the grants */
#define g_cur_mu NR_CELL_REF(g_cur_mu)
NR_CELL_STATIC(int64_t, g_latest_slot); /* newest event slot: the clock of the RRC windows */
#define g_latest_slot NR_CELL_REF(g_latest_slot)
NR_CELL_STATIC(uint32_t, g_rrc_pending);
#define g_rrc_pending NR_CELL_REF(g_rrc_pending)
NR_CELL_STATIC(ring_slot_t *, g_ring);
#define g_ring NR_CELL_REF(g_ring)
NR_CELL_STATIC(entry_t *, g_entries);
#define g_entries NR_CELL_REF(g_entries)
typedef int16_t g_active_index_arr_t[UINT16_MAX + 1]; /* 128 KiB per cell: heap, pre-allocated per active cell before the RX fence */
NR_CELL_STATIC_HEAP(g_active_index_arr_t, g_active_index);
#define g_active_index NR_CELL_HEAP_REF(g_active_index_arr_t, g_active_index)
NR_CELL_STATIC(uint32_t, g_capacity);
#define g_capacity NR_CELL_REF(g_capacity)
NR_CELL_STATIC(uint64_t, g_head);
#define g_head NR_CELL_REF(g_head)
NR_CELL_STATIC(_Atomic uint64_t, g_tail);
#define g_tail NR_CELL_REF(g_tail)
NR_CELL_STATIC(_Atomic uint64_t, g_pushed);
#define g_pushed NR_CELL_REF(g_pushed)
NR_CELL_STATIC(_Atomic uint64_t, g_written);
#define g_written NR_CELL_REF(g_written)
NR_CELL_STATIC(_Atomic uint64_t, g_dropped);
#define g_dropped NR_CELL_REF(g_dropped)
NR_CELL_STATIC(_Atomic uint32_t, g_producers);
#define g_producers NR_CELL_REF(g_producers)
NR_CELL_STATIC(_Atomic bool, g_running);
#define g_running NR_CELL_REF(g_running)
/* nr_ue_ctx_fast_open (header, read by nr_ue_ctx_enabled()) is the process-wide hint: true while ANY cell's session is open.
 * The per-cell open flag below is what enqueue()/open()/close() use (PROCESS hint, PER_CELL session). */
bool nr_ue_ctx_fast_open;
NR_CELL_STATIC(_Atomic bool, g_cell_open);
#define g_cell_open NR_CELL_REF(g_cell_open)
/* In this file "enabled" means this cell's session is open (the header's inline reads the process-wide hint, which is true while any cell is open). */
#define nr_ue_ctx_enabled() atomic_load_explicit(&g_cell_open, memory_order_seq_cst)
NR_CELL_STATIC(pthread_t, g_worker);
#define g_worker NR_CELL_REF(g_worker)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_ctx_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_ctx_lock NR_CELL_REF(g_ctx_lock)
NR_CELL_STATIC(FILE *, g_output);
#define g_output NR_CELL_REF(g_output)
NR_CELL_STATIC(uint32_t, g_epoch);
#define g_epoch NR_CELL_REF(g_epoch)
NR_CELL_STATIC(uint32_t, g_identity);
#define g_identity NR_CELL_REF(g_identity)
typedef int64_t g_sib1_values_arr_t[3];
NR_CELL_STATIC_INIT(g_sib1_values_arr_t, g_sib1_values, {-1,-1,-1});
#define g_sib1_values NR_CELL_REF(g_sib1_values)
NR_CELL_STATIC(uint64_t, g_last_period_ns);
#define g_last_period_ns NR_CELL_REF(g_last_period_ns)
NR_CELL_STATIC(uint64_t, g_last_age_ns);
#define g_last_age_ns NR_CELL_REF(g_last_age_ns)
NR_CELL_STATIC(double, g_period_s);
#define g_period_s NR_CELL_REF(g_period_s)
NR_CELL_STATIC(bool, g_real_time_session);
#define g_real_time_session NR_CELL_REF(g_real_time_session)

static void fast_open_refresh(void)
{
  bool any = false;
  for (int c = 0; c < NR_CELL_MAX; c++)
    any = any || atomic_load_explicit(&g_cell_open_cells[c], memory_order_seq_cst);
  __atomic_store_n(&nr_ue_ctx_fast_open, any, __ATOMIC_SEQ_CST);
}
/* Allocate cell `cell`'s heap object (call for every active cell before nr_cell_alloc_fence(); cell 0 included). */
void nr_passive_ue_ctx_cell_prealloc(int cell)
{
  NR_CELL_HEAP_PREALLOC(g_active_index_arr_t, g_active_index, cell);
}
extern void nr_cfg_epoch_note_rnti_reopened(uint16_t, bool, uint64_t) __attribute__((weak));
extern void nr_pusch_passive_ul_ids_reset(uint16_t) __attribute__((weak));
extern void nr_pusch_passive_ul_ids_reset_all(void) __attribute__((weak));

static uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
/* Bounded MPSC sequence ring. Producers reserve with CAS and publish with release; the only
 * consumer frees a slot after copying it. No lock, allocation, sleep, or I/O in enqueue. */
static bool enqueue(const event_t *e)
{
  atomic_fetch_add_explicit(&g_producers, 1, memory_order_seq_cst);
  if (!atomic_load_explicit(&g_cell_open, memory_order_seq_cst)) {
    atomic_fetch_sub_explicit(&g_producers, 1, memory_order_seq_cst);
    return false;
  }
  uint64_t pos = atomic_load_explicit(&g_tail, memory_order_relaxed);
  for (;;) {
    ring_slot_t *s = &g_ring[pos % g_capacity];
    int64_t dif = (int64_t)(atomic_load_explicit(&s->seq, memory_order_acquire) - pos);
    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&g_tail, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
        s->event = *e;
        atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
        atomic_fetch_add_explicit(&g_pushed, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(&g_producers, 1, memory_order_seq_cst);
        return true;
      }
    } else if (dif < 0) {
      atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
      atomic_fetch_sub_explicit(&g_producers, 1, memory_order_seq_cst);
      return false;
    } else pos = atomic_load_explicit(&g_tail, memory_order_relaxed);
  }
}
static bool dequeue(event_t *e)
{
  ring_slot_t *s = &g_ring[g_head % g_capacity];
  if (atomic_load_explicit(&s->seq, memory_order_acquire) != g_head + 1) return false;
  *e = s->event;
  atomic_store_explicit(&s->seq, g_head + g_capacity, memory_order_release);
  g_head++;
  return true;
}
static const char *param_name[NR_UEP_COUNT] = {
  "RNTI_CLASS","ANCHOR","CORESET","DCI_LEN_DL","DCI_LEN_UL","DCI_LEN_STATE",
  "PDCCH_SCR_ID","PDSCH_SCR_ID","TD_WINNER","TD_STATE","MCS_TABLE","DMRS_CFG",
  "MAX_LAYERS","LBRM","BWP","PUSCH_LAYOUT","SIB1_HASH","SIB1_BWP",
  "SIB1_TDRA_HASH","APERIODIC_CSI","UE_ID","REATTACH","RESUME_OF"
};
static const char *state_name[] = {"FIRST_SEEN","ACTIVE","IDLE","GONE","INACTIVE"};
static const char *verif_name[] = {"TRUSTED","HINT","SUSPECT"};
static const char *cause_name[] = {
  "FIRST_LEARNED","CONVERGED","RELOCK","REOPENED_NEW_WINNER","BWP_CHANGE",
  "CORESET_CHANGE","EPOCH_REVERIFIED","EPOCH_DISCARDED","HARD_RESET",
  "REATTACH_RAR","REATTACH_MSG4","REATTACH_HARQ","REATTACH_DCI_SIZE","RESUME_LINK","RRC_WINDOW"
};
static uint8_t source_for_param(nr_ue_param_t p)
{
  if(p>=NR_UEP_SIB1_HASH && p<=NR_UEP_SIB1_TDRA_HASH) return NR_UES_SIB1;
  if(p==NR_UEP_ANCHOR || p==NR_UEP_UE_ID || p==NR_UEP_REATTACH || p==NR_UEP_RESUME_OF) return NR_UES_RAR;
  if(p==NR_UEP_BWP) return NR_UES_BWP;
  if(p==NR_UEP_TD_WINNER || p==NR_UEP_TD_STATE || p==NR_UEP_MCS_TABLE || p==NR_UEP_DMRS_CFG)
    return NR_UES_SWEEP;
  if(p==NR_UEP_PDSCH_SCR_ID || p==NR_UEP_MAX_LAYERS) return NR_UES_OBSERVATION;
  return NR_UES_PDCCH;
}
static void json_int(int64_t v) { if (v < 0) fputs("null", g_output); else fprintf(g_output, "%" PRId64, v); }
static void json_float(double v) { if (isfinite(v)) fprintf(g_output, "%.8g", v); else fputs("null", g_output); }
/* The file is bounded: past nr_ue_ctx_test_file_cap bytes (512 MiB; settable by tests) the output moves to /dev/null and every later line is counted as dropped. */
int64_t nr_ue_ctx_test_file_cap = 512ll << 20;
NR_CELL_STATIC(bool, g_file_capped);
#define g_file_capped NR_CELL_REF(g_file_capped)
static void line_done(void)
{
  if (g_file_capped) atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
  else if (!ferror(g_output)) atomic_fetch_add_explicit(&g_written, 1, memory_order_relaxed);
}
static void snapshot(entry_t *e, uint64_t ns)
{
  nr_ue_ctx_t *c = &e->ctx;
  fprintf(g_output, "{\"schema\":\"uectx/1\",\"type\":\"ue_snapshot\",\"t_mono_ns\":%" PRIu64
                  ",\"identity_gen\":%u,\"rnti\":%u,\"incarnation\":%u,\"state\":\"%s\",\"cfg\":{",
          ns, c->identity_gen, c->rnti, c->incarnation, state_name[c->state]);
  for (int p = 0; p < NR_UEP_COUNT; p++) {
    nr_ue_cfg_value_t *v = &c->cfg[p];
    fprintf(g_output, "%s\"%s\":", p ? "," : "", param_name[p]);
    if (v->value < 0) fputs("null", g_output);
    else {
      fprintf(g_output, "{\"value\":%" PRId64 ",\"epoch_learned\":%u,\"verif\":\"%s\",\"first_abs_slot\":",
              v->value, v->epoch_learned, verif_name[v->verif]);
      json_int(v->first_abs_slot);
      fputs(",\"last_confirmed_abs_slot\":", g_output); json_int(v->last_confirmed_abs_slot);
      fprintf(g_output, ",\"source\":%u}", v->source);
    }
  }
  nr_ue_stats_t *s = &c->st;
  fprintf(g_output, "},\"stats\":{\"grants_dl\":%" PRIu64 ",\"grants_ul\":%" PRIu64
                  ",\"crc_ok_dl\":%" PRIu64 ",\"crc_ok_ul\":%" PRIu64
                  ",\"bytes_dl\":%" PRIu64 ",\"bytes_ul\":%" PRIu64 ",\"retx_dl\":%" PRIu64,
          s->grants_dl,s->grants_ul,s->crc_ok_dl,s->crc_ok_ul,s->bytes_dl,s->bytes_ul,s->retx_dl);
  fputs(",\"mcs_hist\":[",g_output);
  for (int i=0;i<32;i++) fprintf(g_output,"%s%u",i?",":"",s->mcs_hist[i]);
  fputs("],\"layers_hist\":[",g_output);
  for (int i=0;i<5;i++) fprintf(g_output,"%s%u",i?",":"",s->layers_hist[i]);
  fputs("],\"snr_ema_db\":",g_output); json_float(s->snr_ema_db);
  fputs(",\"nvar_ema\":",g_output); json_float(s->nvar_ema);
  fputs(",\"fo_ema_hz\":",g_output); json_float(s->fo_ema_hz);
  fputs(",\"ta_ema_samples\":",g_output); json_float(s->ta_ema_samples);
  fputs(",\"prb_start_min\":",g_output); json_int(s->prb_start_min);
  fputs(",\"prb_start_max\":",g_output); json_int(s->prb_start_max);
  fputs(",\"prb_size_min\":",g_output); json_int(s->prb_size_min);
  fputs(",\"prb_size_max\":",g_output); json_int(s->prb_size_max);
  fputs(",\"prb_size_mean\":",g_output); json_float(s->prb_size_mean);
  fputs(",\"last_abs_slot\":",g_output); json_int(s->last_abs_slot);
  {
    static const char *names[NR_UL_F_COUNT] = {"dci_len","layout","bwp","tda","k2",
      "dmrs_id0","dmrs_id1","data_id","ta_samples","cfo_hz"};
    fputs("},\"ul\":{", g_output);
    for (int i=0;i<NR_UL_F_COUNT;i++) {
      const nr_ue_ul_value_t *v=&c->ul.field[i];
      fprintf(g_output,"%s\"%s\":",i?",":"",names[i]);
      if(v->evidence==NR_UL_EVID_UNKNOWN) fputs("null",g_output);
      else fprintf(g_output,"{\"value\":%" PRId64 ",\"evidence\":%u,\"epoch\":%u}",
                   v->value,v->evidence,v->epoch);
    }
    fputs(",\"id_priors\":[",g_output);
    for(int i=0;i<c->ul.n_id_priors;i++) fprintf(g_output,"%s%u",i?",":"",c->ul.id_priors[i]);
    fputs("],\"dl_bwp_prior\":",g_output); json_int(c->ul.dl_bwp_prior);
    fputs(",\"dl_cfo_prior_hz\":",g_output);
    if(c->ul.dl_cfo_prior_valid) fprintf(g_output,"%d",c->ul.dl_cfo_prior_hz);
    else fputs("null",g_output);
    fputs(",\"ta_command_units\":",g_output);
    if(c->ul.ta_command_valid) fprintf(g_output,"%d",c->ul.ta_command_units);
    else fputs("null",g_output);
  }
  fprintf(g_output, "},\"n_changes\":%u,\"n_reconfigs\":%u}\n",c->n_changes,c->n_reconfigs);
  line_done();
}
static const char *reconfig_class(uint64_t mask)
{
  if (__builtin_popcountll(mask) != 1) return "MIXED";
  if (mask & ((1ull<<NR_UEP_DCI_LEN_DL)|(1ull<<NR_UEP_DCI_LEN_UL)|(1ull<<NR_UEP_DCI_LEN_STATE))) return "DCI_SIZE";
  if (mask & ((1ull<<NR_UEP_TD_WINNER)|(1ull<<NR_UEP_TD_STATE)|(1ull<<NR_UEP_DMRS_CFG))) return "PDSCH_TDRA_DMRS";
  if (mask & (1ull<<NR_UEP_MCS_TABLE)) return "MCS_TABLE";
  if (mask & (1ull<<NR_UEP_MAX_LAYERS)) return "MIMO";
  if (mask & (1ull<<NR_UEP_BWP)) return "BWP";
  if (mask & (1ull<<NR_UEP_CORESET)) return "CORESET";
  return "MIXED";
}
static void flush_reconfig(entry_t *e)
{
  if (!e->pending_params) return;
  nr_ue_ctx_t *c=&e->ctx;
  fprintf(g_output,"{\"schema\":\"uectx/1\",\"type\":\"ue_reconfig\",\"t_mono_ns\":%" PRIu64
                 ",\"abs_slot\":",e->pending_ns);
  json_int(e->pending_slot);
  fprintf(g_output,",\"epoch\":%u,\"identity_gen\":%u,\"rnti\":%u,\"incarnation\":%u,\"params\":[",
          g_epoch,c->identity_gen,c->rnti,c->incarnation);
  bool comma=false;
  for(int p=0;p<NR_UEP_COUNT;p++) if(e->pending_params & (1ull<<p)) {
    fprintf(g_output,"%s\"%s\"",comma?",":"",param_name[p]); comma=true;
  }
  fprintf(g_output,"],\"class\":\"%s\"}\n",reconfig_class(e->pending_params));
  line_done(); c->n_reconfigs++;
  /* DL length and TD reopen owners already notify the epoch authority directly. The UL 0_1 length is not epoch evidence: its owner
   * deliberately does not notify (nr_pdsch_config_sweep_relock_length), and OTA it flips between two decodable sizes on every UE of both
   * cells (42 <-> 44, ab_rfxT1 2026-10-20: 39 flips, the 6 remaining DEDICATED_CHANGE_SUSPECTED epochs). A real reconfiguration of the
   * DCI sizes moves the DL length too. */
  const uint64_t directly_notified = (1ull << NR_UEP_DCI_LEN_DL) | (1ull << NR_UEP_DCI_LEN_UL) |
      (1ull << NR_UEP_TD_WINNER);
  if (nr_cfg_epoch_note_rnti_reopened && e->pending_slot >= 0 &&
      !(e->pending_params & directly_notified))
    nr_cfg_epoch_note_rnti_reopened(c->rnti,true,(uint64_t)e->pending_slot);
  e->pending_params=0;
}
static void change(entry_t *e,nr_ue_param_t p,int64_t old,int64_t value,int cause,
                   int64_t slot,uint64_t ns,int64_t evidence)
{
  nr_ue_ctx_t *c=&e->ctx;
  fprintf(g_output,"{\"schema\":\"uectx/1\",\"type\":\"ue_change\",\"t_mono_ns\":%" PRIu64
                 ",\"abs_slot\":",ns); json_int(slot);
  fprintf(g_output,",\"epoch\":%u,\"identity_gen\":%u,\"rnti\":%u,\"incarnation\":%u,\"param\":\"%s\",\"old\":",
          g_epoch,c->identity_gen,c->rnti,c->incarnation,param_name[p]);
  json_int(old); fputs(",\"new\":",g_output); json_int(value);
  fprintf(g_output,",\"cause\":\"%s\",\"evidence\":",
          cause>=0 && cause<=NR_UEC_RRC_WINDOW?cause_name[cause]:"FIRST_LEARNED");
  if(p==NR_UEP_APERIODIC_CSI && evidence>=0) {
    const uint64_t bits=(uint64_t)evidence;
    fprintf(g_output,"{\"row\":%u,\"freq_domain\":%u,\"start_rb\":%u,\"nr_of_rbs\":%u,\"symb_l0\":%u,\"scramb_id\":%u}",
            (unsigned)(bits&63u),(unsigned)((bits>>6)&65535u),
            (unsigned)((bits>>22)&511u),(unsigned)((bits>>31)&511u),
            (unsigned)((bits>>40)&15u),(unsigned)((bits>>44)&65535u));
  } else json_int(evidence);
  fputs("}\n",g_output); line_done(); c->n_changes++;
}
static void init_entry(entry_t *e,uint16_t rnti,uint16_t incarnation,uint64_t ns)
{
  if(nr_pusch_passive_ul_ids_reset) nr_pusch_passive_ul_ids_reset(rnti);
  memset(e,0,sizeof(*e));
  e->rrc_eff_slot=-1;
  e->used=true; e->ctx.rnti=rnti; e->ctx.incarnation=incarnation;
  e->ctx.identity_gen=g_identity; e->ctx.state=NR_UE_FIRST_SEEN; e->last_activity_ns=ns;
  e->ctx.ul.dl_bwp_prior=-1;
  for(int p=0;p<NR_UEP_COUNT;p++) {
    e->ctx.cfg[p].value=-1;
    e->ctx.cfg[p].first_abs_slot=-1;
    e->ctx.cfg[p].last_confirmed_abs_slot=-1;
  }
  nr_ue_stats_t *s=&e->ctx.st;
  s->snr_ema_db=s->nvar_ema=s->fo_ema_hz=s->ta_ema_samples=NAN;
  s->prb_start_min=s->prb_start_max=s->prb_size_min=s->prb_size_max=-1;
  s->prb_size_mean=NAN; s->last_abs_slot=-1;
  for(int i=0;i<3;i++) if(g_sib1_values[i]>=0) {
    const int p=NR_UEP_SIB1_HASH+i;
    e->ctx.cfg[p].value=g_sib1_values[i];
    e->ctx.cfg[p].verif=NR_UEV_TRUSTED;
    e->ctx.cfg[p].epoch_learned=g_epoch;
  }
  snapshot(e,ns);
}
static entry_t *find_entry(uint16_t rnti,uint64_t ns,bool create)
{
  const int16_t active=g_active_index[rnti];
  if(active>=0 && g_entries[active].used && g_entries[active].ctx.identity_gen==g_identity &&
     g_entries[active].ctx.state!=NR_UE_GONE) return &g_entries[active];
  entry_t *free_slot=NULL,*evict=NULL;
  uint16_t next_incarnation=0;
  for(int i=0;i<NR_UECTX_MAX_UE;i++) {
    entry_t *e=&g_entries[i];
    if(e->used && e->ctx.rnti==rnti) {
      if(e->ctx.identity_gen==g_identity && e->ctx.incarnation>=next_incarnation)
        next_incarnation=e->ctx.incarnation+1;
      if(e->ctx.identity_gen==g_identity && e->ctx.state!=NR_UE_GONE) return e;
    }
    if(!e->used && !free_slot) free_slot=e;
    if(e->used && e->ctx.state==NR_UE_GONE && (!evict || e->last_activity_ns<evict->last_activity_ns)) evict=e;
  }
  if(!create) return NULL;
  entry_t *e=free_slot?free_slot:evict;
  if(!e) return NULL;
  if(e->used) flush_reconfig(e);
  if(e->used && g_active_index[e->ctx.rnti]==e-g_entries) g_active_index[e->ctx.rnti]=-1;
  init_entry(e,rnti,next_incarnation,ns);
  g_active_index[rnti]=(int16_t)(e-g_entries);
  return e;
}
static void ul_dl_prior(entry_t *e,nr_ue_ul_field_t field,int64_t value)
{
  nr_ue_ul_value_t *v=&e->ctx.ul.field[field];
  if(v->evidence<=NR_UL_EVID_DL_PRIOR)
    *v=(nr_ue_ul_value_t){.value=value,.evidence=NR_UL_EVID_DL_PRIOR,.epoch=g_epoch};
}
static const nr_ue_param_t rrc_params_inferred[]={NR_UEP_CORESET,NR_UEP_DCI_LEN_DL,NR_UEP_DCI_LEN_UL,NR_UEP_PDCCH_SCR_ID,NR_UEP_PDSCH_SCR_ID,NR_UEP_TD_WINNER,
                                                  NR_UEP_MCS_TABLE,NR_UEP_DMRS_CFG,NR_UEP_MAX_LAYERS,NR_UEP_BWP,NR_UEP_PUSCH_LAYOUT};
static void infer_reconfiguration(entry_t *e,nr_ue_param_t changed,int64_t slot);
static void param(entry_t *e,nr_ue_param_t p,int64_t value,nr_ue_verif_t verif,
                  int cause,int64_t slot,uint64_t ns,int64_t evidence)
{
  if(p<0 || p>=NR_UEP_COUNT || value<0) return;
  nr_ue_cfg_value_t *v=&e->ctx.cfg[p];
  int64_t old=v->value;
  bool changed=old!=value;
  if(changed) {
    if(cause==NR_UEC_FIRST_LEARNED && old>=0 && v->verif==NR_UEV_HINT) cause=NR_UEC_EPOCH_DISCARDED;
    change(e,p,old,value,cause,slot,ns,evidence);
    /* Cell-common SIB1 facts and RNTI anchor/class changes are logged, but are not evidence of
     * a dedicated UE reconfiguration. The epoch authority handles cell-common changes.
     * CORESET and its PDCCH DM-RS identity are per CORESET, and a UE monitors up to 3 CORESETs per BWP (TS 38.213 10.1,
     * TS 38.331 PDCCH-Config): a DCI on another one is not a reconfiguration (OTA TIM 2026-10-20: two CORESETs alternated 8839 times in
     * 30 min, each an inferred RRC window -> 2240 false DEDICATED_CHANGE_SUSPECTED epochs). A removed CORESET is the bank's (SUSPECT) path. */
    if(p!=NR_UEP_APERIODIC_CSI && p!=NR_UEP_RNTI_CLASS && p!=NR_UEP_ANCHOR &&
       p!=NR_UEP_CORESET && p!=NR_UEP_PDCCH_SCR_ID &&
       p!=NR_UEP_UE_ID && p!=NR_UEP_REATTACH && p!=NR_UEP_RESUME_OF &&
       (p<NR_UEP_SIB1_HASH || p>NR_UEP_SIB1_TDRA_HASH) &&
       p!=NR_UEP_TD_STATE && p!=NR_UEP_DCI_LEN_STATE &&
       old>=0 && (v->verif==NR_UEV_TRUSTED || (e->inferred_hint>>p & 1)) && verif==NR_UEV_TRUSTED) {
      if(e->pending_params && ns>=e->pending_ns && ns-e->pending_ns>2000000000ull) flush_reconfig(e);
      if(!e->pending_params) {e->pending_ns=ns; e->pending_slot=slot;}
      else if(e->pending_slot<0 && slot>=0) e->pending_slot=slot;
      e->pending_params|=1ull<<p;
      infer_reconfiguration(e,p,slot);
    }
    v->first_abs_slot=slot;
  } else if(v->verif==NR_UEV_HINT && verif==NR_UEV_TRUSTED) {
    change(e,p,old,value,NR_UEC_EPOCH_REVERIFIED,slot,ns,evidence);
  } else if(v->verif!=verif && verif==NR_UEV_SUSPECT) {
    change(e,p,old,value,cause,slot,ns,evidence);
  }
  if(verif==NR_UEV_TRUSTED) e->inferred_hint&=~(1ull<<p);
  v->value=value; v->verif=verif; v->epoch_learned=g_epoch;
  if(verif==NR_UEV_TRUSTED) v->last_confirmed_abs_slot=slot;
  v->source=source_for_param(p);
  if(verif==NR_UEV_TRUSTED) {
    if(p==NR_UEP_DCI_LEN_DL) ul_dl_prior(e,NR_UL_F_DCI_LEN,value);
    if(p==NR_UEP_BWP) {
      e->ctx.ul.dl_bwp_prior=value;
      ul_dl_prior(e,NR_UL_F_BWP,value);
    }
  }
}
static float ema(float old,float v)
{ return isfinite(v)?(isfinite(old)?old*0.875f+v*0.125f:v):old; }
/* TC-RNTI -> C-RNTI without any Msg4 (TS 38.321 5.1.5: the TC-RNTI becomes the C-RNTI on successful contention resolution): a TC-RNTI that is then
 * scheduled with a C-RNTI-class grant, or a dedicated-format DCI (1_1/0_1, UE-specific), has completed contention resolution. The Msg4 identity is only a
 * confirmation. */
static void promote_tc(entry_t *e,int64_t slot,uint64_t ns)
{
  param(e,NR_UEP_RNTI_CLASS,0 /* NR_BLIND_RNTI_CLASS_C */,NR_UEV_TRUSTED,NR_UEC_CONVERGED,slot,ns,-1);
  e->attach_open=false;
}
static void observe(entry_t *e,const nr_passive_obs_t *o,uint64_t ns)
{
  nr_ue_ctx_t *c=&e->ctx; nr_ue_stats_t *s=&c->st;
  if(c->state!=NR_UE_ACTIVE) {c->state=NR_UE_ACTIVE; snapshot(e,ns);}
  e->last_activity_ns=ns;
  if(o->dir==NR_OBS_DIR_UL) {
    s->grants_ul++; if(o->crc==NR_OBS_CRC_OK) s->crc_ok_ul++;
    if(o->tbs>0 && o->crc==NR_OBS_CRC_OK) s->bytes_ul+=(uint64_t)o->tbs/8;
  } else {
    s->grants_dl++; if(o->crc==NR_OBS_CRC_OK) s->crc_ok_dl++;
    if(o->tbs>0 && o->crc==NR_OBS_CRC_OK) s->bytes_dl+=(uint64_t)o->tbs/8;
    if(o->rv>0) s->retx_dl++;
  }
  if(o->mcs>=0 && o->mcs<32) s->mcs_hist[o->mcs]++;
  if(o->nl>=1 && o->nl<=4) s->layers_hist[o->nl]++;
  s->snr_ema_db=ema(s->snr_ema_db,o->snr_db); s->nvar_ema=ema(s->nvar_ema,o->nvar);
  s->fo_ema_hz=ema(s->fo_ema_hz,o->fo_comp_hz); s->ta_ema_samples=ema(s->ta_ema_samples,o->delay_samples);
  if(o->dir==NR_OBS_DIR_DL && isfinite(s->fo_ema_hz)) {
    c->ul.dl_cfo_prior_hz=(int32_t)lrintf(s->fo_ema_hz);
    c->ul.dl_cfo_prior_valid=true;
    ul_dl_prior(e,NR_UL_F_CFO,c->ul.dl_cfo_prior_hz);
  }
  if(o->start_rb>=0) {
    if(s->prb_start_min<0 || o->start_rb<s->prb_start_min)s->prb_start_min=o->start_rb;
    if(o->start_rb>s->prb_start_max)s->prb_start_max=o->start_rb;
  }
  if(o->nb_rb>=0) {
    if(s->prb_size_min<0 || o->nb_rb<s->prb_size_min)s->prb_size_min=o->nb_rb;
    if(o->nb_rb>s->prb_size_max)s->prb_size_max=o->nb_rb;
    uint64_t n=s->grants_dl+s->grants_ul;
    s->prb_size_mean=isfinite(s->prb_size_mean)?s->prb_size_mean+((double)o->nb_rb-s->prb_size_mean)/n:o->nb_rb;
  }
  s->last_abs_slot=o->abs_slot;
  if(o->scs_khz>=15) g_cur_mu=__builtin_ctz((unsigned)o->scs_khz/15u);
  if(o->rnti_class>=0 && c->cfg[NR_UEP_RNTI_CLASS].value<0)
    param(e,NR_UEP_RNTI_CLASS,o->rnti_class,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,o->abs_slot,ns,-1);
  else if(o->rnti_class==0 && c->cfg[NR_UEP_RNTI_CLASS].value==1)
    promote_tc(e,o->abs_slot,ns);
}
static void age(uint64_t ns)
{
  for(int i=0;i<NR_UECTX_MAX_UE;i++) {
    entry_t *e=&g_entries[i];
    if(e->used && e->pending_params && ns>=e->pending_ns &&
       ns-e->pending_ns>=2000000000ull) flush_reconfig(e);
    if(!e->used || e->ctx.state==NR_UE_GONE || ns<e->last_activity_ns) continue;
    uint64_t delta=ns-e->last_activity_ns;
    if(delta>=NR_RECONF_GONE_NS) {flush_reconfig(e); e->ctx.state=NR_UE_GONE; g_active_index[e->ctx.rnti]=-1; snapshot(e,ns);}
    else if(delta>=NR_RECONF_INACTIVE_NS && e->ctx.state==NR_UE_ACTIVE) {
      /* a UE known by a resume identity has used RRC_INACTIVE before (TS 38.331 5.3.13); every other silent UE is IDLE or in DRX */
      e->ctx.state=(e->ctx.cfg[NR_UEP_UE_ID].value>>48)==NR_RECONF_ID_RESUME ? NR_UE_INACTIVE : NR_UE_IDLE;
      snapshot(e,ns);
    }
  }
}

/* ---- re-attachment (TS 38.321 5.1.5 contention resolution, 5.3.2.2 NDI), RRC delay (TS 38.331 12) ---- */
/* Retire the live incarnation of rnti (a different UE now uses the value) and open the next one; returns it (NULL: table full). */
static entry_t *reattach(entry_t *old,int cause,int64_t slot,uint64_t ns)
{
  const uint16_t rnti=old->ctx.rnti;
  const int64_t prev=((int64_t)rnti<<16)|old->ctx.incarnation;
  flush_reconfig(old);
  old->ctx.state=NR_UE_GONE; g_active_index[rnti]=-1; snapshot(old,ns);
  entry_t *n=find_entry(rnti,ns,true);
  if(!n) return NULL;
  n->attach_open=cause==NR_UEC_REATTACH_RAR || cause==NR_UEC_REATTACH_MSG4; n->attach_ns=ns;
  param(n,NR_UEP_REATTACH,prev,NR_UEV_TRUSTED,cause,slot,ns,-1);
  return n;
}
/* TS 38.321 5.3.2.2: a DL assignment with an untoggled NDI is a retransmission. Two HARQ processes that each decode (CRC pass) a DIFFERENT TBS
 * under an untoggled NDI within NR_RECONF_HQ_WINDOW_NS mean the HARQ entity was reset: a new UE on a reused RNTI. Measured on a recorded capture (50 % of
 * grants lost): inside continuous traffic a missed grant makes an untoggled NDI look like a violation (20 false events on one live UE in 12 s), so the
 * test is armed only for NR_RECONF_HQ_WINDOW_NS after the RNTI was silent for NR_RECONF_REATTACH_GAP_NS..NR_RECONF_REUSE_NS, the situation of a reuse. */
static bool hq_discontinuity(entry_t *e,const nr_passive_obs_t *o,uint64_t ns)
{
  if(ns>e->last_activity_ns && ns-e->last_activity_ns>=NR_RECONF_REATTACH_GAP_NS && ns-e->last_activity_ns<=NR_RECONF_REUSE_NS)
    e->hq_armed_until=ns+NR_RECONF_HQ_WINDOW_NS;
  if(ns>e->hq_armed_until) return false;
  if(o->dir!=NR_OBS_DIR_DL || o->crc!=NR_OBS_CRC_OK || o->harq_pid<0 || o->harq_pid>15 || (o->ndi!=0 && o->ndi!=1) || o->tbs<=0) return false;
  bool hit=false;
  if(nr_reconf_hq_discontinuity(e->hq[o->harq_pid].v,e->hq[o->harq_pid].ndi,e->hq[o->harq_pid].tbs,(uint8_t)o->ndi,o->tbs)) {
    if(ns-e->hq_viol_ns>NR_RECONF_HQ_WINDOW_NS) {e->hq_viol_mask=0;}
    e->hq_viol_ns=ns; e->hq_viol_mask|=(uint16_t)(1u<<o->harq_pid);
    hit=__builtin_popcount(e->hq_viol_mask)>=NR_RECONF_HQ_VIOLATIONS;
  }
  return hit;
}
static void hq_store(entry_t *e,const nr_passive_obs_t *o)
{
  if(o->dir==NR_OBS_DIR_DL && o->crc==NR_OBS_CRC_OK && o->harq_pid>=0 && o->harq_pid<16 && (o->ndi==0 || o->ndi==1) && o->tbs>0) {
    e->hq[o->harq_pid].v=true; e->hq[o->harq_pid].ndi=(uint8_t)o->ndi; e->hq[o->harq_pid].tbs=o->tbs;
  }
}
static entry_t *find_by_id(int64_t packed,const entry_t *skip)
{
  entry_t *best=NULL;
  for(int i=0;i<NR_UECTX_MAX_UE;i++) {
    entry_t *x=&g_entries[i];
    if(x!=skip && x->used && x->ctx.identity_gen==g_identity && x->ctx.cfg[NR_UEP_UE_ID].value==packed &&
       (!best || x->last_activity_ns>best->last_activity_ns)) best=x;
  }
  return best;
}
/* Msg3 seen on a TC-RNTI: remember its 48-bit CCCH SDU for the contention-resolution check. */
static void on_msg3(const event_t *v,uint64_t ns)
{
  entry_t *e=find_entry(v->rnti,ns,true);
  if(!e) {atomic_fetch_add_explicit(&g_dropped,1,memory_order_relaxed); return;}
  e->msg3=(uint64_t)v->value; e->msg3_valid=true; e->n_events++;
}
/* Msg4 with a UE Contention Resolution Identity: the TC-RNTI is now this UE's C-RNTI (TS 38.321 5.1.5). */
static void on_msg4(const event_t *v,uint64_t ns)
{
  entry_t *e=find_entry(v->rnti,ns,true);
  if(!e) {atomic_fetch_add_explicit(&g_dropped,1,memory_order_relaxed); return;}
  /* A Msg4 starts an attachment: when the incarnation was not opened by this attachment's RAR (or earlier Msg4) and already has history, the RNTI was
   * reused by a new UE. */
  if(!(e->attach_open && ns-e->attach_ns<2000000000ull) && e->n_events>0) {
    const bool msg3_kept=e->msg3_valid; const uint64_t m3=e->msg3;
    e=reattach(e,NR_UEC_REATTACH_MSG4,v->slot,ns);
    if(!e) {atomic_fetch_add_explicit(&g_dropped,1,memory_order_relaxed); return;}
    e->msg3_valid=msg3_kept; e->msg3=m3; /* a Msg3 heard after the RAR belongs to this attachment */
  }
  e->n_events++; e->last_activity_ns=ns;
  if(e->msg3_valid && e->msg3!=(uint64_t)v->value) e->msg3_valid=false; /* the Msg3 we heard lost the contention: its owner is not the winner */
  uint8_t cr[6]; nr_reconf_unpack48((uint64_t)v->value,cr);
  nr_reconf_id_t id;
  if(nr_reconf_ccch_identity(cr,&id)) {
    const int64_t packed=((int64_t)id.kind<<48)|(int64_t)id.value;
    param(e,NR_UEP_UE_ID,packed,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,v->slot,ns,-1);
    if(nr_reconf_id_linkable(&id)) {
      const entry_t *prev=find_by_id(packed,e);
      if(prev) param(e,NR_UEP_RESUME_OF,((int64_t)prev->ctx.rnti<<16)|prev->ctx.incarnation,NR_UEV_TRUSTED,NR_UEC_RESUME_LINK,v->slot,ns,-1);
    }
  }
  param(e,NR_UEP_RNTI_CLASS,0 /* NR_BLIND_RNTI_CLASS_C */,NR_UEV_TRUSTED,NR_UEC_CONVERGED,v->slot,ns,-1);
  e->attach_open=false;
}
/* RRC-free primary path (NSA cells show no RRCSetup / RRCReconfiguration): a dedicated parameter that changed under traffic (DCI size, layout, TD winner,
 * MCS table ...) is the observable moment an RRCReconfiguration took effect. TS 38.331 12: it was received at most the processing delay earlier, so the
 * window [slot - delay, slot] holds evidence of both configurations. The other dedicated parameters of the same UE came with the same message: they are
 * re-verified (HINT) now, and the PDCCH monitor's per-RNTI re-lock runs accelerated from this slot (nr_reconf_rrc_phase ARMED). */
static void infer_reconfiguration(entry_t *e,nr_ue_param_t changed,int64_t slot)
{
  if(slot<0) return;
  const int d=nr_reconf_rrc_delay_slots(NR_RECONF_RRC_RECONF,g_cur_mu,false);
  if(d>=0) nr_reconf_rrc_expect(e->ctx.rnti,slot-d,NR_RECONF_RRC_RECONF,g_cur_mu,false); /* effective slot = the observed change */
  for(unsigned k=0;k<sizeof(rrc_params_inferred)/sizeof(rrc_params_inferred[0]);k++) {
    nr_ue_cfg_value_t *v=&e->ctx.cfg[rrc_params_inferred[k]];
    if(rrc_params_inferred[k]!=changed && v->value>=0 && v->verif==NR_UEV_TRUSTED) {v->verif=NR_UEV_HINT; e->inferred_hint|=1ull<<rrc_params_inferred[k];}
  }
}
/* An RRC message (read, or its MAC header seen) reached rnti: after the TS 38.331 12 processing delay its dedicated configuration may differ. Parameters learned
 * before are re-verified (HINT) and the epoch authority is told at that moment, not at reception. */
static void on_rrc(const event_t *v,uint64_t ns)
{
  entry_t *e=find_entry(v->rnti,ns,true);
  if(!e) {atomic_fetch_add_explicit(&g_dropped,1,memory_order_relaxed); return;}
  const int d=nr_reconf_rrc_delay_slots((nr_reconf_rrc_kind_t)v->cause,v->p,false);
  if(d<0 || v->slot<0) return;
  if(e->rrc_eff_slot<0) g_rrc_pending++;
  e->rrc_eff_slot=v->slot+d; e->rrc_kind=v->cause; e->n_events++; e->last_activity_ns=ns;
}
static void rrc_fire(uint64_t ns)
{
  if(!g_rrc_pending) return;
  for(int i=0;i<NR_UECTX_MAX_UE;i++) {
    entry_t *e=&g_entries[i];
    if(!e->used || e->rrc_eff_slot<0 || g_latest_slot<e->rrc_eff_slot) continue;
    nr_ue_ctx_t *c=&e->ctx; uint64_t mask=0;
    for(unsigned k=0;k<sizeof(rrc_params_inferred)/sizeof(rrc_params_inferred[0]);k++) {
      nr_ue_cfg_value_t *v=&c->cfg[rrc_params_inferred[k]];
      if(v->value>=0 && v->verif==NR_UEV_TRUSTED) {v->verif=NR_UEV_HINT; mask|=1ull<<rrc_params_inferred[k];}
    }
    const int64_t eff=e->rrc_eff_slot;
    e->rrc_eff_slot=-1; g_rrc_pending--;
    if(c->state==NR_UE_GONE) continue;
    flush_reconfig(e);
    fprintf(g_output,"{\"schema\":\"uectx/1\",\"type\":\"ue_reconfig\",\"t_mono_ns\":%" PRIu64 ",\"abs_slot\":%" PRId64
                   ",\"epoch\":%u,\"identity_gen\":%u,\"rnti\":%u,\"incarnation\":%u,\"params\":[",
            ns,eff,g_epoch,c->identity_gen,c->rnti,c->incarnation);
    bool comma=false;
    for(int p=0;p<NR_UEP_COUNT;p++) if(mask & (1ull<<p)) {fprintf(g_output,"%s\"%s\"",comma?",":"",param_name[p]); comma=true;}
    fputs("],\"class\":\"RRC_WINDOW_CLOSED\"}\n",g_output);
    line_done(); c->n_reconfigs++;
    /* No epoch note here: a closed window is not a reopened UE (spec 4.4/4.7: only a TRUSTED value that changed counts). An inferred window was
     * already noted by flush_reconfig() for the change that opened it, and an RRC message seen without any change proves nothing. */
  }
}
static void ul_put(nr_ue_ul_state_t *s, nr_ue_ul_field_t f, int64_t value, uint8_t evidence)
{
  nr_ue_ul_value_t *v=&s->field[f];
  if (v->evidence==NR_UL_EVID_TB_CONFIRMED && evidence<NR_UL_EVID_TB_CONFIRMED) return;
  v->value=value; v->evidence=evidence; v->epoch=g_epoch;
}
static void ul_prior_id(nr_ue_ul_state_t *s, int64_t id)
{
  if(id<0 || id>65535) return;
  for(int i=0;i<s->n_id_priors;i++) if(s->id_priors[i]==id) return;
  if(s->n_id_priors<3) s->id_priors[s->n_id_priors++]=(uint16_t)id;
}
static void ul_update(entry_t *e,const nr_ue_ul_update_t *u,bool confirmed)
{
  nr_ue_ul_state_t *s=&e->ctx.ul;
  if(!s->n_id_priors) {
    ul_prior_id(s,u->pci);
    ul_prior_id(s,e->ctx.cfg[NR_UEP_PDSCH_SCR_ID].value);
    ul_prior_id(s,e->ctx.cfg[NR_UEP_PDCCH_SCR_ID].value);
    const nr_ue_cfg_value_t *b=&e->ctx.cfg[NR_UEP_BWP];
    if(b->value>=0) {
      s->dl_bwp_prior=b->value;
      ul_put(s,NR_UL_F_BWP,b->value,NR_UL_EVID_DL_PRIOR);
    }
    if(isfinite(e->ctx.st.fo_ema_hz)) {
      s->dl_cfo_prior_hz=(int32_t)lrintf(e->ctx.st.fo_ema_hz);
      s->dl_cfo_prior_valid=true;
      ul_put(s,NR_UL_F_CFO,s->dl_cfo_prior_hz,NR_UL_EVID_DL_PRIOR);
    }
  }
  const uint8_t grade=confirmed?NR_UL_EVID_TB_CONFIRMED:NR_UL_EVID_PROVISIONAL;
  ul_put(s,NR_UL_F_DCI_LEN,u->dci_len,grade);
  if(u->layout!=UINT16_MAX) ul_put(s,NR_UL_F_LAYOUT,u->layout,grade);
  if(u->bwp_size) ul_put(s,NR_UL_F_BWP,(int64_t)u->bwp_start|((int64_t)u->bwp_size<<16),grade);
  ul_put(s,NR_UL_F_TDA,u->tda,grade); ul_put(s,NR_UL_F_K2,u->k2,grade);
  ul_put(s,u->nscid?NR_UL_F_DMRS_ID1:NR_UL_F_DMRS_ID0,u->dmrs_id,grade);
  ul_put(s,NR_UL_F_DATA_ID,u->data_id,grade);
  ul_put(s,NR_UL_F_TA,u->ta_samples,grade);
  ul_put(s,NR_UL_F_CFO,u->cfo_hz,grade);
}
static void handle(const event_t *v)
{
  pthread_mutex_lock(&g_ctx_lock);
  uint64_t ns=v->ns?v->ns:now_ns();
  if(v->kind!=EV_TICK && ns>now_ns()-5000000000ull) g_real_time_session=true;
  if(!g_last_age_ns || (ns>=g_last_age_ns && ns-g_last_age_ns>=1000000000ull)) {
    age(ns);
    g_last_age_ns=ns;
  }
  if(v->kind==EV_EPOCH) {
    if((v->epoch.last_class!=NR_EPOCH_SOFT ||
       v->epoch.last_cause==NR_CAUSE_DEDICATED_CHANGE_SUSPECTED) && nr_pusch_passive_ul_ids_reset_all)
      nr_pusch_passive_ul_ids_reset_all();
    for(int i=0;i<NR_UECTX_MAX_UE;i++) if(g_entries[i].used && g_entries[i].ctx.state!=NR_UE_GONE) {
      entry_t *e=&g_entries[i];
      flush_reconfig(e);
      if(v->epoch.last_class==NR_EPOCH_HARD_RESET) {e->ctx.state=NR_UE_GONE; g_active_index[e->ctx.rnti]=-1; snapshot(e,ns);}
      else if(v->epoch.last_class!=NR_EPOCH_SOFT ||
              v->epoch.last_cause==NR_CAUSE_DEDICATED_CHANGE_SUSPECTED)
        for(int p=0;p<NR_UEP_COUNT;p++) if(e->ctx.cfg[p].value>=0) e->ctx.cfg[p].verif=NR_UEV_HINT;
      if(v->epoch.last_class!=NR_EPOCH_SOFT ||
         v->epoch.last_cause==NR_CAUSE_DEDICATED_CHANGE_SUSPECTED) {
        /* A value learned in the old epoch is not a DL prior for the new one.
         * Reseed only after a fresh DL lock supplies current facts. */
        memset(e->ctx.ul.field,0,sizeof(e->ctx.ul.field));
        e->ctx.ul.n_id_priors=0;
        e->ctx.ul.dl_bwp_prior=-1;
        e->ctx.ul.dl_cfo_prior_valid=false;
        e->ctx.ul.ta_command_valid=false;
      }
    }
    g_epoch=v->epoch.epoch; g_identity=v->epoch.identity_gen;
    if(v->epoch.last_class==NR_EPOCH_HARD_RESET)
      for(int i=0;i<3;i++) g_sib1_values[i]=-1;
  } else if(v->kind==EV_SIB1) {
    if(v->p>=NR_UEP_SIB1_HASH && v->p<=NR_UEP_SIB1_TDRA_HASH && v->value>=0) {
      g_sib1_values[v->p-NR_UEP_SIB1_HASH]=v->value;
      for(int i=0;i<NR_UECTX_MAX_UE;i++) if(g_entries[i].used && g_entries[i].ctx.state!=NR_UE_GONE)
        param(&g_entries[i],(nr_ue_param_t)v->p,v->value,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,v->slot,ns,-1);
    }
  } else if(v->kind==EV_UL_GRANT || v->kind==EV_UL_FEEDBACK) {
    const nr_ue_ul_ticket_t *t=&v->ul_ticket;
    const int16_t idx=g_active_index[t->rnti];
    if(idx>=0) {
      entry_t *e=&g_entries[idx];
      if(e->used && e->ctx.state!=NR_UE_GONE && e->ctx.identity_gen==t->identity_gen &&
         e->ctx.incarnation==t->incarnation && t->epoch==g_epoch &&
         e->ctx.cfg[NR_UEP_DCI_LEN_DL].value>=0)
        ul_update(e,&v->ul_update,v->kind==EV_UL_FEEDBACK);
    }
  } else if(v->kind==EV_UL_TA) {
    entry_t *e=v->ul_ticket.epoch==g_epoch ? find_entry(v->rnti,ns,v->cause!=0) : NULL;
    if(e && v->p>=0 && v->p<=4) {
      if(v->cause) {e->ctx.ul.ta_command_units=(int32_t)v->value;e->ctx.ul.ta_command_valid=true;}
      else if(e->ctx.ul.ta_command_valid && e->ctx.ul.ta_mu==v->p) {
        const int64_t next=(int64_t)e->ctx.ul.ta_command_units+v->value;
        e->ctx.ul.ta_command_units=(int32_t)(next<0?0:next);
      }
      e->ctx.ul.ta_mu=(uint8_t)v->p;
    }
  } else if(v->kind==EV_MSG3) on_msg3(v,ns);
  else if(v->kind==EV_MSG4) on_msg4(v,ns);
  else if(v->kind==EV_RRC) on_rrc(v,ns);
  else if(v->kind!=EV_TICK) {
    entry_t *e=find_entry(v->rnti,ns,true);
    if(e) {
      if(v->kind==EV_OBS && hq_discontinuity(e,&v->obs,ns)) e=reattach(e,NR_UEC_REATTACH_HARQ,v->slot,ns); /* TS 38.321 5.3.2.2 */
      /* A DCI size change after the RNTI went silent is a new attachment, not a reconfiguration (a reconfiguration needs DL traffic, which would have
       * kept the RNTI sighted): the gap is NR_RECONF_REATTACH_GAP_NS..NR_RECONF_REUSE_NS. */
      if(e && v->kind==EV_PARAM && (v->p==NR_UEP_DCI_LEN_DL || v->p==NR_UEP_DCI_LEN_UL) && v->verif==NR_UEV_TRUSTED &&
         e->ctx.cfg[v->p].value>=0 && e->ctx.cfg[v->p].value!=v->value && e->ctx.cfg[v->p].verif==NR_UEV_TRUSTED &&
         ns>e->last_activity_ns && ns-e->last_activity_ns>=NR_RECONF_REATTACH_GAP_NS && ns-e->last_activity_ns<=NR_RECONF_REUSE_NS)
        e=reattach(e,NR_UEC_REATTACH_DCI_SIZE,v->slot,ns);
      if(!e) {atomic_fetch_add_explicit(&g_dropped,1,memory_order_relaxed); goto done;}
      const uint64_t silent_ns=ns>e->last_activity_ns?ns-e->last_activity_ns:0;
      if(v->kind==EV_PARAM || v->kind==EV_ANCHOR || v->kind==EV_CSI) e->last_activity_ns=ns;
      e->n_events++;
      if(v->kind==EV_OBS) {
        hq_store(e,&v->obs);
        observe(e,&v->obs,ns);
      }
      else if(v->kind==EV_PARAM) {
        if((v->p==NR_UEP_DCI_LEN_DL || v->p==NR_UEP_DCI_LEN_UL) && e->ctx.cfg[NR_UEP_RNTI_CLASS].value==1) promote_tc(e,v->slot,ns);
        param(e,(nr_ue_param_t)v->p,v->value,v->verif,v->cause,v->slot,ns,-1);
      }
      else if(v->kind==EV_ANCHOR) {
        /* RAR. An RNTI value still in use cannot be handed to ANOTHER UE (TS 38.321 7.1), but a contention-free RA (PDCCH order, beam failure recovery,
         * reconfigurationWithSync; TS 38.321 5.1.4) is answered for a UE the gNB knows, and the RAR's TC-RNTI field then carries that UE's own C-RNTI
         * (OTA TIM 2026-10-20: 1 RAR per run named an RNTI with a DCI in the previous 10 s). So a RAR naming a recently active RNTI is the same UE,
         * still a C-RNTI; only after the IDLE/INACTIVE silence horizon is the value free for a new UE (TS 38.321 5.1.4/5.1.5). */
        const bool repeat=e->attach_open && ns-e->attach_ns<2000000000ull; /* a repeated RAR decode is the same attachment */
        const bool same_ue=e->n_events>1 && e->ctx.state!=NR_UE_GONE && !repeat && silent_ns<NR_RECONF_INACTIVE_NS;
        if(e->n_events>1 && e->ctx.state!=NR_UE_GONE && !repeat && !same_ue) {
          e=reattach(e,NR_UEC_REATTACH_RAR,v->slot,ns);
          if(!e) {atomic_fetch_add_explicit(&g_dropped,1,memory_order_relaxed); goto done;}
          e->n_events++;
        }
        if(!same_ue) {e->attach_open=true; e->attach_ns=ns;}
        if(v->value==1 && !same_ue) param(e,NR_UEP_RNTI_CLASS,1 /* TC */,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,v->slot,ns,-1);
        param(e,NR_UEP_ANCHOR,v->value,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,v->slot,ns,-1);
      } else if(v->kind==EV_CSI)
        change(e,NR_UEP_APERIODIC_CSI,-1,v->value,NR_UEC_FIRST_LEARNED,v->slot,ns,v->extra);
    } else atomic_fetch_add_explicit(&g_dropped,1,memory_order_relaxed);
  }
done:
  if(v->slot>g_latest_slot && v->kind!=EV_UL_TA) g_latest_slot=v->slot;
  rrc_fire(ns);
  if(g_period_s>0 && (!g_last_period_ns || (ns>=g_last_period_ns && ns-g_last_period_ns>=(uint64_t)(g_period_s*1e9)))) {
    for(int i=0;i<NR_UECTX_MAX_UE;i++) if(g_entries[i].used && g_entries[i].ctx.state!=NR_UE_GONE) snapshot(&g_entries[i],ns);
    g_last_period_ns=ns;
  }
  if(!g_file_capped && ftell(g_output)>nr_ue_ctx_test_file_cap) {
    FILE *nf=fopen("/dev/null","w");
    if(nf) {fclose(g_output);g_output=nf;g_file_capped=true;}
  }
  pthread_mutex_unlock(&g_ctx_lock);
}
/* Test hook: the writer sleeps first so the ring fills (overflow tests). */
int nr_ue_ctx_test_writer_pause_ms;
static void *run(void *cell)
{
  nr_cell_bind((int)(intptr_t)cell); /* the worker serves the cell that opened the session (nr_core_plan_create does not bind) */
  if(nr_ue_ctx_test_writer_pause_ms>0) {struct timespec t={nr_ue_ctx_test_writer_pause_ms/1000,(nr_ue_ctx_test_writer_pause_ms%1000)*1000000L}; nanosleep(&t,NULL);}
  event_t e;
  uint64_t last_idle_tick=now_ns();
  while(atomic_load_explicit(&g_running,memory_order_seq_cst) ||
        atomic_load_explicit(&g_producers,memory_order_seq_cst) ||
        g_head<atomic_load_explicit(&g_tail,memory_order_acquire)) {
    if(dequeue(&e)) handle(&e);
    else {
      uint64_t ns=now_ns();
      if(g_real_time_session && ns-last_idle_tick>=100000000ull) {
        event_t tick={.kind=EV_TICK,.ns=ns};
        handle(&tick);
        last_idle_tick=ns;
      }
      struct timespec t={0,1000000}; nanosleep(&t,NULL);
    }
  }
  pthread_mutex_lock(&g_ctx_lock);
  for(int i=0;i<NR_UECTX_MAX_UE;i++) if(g_entries[i].used) {
    flush_reconfig(&g_entries[i]);
    snapshot(&g_entries[i],now_ns());
  }
  fflush(g_output);
  pthread_mutex_unlock(&g_ctx_lock);
  return NULL;
}
/* nr_ue_ctx_open: opens the UE context output file (ISAC_UECTX_PATH) and starts its writer: allocates the slot ring (at least two slots) and the per-UE table, initialises the sequence and the SIB1 values, and starts the worker thread.
 *   Returns false when the context is already enabled, the path or capacity is invalid, an allocation fails, or the worker cannot start (everything is then released). Caller: executables/nr-uesoftmodem.c (passive start-up).
 */
bool nr_ue_ctx_open(const char *path,uint32_t ring_capacity,double snapshot_period_s)
{
  if(!ring_capacity || atomic_load_explicit(&g_cell_open,memory_order_seq_cst)) return false;
  /* The sequence algorithm needs at least two physical slots; a requested one-slot ring keeps
   * the same bounded/drop behavior with two physical slots. */
  if(ring_capacity<2) ring_capacity=2;
  /* A state-only U4 session has no JSON destination. Discard its records
   * instead of growing a temporary file for the whole receiver lifetime. */
  FILE *f=path && *path ? fopen(path,"a") : fopen("/dev/null","w"); if(!f) return false;
  ring_slot_t *r=calloc(ring_capacity,sizeof(*r));
  entry_t *x=calloc(NR_UECTX_MAX_UE,sizeof(*x));
  if(!r || !x) {free(r);free(x);fclose(f);return false;}
  free(g_entries);
  g_output=f; g_ring=r; g_entries=x; g_capacity=ring_capacity; g_head=0;
  g_period_s=snapshot_period_s; g_last_period_ns=g_last_age_ns=0; g_epoch=g_identity=0; g_file_capped=false; g_latest_slot=-1; g_rrc_pending=0;
  for(int i=0;i<=UINT16_MAX;i++) g_active_index[i]=-1;
  g_real_time_session=false;
  for(int i=0;i<3;i++) g_sib1_values[i]=-1;
  for(uint32_t i=0;i<g_capacity;i++) atomic_init(&g_ring[i].seq,i);
  atomic_store(&g_tail,0);atomic_store(&g_pushed,0);atomic_store(&g_written,0);atomic_store(&g_dropped,0);
  atomic_store_explicit(&g_producers,0,memory_order_seq_cst);
  atomic_store_explicit(&g_running,true,memory_order_seq_cst);
  atomic_store_explicit(&g_cell_open,true,memory_order_seq_cst);
  fast_open_refresh();
  if(nr_core_plan_create(&g_worker,NULL,run,(void *)(intptr_t)nr_cell_idx(), -1)!=0) {
    atomic_store_explicit(&g_cell_open,false,memory_order_seq_cst);
    fast_open_refresh();
    atomic_store_explicit(&g_running,false,memory_order_seq_cst);
    free(g_ring);free(g_entries);fclose(g_output);g_ring=NULL;g_entries=NULL;g_output=NULL;
    return false;
  }
  return true;
}
void nr_ue_ctx_close(void)
{
  if(!atomic_load_explicit(&g_cell_open,memory_order_seq_cst)) return;
  atomic_store_explicit(&g_cell_open,false,memory_order_seq_cst);
  fast_open_refresh();
  atomic_store_explicit(&g_running,false,memory_order_seq_cst);
  pthread_join(g_worker,NULL);
  fclose(g_output);free(g_ring);g_output=NULL;g_ring=NULL;
}
void nr_ue_ctx_on_obs(const nr_passive_obs_t *o)
{ if(o && nr_ue_ctx_enabled()) {event_t e={.kind=EV_OBS,.ns=o->t_mono_ns,.slot=o->abs_slot,.rnti=o->rnti,.obs=*o};enqueue(&e);} }
void nr_ue_ctx_on_param(uint16_t rnti,nr_ue_param_t p,int64_t value,nr_ue_verif_t v,int cause,int64_t slot)
{ if(nr_ue_ctx_enabled()) {event_t e={.kind=EV_PARAM,.ns=now_ns(),.slot=slot,.rnti=rnti,.p=p,.value=value,.verif=v,.cause=cause};enqueue(&e);} }
void nr_ue_ctx_on_dci_accept(uint16_t rnti,bool dedicated_format,bool is_ul,bool dedicated_uss,
                             int dci_length,int64_t coreset,int pdcch_scr_id,int64_t slot)
{
  if(!dedicated_format || !nr_ue_ctx_enabled()) return;
  nr_ue_ctx_on_param(rnti,is_ul?NR_UEP_DCI_LEN_UL:NR_UEP_DCI_LEN_DL,dci_length,
                     NR_UEV_TRUSTED,NR_UEC_CONVERGED,slot);
  if(dedicated_uss) {
    nr_ue_ctx_on_param(rnti,NR_UEP_CORESET,coreset,NR_UEV_TRUSTED,NR_UEC_CORESET_CHANGE,slot);
    nr_ue_ctx_on_param(rnti,NR_UEP_PDCCH_SCR_ID,pdcch_scr_id,NR_UEV_TRUSTED,NR_UEC_CONVERGED,slot);
  }
}
void nr_ue_ctx_on_anchor(uint16_t rnti,int kind,int64_t slot)
{ if(nr_ue_ctx_enabled()) {event_t e={.kind=EV_ANCHOR,.ns=now_ns(),.slot=slot,.rnti=rnti,.value=kind};enqueue(&e);} }
void nr_ue_ctx_on_sib1(uint32_t hash,int64_t slot)
{ nr_ue_ctx_on_sib1_param(NR_UEP_SIB1_HASH,hash,slot); }
void nr_ue_ctx_on_sib1_param(nr_ue_param_t p,int64_t value,int64_t slot)
{ if(nr_ue_ctx_enabled() && p>=NR_UEP_SIB1_HASH && p<=NR_UEP_SIB1_TDRA_HASH)
    {event_t e={.kind=EV_SIB1,.ns=now_ns(),.slot=slot,.p=p,.value=value};enqueue(&e);} }
void nr_ue_ctx_on_epoch(const nr_cfg_epoch_snapshot_t *s)
{ if(s && nr_ue_ctx_enabled()) {event_t e={.kind=EV_EPOCH,.ns=now_ns(),.epoch=*s};enqueue(&e);} }
void nr_ue_ctx_on_aperiodic_csi(uint16_t rnti,uint8_t request,int64_t slot,int64_t resource)
{ if(request && nr_ue_ctx_enabled()) {event_t e={.kind=EV_CSI,.ns=now_ns(),.slot=slot,.rnti=rnti,.value=request,.extra=resource};enqueue(&e);} }
void nr_ue_ctx_on_dci01_csi(uint16_t rnti,uint8_t request,int64_t slot,
                            const nr_ue_csi_resource_t *observed)
{
  if(!request || !nr_ue_ctx_enabled()) return;
  int64_t resource=-1;
  if(observed) {
    const uint64_t bits=(uint64_t)(observed->row&63u)
       | ((uint64_t)observed->freq_domain<<6)
       | ((uint64_t)(observed->start_rb&511u)<<22)
       | ((uint64_t)(observed->nr_of_rbs&511u)<<31)
       | ((uint64_t)(observed->symb_l0&15u)<<40)
       | ((uint64_t)observed->scramb_id<<44);
    resource=(int64_t)bits;
  }
  nr_ue_ctx_on_aperiodic_csi(rnti,request,slot,resource);
}
void nr_ue_ctx_tick(int64_t slot,uint64_t ns)
{ if(nr_ue_ctx_enabled()) {event_t e={.kind=EV_TICK,.ns=ns,.slot=slot};enqueue(&e);} }
bool nr_ue_ctx_get(uint16_t rnti,nr_ue_ctx_t *out)
{
  if(!out || !g_entries) return false;
  pthread_mutex_lock(&g_ctx_lock);
  bool found=false;
  for(int i=0;i<NR_UECTX_MAX_UE;i++) if(g_entries[i].used && g_entries[i].ctx.rnti==rnti &&
      (!found || g_entries[i].ctx.incarnation>=out->incarnation)) {*out=g_entries[i].ctx;found=true;}
  pthread_mutex_unlock(&g_ctx_lock);
  return found;
}
bool nr_ue_ctx_try_get(uint16_t rnti,nr_ue_ctx_t *out)
{
  if (!out || !g_entries || pthread_mutex_trylock(&g_ctx_lock)) return false;
  bool found=false;
  {
    const int16_t idx=g_active_index[rnti];
    if(idx>=0 && g_entries[idx].used && g_entries[idx].ctx.state!=NR_UE_GONE) {*out=g_entries[idx].ctx;found=true;}
  }
  pthread_mutex_unlock(&g_ctx_lock);
  return found;
}
bool nr_ue_ctx_ul_ticket(uint16_t rnti,nr_ue_ul_ticket_t *out)
{
  if(!out || !nr_ue_ctx_enabled() || pthread_mutex_trylock(&g_ctx_lock)) return false;
  const int16_t idx=g_active_index[rnti];
  bool ok=false;
  if(idx>=0) {
    const nr_ue_ctx_t *c=&g_entries[idx].ctx;
    const nr_ue_cfg_value_t *dl=&c->cfg[NR_UEP_DCI_LEN_DL];
    ok=c->state!=NR_UE_GONE && dl->value>=0 && dl->verif==NR_UEV_TRUSTED;
    if(ok) *out=(nr_ue_ul_ticket_t){.identity_gen=c->identity_gen,.epoch=g_epoch,
                                   .incarnation=c->incarnation,.rnti=rnti};
  }
  pthread_mutex_unlock(&g_ctx_lock);
  return ok;
}
void nr_ue_ctx_ul_grant(const nr_ue_ul_ticket_t *t,const nr_ue_ul_update_t *u)
{
  if(t && u && nr_ue_ctx_enabled()) {
    event_t e={.kind=EV_UL_GRANT,.ns=now_ns(),.rnti=t->rnti,.ul_ticket=*t,.ul_update=*u};
    enqueue(&e);
  }
}
void nr_ue_ctx_ul_feedback(const nr_ue_ul_ticket_t *t,const nr_ue_ul_update_t *u,bool tb_crc_ok)
{
  if(t && u && tb_crc_ok && nr_ue_ctx_enabled()) {
    event_t e={.kind=EV_UL_FEEDBACK,.ns=now_ns(),.rnti=t->rnti,.ul_ticket=*t,.ul_update=*u};
    enqueue(&e);
  }
}
void nr_ue_ctx_ul_ta_command(uint16_t rnti,int units,int mu,bool absolute)
{
  if(!nr_ue_ctx_enabled() || mu<0 || mu>4 || rnti==0) return;
  event_t e={.kind=EV_UL_TA,.ns=now_ns(),.rnti=rnti,.value=units,.p=mu,.cause=absolute?1:0};
  e.ul_ticket.epoch=nr_cfg_reconf_enabled()?nr_cfg_epoch_work_stamp():0;
  enqueue(&e);
}
void nr_ue_ctx_on_msg3(uint16_t rnti,uint64_t ccch48,int64_t slot)
{ if(nr_ue_ctx_enabled() && rnti) {event_t e={.kind=EV_MSG3,.ns=now_ns(),.slot=slot,.rnti=rnti,.value=(int64_t)ccch48};enqueue(&e);} }
void nr_ue_ctx_on_msg4(uint16_t rnti,uint64_t cr48,int64_t slot)
{ if(nr_ue_ctx_enabled() && rnti) {event_t e={.kind=EV_MSG4,.ns=now_ns(),.slot=slot,.rnti=rnti,.value=(int64_t)cr48};enqueue(&e);} }
void nr_ue_ctx_on_rrc(uint16_t rnti,int kind,int64_t rx_slot,int mu)
{
  if(rnti==0 || rx_slot<0) return;
  nr_reconf_rrc_expect(rnti,rx_slot,(nr_reconf_rrc_kind_t)kind,mu,false); /* read lock-free by the PDCCH thread, even when the writer is off */
  if(nr_ue_ctx_enabled()) {event_t e={.kind=EV_RRC,.ns=now_ns(),.slot=rx_slot,.rnti=rnti,.cause=kind,.p=mu};enqueue(&e);}
}
void nr_ue_ctx_stats(uint64_t *events,uint64_t *lines,uint64_t *drops)
{if(events)*events=atomic_load(&g_pushed);if(lines)*lines=atomic_load(&g_written);if(drops)*drops=atomic_load(&g_dropped);}
