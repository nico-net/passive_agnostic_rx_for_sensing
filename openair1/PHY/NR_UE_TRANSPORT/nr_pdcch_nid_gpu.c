/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "common/utils/nr_bg_lane.h"
#include "common/utils/nr_core_layout.h"
#pragma weak nr_core_layout_active /* unit tests link this file without the utils library: no plan */
#pragma weak nr_core_layout_env_default
#include "nr_pdcch_nid_gpu.h"
#include "PHY/TOOLS/nr_admit.h"
#include "PHY/TOOLS/nr_gpu_budget.h"
#include "PHY/TOOLS/nr_gpu_elastic.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include <dlfcn.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* PER_CELL (mpci DBG2): one nID sweep (device window, batches, duty, published records) per cell. A process-wide instance served the
 * first cell's grid only, and a second cell's batches reset its duty and records on every geometry change (and its records reached the
 * other cell). Users run bound to the cell: the producer (CORESET map, slot path), the consumer feedback/records (PDCCH monitor), the
 * ACQMISS rearm (traffic probe worker); the worker is created bound to the cell that started it. */
typedef struct {
 pthread_mutex_t mu;
 pthread_cond_t cv;
 pthread_t thread;
 void *module, *device;
 const nr_pdcch_nid_ops_t *ops;
 nr_pdcch_nid_batch_t batch[3];
 int state[3]; /* 0 free, 1 producer, 2 full, 3 worker */
 uint64_t sequence[3], produced, generation, passes, dropped;
 nr_pdcch_nid_result_t result;
 int producer, n, rb, dur, window, batch_size;
 bool duty_enabled, all_verified;
 uint32_t epoch, rearm; /* rearm: ACQMISS recovery counter, folded into the batch cfg epoch so the device window and duty restart */
 uint64_t wake, last_outside_us;
 unsigned acquire_ms, maintain_ms, recheck_ms;
 bool stop;
 uint64_t runs; /* sweep GPU passes run */
 double kernel_ms;
 _Atomic bool running;
 nr_gpu_elastic_t el; /* on-demand window: base = what the memory budget share holds (T6b), full = the requested window */
 int win_passes;      /* passes since the window was (re)started: the window is full when this reaches el.cur */
} nr_nid_gpu_state_t;
NR_CELL_STATIC_INIT(nr_nid_gpu_state_t, g_nid, {.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER,.producer=-1});
#define g NR_CELL_REF(g_nid)

bool nr_pdcch_nid_gpu_enabled(void) {return true;}
bool nr_pdcch_nid_gpu_running(void) {return atomic_load_explicit(&g.running,memory_order_acquire);}
/* env_int: reads an integer environment variable, returning def when unset, empty, non-numeric or outside [lo, hi].
 *   How it works: strtol with end-pointer check and range test.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:212, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:213, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:215, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:221 (+3 more).
 */
static int env_int(const char *key,int def,int lo,int hi)
{
 const char *s=getenv(key);if(!s || !*s)return def;
 char *end;long n=strtol(s,&end,10);
 return *end || n<lo || n>hi?def:(int)n;
}
/* On-demand window (PHY/TOOLS/nr_gpu_elastic.h). The module holds the window its budget share allows (base); when the sweep is still ACQUIRING with a full window
 * (more evidence span is what is missing) it asks for one more batch per pass, up to the requested window, as far as the shared budget has headroom; once records are
 * published it gives the extra back after idle_limit ticks. A refusal or an allocation failure leaves the window as it is (one log, counted, back-off). Never runs with a
 * pending batch: called from the worker between passes. GB10 / a share that holds the full window: base == full, nothing here ever acts. */
