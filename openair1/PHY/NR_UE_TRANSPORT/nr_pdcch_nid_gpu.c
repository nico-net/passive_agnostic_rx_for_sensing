/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_pdcch_nid_gpu.h"
#include "PHY/TOOLS/nr_gpu_budget.h"
#include "common/utils/LOG/log.h"
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

static struct {
 pthread_mutex_t mu;
 pthread_cond_t cv;
 pthread_cond_t cv_free; /* ISAC_PDCCH_NID_GPU_NODROP: a staging batch was freed; CLOCK_MONOTONIC (initialised in start()) */
 pthread_t thread;
 void *module, *device;
 const nr_pdcch_nid_ops_t *ops;
 nr_pdcch_nid_batch_t batch[3];
 int state[3]; /* 0 free, 1 producer, 2 full, 3 worker */
 uint64_t sequence[3], produced, generation, passes, dropped;
 nr_pdcch_nid_result_t result;
 int producer, n, rb, dur, every, window, batch_size;
 bool incremental;
 bool duty_enabled, all_verified;
 /* ISAC_PDCCH_NID_GPU_NODROP state. nodrop is set in start(). The push-side fields below are touched by the single producer thread only;
  * slot_ticks is read by the worker and the feedback caller. */
 bool nodrop, snap_started, snap_refused;
 int snap_slot;
 _Atomic uint64_t slot_ticks, dropped_snapshots, dropped_partial;
 uint64_t clock_us[3]; /* duty clock stamped on each batch when it is queued */
 uint32_t epoch;
 uint64_t wake, last_outside_us;
 unsigned acquire_ms, maintain_ms, recheck_ms;
 bool stop;
 _Atomic bool running;
} g = {.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER,.producer=-1};

/* nr_pdcch_nid_gpu_enabled: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_PDCCH_NID_GPU.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:983, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1166, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1422, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1423 (+8 more).
 */
bool nr_pdcch_nid_gpu_enabled(void)
{
 const char *e=getenv("ISAC_PDCCH_NID_GPU");
 return e && strcmp(e,"1")==0;
}
/* nr_pdcch_nid_gpu_running: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1394, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:436, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:207, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:289 (+4 more).
 */
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
/* NODROP_WAIT_NS: the longest the real-time producer may wait for a free staging batch or the mutex: one slot at 30 kHz SCS. */
#define NODROP_WAIT_NS 500000L
#define NODROP_SLOT_US 500ull
/* duty_clock_us: the duty-cycle clock. Flag off: wall-clock budget time. NODROP: 500 us per pushed slot, so identical input gives an identical
 *   duty schedule. batch_clock is the value stamped on the batch being handled (nodrop only). Caller: worker(); feedback reads slot_clock_us() directly. */
static uint64_t duty_clock_us(uint64_t batch_clock)
{
 if(!g.nodrop)return nr_gpu_budget_now_us();
 return batch_clock;
}
/* slot_clock_us: no-drop mode clock: the slot tick counter converted to microseconds.
 *   How it works: (slot_ticks + 1) * NODROP_SLOT_US, relaxed atomic load.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:60, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:326, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:362.
 */
static uint64_t slot_clock_us(void)
{
 return (atomic_load_explicit(&g.slot_ticks,memory_order_relaxed)+1)*NODROP_SLOT_US;
}
/* batch_freed: called with g.mu held after a staging batch returned to state 0; wakes a producer waiting in nodrop_acquire(). Callers: worker(), feedback. */
static void batch_freed(void)
{
 if(g.nodrop)pthread_cond_signal(&g.cv_free);
}
/* nodrop_acquire: bounded (<= NODROP_WAIT_NS in total) replacement for trylock + "all batches busy" drop. Takes g.mu with trylock polling
 *   (never a blocking lock), then, if no producer batch is open, waits on cv_free with a CLOCK_MONOTONIC deadline for the worker to free one.
 *   Returns true with g.mu held and g.producer >= 0; false with g.mu released. Caller: nr_pdcch_nid_gpu_push() (real-time path). */
