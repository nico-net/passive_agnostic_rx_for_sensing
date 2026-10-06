/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_td.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
configmodule_interface_t *uniqCfg;
void exit_function(const char *file,const char *function,int line,const char *message,int fatal) { abort(); }
static PHY_VARS_NR_UE ue;
static int parity_tolerance;
static void setup(int mu, int rb)
{
  static bool initialized;
  if (!initialized) { logInit(); load_dftslib(); InitSinLUT(); set_glog(OAILOG_ERR); initialized=true; }
  memset(&ue,0,sizeof(ue));
  NR_DL_FRAME_PARMS *fp=&ue.frame_parms;
  fp->numerology_index=mu;fp->slots_per_subframe=1<<mu;fp->slots_per_frame=10<<mu;
  fp->ofdm_symbol_size=4096;fp->nb_prefix_samples=288;fp->nb_prefix_samples0=288+(64<<mu);
  fp->symbols_per_slot=14;fp->samples_per_slot_wCP=14*4096;
  fp->samples_per_slotN0=14*(4096+288);
  fp->samples_per_slot0=fp->samples_per_slotN0+fp->nb_prefix_samples0-288;
  fp->samples_per_subframe=fp->samples_per_slotN0*(1<<mu)+2*(fp->nb_prefix_samples0-288);
  fp->samples_per_frame=10*fp->samples_per_subframe;
  fp->nb_antennas_rx=4;fp->N_RB_DL=rb;fp->first_carrier_offset=4096-6*rb;fp->ofdm_offset_divisor=8;
  for(int l=0;l<14*(1<<mu);++l)
    fp->symbol_rotation[0][l]=(c16_t){lrint(32767*cos(.13*l)),lrint(32767*sin(.13*l))};
  for(int k=0;k<4096;++k)
    fp->timeshift_symbol_rotation[k]=(c16_t){lrint(32767*cos(2*M_PI*k*36/4096)),lrint(32767*sin(2*M_PI*k*36/4096))};
  ue.common_vars.rxdata=calloc(4,sizeof(c16_t *));
  uint32_t rng=123;
  for(int a=0;a<4;++a) {
    ue.common_vars.rxdata[a]=aligned_alloc(64,2*fp->samples_per_frame*sizeof(c16_t));
    for(int k=0;k<2*fp->samples_per_frame;++k) {
      rng=rng*1664525+1013904223;ue.common_vars.rxdata[a][k].r=(rng>>22)-512;
      rng=rng*1664525+1013904223;ue.common_vars.rxdata[a][k].i=(rng>>22)-512;
    }
  }
}
static void cleanup(void)
{
  nr_csirs_disc_td_free();
  for(int a=0;a<4;++a) free(ue.common_vars.rxdata[a]);
  free(ue.common_vars.rxdata);
}
int csirs_td_parity(int mu, int rb, int sync, int cfo)
{
  setup(mu,rb);
  NR_DL_FRAME_PARMS *fp=&ue.frame_parms;
  ue.is_synchronized=sync;ue.cont_fo_comp=cfo;ue.freq_offset=4100;
  nr_slot_fep_fo_override_hz=cfo==2 ? -721.25 : NAN;
  nr_csirs_disc_ring_t actual={0},expected={0};
  int rc=nr_csirs_disc_td_init(fp,4,16);
  rc|=nr_csirs_disc_ring_init(&actual,4,rb,fp->slots_per_frame,16,parity_tolerance>0);
  rc|=nr_csirs_disc_ring_init(&expected,4,rb,fp->slots_per_frame,16,false);
  c16_t (*grid)[fp->samples_per_slot_wCP]=aligned_alloc(64,4*fp->samples_per_slot_wCP*sizeof(c16_t));
  const int16_t *ants[4];for(int a=0;a<4;++a) ants[a]=(const int16_t *)grid[a];
  int slots[]={0,1,2,3,fp->slots_per_frame-1};
  for(int i=0;i<5 && !rc;++i) {
    int slot=slots[i];
    for(int l=0;l<14;++l) nr_slot_fep(&ue,fp,slot,l,grid,link_type_dl,0,ue.common_vars.rxdata);
    rc=nr_csirs_disc_ring_push(&expected,ants,4096,fp->first_carrier_offset,slot,0,slot)<0;
    rc|=nr_csirs_disc_td_push(&ue,slot,0,slot);
  }
  /* Prove that deferred processing does not read capture-time mutable state. */
  ue.freq_offset=-2000;nr_slot_fep_fo_override_hz=55;
  for(int a=0;a<4;++a) memset(ue.common_vars.rxdata[a],0,2*fp->samples_per_frame*sizeof(c16_t));
  uint64_t gpu_before=csirs_disc_fep_gpu_chunks;
  if(nr_csirs_disc_td_drain(&actual)!=5) rc=-2;
  if(parity_tolerance && csirs_disc_fep_gpu_chunks!=gpu_before+1) rc=-5;
  int maxerr=0;double mse=0;
  for(size_t k=0;k<5*actual.slot_stride;++k) {
    int error=abs((int)actual.grid_base[k]-expected.grid_base[k]);
    if(error>maxerr) maxerr=error;
    mse+=(double)error*error;
  }
  double rms=sqrt(mse/(5*actual.slot_stride));
  if(parity_tolerance) printf("T9 FEP mu=%d rb=%d sync=%d cfo=%d max=%d LSB RMS=%.4f LSB\n",mu,rb,sync,cfo,maxerr,rms);
  if(maxerr>parity_tolerance || (parity_tolerance && rms>16)) rc=-3;
  if(!rc && memcmp(actual.meta,expected.meta,5*sizeof(*actual.meta))) rc=-4;
  free(grid);nr_csirs_disc_ring_free(&actual);nr_csirs_disc_ring_free(&expected);cleanup();
  nr_slot_fep_fo_override_hz=NAN;
  return rc;
}
int csirs_td_overflow_reset(void)
{
  setup(1,217);
  int rc=nr_csirs_disc_td_init(&ue.frame_parms,4,2);
  uint64_t drops=csirs_disc_feed_drops;
  rc|=nr_csirs_disc_td_push(&ue,1,0,1);rc|=nr_csirs_disc_td_push(&ue,2,0,2);
  if(nr_csirs_disc_td_push(&ue,3,0,3)!=-1 || csirs_disc_feed_drops!=drops+1) rc=-2;
  nr_csirs_disc_td_reset();
  if(nr_csirs_disc_td_pending()!=0) rc=-3;
  rc|=nr_csirs_disc_td_push(&ue,4,0,4);
  nr_csirs_disc_ring_t r={0};nr_csirs_disc_ring_init(&r,4,217,20,2,false);
  if(nr_csirs_disc_td_drain(&r)!=1 || r.meta[0].abs_slot!=4) rc=-4;
  nr_csirs_disc_ring_free(&r);cleanup();return rc;
}
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1000.; }
double csirs_td_benchmark(void)
{
  setup(1,217);ue.is_synchronized=true;
  if(nr_csirs_disc_td_init(&ue.frame_parms,4,1024)) return -1;
  double total=0;
  for(int round=0;round<10;++round) {
    nr_csirs_disc_td_reset();
    double start=now();
    for(int i=0;i<1024;++i) if(nr_csirs_disc_td_push(&ue,i,i/20,i%20)) abort();
    total+=now()-start;
  }
  cleanup();return total/10240;
}
/* GPU parity uses the same full production TD->FD conversion, with the CPU
 * reference ring generated by nr_slot_fep. CPU equality above remains exact. */