static struct {int idle_limit, backoff;} elastic_policy={120,20};
void nr_pdcch_nid_gpu_test_elastic(int idle_limit,int backoff) {elastic_policy.idle_limit=idle_limit;elastic_policy.backoff=backoff;}
void nr_pdcch_nid_gpu_elastic_stats(int *window,int *base,uint64_t *grows,uint64_t *shrinks,uint64_t *denied)
{
 if(window)*window=g.el.cur;
 if(base)*base=g.el.base;
 if(grows)*grows=g.el.grows;
 if(shrinks)*shrinks=g.el.shrinks;
 if(denied)*denied=g.el.denied;
}
static void elastic_tick(int demand)
{
 if(!g.ops || !g.ops->resize || !nr_gpu_elastic_enabled(&g.el))return;
 const int cur=g.el.cur,want=nr_gpu_elastic_target(&g.el,demand);
 if(want==cur)return;
 const int now=g.ops->resize(g.device,want);
 const bool ok=now>0;
 if(nr_gpu_elastic_done(&g.el,want,ok?now:cur,ok))
  LOG_W(PHY,"SENSING: PDCCH_NID_GPU window %d -> %d refused (%s); keeping %d batches, further refusals are only counted\n",cur,want,ok?"memory budget headroom":"allocation failed",g.el.cur);
 if(ok && now!=cur) {
  g.win_passes=0;
  LOG_A(PHY,"SENSING: PDCCH_NID_GPU window %d -> %d batches (base %d, full %d; grows=%llu shrinks=%llu denied=%llu)\n",cur,now,g.el.base,g.el.full,
        (unsigned long long)g.el.grows,(unsigned long long)g.el.shrinks,(unsigned long long)g.el.denied);
 }
}
/* worker: the PDCCH NID GPU worker thread: takes a filled batch and decides whether to run it.
 *   How it works: Waits on g.cv for a batch in state 2 (lowest sequence). With duty on it runs the ACQUIRE/MAINTAIN/PAUSED duty state machine (nr_gpu_discovery_due) and frees skipped batches. A geometry change resets the duty and the published result. TODO(doc): the device-run and publish tail (after line 308) was not reviewed.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:60, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:70.
 */