static bool nodrop_acquire(void)
{
 struct timespec dl,nap={0,10000};
 clock_gettime(CLOCK_MONOTONIC,&dl);
 dl.tv_nsec+=NODROP_WAIT_NS;
 if(dl.tv_nsec>=1000000000L) {dl.tv_sec++;dl.tv_nsec-=1000000000L;}
 while(pthread_mutex_trylock(&g.mu)) {
  struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
  if(now.tv_sec>dl.tv_sec || (now.tv_sec==dl.tv_sec && now.tv_nsec>=dl.tv_nsec))return false;
  nanosleep(&nap,NULL);
 }
 bool timed_out=false;
 for(;;) {
  if(g.producer<0)for(int j=0;j<3;j++)if(g.state[j]==0){g.producer=j;g.state[j]=1;g.n=0;break;}
  if(g.producer>=0)return true;
  if(timed_out) {pthread_mutex_unlock(&g.mu);return false;}
  timed_out=pthread_cond_timedwait(&g.cv_free,&g.mu,&dl)==ETIMEDOUT;
 }
}
/* worker: the PDCCH NID GPU worker thread: takes a filled batch and decides whether to run it.
 *   How it works: Waits on g.cv for a batch in state 2 (lowest sequence). With ISAC_PDCCH_NID_GPU_DUTY=1 it runs the ACQUIRE/MAINTAIN/PAUSED duty state machine (nr_gpu_discovery_due) and frees skipped batches. A geometry change resets the duty and the published result. TODO(doc): the device-run and publish tail (after line 308) was not reviewed.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:60, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:70.
 */
static void *worker(void *unused)
{
 (void)unused;
 unsigned batches=0;
 nr_pdcch_nid_config_t previous={0};
 bool have_config=false;
 nr_gpu_discovery_duty_t duty={0};
 uint64_t wake=0;
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
  const uint64_t batch_clock=g.clock_us[b];
  const uint32_t epoch=g.epoch;
  const bool geometry_changed=have_config && memcmp(&previous,&g.batch[b].cfg,sizeof(previous));
  const bool unexplained=wake!=g.wake;
  if(geometry_changed)duty=(nr_gpu_discovery_duty_t){0};
  wake=g.wake;
  if(g.duty_enabled) {
   int due=nr_gpu_discovery_due(&duty,duty_clock_us(batch_clock),epoch,
       !geometry_changed && g.result.count>0,!geometry_changed && g.all_verified,unexplained,1000ull*g.acquire_ms,
       1000ull*g.maintain_ms,1000ull*g.recheck_ms);
   if(logged_mode!=duty.mode) {
    static const char *names[]={"ACQUIRE","MAINTAIN","PAUSED"};
    LOG_A(PHY,"SENSING: PDCCH_NID_GPU mode %s -> %s epoch=%u published=%d verified=%d wake=%d\n",
          logged_mode<0?"START":names[logged_mode],names[duty.mode],epoch,g.result.count,g.all_verified,unexplained);
    logged_mode=duty.mode;
   }
   if(!due) {g.state[b]=0;batch_freed();pthread_mutex_unlock(&g.mu);continue;}
  }
  pthread_mutex_unlock(&g.mu);
  bool changed=have_config && memcmp(&previous,&g.batch[b].cfg,sizeof(previous));
  if(!have_config || changed) {
   previous=g.batch[b].cfg;have_config=true;batches=0;
   if(changed) {
    pthread_mutex_lock(&g.mu);
    memset(&g.result,0,sizeof(g.result));g.generation++;g.all_verified=false;
    pthread_mutex_unlock(&g.mu);
   }
  }
  int rc=g.ops->upload(g.device,&g.batch[b]);
  pthread_mutex_lock(&g.mu);
  g.state[b]=0;
  batch_freed();
  pthread_mutex_unlock(&g.mu);
  if(rc<0)goto failed;
  batches++;
  if(!g.incremental && (batches<(unsigned)g.window || (batches-g.window)%(unsigned)g.every))continue;
  nr_pdcch_nid_result_t result={0};
  rc=g.ops->run(g.device,&result);
  if(rc<0)goto failed;
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
   duty.next_us=duty_clock_us(batch_clock)+1000ull*g.maintain_ms;
   LOG_A(PHY,"SENSING: PDCCH_NID_GPU mode ACQUIRE -> MAINTAIN epoch=%u published=%d reason=publication\n",epoch,result.count);
   logged_mode=duty.mode;
  }
  uint64_t pass=g.passes;
  pthread_mutex_unlock(&g.mu);
  char records[2048];size_t used=0;records[0]=0;
  for(int j=0;j<result.count;j++) {
   const nr_pdcch_nid_record_t *r=result.records+j;
   int n=snprintf(records+used,sizeof(records)-used,
      " nid=%d sym=%d dur=%d grid_rb=%d groups=%d L=%d R=%d shift=%d ties=%d;",
      r->nid,r->sym,r->dur,r->grid_rb,r->groups,r->L,r->R,r->shift,r->ties);
   if(n<0 || (size_t)n>=sizeof(records)-used)break;
   used+=n;
  }
  if(g.nodrop)
   LOG_A(PHY,"SENSING: PDCCH_NID_GPU pass %llu: stage1 %.3f ms stage2 %.3f ms (kernels max %.3f ms) dropped_snapshots=%llu dropped_partial=%llu -> %d CORESETs:%s\n",
         (unsigned long long)pass,result.stage1_ms,result.stage2_ms,result.kernel_max_ms,
         (unsigned long long)atomic_load_explicit(&g.dropped_snapshots,memory_order_relaxed),
         (unsigned long long)atomic_load_explicit(&g.dropped_partial,memory_order_relaxed),result.count,records);
  else
  LOG_A(PHY,"SENSING: PDCCH_NID_GPU pass %llu: stage1 %.3f ms stage2 %.3f ms (kernels max %.3f ms) -> %d CORESETs:%s\n",
        (unsigned long long)pass,result.stage1_ms,result.stage2_ms,result.kernel_max_ms,result.count,records);
  continue;