void csirs_td_test_init(void) { setup(1,217);cleanup(); }

int csirs_td_gpu_parity(void)
{
  parity_tolerance=64; /* two Q15 rotations after a <=32-LSB FFT tolerance */
  int rc=0;
  for(int mu=0;mu<3 && !rc;++mu) for(int rb=216;rb<=217 && !rc;++rb)
    for(int sync=0;sync<2 && !rc;++sync) for(int cfo=0;cfo<3 && !rc;++cfo) rc=csirs_td_parity(mu,rb,sync,cfo);
  parity_tolerance=0;
  return rc;
}

/* Pause inside the actual worker FFT, then fill/reset the producer queue. This
 * would deadlock if FFT computation held the actor's queue lock. */
#include <pthread.h>
static pthread_mutex_t fft_gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fft_gate_cv=PTHREAD_COND_INITIALIZER;
static bool fft_entered, fft_release;
static dftfunc_t saved_dft;
static void gated_dft(uint8_t size, int16_t *in, int16_t *out, unsigned char scale)
{
  pthread_mutex_lock(&fft_gate_lock);
  fft_entered=true;
  pthread_cond_signal(&fft_gate_cv);
  while(!fft_release) pthread_cond_wait(&fft_gate_cv,&fft_gate_lock);
  pthread_mutex_unlock(&fft_gate_lock);
  saved_dft(size,in,out,scale);
}
static void *drain_thread(void *ring)
{
  return (void *)(intptr_t)nr_csirs_disc_td_drain(ring);
}
int csirs_td_concurrent_reset(void)
{
  setup(1,217);
  int rc=nr_csirs_disc_td_init(&ue.frame_parms,4,4);
  nr_csirs_disc_ring_t ring={0};
  rc|=nr_csirs_disc_ring_init(&ring,4,217,20,4,false);
  rc|=nr_csirs_disc_td_push(&ue,1,0,1);
  if(rc) {nr_csirs_disc_ring_free(&ring);cleanup();return -1;}
  fft_entered=fft_release=false;saved_dft=dft;dft=gated_dft;
  pthread_t worker;
  if(pthread_create(&worker,NULL,drain_thread,&ring)) abort();
  pthread_mutex_lock(&fft_gate_lock);
  while(!fft_entered) pthread_cond_wait(&fft_gate_cv,&fft_gate_lock);
  pthread_mutex_unlock(&fft_gate_lock);
  for(int slot=2;slot<6;++slot) rc|=nr_csirs_disc_td_push(&ue,slot,0,slot);
  nr_csirs_disc_td_reset(); // Must finish even while the FFT is paused.
  rc|=nr_csirs_disc_td_push(&ue,9,0,9);
  pthread_mutex_lock(&fft_gate_lock);fft_release=true;
  pthread_cond_signal(&fft_gate_cv);pthread_mutex_unlock(&fft_gate_lock);
  void *result;pthread_join(worker,&result);dft=saved_dft;
  if(result || ring.written) rc=-2; // The in-flight old epoch was discarded.
  if(nr_csirs_disc_td_drain(&ring)!=1 || ring.meta[0].abs_slot!=9) rc=-3;
  nr_csirs_disc_ring_free(&ring);cleanup();return rc;
}