static void *worker(void *unused)
{
 (void)unused;
 nr_bg_lane_enter("ISAC_PDCCH_NID_GPU_CORE");
 nr_pdcch_nid_config_t previous={0};
 bool have_config=false;
 nr_gpu_discovery_duty_t duty={0};
 uint64_t wake=0,admit_t0=0;
 int logged_mode=-1;
 for(;;) {
  pthread_mutex_lock(&g.mu);
  int b=-1;
  while(!g.stop) {
   for(int j=0;j<3;j++)if(g.state[j]==2 && (b<0 || g.sequence[j]<g.sequence[b]))b=j;
   if(b>=0)break;
   pthread_cond_wait(&g.cv,&g.mu);
  }
  if(g.stop) {pthread_mutex_unlock(&g.mu);break;}
  g.state[b]=3;
  const uint32_t epoch=g.epoch;
  const bool geometry_changed=have_config && memcmp(&previous,&g.batch[b].cfg,sizeof(previous));
  const bool unexplained=wake!=g.wake;
  if(geometry_changed)duty=(nr_gpu_discovery_duty_t){0};
  wake=g.wake;
  if(g.duty_enabled) {
   int due=nr_gpu_discovery_due(&duty,nr_gpu_budget_now_us(),epoch,
       !geometry_changed && g.result.count>0,!geometry_changed && g.all_verified,unexplained,1000ull*g.acquire_ms,
       1000ull*g.maintain_ms,1000ull*g.recheck_ms);
   nr_gpu_budget_discovery_prefer_cell(nr_cell_idx(),duty.mode==NR_GPU_DISC_ACQUIRE); /* OTATAIL: acquisition goes before CSI-RS discovery */
   if(logged_mode!=duty.mode) {
    static const char *names[]={"ACQUIRE","MAINTAIN","PAUSED"};
    LOG_A(PHY,"SENSING: PDCCH_NID_GPU mode %s -> %s epoch=%u published=%d verified=%d wake=%d\n",
          logged_mode<0?"START":names[logged_mode],names[duty.mode],epoch,g.result.count,g.all_verified,unexplained);
    logged_mode=duty.mode;
   }
   if(!due) {g.state[b]=0;pthread_mutex_unlock(&g.mu);elastic_tick(g.el.base);continue;} /* paused / maintaining: no extra evidence span needed */
  }
  if(nr_admit_on()) { /* ISAC_BG_LANE: a skipped batch is freed; the next one carries newer samples */
   const uint64_t t=nr_admit_now_ns();
   if(!nr_admit_try(NR_ADMIT_NID,t,0,false)) {g.state[b]=0;pthread_mutex_unlock(&g.mu);continue;}
   admit_t0=t;
  }
  pthread_mutex_unlock(&g.mu);
  bool changed=have_config && memcmp(&previous,&g.batch[b].cfg,sizeof(previous));
  if(!have_config || changed) {
   previous=g.batch[b].cfg;have_config=true;
   if(changed) {
    pthread_mutex_lock(&g.mu);
    memset(&g.result,0,sizeof(g.result));g.generation++;g.all_verified=false;
    pthread_mutex_unlock(&g.mu);
   }
  }
  int rc=g.ops->upload(g.device,&g.batch[b]);
  pthread_mutex_lock(&g.mu);
  g.state[b]=0;
  
  pthread_mutex_unlock(&g.mu);
  if(rc<0)goto failed;
  nr_pdcch_nid_result_t result={0};
  rc=g.ops->run(g.device,&result);
  if(admit_t0){nr_admit_done(NR_ADMIT_NID,nr_admit_now_ns()-admit_t0);admit_t0=0;}
  if(rc<0)goto failed;
  if(rc==0) {
   g.runs++;
   for(int k=0;k<NR_PDCCH_NID_KERNEL_KINDS;k++)g.kernel_ms+=result.kernel_total_ms[k];
   if((g.runs&7)==1)LOG_A(PHY,"SENSING: PDCCH_NID_GPU cumulative: sweep_runs=%llu kernel_ms=%.3f\n",
     (unsigned long long)g.runs,g.kernel_ms);
  }
  if(rc>0) {
   if(rc==2) LOG_W(PHY,"SENSING: PDCCH_NID_GPU pass budget exhausted; previous records retained\n");
   continue;
  }
  if(result.shortlist_overflow)LOG_W(PHY,"SENSING: PDCCH_NID_GPU shortlist overflow; no truncated evidence published\n");
  if(result.budget_exceeded)LOG_W(PHY,"SENSING: PDCCH_NID_GPU soft pass budget exceeded; completed records published\n");
  pthread_mutex_lock(&g.mu);
  if(epoch!=g.epoch) {pthread_mutex_unlock(&g.mu);continue;}
  bool same=g.result.count==result.count;
  for(int i=0;i<result.count && same;++i) {
   bool found=false;
   for(int j=0;j<g.result.count;++j)
    found|=!memcmp(result.records+i,g.result.records+j,offsetof(nr_pdcch_nid_record_t,ties));
   same=found;
  }
  g.result=result;if(!same || !g.duty_enabled) {g.generation++;g.all_verified=false;}g.passes++;
  if(g.duty_enabled && result.count && duty.mode==NR_GPU_DISC_ACQUIRE) {
   duty.published=1;duty.mode=NR_GPU_DISC_MAINTAIN;
   duty.next_us=nr_gpu_budget_now_us()+1000ull*g.maintain_ms;
   LOG_A(PHY,"SENSING: PDCCH_NID_GPU mode ACQUIRE -> MAINTAIN epoch=%u published=%d reason=publication\n",epoch,result.count);
   logged_mode=duty.mode;
  }
  uint64_t pass=g.passes;
  pthread_mutex_unlock(&g.mu);
  char records[2048];size_t used=0;records[0]=0;
  struct timespec pts;clock_gettime(CLOCK_MONOTONIC,&pts); /* OTATAIL: align passes with PDTIMSLOW t= */
  for(int j=0;j<result.count;j++) {
   const nr_pdcch_nid_record_t *r=result.records+j;
   int n=snprintf(records+used,sizeof(records)-used,
      " nid=%d sym=%d dur=%d grid_rb=%d groups=%d L=%d R=%d shift=%d ties=%d;",
      r->nid,r->sym,r->dur,r->grid_rb,r->groups,r->L,r->R,r->shift,r->ties);
   if(n<0 || (size_t)n>=sizeof(records)-used)break;
   used+=n;
  }
  LOG_A(PHY,"SENSING: PDCCH_NID_GPU pass %llu: stage1 %.3f ms stage2 %.3f ms t=%ld.%03ld (kernels max %.3f ms) -> %d CORESETs:%s\n",
        (unsigned long long)pass,result.stage1_ms,result.stage2_ms,(long)pts.tv_sec,pts.tv_nsec/1000000,result.kernel_max_ms,result.count,records);
  if(result.count>0)elastic_tick(g.el.base);                                  /* evidence found: the extra span is no longer needed */
  else {g.win_passes++;elastic_tick(g.win_passes>=g.el.cur?g.el.cur+1:g.el.cur);} /* still nothing with a full window: ask for one more batch of evidence span */
  continue;
failed:
  LOG_E(PHY,"SENSING: PDCCH_NID_GPU device failure; capture stopped (no CPU sweep fallback)\n");
  atomic_store_explicit(&g.running,false,memory_order_release);
  break;
 }
 nr_gpu_budget_discovery_prefer_cell(nr_cell_idx(),0);
 return NULL;
}
/* Policy of the sweep: 3-symbol evidence, 10-batch window of 64 snapshots, duty ACQUIRE 5 s / MAINTAIN 500 ms / recheck 250 ms. Only the unit test overrides it (before start()). */
static struct {int dur,window,batch_size;bool duty;unsigned acquire_ms,maintain_ms,recheck_ms;} policy={3,10,64,true,5000,500,250};
void nr_pdcch_nid_gpu_test_policy(int dur,int window,int batch_size,bool duty,unsigned acquire_ms,unsigned maintain_ms,unsigned recheck_ms)
{
 policy.dur=dur;policy.window=window;policy.batch_size=batch_size;policy.duty=duty;
 policy.acquire_ms=acquire_ms;policy.maintain_ms=maintain_ms;policy.recheck_ms=recheck_ms;
}
/* nr_pdcch_nid_gpu_start: loads the device module and starts the worker thread, pinned to ISAC_PDCCH_NID_GPU_CORE (default 13).
 *   How it works: dlopen libpdcch_nid_gpu.so, check the ABI, create the device and set the worker to SCHED_OTHER with an explicit schedule.
 *   Callers: executables/nr-uesoftmodem.c, tests/nr_pdcch_nid_worker_test.cc.
 */
