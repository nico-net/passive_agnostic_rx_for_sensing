/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "nr_pdcch_nid_gpu.h"
#include "PHY/TOOLS/nr_gpu_budget.h"
#include "common/utils/LOG/log.h"
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
 pthread_mutex_t mu;
 pthread_cond_t cv;
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
 uint32_t epoch;
 uint64_t wake;
 unsigned acquire_ms, maintain_ms, recheck_ms;
 bool stop;
 _Atomic bool running;
} g = {.mu=PTHREAD_MUTEX_INITIALIZER,.cv=PTHREAD_COND_INITIALIZER,.producer=-1};

bool nr_pdcch_nid_gpu_enabled(void)
{
 const char *e=getenv("ISAC_PDCCH_NID_GPU");
 return e && strcmp(e,"1")==0;
}
bool nr_pdcch_nid_gpu_running(void) {return atomic_load_explicit(&g.running,memory_order_acquire);}
static int env_int(const char *key,int def,int lo,int hi)
{
 const char *s=getenv(key);if(!s || !*s)return def;
 char *end;long n=strtol(s,&end,10);
 return *end || n<lo || n>hi?def:(int)n;
}
static void *worker(void *unused)
{
 (void)unused;
 unsigned batches=0;
 nr_pdcch_nid_config_t previous={0};
 bool have_config=false;
 nr_gpu_discovery_duty_t duty={0};
 uint64_t wake=0;
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
  const bool unexplained=wake!=g.wake || (have_config && memcmp(&previous,&g.batch[b].cfg,sizeof(previous)));
  wake=g.wake;
  if(g.duty_enabled && !nr_gpu_discovery_due(&duty,nr_gpu_budget_now_us(),epoch,
       g.result.count>0,g.all_verified,unexplained,1000ull*g.acquire_ms,
       1000ull*g.maintain_ms,1000ull*g.recheck_ms)) {
    g.state[b]=0;pthread_mutex_unlock(&g.mu);continue;
  }
  pthread_mutex_unlock(&g.mu);
  bool changed=have_config && memcmp(&previous,&g.batch[b].cfg,sizeof(previous));
  if(!have_config || changed) {
   previous=g.batch[b].cfg;have_config=true;batches=0;
   if(changed) {
    pthread_mutex_lock(&g.mu);
    memset(&g.result,0,sizeof(g.result));g.generation++;
    pthread_mutex_unlock(&g.mu);
   }
  }
  int rc=g.ops->upload(g.device,&g.batch[b]);
  pthread_mutex_lock(&g.mu);
  g.state[b]=0;
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
  bool same=g.result.count==result.count && !memcmp(g.result.records,result.records,result.count*sizeof(result.records[0]));
  g.result=result;if(!same || !g.duty_enabled) {g.generation++;g.all_verified=false;}g.passes++;
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
 g.recheck_ms=env_int("ISAC_PDCCH_NID_GPU_RECHECK_MS",10000,1,600000);
 const char *duration=getenv("ISAC_COREMAP_IDSWEEP_DUR");
 g.dur=duration?atoi(duration):3;
 if(g.dur<1)g.dur=1;
 if(g.dur>3)g.dur=3;
 g.incremental=(getenv("ISAC_PDCCH_NID_GPU_INCREMENTAL")==NULL || strcmp(getenv("ISAC_PDCCH_NID_GPU_INCREMENTAL"),"0")); /* own flag, default on (was ISAC_CSIRS_DISC) */
 g.batch_size=g.incremental?env_int("ISAC_PDCCH_NID_GPU_BATCH",64,1,NR_PDCCH_NID_BATCH):NR_PDCCH_NID_BATCH;
 g.every=g.incremental?1:env_int("ISAC_PDCCH_NID_GPU_EVERY",2,1,1000);
 g.window=env_int("ISAC_PDCCH_NID_GPU_WINDOW",10,1,NR_PDCCH_NID_MAX_WINDOW);
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
void nr_pdcch_nid_gpu_stop(void)
{
 if(!g.device)return;
 atomic_store_explicit(&g.running,false,memory_order_release);
 pthread_mutex_lock(&g.mu);g.stop=true;pthread_cond_signal(&g.cv);pthread_mutex_unlock(&g.mu);
 pthread_join(g.thread,NULL);
 g.ops->destroy(g.device);dlclose(g.module);g.device=g.module=NULL;g.ops=NULL;
 pthread_mutex_lock(&g.mu);
 memset(g.state,0,sizeof(g.state));g.producer=-1;g.n=0;g.stop=false;
 g.result.count=0;g.generation++;
 pthread_mutex_unlock(&g.mu);
}
int nr_pdcch_nid_gpu_push(const void *rx,int fft,int fco,int slot,int sym,const nr_pdcch_nid_config_t *cfg)
{
 if(!nr_pdcch_nid_gpu_running() || !rx || !cfg || cfg->dur!=g.dur || cfg->nsc<72
    || cfg->nsc>12*g.rb || cfg->nsc>fft || fco<0 || fco>=fft || cfg->sps<1 || cfg->sps>14
    || sym<0 || (g.incremental?sym>=cfg->sps:sym+cfg->dur>cfg->sps) || slot<0 || slot>=160
    || (cfg->nsc-cfg->point_a_sc)/12>275
    || (cfg->nsc-cfg->point_a_sc)/12-(cfg->point_a_sc<0?(-cfg->point_a_sc+11)/12:0)<6)return -1;
 if(pthread_mutex_trylock(&g.mu))return -1;
 if(g.producer<0) {
  for(int j=0;j<3;j++)if(g.state[j]==0){g.producer=j;g.state[j]=1;g.n=0;break;}
 }
 if(g.producer<0) {g.dropped++;pthread_mutex_unlock(&g.mu);return -1;}
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
  g.state[g.producer]=2;g.sequence[g.producer]=++g.produced;g.producer=-1;g.n=0;
  pthread_cond_signal(&g.cv);
 }
 pthread_mutex_unlock(&g.mu);return 0;
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
void nr_pdcch_nid_gpu_feedback(uint32_t epoch,uint64_t generation,bool verified,bool unexplained)
{
 if(!g.duty_enabled)return;
 if(pthread_mutex_trylock(&g.mu))return;
 if(g.epoch!=epoch) {
  g.epoch=epoch;g.result.count=0;g.generation++;g.all_verified=false;
  /* Drop queued old-epoch snapshots, but never reclaim the worker's buffer. */
  for(int i=0;i<3;++i)if(g.state[i]==2)g.state[i]=0;
  if(g.producer>=0)g.n=0;
 }
 if(generation==g.generation)g.all_verified=verified;
 if(unexplained)g.wake++;
 pthread_mutex_unlock(&g.mu);
}