failed:
  LOG_E(PHY,"SENSING: PDCCH_NID_GPU device failure; capture stopped (no CPU sweep fallback)\n");
  atomic_store_explicit(&g.running,false,memory_order_release);
  break;
 }
 return NULL;
}
/* nr_pdcch_nid_gpu_start: starts the PDCCH NID GPU discovery: reads the ISAC_PDCCH_NID_GPU_* policy flags, loads the device module and starts the worker thread.
 *   How it works: Returns 0 when the feature is off. Reads DUTY, ACQUIRE_MS, MAINTAIN_MS, NODROP, RECHECK_MS, the ISAC_COREMAP_IDSWEEP_DUR duration (clamped 1..3), INCREMENTAL (default on), BATCH, EVERY and WINDOW. In no-drop mode it sets a monotonic condvar and resets the slot ticks. dlopen libpdcch_nid_gpu.so, check the ABI, create the device and set the worker to SCHED_OTHER with an explicit schedule.
 *   ISAC_* flags read: ISAC_COREMAP_IDSWEEP_DUR, ISAC_CSIRS_DISC, ISAC_PDCCH_NID_GPU_ACQUIRE_MS, ISAC_PDCCH_NID_GPU_BATCH, ISAC_PDCCH_NID_GPU_CORE, ISAC_PDCCH_NID_GPU_DUTY, ISAC_PDCCH_NID_GPU_EVERY, ISAC_PDCCH_NID_GPU_INCREMENTAL, ISAC_PDCCH_NID_GPU_MAINTAIN_MS, ISAC_PDCCH_NID_GPU_NODROP, ISAC_PDCCH_NID_GPU_RECHECK_MS, ISAC_PDCCH_NID_GPU_WINDOW.
 *   Callers: executables/nr-uesoftmodem.c:620, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:59, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:32, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:39.
 */