int nr_pdcch_nid_gpu_start(int max_rb)
{
 if(!nr_pdcch_nid_gpu_enabled())return 0;
 if(g.device)return nr_pdcch_nid_gpu_running()?0:-1;
 if(max_rb<6 || max_rb>275)return -1;
 g.rb=max_rb;
 g.duty_enabled=policy.duty;
 g.acquire_ms=policy.acquire_ms;g.maintain_ms=policy.maintain_ms;g.recheck_ms=policy.recheck_ms;
 g.dur=policy.dur;g.batch_size=policy.batch_size;g.window=policy.window;
 g.module=dlopen("libpdcch_nid_gpu.so",RTLD_NOW|RTLD_LOCAL);
 if(!g.module)goto fail;
 const nr_pdcch_nid_ops_t *(*get_ops)(void)=dlsym(g.module,"nr_pdcch_nid_gpu_ops");
 if(!get_ops || !(g.ops=get_ops()) || g.ops->abi!=NR_PDCCH_NID_ABI)goto fail;
 nr_pdcch_nid_iq_t *staging=NULL;
 g.device=g.ops->create(max_rb,g.dur,g.window,&staging);
 if(!g.device)goto fail;
 for(int j=0;j<3;j++)g.batch[j].iq=staging+(size_t)j*NR_PDCCH_NID_BATCH*g.dur*12*max_rb;
 {
  const int base=g.ops->resize?g.ops->resize(g.device,0):g.window;
  nr_gpu_elastic_init(&g.el,base>0?base:g.window,g.window,1,elastic_policy.idle_limit,elastic_policy.backoff);
  g.win_passes=0;
  if(nr_gpu_elastic_enabled(&g.el))
   LOG_A(PHY,"SENSING: PDCCH_NID_GPU on-demand window: base %d of %d batches (the budget share holds %d); grows from the budget headroom while ACQUIRE has a full window and no record\n",g.el.base,g.el.full,g.el.base);
 }
 // Explicit policy prevents inheriting FIFO from receiver initialization.
 pthread_attr_t attr;
 pthread_attr_init(&attr);
 struct sched_param sp={.sched_priority=0};
 int rc=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
 rc|=pthread_attr_setschedpolicy(&attr,SCHED_OTHER);
 rc|=pthread_attr_setschedparam(&attr,&sp);
 cpu_set_t allowed,pin;CPU_ZERO(&allowed);CPU_ZERO(&pin);
 if(sched_getaffinity(0,sizeof(allowed),&allowed)!=0)rc=-1;
 // ISAC_PDCCH_NID_GPU_CORE: exported by the core plan (nr_core_layout.h); -1 = the highest CPU of this thread's mask. Unset with an active plan: the plan's nID role
 // (nr_core_layout_env_default, same value the export would have set); unset with ISAC_CORE_PLAN=0 (no plan): legacy 13.
 int core_def=13;
 if(nr_core_layout_active&&nr_core_layout_env_default&&nr_core_layout_active()){
  const char*pv=nr_core_layout_env_default("ISAC_PDCCH_NID_GPU_CORE");
  core_def=pv&&*pv?atoi(pv):-1;
  if(core_def<-1||core_def>=CPU_SETSIZE)core_def=-1;
 }
 int core=env_int("ISAC_PDCCH_NID_GPU_CORE",core_def,-1,CPU_SETSIZE-1);
 if(core<0)for(int j=CPU_SETSIZE-1;j>=0;j--)if(CPU_ISSET(j,&allowed)){core=j;break;}
 if(core<0 || !CPU_ISSET(core,&allowed))rc=-1;
 else if(nr_cell_idx()>0 && !getenv("ISAC_PDCCH_NID_GPU_CORE")) {
  /* G4: the nID core of the plan serves cell 0. Another cell's worker floats over the thread's allowed set (SCHED_OTHER, low duty) instead of piling onto the same single core;
   * an explicit ISAC_PDCCH_NID_GPU_CORE still pins every worker (operator choice). The core pool (B1b/B5) can lease a per-cell core on top. */
  rc|=pthread_attr_setaffinity_np(&attr,sizeof(allowed),&allowed);core=-1;
 } else {CPU_SET(core,&pin);rc|=pthread_attr_setaffinity_np(&attr,sizeof(pin),&pin);}
 if(!rc)rc=nr_cell_thread_create(&g.thread,&attr,worker,NULL,nr_cell_idx()); /* the worker serves the starting cell */
 pthread_attr_destroy(&attr);
 if(rc)goto fail;
 atomic_store_explicit(&g.running,true,memory_order_release);
 LOG_I(PHY,"SENSING: PDCCH_NID_GPU ready: core=%d SCHED_OTHER window=%d dur=%d batch=%d pinned staging=3 batches\n",
       core,g.el.cur,g.dur,g.batch_size);
 {struct timespec t0;clock_gettime(CLOCK_MONOTONIC,&t0);LOG_A(PHY,"SENSING: PDCCH_NID_GPU t0 t=%ld.%03ld\n",(long)t0.tv_sec,t0.tv_nsec/1000000);}
 return 0;
fail:
 if(g.device)g.ops->destroy(g.device);
 if(g.module)dlclose(g.module);
 g.device=g.module=NULL;g.ops=NULL;
 LOG_E(PHY,"SENSING: PDCCH_NID_GPU init failed; capture disabled\n");
 return -1;
}
/* nr_pdcch_nid_gpu_stop: stops the worker and releases the device.
 *   How it works: Clears running, sets stop and joins the thread, destroys the device, dlcloses the module and resets the batch state and result.
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-uesoftmodem.c:631, executables/nr-uesoftmodem.c:743, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:60, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:92 (+2 more).
 */
