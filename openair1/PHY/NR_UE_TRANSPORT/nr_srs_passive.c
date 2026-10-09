/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "common/utils/nr_core_plan.h"
#include "nr_srs_passive.h"
#include "nr_srs_fep.h"
#include "nr_sense_ul.h"
#include "nr_srs_export.h"
#include "nr_sense_api.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "nr_pdcch_blind_monitor.h"
#include "nr_passive_cfg_epoch.h"
#include "PHY/defs_nr_UE.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "common/config/config_userapi.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SRS_DEPTH 4
#define SRS_MARGIN 400
#define SRS_EVENTS 64
typedef struct {
  int type, n, delay; uint16_t rnti; unsigned request; uint64_t slot;
  float h[128]; uint16_t k[64];
  nr_srs_candidate_t hint; uint64_t stamp;
} srs_event_t;
/* Raw symbol windows are copied before returning to the slot producer. Each
 * queue slot remains owned until the worker finishes all delay hypotheses. */
typedef struct {
  c16_t *raw;
  nr_srs_capture_t meta;
  uint16_t mask;
  int advance, n_rb, first_carrier_offset;
  c16_t rot[224];
  c16_t shift[4096] __attribute__((aligned(32)));
  double fo_hz;
  int64_t sample[6];
} srs_job_t;
static struct {
  pthread_mutex_t lock;
  pthread_t thread;
  _Atomic bool active;
  _Atomic uint64_t dropped, elapsed_us;
  _Atomic uint64_t pregate_skip, backoff_skip, budget_skip, idle_resets;
  nr_srs_idle_t idle;
  bool stop;
  unsigned head, tail, count;
  PHY_VARS_NR_UE *owner;
  nr_srs_grid_t grid;
  nr_srs_blind_t *search;
  srs_job_t jobs[SRS_DEPTH];
  c16_t *freq;
  float *cfr;
  uint16_t *k;
  uint8_t *l;
  int cfr_max_re, export_ant, slots_per_subframe;
  double samples_per_second;
  srs_event_t events[SRS_EVENTS]; unsigned event_count;
  nr_srs_candidate_t hint; bool have_hint;
  uint64_t stamp; bool stamp_valid;
} sr={.lock=PTHREAD_MUTEX_INITIALIZER};

static bool srs_budget_checkpoint(struct timespec *cpu_begin)
{
  struct timespec end,wall;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID,&end);
  clock_gettime(CLOCK_MONOTONIC,&wall);
  uint64_t now=(uint64_t)wall.tv_sec*1000000000ull+wall.tv_nsec;
  uint64_t used=(uint64_t)(end.tv_sec-cpu_begin->tv_sec)*1000000000ull+end.tv_nsec-cpu_begin->tv_nsec;
  if(used) nr_srs_cpu_charge(&sr.idle,now,used);
  *cpu_begin=end;
  return nr_srs_cpu_allow(&sr.idle,now,1000000);
}

bool nr_srs_passive_enabled(void) { return atomic_load_explicit(&sr.active,memory_order_relaxed); }

