/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_srs_passive.h"
#include "nr_srs_export.h"
#include "nr_passive_cfg_epoch.h"
#include "PHY/defs_nr_UE.h"
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
/* Raw symbol windows are copied before returning to the slot producer. Each
 * queue slot remains owned until the worker finishes all delay hypotheses. */
typedef struct {
  c16_t *raw;
  nr_srs_capture_t meta;
  uint16_t mask;
  int advance;
  double fo_hz;
  int64_t sample[6];
} srs_job_t;
static struct {
  pthread_mutex_t lock;
  pthread_t thread;
  _Atomic bool active;
  _Atomic uint64_t dropped;
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
  int cfr_max_re, export_ant;
  double samples_per_second;
} sr={.lock=PTHREAD_MUTEX_INITIALIZER};

bool nr_srs_passive_enabled(void) { return atomic_load_explicit(&sr.active,memory_order_relaxed); }

static void process_srs(srs_job_t *job)
{
  const int N=sr.grid.fft_size,stride=14*N,win=N+2*SRS_MARGIN;
  static const int offsets[]={0,-100,100,-200,200,-300,300,-400,400};
  c16_t rotated[N] __attribute__((aligned(32)));
  nr_srs_candidate_t candidates[NR_SRS_MAX_CANDIDATES];
  for(unsigned d=0;d<sizeof(offsets)/sizeof(*offsets);d++) {
    memset(sr.freq,0,sr.grid.n_ant*stride*sizeof(c16_t));
    for(int a=0;a<sr.grid.n_ant;a++) for(int l=0;l<6;l++) {
      if(!(job->mask&(1u<<(l+8)))) continue;
      const c16_t *raw=job->raw+(a*6+l)*win+SRS_MARGIN+offsets[d];
      double step=-2*M_PI*job->fo_hz/sr.samples_per_second;
      double phase=step*(job->sample[l]+SRS_MARGIN+offsets[d]);
      for(int i=0;i<N;i++) {
        double co=cos(phase+step*i),si=sin(phase+step*i);
        rotated[i].r=lrint(raw[i].r*co-raw[i].i*si);
        rotated[i].i=lrint(raw[i].r*si+raw[i].i*co);
      }
      dft(get_dft(N),(int16_t *)rotated,(int16_t *)(sr.freq+a*stride+(l+8)*N),1);
    }
    int n=nr_srs_comb_detect(&sr.grid,(int16_t *)sr.freq,stride,job->mask,candidates);
    bool found=false;
    for(int i=0;i<n;i++) {
      nr_srs_candidate_t *c=&candidates[i];
      if(!nr_srs_group_cs_search(sr.search,(int16_t *)sr.freq,stride,c,NULL)) continue;
      int nre=nr_srs_estimate(sr.search,(int16_t *)sr.freq,stride,c,sr.cfr,sr.k,sr.l);
      if(!nre) continue;
      nr_srs_observe(sr.search,c,job->meta.abs_slot);
      /* Positive advance = arrival earlier than the DL-frame FFT window, same
       * convention as passive PUSCH: ta_new = ta_old - residual_delay. */
      job->meta.ta_est_samples=job->advance-offsets[d]-c->delay_samples;
      job->meta.n_ant=sr.export_ant;
      nr_srs_export(c,&job->meta,nre,sr.cfr,sr.k,sr.l);
      found=true;
    }
    if(found) break;
  }
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
    process_srs(job);
    pthread_mutex_lock(&sr.lock);sr.tail=(sr.tail+1)%SRS_DEPTH;sr.count--;pthread_mutex_unlock(&sr.lock);
  }
  return NULL;
}
static bool srs_selected(const char *sources)
{
  if(!sources) return true;
  while(*sources) {
    sources+=strspn(sources,", \t");size_t n=strcspn(sources,", \t");
    if((n==3 && !strncmp(sources,"srs",n)) || (n==3 && !strncmp(sources,"all",n))) return true;
    sources+=n;
  }
  return false;
}
void nr_srs_passive_start(PHY_VARS_NR_UE *ue)
{
  if(nr_srs_passive_enabled() || !nr_isac_enabled() || !ue || ue->frame_parms.frame_type!=TDD) return;
  char *sources=NULL;int max_re=46200,antennas=0;
  paramdef_t p[]={
    {.optname="sources",.type=TYPE_STRING,.strptr=&sources,.defstrval="all"},
    {.optname="cfr_max_re",.type=TYPE_INT,.iptr=&max_re,.defintval=46200},
    {.optname="cfr_antennas",.type=TYPE_INT,.iptr=&antennas,.defintval=0}};
  config_get(uniqCfg,p,3,"sensing");
  if(!srs_selected(sources)) return;
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
  sr.owner=ue;sr.stop=false;sr.head=sr.tail=sr.count=0;
  sr.samples_per_second=1000.0*fp->samples_per_subframe;
  sr.cfr_max_re=max_re;sr.export_ant=antennas==1?1:sr.grid.n_ant;
  if(!ok || pthread_create(&sr.thread,NULL,srs_worker,NULL)) {
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
  if(sr.count==SRS_DEPTH || fp->ofdm_symbol_size!=sr.grid.fft_size || fp->N_RB_UL!=sr.grid.n_rb
      || fp->nb_antennas_rx!=sr.grid.n_ant || fp->frame_type!=TDD || fp->ofdm_offset_divisor==0) {
    atomic_fetch_add(&sr.dropped,1);pthread_mutex_unlock(&sr.lock);return;
  }
  /* Process the preceding slot so all +/-400 windows are already in RX memory. */
  if(!abs_slot) {pthread_mutex_unlock(&sr.lock);return;}
  abs_slot--;if(--slot<0) {slot=fp->slots_per_frame-1;frame=(frame+1023)%1024;}
  uint16_t mask=0x3f00;
  const fapi_nr_tdd_table_t *t=&ue->nrUE_config.tdd_table;
  if(t->max_tdd_periodicity_list && t->tdd_period_in_slots) {
    const fapi_nr_max_tdd_periodicity_t *sl=&t->max_tdd_periodicity_list[slot%t->tdd_period_in_slots];
    for(int l=8;l<14;l++) if(sl->max_num_of_symbol_per_slot_list[l].slot_config==0) mask&=~(1u<<l);
  }
  if(!mask) {pthread_mutex_unlock(&sr.lock);return;}
  srs_job_t *job=&sr.jobs[sr.head];job->mask=mask;
  bool reconf=nr_cfg_reconf_enabled();uint64_t stamp=reconf?nr_cfg_epoch_cfr_stamp():0;
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  job->meta=(nr_srs_capture_t){.abs_slot=abs_slot,.frame=frame,.slot=slot,.pci=fp->Nid_cell,
    .t_mono_ns=(uint64_t)ts.tv_sec*1000000000+ts.tv_nsec,.reconf=reconf,.epoch=stamp>>32,.identity_gen=stamp,
    .n_ant=sr.export_ant,.cfr_max_re=sr.cfr_max_re};
  job->advance=(uint64_t)25600*fp->samples_per_subframe/(4096ull*480ull);
  job->fo_hz=ue->cont_fo_comp?ue->dl_Doppler_shift+ue->freq_offset:0;
  const int N=sr.grid.fft_size,win=N+2*SRS_MARGIN;
  for(int l=8;l<14;l++) {
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