void nr_pdcch_nid_gpu_stop(void)
{
 if(!g.device)return;
 atomic_store_explicit(&g.running,false,memory_order_release);
 LOG_A(PHY,"SENSING: PDCCH_NID_GPU stop summary: sweep_runs=%llu kernel_ms=%.3f window=%d base=%d full=%d grows=%llu shrinks=%llu denied=%llu\n",
   (unsigned long long)g.runs,g.kernel_ms,g.el.cur,g.el.base,g.el.full,(unsigned long long)g.el.grows,(unsigned long long)g.el.shrinks,(unsigned long long)g.el.denied);
 pthread_mutex_lock(&g.mu);g.stop=true;pthread_cond_signal(&g.cv);pthread_mutex_unlock(&g.mu);
 pthread_join(g.thread,NULL);
 g.ops->destroy(g.device);dlclose(g.module);g.device=g.module=NULL;g.ops=NULL;
 pthread_mutex_lock(&g.mu);
 memset(g.state,0,sizeof(g.state));g.producer=-1;g.n=0;g.stop=false;
 g.result.count=0;g.generation++;
 pthread_mutex_unlock(&g.mu);
}
/* nr_pdcch_nid_gpu_push: producer entry, called per RX symbol: validates the geometry and copies the symbol IQ into the current batch.
 *   How it works: Rejects bad geometry (nsc, sps, slot, FFT offset, RB count). Takes the mutex (trylock) and a free batch. Copies dur symbols per candidate with the cyclic FFT offset, zero-fills past sps, and marks the batch ready (state 2) at batch_size.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:526, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:77, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:65, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:52 (+1 more).
 */
