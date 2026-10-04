/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_ue_ctx.h"
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { EV_OBS, EV_PARAM, EV_ANCHOR, EV_EPOCH, EV_SIB1, EV_CSI, EV_TICK } kind_t;
typedef struct {
  kind_t kind;
  uint64_t ns;
  int64_t slot, value, extra;
  uint16_t rnti;
  int p, cause;
  nr_ue_verif_t verif;
  nr_cfg_epoch_snapshot_t epoch;
  nr_passive_obs_t obs;
} event_t;
typedef struct { _Atomic uint64_t seq; event_t event; } ring_slot_t;
typedef struct {
  nr_ue_ctx_t ctx;
  bool used;
  uint64_t last_activity_ns, pending_ns;
  int64_t pending_slot;
  uint64_t pending_params;
} entry_t;
static ring_slot_t *ring;
static entry_t *entries;
static int16_t active_index[UINT16_MAX + 1];
static uint32_t capacity;
static uint64_t head;
static _Atomic uint64_t tail, pushed, written, dropped;
static _Atomic uint32_t producers;
static _Atomic bool running;
bool nr_ue_ctx_fast_open;
static pthread_t worker;
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *output;
static uint32_t epoch, identity;
static int64_t sib1_values[3] = {-1,-1,-1};
static uint64_t last_period_ns;
static uint64_t last_age_ns;
static double period_s;
static bool real_time_session;
extern void nr_cfg_epoch_note_rnti_reopened(uint16_t, bool, uint64_t) __attribute__((weak));

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
  atomic_fetch_add_explicit(&producers, 1, memory_order_seq_cst);
  if (!__atomic_load_n(&nr_ue_ctx_fast_open, __ATOMIC_SEQ_CST)) {
    atomic_fetch_sub_explicit(&producers, 1, memory_order_seq_cst);
    return false;
  }
  uint64_t pos = atomic_load_explicit(&tail, memory_order_relaxed);
  for (;;) {
    ring_slot_t *s = &ring[pos % capacity];
    int64_t dif = (int64_t)(atomic_load_explicit(&s->seq, memory_order_acquire) - pos);
    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&tail, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) {
        s->event = *e;
        atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
        atomic_fetch_add_explicit(&pushed, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(&producers, 1, memory_order_seq_cst);
        return true;
      }
    } else if (dif < 0) {
      atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
      atomic_fetch_sub_explicit(&producers, 1, memory_order_seq_cst);
      return false;
    } else pos = atomic_load_explicit(&tail, memory_order_relaxed);
  }
}
static bool dequeue(event_t *e)
{
  ring_slot_t *s = &ring[head % capacity];
  if (atomic_load_explicit(&s->seq, memory_order_acquire) != head + 1) return false;
  *e = s->event;
  atomic_store_explicit(&s->seq, head + capacity, memory_order_release);
  head++;
  return true;
}
static const char *param_name[NR_UEP_COUNT] = {
  "RNTI_CLASS","ANCHOR","CORESET","DCI_LEN_DL","DCI_LEN_UL","DCI_LEN_STATE",
  "PDCCH_SCR_ID","PDSCH_SCR_ID","TD_WINNER","TD_STATE","MCS_TABLE","DMRS_CFG",
  "MAX_LAYERS","LBRM","BWP","PUSCH_LAYOUT","SIB1_HASH","SIB1_BWP",
  "SIB1_TDRA_HASH","APERIODIC_CSI"
};
static const char *state_name[] = {"FIRST_SEEN","ACTIVE","IDLE","GONE"};
static const char *verif_name[] = {"TRUSTED","HINT","SUSPECT"};
static const char *cause_name[] = {
  "FIRST_LEARNED","CONVERGED","RELOCK","REOPENED_NEW_WINNER","BWP_CHANGE",
  "CORESET_CHANGE","EPOCH_REVERIFIED","EPOCH_DISCARDED","HARD_RESET"
};
static uint8_t source_for_param(nr_ue_param_t p)
{
  if(p>=NR_UEP_SIB1_HASH && p<=NR_UEP_SIB1_TDRA_HASH) return NR_UES_SIB1;
  if(p==NR_UEP_ANCHOR) return NR_UES_RAR;
  if(p==NR_UEP_BWP) return NR_UES_BWP;
  if(p==NR_UEP_TD_WINNER || p==NR_UEP_TD_STATE || p==NR_UEP_MCS_TABLE || p==NR_UEP_DMRS_CFG)
    return NR_UES_SWEEP;
  if(p==NR_UEP_PDSCH_SCR_ID || p==NR_UEP_MAX_LAYERS) return NR_UES_OBSERVATION;
  return NR_UES_PDCCH;
}
static void json_int(int64_t v) { if (v < 0) fputs("null", output); else fprintf(output, "%" PRId64, v); }
static void json_float(double v) { if (isfinite(v)) fprintf(output, "%.8g", v); else fputs("null", output); }
static void line_done(void) { if (!ferror(output)) atomic_fetch_add_explicit(&written, 1, memory_order_relaxed); }
static void snapshot(entry_t *e, uint64_t ns)
{
  nr_ue_ctx_t *c = &e->ctx;
  fprintf(output, "{\"schema\":\"uectx/1\",\"type\":\"ue_snapshot\",\"t_mono_ns\":%" PRIu64
                  ",\"identity_gen\":%u,\"rnti\":%u,\"incarnation\":%u,\"state\":\"%s\",\"cfg\":{",
          ns, c->identity_gen, c->rnti, c->incarnation, state_name[c->state]);
  for (int p = 0; p < NR_UEP_COUNT; p++) {
    nr_ue_cfg_value_t *v = &c->cfg[p];
    fprintf(output, "%s\"%s\":", p ? "," : "", param_name[p]);
    if (v->value < 0) fputs("null", output);
    else {
      fprintf(output, "{\"value\":%" PRId64 ",\"epoch_learned\":%u,\"verif\":\"%s\",\"first_abs_slot\":",
              v->value, v->epoch_learned, verif_name[v->verif]);
      json_int(v->first_abs_slot);
      fputs(",\"last_confirmed_abs_slot\":", output); json_int(v->last_confirmed_abs_slot);
      fprintf(output, ",\"source\":%u}", v->source);
    }
  }
  nr_ue_stats_t *s = &c->st;
  fprintf(output, "},\"stats\":{\"grants_dl\":%" PRIu64 ",\"grants_ul\":%" PRIu64
                  ",\"crc_ok_dl\":%" PRIu64 ",\"crc_ok_ul\":%" PRIu64
                  ",\"bytes_dl\":%" PRIu64 ",\"bytes_ul\":%" PRIu64 ",\"retx_dl\":%" PRIu64,
          s->grants_dl,s->grants_ul,s->crc_ok_dl,s->crc_ok_ul,s->bytes_dl,s->bytes_ul,s->retx_dl);
  fputs(",\"mcs_hist\":[",output);
  for (int i=0;i<32;i++) fprintf(output,"%s%u",i?",":"",s->mcs_hist[i]);
  fputs("],\"layers_hist\":[",output);
  for (int i=0;i<5;i++) fprintf(output,"%s%u",i?",":"",s->layers_hist[i]);
  fputs("],\"snr_ema_db\":",output); json_float(s->snr_ema_db);
  fputs(",\"nvar_ema\":",output); json_float(s->nvar_ema);
  fputs(",\"fo_ema_hz\":",output); json_float(s->fo_ema_hz);
  fputs(",\"ta_ema_samples\":",output); json_float(s->ta_ema_samples);
  fputs(",\"prb_start_min\":",output); json_int(s->prb_start_min);
  fputs(",\"prb_start_max\":",output); json_int(s->prb_start_max);
  fputs(",\"prb_size_min\":",output); json_int(s->prb_size_min);
  fputs(",\"prb_size_max\":",output); json_int(s->prb_size_max);
  fputs(",\"prb_size_mean\":",output); json_float(s->prb_size_mean);
  fputs(",\"last_abs_slot\":",output); json_int(s->last_abs_slot);
  fprintf(output, "},\"n_changes\":%u,\"n_reconfigs\":%u}\n",c->n_changes,c->n_reconfigs);
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
  fprintf(output,"{\"schema\":\"uectx/1\",\"type\":\"ue_reconfig\",\"t_mono_ns\":%" PRIu64
                 ",\"abs_slot\":",e->pending_ns);
  json_int(e->pending_slot);
  fprintf(output,",\"epoch\":%u,\"identity_gen\":%u,\"rnti\":%u,\"incarnation\":%u,\"params\":[",
          epoch,c->identity_gen,c->rnti,c->incarnation);
  bool comma=false;
  for(int p=0;p<NR_UEP_COUNT;p++) if(e->pending_params & (1ull<<p)) {
    fprintf(output,"%s\"%s\"",comma?",":"",param_name[p]); comma=true;
  }
  fprintf(output,"],\"class\":\"%s\"}\n",reconfig_class(e->pending_params));
  line_done(); c->n_reconfigs++;
  if (nr_cfg_epoch_note_rnti_reopened && e->pending_slot >= 0)
    nr_cfg_epoch_note_rnti_reopened(c->rnti,true,(uint64_t)e->pending_slot);
  e->pending_params=0;
}
static void change(entry_t *e,nr_ue_param_t p,int64_t old,int64_t value,int cause,
                   int64_t slot,uint64_t ns,int64_t evidence)
{
  nr_ue_ctx_t *c=&e->ctx;
  fprintf(output,"{\"schema\":\"uectx/1\",\"type\":\"ue_change\",\"t_mono_ns\":%" PRIu64
                 ",\"abs_slot\":",ns); json_int(slot);
  fprintf(output,",\"epoch\":%u,\"identity_gen\":%u,\"rnti\":%u,\"incarnation\":%u,\"param\":\"%s\",\"old\":",
          epoch,c->identity_gen,c->rnti,c->incarnation,param_name[p]);
  json_int(old); fputs(",\"new\":",output); json_int(value);
  fprintf(output,",\"cause\":\"%s\",\"evidence\":",
          cause>=0 && cause<=NR_UEC_HARD_RESET?cause_name[cause]:"FIRST_LEARNED");
  if(p==NR_UEP_APERIODIC_CSI && evidence>=0) {
    const uint64_t bits=(uint64_t)evidence;
    fprintf(output,"{\"row\":%u,\"freq_domain\":%u,\"start_rb\":%u,\"nr_of_rbs\":%u,\"symb_l0\":%u,\"scramb_id\":%u}",
            (unsigned)(bits&63u),(unsigned)((bits>>6)&65535u),
            (unsigned)((bits>>22)&511u),(unsigned)((bits>>31)&511u),
            (unsigned)((bits>>40)&15u),(unsigned)((bits>>44)&65535u));
  } else json_int(evidence);
  fputs("}\n",output); line_done(); c->n_changes++;
}
static void init_entry(entry_t *e,uint16_t rnti,uint16_t incarnation,uint64_t ns)
{
  memset(e,0,sizeof(*e));
  e->used=true; e->ctx.rnti=rnti; e->ctx.incarnation=incarnation;
  e->ctx.identity_gen=identity; e->ctx.state=NR_UE_FIRST_SEEN; e->last_activity_ns=ns;
  for(int p=0;p<NR_UEP_COUNT;p++) {
    e->ctx.cfg[p].value=-1;
    e->ctx.cfg[p].first_abs_slot=-1;
    e->ctx.cfg[p].last_confirmed_abs_slot=-1;
  }
  nr_ue_stats_t *s=&e->ctx.st;
  s->snr_ema_db=s->nvar_ema=s->fo_ema_hz=s->ta_ema_samples=NAN;
  s->prb_start_min=s->prb_start_max=s->prb_size_min=s->prb_size_max=-1;
  s->prb_size_mean=NAN; s->last_abs_slot=-1;
  for(int i=0;i<3;i++) if(sib1_values[i]>=0) {
    const int p=NR_UEP_SIB1_HASH+i;
    e->ctx.cfg[p].value=sib1_values[i];
    e->ctx.cfg[p].verif=NR_UEV_TRUSTED;
    e->ctx.cfg[p].epoch_learned=epoch;
  }
  snapshot(e,ns);
}
static entry_t *find_entry(uint16_t rnti,uint64_t ns,bool create)
{
  const int16_t active=active_index[rnti];
  if(active>=0 && entries[active].used && entries[active].ctx.identity_gen==identity &&
     entries[active].ctx.state!=NR_UE_GONE) return &entries[active];
  entry_t *free_slot=NULL,*evict=NULL;
  uint16_t next_incarnation=0;
  for(int i=0;i<NR_UECTX_MAX_UE;i++) {
    entry_t *e=&entries[i];
    if(e->used && e->ctx.rnti==rnti) {
      if(e->ctx.identity_gen==identity && e->ctx.incarnation>=next_incarnation)
        next_incarnation=e->ctx.incarnation+1;
      if(e->ctx.identity_gen==identity && e->ctx.state!=NR_UE_GONE) return e;
    }
    if(!e->used && !free_slot) free_slot=e;
    if(e->used && e->ctx.state==NR_UE_GONE && (!evict || e->last_activity_ns<evict->last_activity_ns)) evict=e;
  }
  if(!create) return NULL;
  entry_t *e=free_slot?free_slot:evict;
  if(!e) return NULL;
  if(e->used) flush_reconfig(e);
  if(e->used && active_index[e->ctx.rnti]==e-entries) active_index[e->ctx.rnti]=-1;
  init_entry(e,rnti,next_incarnation,ns);
  active_index[rnti]=(int16_t)(e-entries);
  return e;
}
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
     * a dedicated UE reconfiguration. The epoch authority handles cell-common changes. */
    if(p!=NR_UEP_APERIODIC_CSI && p!=NR_UEP_RNTI_CLASS && p!=NR_UEP_ANCHOR &&
       (p<NR_UEP_SIB1_HASH || p>NR_UEP_SIB1_TDRA_HASH) &&
       p!=NR_UEP_TD_STATE && p!=NR_UEP_DCI_LEN_STATE &&
       old>=0 && v->verif==NR_UEV_TRUSTED && verif==NR_UEV_TRUSTED) {
      if(e->pending_params && ns>=e->pending_ns && ns-e->pending_ns>2000000000ull) flush_reconfig(e);
      if(!e->pending_params) {e->pending_ns=ns; e->pending_slot=slot;}
      else if(e->pending_slot<0 && slot>=0) e->pending_slot=slot;
      e->pending_params|=1ull<<p;
    }
    v->first_abs_slot=slot;
  } else if(v->verif==NR_UEV_HINT && verif==NR_UEV_TRUSTED) {
    change(e,p,old,value,NR_UEC_EPOCH_REVERIFIED,slot,ns,evidence);
  } else if(v->verif!=verif && verif==NR_UEV_SUSPECT) {
    change(e,p,old,value,cause,slot,ns,evidence);
  }
  v->value=value; v->verif=verif; v->epoch_learned=epoch;
  if(verif==NR_UEV_TRUSTED) v->last_confirmed_abs_slot=slot;
  v->source=source_for_param(p);
}
static float ema(float old,float v)
{ return isfinite(v)?(isfinite(old)?old*0.875f+v*0.125f:v):old; }
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
  if(o->rnti_class>=0 && c->cfg[NR_UEP_RNTI_CLASS].value<0)
    param(e,NR_UEP_RNTI_CLASS,o->rnti_class,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,o->abs_slot,ns,-1);
}
static void age(uint64_t ns)
{
  for(int i=0;i<NR_UECTX_MAX_UE;i++) {
    entry_t *e=&entries[i];
    if(e->used && e->pending_params && ns>=e->pending_ns &&
       ns-e->pending_ns>=2000000000ull) flush_reconfig(e);
    if(!e->used || e->ctx.state==NR_UE_GONE || ns<e->last_activity_ns) continue;
    uint64_t delta=ns-e->last_activity_ns;
    if(delta>=60000000000ull) {flush_reconfig(e); e->ctx.state=NR_UE_GONE; active_index[e->ctx.rnti]=-1; snapshot(e,ns);}
    else if(delta>=10000000000ull && e->ctx.state==NR_UE_ACTIVE) {e->ctx.state=NR_UE_IDLE; snapshot(e,ns);}
  }
}
static void handle(const event_t *v)
{
  pthread_mutex_lock(&ctx_lock);
  uint64_t ns=v->ns?v->ns:now_ns();
  if(v->kind!=EV_TICK && ns>now_ns()-5000000000ull) real_time_session=true;
  if(!last_age_ns || (ns>=last_age_ns && ns-last_age_ns>=1000000000ull)) {
    age(ns);
    last_age_ns=ns;
  }
  if(v->kind==EV_EPOCH) {
    for(int i=0;i<NR_UECTX_MAX_UE;i++) if(entries[i].used && entries[i].ctx.state!=NR_UE_GONE) {
      entry_t *e=&entries[i];
      flush_reconfig(e);
      if(v->epoch.last_class==NR_EPOCH_HARD_RESET) {e->ctx.state=NR_UE_GONE; active_index[e->ctx.rnti]=-1; snapshot(e,ns);}
      else for(int p=0;p<NR_UEP_COUNT;p++) if(e->ctx.cfg[p].value>=0) e->ctx.cfg[p].verif=NR_UEV_HINT;
    }
    epoch=v->epoch.epoch; identity=v->epoch.identity_gen;
    if(v->epoch.last_class==NR_EPOCH_HARD_RESET)
      for(int i=0;i<3;i++) sib1_values[i]=-1;
  } else if(v->kind==EV_SIB1) {
    if(v->p>=NR_UEP_SIB1_HASH && v->p<=NR_UEP_SIB1_TDRA_HASH && v->value>=0) {
      sib1_values[v->p-NR_UEP_SIB1_HASH]=v->value;
      for(int i=0;i<NR_UECTX_MAX_UE;i++) if(entries[i].used && entries[i].ctx.state!=NR_UE_GONE)
        param(&entries[i],(nr_ue_param_t)v->p,v->value,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,v->slot,ns,-1);
    }
  } else if(v->kind!=EV_TICK) {
    entry_t *e=find_entry(v->rnti,ns,true);
    if(e) {
      if(v->kind==EV_PARAM || v->kind==EV_ANCHOR || v->kind==EV_CSI) e->last_activity_ns=ns;
      if(v->kind==EV_OBS) observe(e,&v->obs,ns);
      else if(v->kind==EV_PARAM) param(e,(nr_ue_param_t)v->p,v->value,v->verif,v->cause,v->slot,ns,-1);
      else if(v->kind==EV_ANCHOR) {
        if(e->ctx.state==NR_UE_IDLE) {
          e->ctx.state=NR_UE_GONE; active_index[e->ctx.rnti]=-1; snapshot(e,ns);
          e=find_entry(v->rnti,ns,true);
        }
        if(e) param(e,NR_UEP_ANCHOR,v->value,NR_UEV_TRUSTED,NR_UEC_FIRST_LEARNED,v->slot,ns,-1);
      } else if(v->kind==EV_CSI)
        change(e,NR_UEP_APERIODIC_CSI,-1,v->value,NR_UEC_FIRST_LEARNED,v->slot,ns,v->extra);
    } else atomic_fetch_add_explicit(&dropped,1,memory_order_relaxed);
  }
  if(period_s>0 && (!last_period_ns || (ns>=last_period_ns && ns-last_period_ns>=(uint64_t)(period_s*1e9)))) {
    for(int i=0;i<NR_UECTX_MAX_UE;i++) if(entries[i].used && entries[i].ctx.state!=NR_UE_GONE) snapshot(&entries[i],ns);
    last_period_ns=ns;
  }
  pthread_mutex_unlock(&ctx_lock);
}
static void *run(void *unused)
{
  (void)unused;
  const char *pause=getenv("ISAC_UECTX_TEST_WRITER_PAUSE_MS");
  if(pause && atoi(pause)>0) {struct timespec t={atoi(pause)/1000,(atoi(pause)%1000)*1000000L}; nanosleep(&t,NULL);}
  event_t e;
  uint64_t last_idle_tick=now_ns();
  while(atomic_load_explicit(&running,memory_order_seq_cst) ||
        atomic_load_explicit(&producers,memory_order_seq_cst) ||
        head<atomic_load_explicit(&tail,memory_order_acquire)) {
    if(dequeue(&e)) handle(&e);
    else {
      uint64_t ns=now_ns();
      if(real_time_session && ns-last_idle_tick>=100000000ull) {
        event_t tick={.kind=EV_TICK,.ns=ns};
        handle(&tick);
        last_idle_tick=ns;
      }
      struct timespec t={0,1000000}; nanosleep(&t,NULL);
    }
  }
  pthread_mutex_lock(&ctx_lock);
  for(int i=0;i<NR_UECTX_MAX_UE;i++) if(entries[i].used) {
    flush_reconfig(&entries[i]);
    snapshot(&entries[i],now_ns());
  }
  fflush(output);
  pthread_mutex_unlock(&ctx_lock);
  return NULL;
}
bool nr_ue_ctx_open(const char *path,uint32_t ring_capacity,double snapshot_period_s)
{
  if(!path || !*path || !ring_capacity || nr_ue_ctx_enabled()) return false;
  /* The sequence algorithm needs at least two physical slots; a requested one-slot ring keeps
   * the same bounded/drop behavior with two physical slots. */
  if(ring_capacity<2) ring_capacity=2;
  FILE *f=fopen(path,"a"); if(!f) return false;
  ring_slot_t *r=calloc(ring_capacity,sizeof(*r));
  entry_t *x=calloc(NR_UECTX_MAX_UE,sizeof(*x));
  if(!r || !x) {free(r);free(x);fclose(f);return false;}
  free(entries);
  output=f; ring=r; entries=x; capacity=ring_capacity; head=0;
  period_s=snapshot_period_s; last_period_ns=last_age_ns=0; epoch=identity=0;
  for(int i=0;i<=UINT16_MAX;i++) active_index[i]=-1;
  real_time_session=false;
  for(int i=0;i<3;i++) sib1_values[i]=-1;
  for(uint32_t i=0;i<capacity;i++) atomic_init(&ring[i].seq,i);
  atomic_store(&tail,0);atomic_store(&pushed,0);atomic_store(&written,0);atomic_store(&dropped,0);
  atomic_store_explicit(&producers,0,memory_order_seq_cst);
  atomic_store_explicit(&running,true,memory_order_seq_cst);
  __atomic_store_n(&nr_ue_ctx_fast_open,true,__ATOMIC_SEQ_CST);
  if(pthread_create(&worker,NULL,run,NULL)!=0) {
    __atomic_store_n(&nr_ue_ctx_fast_open,false,__ATOMIC_SEQ_CST);
    atomic_store_explicit(&running,false,memory_order_seq_cst);
    free(ring);free(entries);fclose(output);ring=NULL;entries=NULL;output=NULL;
    return false;
  }
  return true;
}
void nr_ue_ctx_close(void)
{
  if(!nr_ue_ctx_enabled()) return;
  __atomic_store_n(&nr_ue_ctx_fast_open,false,__ATOMIC_SEQ_CST);
  atomic_store_explicit(&running,false,memory_order_seq_cst);
  pthread_join(worker,NULL);
  fclose(output);free(ring);output=NULL;ring=NULL;
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
  if(!out || !entries) return false;
  pthread_mutex_lock(&ctx_lock);
  bool found=false;
  for(int i=0;i<NR_UECTX_MAX_UE;i++) if(entries[i].used && entries[i].ctx.rnti==rnti &&
      (!found || entries[i].ctx.incarnation>=out->incarnation)) {*out=entries[i].ctx;found=true;}
  pthread_mutex_unlock(&ctx_lock);
  return found;
}
void nr_ue_ctx_stats(uint64_t *events,uint64_t *lines,uint64_t *drops)
{if(events)*events=atomic_load(&pushed);if(lines)*lines=atomic_load(&written);if(drops)*drops=atomic_load(&dropped);}