static void process_srs(srs_job_t *job)
{
  nr_sense_prod_scope_t prod __attribute__((cleanup(nr_sense_prod_scope_stop))) =
      {NR_SENSE_SRC_SRS,nr_sense_now_ns()};
  struct timespec cpu_begin;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_begin);
  const int N=sr.grid.fft_size,stride=14*N,win=N+2*SRS_MARGIN;
  const uint64_t stamp=((uint64_t)job->meta.epoch<<32)|job->meta.identity_gen;
  if(sr.stamp_valid && stamp!=sr.stamp) {nr_srs_blind_reset(sr.search);sr.have_hint=false;}
  sr.stamp=stamp;sr.stamp_valid=true;
  if(job->n_rb!=sr.grid.n_rb || job->first_carrier_offset!=sr.grid.first_carrier_offset) {
    pthread_mutex_lock(&sr.lock);
    sr.grid.n_rb=job->n_rb;sr.grid.first_carrier_offset=job->first_carrier_offset;
    bool refreshed=nr_srs_refresh_grid(sr.search,&sr.grid);
    pthread_mutex_unlock(&sr.lock);
    if(!refreshed) return;
    sr.have_hint=false;
  }
  srs_event_t events[SRS_EVENTS];unsigned event_count;
  pthread_mutex_lock(&sr.lock);
  event_count=sr.event_count;memcpy(events,sr.events,event_count*sizeof(*events));sr.event_count=0;
  pthread_mutex_unlock(&sr.lock);
  for(unsigned i=0;i<event_count;i++) {
    srs_event_t *e=&events[i];
    if(job->meta.reconf && e->stamp!=stamp) continue;
    /* Bootstrap lookup takes a mutex: keep it on the consumer, never on an
     * observation producer. Unconfirmed RNTIs cannot supply attribution. */
    if(e->type<2 && !nr_pdcch_blind_monitor_rnti_confirmed(e->slot%(1024*sr.grid.slots_per_frame),e->rnti)) continue;
    if(e->type==0) {nr_srs_note_trigger(sr.search,e->rnti,e->slot,e->request);nr_srs_idle_result(&sr.idle,true);}
    else if(e->type==1) nr_srs_note_pusch(sr.search,e->rnti,e->slot,e->delay,e->h,e->k,e->n);
    else if(e->type==2) {sr.hint=e->hint;sr.have_hint=true;nr_srs_idle_result(&sr.idle,true);}
    else nr_srs_note_grant(sr.search,e->slot,e->request,e->hint.start_rb,e->hint.nb_rb);
  }
  nr_srs_candidate_t seen[NR_SRS_MAX_CANDIDATES];int nseen=0;
  nr_srs_candidate_t due[NR_SRS_MAX_TRACKS];
  bool tracking=nr_srs_has_confirmed(sr.search,job->meta.abs_slot);
  int ndue=tracking?nr_srs_confirmed_due(sr.search,job->meta.abs_slot,due):0;
  if(tracking && !ndue) {srs_budget_checkpoint(&cpu_begin);return;}
  static const int offsets[]={0,-100,100,-200,200,-300,300,-400,400};
  nr_srs_candidate_t candidates[NR_SRS_MAX_CANDIDATES];
  bool budget_exhausted=false, scanned=true, did_gate=false, any_comb=false;
  for(unsigned d=0;d<sizeof(offsets)/sizeof(*offsets);d++) {
    if(!srs_budget_checkpoint(&cpu_begin)) {budget_exhausted=true;break;}
    memset(sr.freq,0,sr.grid.n_ant*stride*sizeof(c16_t));
    for(int a=0;a<sr.grid.n_ant;a++) for(int l=0;l<6;l++) {
      if(!(job->mask&(1u<<(l+8)))) continue;
      const c16_t *raw=job->raw+(a*6+l)*win+SRS_MARGIN+offsets[d];
      nr_srs_fep_symbol(&sr.grid,sr.slots_per_subframe,job->shift,job->rot,job->meta.slot,l+8,
        job->fo_hz,sr.samples_per_second/1000,job->sample[l]+SRS_MARGIN+offsets[d],raw,
        sr.freq+a*stride+(l+8)*N);
    }
    if(!tracking) {
      bool comb=nr_srs_pregate(&sr.grid,(int16_t *)sr.freq,stride,job->mask);
      did_gate=true;
      if(d==0) scanned=nr_srs_idle_scan(&sr.idle,job->meta.abs_slot,comb);
      else if(comb) nr_srs_idle_result(&sr.idle,true);
      if(!scanned) break;
      if(!comb) continue;
      any_comb=true;
    }
    int n=nr_srs_comb_detect(&sr.grid,(int16_t *)sr.freq,stride,job->mask,candidates);
    if(tracking) {
      int kept=0;
      for(int i=0;i<n;i++) for(int j=0;j<ndue;j++)
        if(candidates[i].comb==due[j].comb && candidates[i].comb_offset==due[j].comb_offset &&
           candidates[i].start_rb==due[j].start_rb && candidates[i].nb_rb==due[j].nb_rb &&
           candidates[i].start_symbol==due[j].start_symbol && candidates[i].n_symbols==due[j].n_symbols) {
          candidates[kept++]=candidates[i];break;
        }
      n=kept;
    }

    for(int i=0;i<n;i++) {
      if(!srs_budget_checkpoint(&cpu_begin)) {budget_exhausted=true;break;}
      nr_srs_candidate_t *c=&candidates[i];
      bool duplicate=false;
      for(int j=0;j<nseen;j++) if(seen[j].start_rb==c->start_rb && seen[j].nb_rb==c->nb_rb
        && seen[j].start_symbol==c->start_symbol && seen[j].comb==c->comb && seen[j].comb_offset==c->comb_offset) duplicate=true;
      if(duplicate) continue;
      nr_srs_candidate_t hint=*c;
      bool use_hint=false;
      if(tracking) for(int j=0;j<ndue;j++)
        if(c->comb==due[j].comb && c->comb_offset==due[j].comb_offset &&
           c->start_rb==due[j].start_rb && c->nb_rb==due[j].nb_rb &&
           c->start_symbol==due[j].start_symbol && c->n_symbols==due[j].n_symbols) {
          hint.group=due[j].group;hint.cs=due[j].cs;use_hint=true;break;
        }
      if(!use_hint && sr.have_hint && c->comb==sr.hint.comb && c->comb_offset==sr.hint.comb_offset) {
        hint.group=sr.hint.group;hint.cs=sr.hint.cs;use_hint=true;
      }
      if(!nr_srs_search_slot(sr.search,(int16_t *)sr.freq,stride,c,use_hint?&hint:NULL,job->meta.abs_slot)) {
        /* Hopping may change u between symbols. Only export jointly when every
         * symbol validates; otherwise search each symbol independently. */
        if(c->n_symbols>1) for(int l=0;l<c->n_symbols && n<NR_SRS_MAX_CANDIDATES;l++) {
          candidates[n]=*c;candidates[n].start_symbol+=l;candidates[n++].n_symbols=1;
        }
        continue;
      }
      int nre=nr_srs_estimate(sr.search,(int16_t *)sr.freq,stride,c,sr.cfr,sr.k,sr.l);
      if(!nre) continue;
      /* Positive advance = arrival earlier than the DL-frame FFT window, same
       * convention as passive PUSCH: ta_new = ta_old - residual_delay. */
      job->meta.ta_est_samples=job->advance-offsets[d]-c->delay_samples;
      c->delay_samples=job->meta.ta_est_samples;
      for(int a=0;a<sr.grid.n_ant;a++) for(int r=0;r<nre;r++)
        nr_sense_ul_to_dl(&sr.cfr[2*(a*nre+r)],&sr.cfr[2*(a*nre+r)+1],sr.k[r],
                         sr.grid.first_carrier_offset,N,job->advance-offsets[d]);
      int old_cs=c->cs;
      nr_srs_attribute(sr.search,c,job->meta.abs_slot,sr.cfr,sr.k,nre);
      if(c->cs!=old_cs) {
        nre=nr_srs_estimate(sr.search,(int16_t *)sr.freq,stride,c,sr.cfr,sr.k,sr.l);
        if(!nre) continue;
        for(int a=0;a<sr.grid.n_ant;a++) for(int r=0;r<nre;r++)
          nr_sense_ul_to_dl(&sr.cfr[2*(a*nre+r)],&sr.cfr[2*(a*nre+r)+1],sr.k[r],
                           sr.grid.first_carrier_offset,N,job->advance-offsets[d]);
      }
      job->meta.ta_est_samples=c->delay_samples;
      nr_srs_observe(sr.search,c,job->meta.abs_slot);
      nr_srs_idle_result(&sr.idle,true);
      job->meta.n_ant=sr.export_ant;
      nr_sense_prod_scope_stop(&prod);
      nr_srs_export(c,&job->meta,nre,sr.cfr,sr.k,sr.l);
      prod.start_ns=nr_sense_now_ns();
      if(nseen<NR_SRS_MAX_CANDIDATES) seen[nseen++]=*c;
    }
    if(budget_exhausted || nseen==NR_SRS_MAX_CANDIDATES) break;
  }
  srs_budget_checkpoint(&cpu_begin);
  if(did_gate && !any_comb && !budget_exhausted) {
    sr.idle.pregate_skip++;
    if(scanned && !budget_exhausted) nr_srs_idle_result(&sr.idle,false);
  }
  if(tracking && !budget_exhausted) nr_srs_confirmed_miss(sr.search,job->meta.abs_slot,seen,nseen);
}
static void *srs_worker(void *unused)
{
  for(;;) {
    pthread_mutex_lock(&sr.lock);
    bool done=sr.stop && sr.count==0;
    srs_job_t *job=sr.count?&sr.jobs[sr.tail]:NULL;
    pthread_mutex_unlock(&sr.lock);
    if(done) break;
    if(!job) {struct timespec ts={.tv_nsec=1000000};nanosleep(&ts,NULL);continue;}
    struct timespec begin,end;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&begin);
    process_srs(job);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&end);
    const uint64_t elapsed_ns=(int64_t)(end.tv_sec-begin.tv_sec)*1000000000+end.tv_nsec-begin.tv_nsec;
    atomic_fetch_add(&sr.elapsed_us,elapsed_ns/1000);
    atomic_store(&sr.pregate_skip,sr.idle.pregate_skip);
    atomic_store(&sr.backoff_skip,sr.idle.backoff_skip);
    atomic_store(&sr.budget_skip,sr.idle.budget_skip);
    atomic_store(&sr.idle_resets,sr.idle.resets);
    pthread_mutex_lock(&sr.lock);sr.tail=(sr.tail+1)%SRS_DEPTH;sr.count--;pthread_mutex_unlock(&sr.lock);
  }
  return NULL;
}
/* nr_srs_passive_start: starts the passive SRS (uplink sounding) worker when the build enables it (NR_SRS_EXPORT_ON), the sensing and the SRS source are on, and the frame is TDD: reads cfr_max_re and cfr_antennas from [sensing], allocates the frequency and CFR buffers and the job ring, and starts the worker.
 *   Does nothing when any requirement is missing or an allocation fails. Caller: executables/nr-uesoftmodem.c (passive receiver start-up).
 */