int nr_pdcch_nid_gpu_push(const void *rx,int fft,int fco,int slot,int sym,const nr_pdcch_nid_config_t *cfg)
{
 if(!nr_pdcch_nid_gpu_running() || !rx || !cfg || cfg->dur!=g.dur || cfg->nsc<72
    || cfg->nsc>12*g.rb || cfg->nsc>fft || fco<0 || fco>=fft || cfg->sps<1 || cfg->sps>14
    || sym<0 || sym>=cfg->sps || slot<0 || slot>=160
    || (cfg->nsc-cfg->point_a_sc)/12>275
    || (cfg->nsc-cfg->point_a_sc)/12-(cfg->point_a_sc<0?(-cfg->point_a_sc+11)/12:0)<6)return -1;
 if(pthread_mutex_trylock(&g.mu))return -1;
 if(g.producer<0) {
  for(int j=0;j<3;j++)if(g.state[j]==0){g.producer=j;g.state[j]=1;g.n=0;break;}
 }
 if(g.producer<0) {g.dropped++;pthread_mutex_unlock(&g.mu);return -1;}
 nr_pdcch_nid_batch_t *b=&g.batch[g.producer];
 nr_pdcch_nid_config_t current=*cfg;current.epoch=g.epoch+(g.rearm<<24);
 if(g.n && memcmp(&b->cfg,&current,sizeof(current)))g.n=0;
 b->cfg=current;
 const nr_pdcch_nid_iq_t *src=rx;
 for(int d=0;d<g.dur;d++) {
  nr_pdcch_nid_iq_t *dst=b->iq+((size_t)g.n*g.dur+d)*12*g.rb;
  if(sym+d<cfg->sps)for(int i=0;i<cfg->nsc;i++)dst[i]=src[(size_t)d*fft+(fco+i)%fft];
  else memset(dst,0,cfg->nsc*sizeof(*dst));
 }
 b->slot[g.n]=slot;b->sym[g.n]=sym;
 if(++g.n==g.batch_size) {
  b->count=g.batch_size;
  g.state[g.producer]=2;g.sequence[g.producer]=++g.produced;g.producer=-1;g.n=0;
  pthread_cond_signal(&g.cv);
 }
 pthread_mutex_unlock(&g.mu);return 0;
}
/* nr_pdcch_nid_gpu_rearm: ACQMISS recovery. Forgets the published records and the incremental window and restarts the ACQUIRE duty (GPU priority over CSI-RS discovery) by changing the batch cfg epoch. No-op when the sweep is not running. Caller: nr_dl_traffic_probe_rt.c . */
void nr_pdcch_nid_gpu_rearm(void)
{
 if(!nr_pdcch_nid_gpu_running())return;
 pthread_mutex_lock(&g.mu);
 g.rearm++;g.result.count=0;g.generation++;g.all_verified=false;
 pthread_mutex_unlock(&g.mu);
}
/* nr_pdcch_nid_gpu_passes: completed sweep passes (0 when the lock is contended). Caller: ACQMISS state line. */
uint64_t nr_pdcch_nid_gpu_passes(void)
{
 if(pthread_mutex_trylock(&g.mu))return 0;
 uint64_t n=g.passes;pthread_mutex_unlock(&g.mu);return n;
}
int nr_pdcch_nid_gpu_records(nr_pdcch_nid_record_t *out,int capacity,uint64_t *generation)
{
 if(!nr_pdcch_nid_gpu_enabled() || !out || capacity<1 || !generation || pthread_mutex_trylock(&g.mu))return -1;
 int n=-1;
 if(*generation!=g.generation) {
  n=g.result.count<capacity?g.result.count:capacity;
  memcpy(out,g.result.records,n*sizeof(*out));*generation=g.generation;
 }
 pthread_mutex_unlock(&g.mu);return n;
}