int nr_pdcch_nid_gpu_start(int max_rb)
{
 if(!nr_pdcch_nid_gpu_enabled())return 0;
 if(g.device)return nr_pdcch_nid_gpu_running()?0:-1;
 if(max_rb<6 || max_rb>275)return -1;
 g.rb=max_rb;
 const char *duty=getenv("ISAC_PDCCH_NID_GPU_DUTY");
 g.duty_enabled=duty && !strcmp(duty,"1");
 g.acquire_ms=env_int("ISAC_PDCCH_NID_GPU_ACQUIRE_MS",5000,1,60000);
 g.maintain_ms=env_int("ISAC_PDCCH_NID_GPU_MAINTAIN_MS",500,1,60000);
 {const char *nd=getenv("ISAC_PDCCH_NID_GPU_NODROP");g.nodrop=nd && !strcmp(nd,"1");}
 g.recheck_ms=env_int("ISAC_PDCCH_NID_GPU_RECHECK_MS",10000,1,600000);
 const char *duration=getenv("ISAC_COREMAP_IDSWEEP_DUR");
 g.dur=duration?atoi(duration):3;
 if(g.dur<1)g.dur=1;
 if(g.dur>3)g.dur=3;
 g.incremental=(getenv("ISAC_PDCCH_NID_GPU_INCREMENTAL")==NULL || strcmp(getenv("ISAC_PDCCH_NID_GPU_INCREMENTAL"),"0")); /* own flag, default on (was ISAC_CSIRS_DISC) */
 g.batch_size=g.incremental?env_int("ISAC_PDCCH_NID_GPU_BATCH",64,1,NR_PDCCH_NID_BATCH):NR_PDCCH_NID_BATCH;
 g.every=g.incremental?1:env_int("ISAC_PDCCH_NID_GPU_EVERY",2,1,1000);
 g.window=env_int("ISAC_PDCCH_NID_GPU_WINDOW",10,1,NR_PDCCH_NID_MAX_WINDOW);
 if(g.nodrop) {
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca,CLOCK_MONOTONIC);
  pthread_cond_destroy(&g.cv_free);
  pthread_cond_init(&g.cv_free,&ca);
  pthread_condattr_destroy(&ca);
  g.snap_started=g.snap_refused=false;
  atomic_store_explicit(&g.slot_ticks,0,memory_order_relaxed);
 }
 g.module=dlopen("libpdcch_nid_gpu.so",RTLD_NOW|RTLD_LOCAL);
 if(!g.module)goto fail;
 const nr_pdcch_nid_ops_t *(*get_ops)(void)=dlsym(g.module,"nr_pdcch_nid_gpu_ops");
 if(!get_ops || !(g.ops=get_ops()) || g.ops->abi!=NR_PDCCH_NID_ABI)goto fail;
 nr_pdcch_nid_iq_t *staging=NULL;
 g.device=g.ops->create(max_rb,g.dur,g.window,&staging);
 if(!g.device)goto fail;
 for(int j=0;j<3;j++)g.batch[j].iq=staging+(size_t)j*NR_PDCCH_NID_BATCH*g.dur*12*max_rb;
 // Explicit policy prevents inheriting FIFO from receiver initialization.
 pthread_attr_t attr;
 pthread_attr_init(&attr);
 struct sched_param sp={.sched_priority=0};
 int rc=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
 rc|=pthread_attr_setschedpolicy(&attr,SCHED_OTHER);
 rc|=pthread_attr_setschedparam(&attr,&sp);
 cpu_set_t allowed,pin;CPU_ZERO(&allowed);CPU_ZERO(&pin);
 if(sched_getaffinity(0,sizeof(allowed),&allowed)!=0)rc=-1;
 int core=env_int("ISAC_PDCCH_NID_GPU_CORE",-1,0,CPU_SETSIZE-1);
 if(core<0)for(int j=CPU_SETSIZE-1;j>=0;j--)if(CPU_ISSET(j,&allowed)){core=j;break;}
 if(core<0 || !CPU_ISSET(core,&allowed))rc=-1;
 else {CPU_SET(core,&pin);rc|=pthread_attr_setaffinity_np(&attr,sizeof(pin),&pin);}
 if(!rc)rc=pthread_create(&g.thread,&attr,worker,NULL);
 pthread_attr_destroy(&attr);
 if(rc)goto fail;
 atomic_store_explicit(&g.running,true,memory_order_release);
 LOG_I(PHY,"SENSING: PDCCH_NID_GPU ready: core=%d SCHED_OTHER window=%d every=%d dur=%d batch=%d incremental=%d pinned staging=3 batches\n",
       core,g.window,g.every,g.dur,g.batch_size,g.incremental);
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
 if(g.nodrop)LOG_A(PHY,"SENSING: PDCCH_NID_GPU stop: dropped_snapshots=%llu dropped_partial=%llu\n",
   (unsigned long long)nr_pdcch_nid_gpu_dropped_snapshots(),(unsigned long long)atomic_load_explicit(&g.dropped_partial,memory_order_relaxed));
 pthread_mutex_lock(&g.mu);g.stop=true;pthread_cond_signal(&g.cv);pthread_mutex_unlock(&g.mu);
 pthread_join(g.thread,NULL);
 g.ops->destroy(g.device);dlclose(g.module);g.device=g.module=NULL;g.ops=NULL;
 pthread_mutex_lock(&g.mu);
 memset(g.state,0,sizeof(g.state));g.producer=-1;g.n=0;g.stop=false;
 g.result.count=0;g.generation++;
 pthread_mutex_unlock(&g.mu);
}
/* nr_pdcch_nid_gpu_dropped_snapshots: returns the number of no-drop snapshots refused so far.
 *   How it works: Relaxed atomic load of dropped_snapshots.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:274, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:62, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:65, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:71 (+3 more).
 */