void nr_srs_passive_start(PHY_VARS_NR_UE *ue)
{
#ifndef NR_SRS_EXPORT_ON
  return;
#endif
  if(nr_srs_passive_enabled() || !nr_sense_enabled() || !ue || ue->frame_parms.frame_type!=TDD) return;
  if (!nr_isac_source_enabled(NR_ISAC_SRC_SRS)) return;
  int max_re=46200,antennas=0;
  paramdef_t p[]={
    {.optname="cfr_max_re",.type=TYPE_INT,.iptr=&max_re,.defintval=46200},
    {.optname="cfr_antennas",.type=TYPE_INT,.iptr=&antennas,.defintval=0}};
  config_get(uniqCfg,p,2,"sensing");
  NR_DL_FRAME_PARMS *fp=&ue->frame_parms;
  sr.grid=(nr_srs_grid_t){fp->ofdm_symbol_size,fp->first_carrier_offset,fp->N_RB_UL,
    fp->nb_antennas_rx,fp->symbols_per_slot,fp->slots_per_frame};
  sr.search=nr_srs_blind_create(&sr.grid);
  if(!sr.search) return;
  const int N=sr.grid.fft_size;
  sr.freq=calloc(sr.grid.n_ant*14*N,sizeof(c16_t));
  sr.cfr=calloc(sr.grid.n_ant*NR_SRS_MAX_RE*2,sizeof(float));
  sr.k=calloc(NR_SRS_MAX_RE,sizeof(uint16_t));sr.l=calloc(NR_SRS_MAX_RE,1);
  for(int i=0;i<SRS_DEPTH;i++) sr.jobs[i].raw=calloc(sr.grid.n_ant*6*(N+2*SRS_MARGIN),sizeof(c16_t));
  bool ok=sr.freq && sr.cfr && sr.k && sr.l;
  for(int i=0;i<SRS_DEPTH;i++) ok=ok && sr.jobs[i].raw;
  sr.owner=ue;sr.stop=false;sr.head=sr.tail=sr.count=sr.event_count=0;sr.have_hint=sr.stamp_valid=false;
  atomic_store(&sr.dropped,0);atomic_store(&sr.elapsed_us,0);
  sr.idle=(nr_srs_idle_t){.pct=5};
  const char *cpu_pct=getenv("ISAC_SRS_CPU_PCT");
  if(cpu_pct) {char *end;long value=strtol(cpu_pct,&end,10);if(end!=cpu_pct && !*end && value>=1 && value<=100) sr.idle.pct=value;}
  atomic_store(&sr.pregate_skip,0);atomic_store(&sr.backoff_skip,0);
  atomic_store(&sr.budget_skip,0);atomic_store(&sr.idle_resets,0);
  sr.slots_per_subframe=fp->slots_per_subframe;
  LOG_W(PHY,"SENSING: blind SRS sequence hopping v=1 is unsupported\n");
  sr.samples_per_second=1000.0*fp->samples_per_subframe;
  sr.cfr_max_re=max_re;sr.export_ant=antennas==1?1:sr.grid.n_ant;
  if(!ok || nr_core_plan_create(&sr.thread,NULL,srs_worker,NULL, -1)) {
    nr_srs_blind_destroy(sr.search);sr.search=NULL;free(sr.freq);free(sr.cfr);free(sr.k);free(sr.l);
    for(int i=0;i<SRS_DEPTH;i++) {free(sr.jobs[i].raw);sr.jobs[i].raw=NULL;}
    return;
  }
  atomic_store_explicit(&sr.active,true,memory_order_release);
}
void nr_srs_passive_stop(void)
{
  if(!atomic_exchange_explicit(&sr.active,false,memory_order_acq_rel)) return;
  pthread_mutex_lock(&sr.lock);sr.stop=true;pthread_mutex_unlock(&sr.lock);
  pthread_join(sr.thread,NULL);
  pthread_mutex_lock(&sr.lock);
  nr_srs_blind_destroy(sr.search);sr.search=NULL;free(sr.freq);free(sr.cfr);free(sr.k);free(sr.l);
  for(int i=0;i<SRS_DEPTH;i++) {free(sr.jobs[i].raw);sr.jobs[i].raw=NULL;}
  pthread_mutex_unlock(&sr.lock);
  LOG_I(PHY,"SENSING: SRS queue dropped=%lu\n",(unsigned long)atomic_load(&sr.dropped));
}
void nr_srs_passive_capture(PHY_VARS_NR_UE *ue,int frame,int slot,uint64_t abs_slot)
{
  if(!nr_srs_passive_enabled()) return;
  if(pthread_mutex_trylock(&sr.lock)) {atomic_fetch_add(&sr.dropped,1);return;}
  if(!atomic_load_explicit(&sr.active,memory_order_relaxed) || ue!=sr.owner) {pthread_mutex_unlock(&sr.lock);return;}
  const NR_DL_FRAME_PARMS *fp=&ue->frame_parms;
  if(sr.count==SRS_DEPTH || fp->ofdm_symbol_size!=sr.grid.fft_size
      || fp->slots_per_subframe!=sr.slots_per_subframe || fp->nb_antennas_rx!=sr.grid.n_ant || 1000.0*fp->samples_per_subframe!=sr.samples_per_second || fp->frame_type!=TDD || fp->ofdm_offset_divisor==0) {
    atomic_fetch_add(&sr.dropped,1);pthread_mutex_unlock(&sr.lock);return;
  }
  nr_srs_grid_t captured=sr.grid;captured.n_rb=fp->N_RB_UL;captured.first_carrier_offset=fp->first_carrier_offset;
  if(!nr_srs_valid_capture_grid(&captured)) {atomic_fetch_add(&sr.dropped,1);pthread_mutex_unlock(&sr.lock);return;}
  /* Process the preceding slot so all +/-400 windows are already in RX memory. */
  if(!abs_slot) {pthread_mutex_unlock(&sr.lock);return;}
  abs_slot--;if(--slot<0) {slot=fp->slots_per_frame-1;frame=(frame+1023)%1024;}
  uint16_t mask=0;
  const fapi_nr_tdd_table_t *t=&ue->nrUE_config.tdd_table;
  if(t->max_tdd_periodicity_list && t->tdd_period_in_slots) {
    const fapi_nr_max_tdd_periodicity_t *sl=&t->max_tdd_periodicity_list[slot%t->tdd_period_in_slots];
    for(int l=8;l<14;l++) if(sl->max_num_of_symbol_per_slot_list[l].slot_config==1) mask|=1u<<l;
  }
  if(!mask) {pthread_mutex_unlock(&sr.lock);return;}
  srs_job_t *job=&sr.jobs[sr.head];job->mask=mask;
  job->n_rb=fp->N_RB_UL;job->first_carrier_offset=fp->first_carrier_offset;
  memcpy(job->rot,fp->symbol_rotation[link_type_ul],sizeof(job->rot));
  memcpy(job->shift,fp->timeshift_symbol_rotation,sr.grid.fft_size*sizeof(c16_t));
  bool reconf=nr_cfg_reconf_enabled();uint64_t stamp=reconf?nr_cfg_epoch_cfr_stamp():0;
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  job->meta=(nr_srs_capture_t){.abs_slot=abs_slot,.frame=frame,.slot=slot,.pci=fp->Nid_cell,
    .t_mono_ns=(uint64_t)ts.tv_sec*1000000000+ts.tv_nsec,.reconf=reconf,.epoch=stamp>>32,.identity_gen=stamp,
    .dedicated_epoch=reconf ? (int)nr_cfg_epoch_dedicated_current() : -1,
    .n_ant=sr.export_ant,.cfr_max_re=sr.cfr_max_re};
  job->advance=(uint64_t)25600*fp->samples_per_subframe/(4096ull*480ull);
  job->fo_hz=ue->cont_fo_comp?ue->dl_Doppler_shift+ue->freq_offset:0;
  const int N=sr.grid.fft_size,win=N+2*SRS_MARGIN;
  for(int l=8;l<14;l++) {
    if(!(mask&(1u<<l))) continue;
    int cp=get_samples_symbol_duration(fp,slot,l,1)-N;
    int64_t first=(int64_t)get_samples_slot_timestamp(fp,slot)+get_samples_symbol_timestamp(fp,slot,l)
      +cp-fp->nb_prefix_samples/fp->ofdm_offset_divisor-job->advance-SRS_MARGIN;
    job->sample[l-8]=first;
    int pos=(first%fp->samples_per_frame+fp->samples_per_frame)%fp->samples_per_frame;
    int n=win;if(pos+n>fp->samples_per_frame) n=fp->samples_per_frame-pos;
    for(int a=0;a<sr.grid.n_ant;a++) {
      c16_t *dst=job->raw+(a*6+l-8)*win;
      memcpy(dst,(c16_t *)ue->common_vars.rxdata[a]+pos,n*sizeof(c16_t));
      if(n<win) memcpy(dst+n,ue->common_vars.rxdata[a],(win-n)*sizeof(c16_t));
    }
  }
  sr.head=(sr.head+1)%SRS_DEPTH;sr.count++;pthread_mutex_unlock(&sr.lock);
}