int nr_pdcch_nid_gpu_snapshot(nr_pdcch_nid_record_t *out,int cap,uint64_t *generation)
{
 if(!out || cap<1 || !generation || pthread_mutex_trylock(&g.mu))return -1;
 int n=g.result.count<cap?g.result.count:cap;
 memcpy(out,g.result.records,n*sizeof(*out));*generation=g.generation;
 pthread_mutex_unlock(&g.mu);return n;
}
/* nr_pdcch_nid_gpu_feedback: consumer feedback from the PDCCH monitor: epoch, generation, verified and unexplained-occupancy flags.
 *   How it works: Ignored unless DUTY is on. A new epoch clears the published result and the queued snapshots. The verified flag is applied only for the current generation. For unexplained occupancy it bumps the wake edge once per episode (rearmed after a quiet MAINTAIN interval or an epoch change).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1405, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4518, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:54, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:101 (+6 more).
 */
void nr_pdcch_nid_gpu_feedback(uint32_t epoch,uint64_t generation,bool verified,bool unexplained)
{
 if(!g.duty_enabled)return;
 if(pthread_mutex_trylock(&g.mu))return;
 if(g.epoch!=epoch) {
  g.epoch=epoch;g.result.count=0;g.generation++;g.all_verified=false;g.last_outside_us=0;
  /* Drop queued old-epoch snapshots, but never reclaim the worker's buffer. */
  for(int i=0;i<3;++i)if(g.state[i]==2)g.state[i]=0;
  if(g.producer>=0)g.n=0;
 }
 if(generation==g.generation)g.all_verified=verified;
 if(unexplained) {
  uint64_t now=nr_gpu_budget_now_us();
  // The monitor reports a level on each occupied occasion. Reopen once per
  // episode, then permit bank acknowledgement to pause again. A quiet
  // MAINTAIN interval rearms the edge; a changed epoch always rearms it.
  if(!g.last_outside_us || now-g.last_outside_us>=1000ull*g.maintain_ms) {
   g.wake++;g.all_verified=false;
  }
  g.last_outside_us=now;
 }
 pthread_mutex_unlock(&g.mu);
}