uint64_t nr_pdcch_nid_gpu_dropped_snapshots(void)
{
 return atomic_load_explicit(&g.dropped_snapshots,memory_order_relaxed);
}
/* nr_pdcch_nid_gpu_push: producer entry, called per RX symbol: validates the geometry and copies the symbol IQ into the current batch.
 *   How it works: Rejects bad geometry (nsc, sps, slot, FFT offset, RB count). No-drop mode accepts a slot atomically: the first symbol decides, and later symbols of a refused slot are counted as partial drops. Otherwise it takes the mutex and a free batch. Copies dur symbols per candidate with the cyclic FFT offset, zero-fills past sps, and marks the batch ready (state 2) at batch_size.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:526, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:77, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:65, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:52 (+1 more).
 */
int nr_pdcch_nid_gpu_push(const void *rx,int fft,int fco,int slot,int sym,const nr_pdcch_nid_config_t *cfg)
{
 if(!nr_pdcch_nid_gpu_running() || !rx || !cfg || cfg->dur!=g.dur || cfg->nsc<72
    || cfg->nsc>12*g.rb || cfg->nsc>fft || fco<0 || fco>=fft || cfg->sps<1 || cfg->sps>14
    || sym<0 || (g.incremental?sym>=cfg->sps:sym+cfg->dur>cfg->sps) || slot<0 || slot>=160
    || (cfg->nsc-cfg->point_a_sc)/12>275
    || (cfg->nsc-cfg->point_a_sc)/12-(cfg->point_a_sc<0?(-cfg->point_a_sc+11)/12:0)<6)return -1;
 if(g.nodrop) {
  /* A snapshot is one slot's push sequence. Its first symbol decides: if that cannot be accepted the rest is refused, so the GPU never sees a partial snapshot. */
  const bool first=!g.snap_started || slot!=g.snap_slot;
  if(first) {
   if(g.snap_started)atomic_fetch_add_explicit(&g.slot_ticks,(uint64_t)((slot-g.snap_slot+160)%160),memory_order_relaxed);
   g.snap_started=true;g.snap_slot=slot;g.snap_refused=false;
  } else if(g.snap_refused)return -1;
  if(!nodrop_acquire()) {
   if(first) {g.snap_refused=true;atomic_fetch_add_explicit(&g.dropped_snapshots,1,memory_order_relaxed);}
   else atomic_fetch_add_explicit(&g.dropped_partial,1,memory_order_relaxed);
   return -1;
  }
 } else {
 if(pthread_mutex_trylock(&g.mu))return -1;
 if(g.producer<0) {
  for(int j=0;j<3;j++)if(g.state[j]==0){g.producer=j;g.state[j]=1;g.n=0;break;}
 }
 if(g.producer<0) {g.dropped++;pthread_mutex_unlock(&g.mu);return -1;}
 }
 nr_pdcch_nid_batch_t *b=&g.batch[g.producer];
 nr_pdcch_nid_config_t current=*cfg;current.epoch=g.epoch;
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
  b->count=g.incremental?g.batch_size:0;
  g.state[g.producer]=2;g.sequence[g.producer]=++g.produced;g.clock_us[g.producer]=slot_clock_us();g.producer=-1;g.n=0;
  pthread_cond_signal(&g.cv);
 }
 pthread_mutex_unlock(&g.mu);return 0;
}
/* nr_pdcch_nid_gpu_records: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:988, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1170, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:50, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:33 (+7 more).
 */
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

/* nr_pdcch_nid_gpu_snapshot: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1396, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.h:53, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:112, openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_nid_worker_test.cc:121.
 */
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
  batch_freed();
  if(g.producer>=0)g.n=0;
 }
 if(generation==g.generation)g.all_verified=verified;
 if(unexplained) {
  uint64_t now=g.nodrop?slot_clock_us():nr_gpu_budget_now_us();
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