static void post_event(const srs_event_t *event)
{
  if(!nr_srs_passive_enabled()) return;
  if(pthread_mutex_trylock(&sr.lock)) {atomic_fetch_add(&sr.dropped,1);return;}
  if(atomic_load_explicit(&sr.active,memory_order_relaxed)) {
    if(sr.event_count<SRS_EVENTS) {
      sr.events[sr.event_count]=*event;
      sr.events[sr.event_count++].stamp=nr_cfg_reconf_enabled()?nr_cfg_epoch_cfr_stamp():0;
    } else atomic_fetch_add(&sr.dropped,1);
  }
  pthread_mutex_unlock(&sr.lock);
}
void nr_srs_passive_trigger(uint16_t rnti,uint64_t slot,unsigned request)
{
  if(!nr_srs_passive_enabled() || !rnti || !request) return;
  srs_event_t e={.type=0,.rnti=rnti,.slot=slot,.request=request};post_event(&e);
}
void nr_srs_passive_pusch(uint16_t rnti,uint64_t slot,int ta_est_samples,const float *cfr,const uint32_t *k,int n_re)
{
  if(!nr_srs_passive_enabled() || !rnti || !cfr || !k || n_re<=0) return;
  srs_event_t e={.type=1,.rnti=rnti,.slot=slot,.delay=ta_est_samples};
  for(int i=0;i<n_re && e.n<64;i++) {
    if(k[i]>UINT16_MAX) continue;
    bool duplicate=false;for(int j=0;j<e.n;j++) if(e.k[j]==k[i]) duplicate=true;
    if(duplicate) continue;
    e.k[e.n]=k[i];e.h[2*e.n]=cfr[2*i];e.h[2*e.n+1]=cfr[2*i+1];e.n++;
  }
  post_event(&e);
}
void nr_srs_passive_hint(int comb,int offset,int group,int cs)
{
  if(!nr_srs_passive_enabled()) return;
  srs_event_t e={.type=2,.hint={.comb=comb,.comb_offset=offset,.group=group,.cs=cs}};post_event(&e);
}

void nr_srs_passive_grant(uint64_t slot,uint16_t mask,int start_rb,int nb_rb)
{
  if(!nr_srs_passive_enabled()) return;
  srs_event_t e={.type=3,.slot=slot,.request=mask,.hint={.start_rb=start_rb,.nb_rb=nb_rb}};
  post_event(&e);
}
void nr_srs_passive_metrics(uint64_t *us,uint64_t *dropped,uint64_t *pregate,uint64_t *backoff,
                            uint64_t *budget,uint64_t *resets)
{
  *us=atomic_load(&sr.elapsed_us);*dropped=atomic_load(&sr.dropped);
  *pregate=atomic_load(&sr.pregate_skip);*backoff=atomic_load(&sr.backoff_skip);
  *budget=atomic_load(&sr.budget_skip);*resets=atomic_load(&sr.idle_resets);
}