/* Same production drain and detector as live reception. The producer offers
 * one owned TD slot every 500 us, with CFO enabled, for eleven seconds. */
static uint64_t bench_batches,bench_slots,bench_max_us,bench_work_us,bench_last;
static uint64_t bench_detect_max_us,bench_fep_last,bench_batch_max_us;
static void bench_process(nr_csirs_disc_ring_t *r)
{
  double start=now();
  nr_csirs_disc_metrics_t before,after;
  nr_csirs_disc_get_metrics(&before);
  nr_csirs_disc_process(r);
  nr_csirs_disc_get_metrics(&after);
  uint64_t elapsed=now()-start,slots=r->written-bench_last;
  if(elapsed>bench_max_us) bench_max_us=elapsed;
  uint64_t normalized=slots?(after.detect_us-before.detect_us)*100/slots:0;
  if(normalized>bench_detect_max_us) bench_detect_max_us=normalized;
  uint64_t fep=__atomic_load_n(&csirs_disc_fep_us,__ATOMIC_RELAXED);
  if(elapsed+fep-bench_fep_last>bench_batch_max_us) bench_batch_max_us=elapsed+fep-bench_fep_last;
  bench_fep_last=fep;
  bench_work_us+=elapsed;bench_slots+=slots;bench_last=r->written;
  __atomic_add_fetch(&bench_batches,1,__ATOMIC_RELEASE);
}
static bool bench_producer_done;
static nr_csirs_disc_ring_t bench_ring;
static void *bench_consume(void *unused)
{
  (void)unused;
  for(;;) {
    bool done=__atomic_load_n(&bench_producer_done,__ATOMIC_ACQUIRE);
    int pending=nr_csirs_disc_td_pending();
    if(done && !pending) break;
    if(pending>=100 || (done && pending)) {
      nr_csirs_disc_td_drain(&bench_ring);
      bench_process(&bench_ring);
    } else {
      struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    }
  }
  return NULL;
}
int csirs_td_throughput(void)
{
  setup(1,217);ue.is_synchronized=true;ue.cont_fo_comp=true;ue.freq_offset=4100;
  uint64_t fep0=csirs_disc_fep_us,gpu0=csirs_disc_fep_gpu_chunks,cpu0=csirs_disc_fep_cpu_chunks;
  uint64_t drops0=csirs_disc_feed_drops,over0=csirs_disc_td_overflows;
  uint64_t detect_gpu0=csirs_disc_gpu_detect_batches,detect_cpu0=csirs_disc_cpu_detect_batches;
  uint64_t lattice_cpu0=csirs_disc_cpu_lattice_calls;
  bench_batches=bench_slots=bench_last=bench_work_us=bench_max_us=bench_detect_max_us=0;
  bench_fep_last=csirs_disc_fep_us;bench_batch_max_us=0;
  nr_csirs_disc_detect_reset();
  nr_csirs_disc_pipeline_init(&ue.frame_parms);
  if(nr_csirs_disc_td_init(&ue.frame_parms,4,1024) || nr_csirs_disc_ring_init(&bench_ring,4,217,20,640,true)) {
    cleanup();return -1;
  }
  bench_producer_done=false;
  pthread_t consumer;
  if(pthread_create(&consumer,NULL,bench_consume,NULL)) abort();
  const int slots=22000;
  double push_us=0,start=now();
  int lost=0;
  for(int i=0;i<slots;++i) {
    double deadline=start+500.*i;
    struct timespec ts={(time_t)(deadline/1e6),(long)(fmod(deadline,1e6)*1000)};
    clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,NULL);
    double push_start=now();lost+=nr_csirs_disc_td_push(&ue,i,i/20%1024,i%20)<0;
    push_us+=now()-push_start;
  }
  double producer_end=now();
  __atomic_store_n(&bench_producer_done,true,__ATOMIC_RELEASE);
  pthread_join(consumer,NULL); // final partial batch uses the same drain + detector
  double end=now();
  uint64_t fep=csirs_disc_fep_us-fep0;
  double service=bench_slots*1e6/(fep+bench_work_us);
  nr_csirs_disc_metrics_t metrics;nr_csirs_disc_get_metrics(&metrics);
  printf("T9 PIPELINE offered=%d duration=%.3fs delivered=%llu wall_rate=%.1f service_rate=%.1f slots/s tail=%.3fms "
         "push=%.2f fep=%.2f detect=%.2f hyp=%.2f sweep=%.2f us/slot "
         "worst_batch=%.3fms worst_process=%.3fms worst_fep=%.3fms detect100_max=%.3fms drops=%d td_overflows=%llu "
         "gpu_fep=%llu cpu_fep=%llu gpu_detect=%llu cpu_detect=%llu\n",
         slots,(producer_end-start)/1e6,(unsigned long long)bench_slots,bench_slots*1e6/(end-start),service,
         (end-producer_end)/1000,push_us/slots,(double)fep/slots,(double)metrics.detect_us/slots,
         (double)metrics.hyp_us/slots,(double)metrics.sweep_us/slots,bench_batch_max_us/1000.,bench_max_us/1000.,csirs_disc_fep_max_us/1000.,
         bench_detect_max_us/1000.,lost,(unsigned long long)(csirs_disc_td_overflows-over0),
         (unsigned long long)(csirs_disc_fep_gpu_chunks-gpu0),(unsigned long long)(csirs_disc_fep_cpu_chunks-cpu0),
         (unsigned long long)(csirs_disc_gpu_detect_batches-detect_gpu0),(unsigned long long)(csirs_disc_cpu_detect_batches-detect_cpu0));
  int rc=lost || bench_slots!=slots || csirs_disc_feed_drops!=drops0 || csirs_disc_td_overflows!=over0
      || csirs_disc_fep_cpu_chunks!=cpu0 || csirs_disc_cpu_detect_batches!=detect_cpu0
      || csirs_disc_cpu_lattice_calls!=lattice_cpu0
      || csirs_disc_fep_gpu_chunks==gpu0 || csirs_disc_gpu_detect_batches==detect_gpu0
      || service<2000 || bench_detect_max_us>=25000 || end-producer_end>100000;
  nr_csirs_disc_ring_free(&bench_ring);
  nr_csirs_disc_detect_reset();cleanup();return rc?-2:0;
}
