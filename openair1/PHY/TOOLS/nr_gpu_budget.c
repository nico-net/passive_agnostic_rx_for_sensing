/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_budget.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int enabled;
static unsigned pct=30, deadline=250;
/* High bit: one in-flight discovery slice; low bits: RT users.
 * RT may enter a high-priority stream while that slice retires. */
#define DISC_OWNER (1u << 31)
static _Atomic unsigned owner;
static _Atomic int fault;
static _Atomic uint64_t next_us, used_us, yields, wait_max, fallbacks, overruns;
static _Thread_local uint64_t job_start;
void nr_gpu_budget_rt_job_start(uint64_t start) {job_start=start;}
uint64_t nr_gpu_budget_rt_start_us(void) {return job_start?job_start:nr_gpu_budget_now_us();}
static uint64_t start_us, slice_start;
/* Discovery owner protects this conservative rolling second. Completion is
 * charged in full to its millisecond bucket; retain the boundary bucket too. */
static uint64_t buckets[1001], window_tick, window_used;
static void advance_window(uint64_t now) {
  uint64_t tick=now/1000;
  if(tick<window_tick)return; /* admission timestamp may predate a competing completion */
  if(tick-window_tick>1000) {
    memset(buckets,0,sizeof(buckets));window_used=0;window_tick=tick;
  } else while(window_tick<tick) {
    ++window_tick;unsigned i=window_tick%1001;
    window_used-=buckets[i];buckets[i]=0;
  }
}
uint64_t nr_gpu_budget_now_us(void) {
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
static unsigned setting(const char *name,unsigned def,unsigned lo,unsigned hi) {
  const char *s=getenv(name);if(!s || !*s)return def;
  char *end;long v=strtol(s,&end,10);return *end || v<(long)lo || v>(long)hi?def:(unsigned)v;
}
static void init(void) {
  const char *s=getenv("ISAC_GPU_BUDGET");enabled=s && !strcmp(s,"1");
  pct=setting("ISAC_GPU_DISCOVERY_BUDGET_PCT",30,1,100);
  deadline=setting("ISAC_GPU_RT_DEADLINE_US",250,1,5000);
  start_us=nr_gpu_budget_now_us();
}
int nr_gpu_budget_enabled(void) {pthread_once(&once,init);return enabled;}
unsigned nr_gpu_rt_deadline_us(void) {nr_gpu_budget_enabled();return deadline;}
int nr_gpu_budget_rt_begin(void) {
  if(!nr_gpu_budget_enabled())return 1;
  if(job_start && nr_gpu_budget_now_us()-job_start>=deadline) {
    atomic_fetch_add(&fallbacks,1);return 0;
  }
  /* Do not reject RT merely because discovery has a kernel in flight.
   * CUDA priority applies at block admission; prevent the NEXT discovery slice
   * until all RT work retires. The absolute RT completion deadline still holds. */
  atomic_fetch_add(&owner,1);
  return 1;
}
void nr_gpu_budget_rt_end(void) {if(nr_gpu_budget_enabled())atomic_fetch_sub(&owner,1);}
void nr_gpu_budget_rt_result(uint64_t us,int fallback) {
  if(!nr_gpu_budget_enabled())return;
  uint64_t v=atomic_load(&wait_max);
  while(us>v && !atomic_compare_exchange_weak(&wait_max,&v,us)) {}
  if(fallback)atomic_fetch_add(&fallbacks,1);
}
int nr_gpu_budget_rt_wait(int (*query)(void *),void *job,uint64_t start,
                          uint64_t (*clock_us)(void *),void *clock_arg) {
  int rc;uint64_t elapsed;
  do {
    rc=query(job);
    uint64_t now=clock_us?clock_us(clock_arg):nr_gpu_budget_now_us();
    elapsed=now-start;
    if(elapsed>=nr_gpu_rt_deadline_us()) {rc=600;break;}
  } while(rc==600);
  nr_gpu_budget_rt_result(elapsed,rc!=0);
  if(rc && rc!=600)nr_gpu_budget_fault();
  return rc;
}
int nr_gpu_budget_discovery_begin(uint64_t now) {
  if(!nr_gpu_budget_enabled())return 1;
  if(atomic_load(&fault))return -1;
  unsigned empty=0;
  if(now<atomic_load(&next_us) || !atomic_compare_exchange_strong(&owner,&empty,DISC_OWNER)) {
    atomic_fetch_add(&yields,1);return 0;
  }
  if(atomic_load(&fault)) {atomic_fetch_and(&owner,~DISC_OWNER);return -1;}
  if(now<atomic_load(&next_us)) {
    atomic_fetch_and(&owner,~DISC_OWNER);atomic_fetch_add(&yields,1);return 0;
  }
  advance_window(now);
  /* Reserve the hard slice ceiling BEFORE launch, even for a tiny kernel. */
  if(window_used+5000>(uint64_t)pct*10000) {
    atomic_fetch_and(&owner,~DISC_OWNER);atomic_fetch_add(&yields,1);return 0;
  }
  slice_start=now;
  return 1;
}
uint64_t nr_gpu_budget_discovery_delay_us(uint64_t now) {
  uint64_t next=atomic_load(&next_us);
  return next>now?next-now:0;
}
void nr_gpu_budget_discovery_end(uint64_t now,uint64_t us) {
  if(!nr_gpu_budget_enabled())return;
  advance_window(now);window_used+=us;buckets[window_tick%1001]+=us;
  atomic_fetch_add(&used_us,us);
  /* Pacing prevents catch-up bursts even after RT has held the device idle. */
  uint64_t paced=slice_start+(us*100+pct-1)/pct;
  atomic_store(&next_us,paced>now?paced:now);
  /* Event intervals can include host descheduling between event/launch calls.
   * Charge the entire interval and report it, but do not turn one slow slice
   * into a permanent synthetic CUDA error on the NEXT upload/kernel. */
  if(us>5000)atomic_fetch_add(&overruns,1);
  atomic_fetch_and(&owner,~DISC_OWNER);
}
void nr_gpu_budget_fault(void) {if(nr_gpu_budget_enabled())atomic_store(&fault,1);}
void nr_gpu_budget_metrics(nr_gpu_budget_metrics_t *out) {
  nr_gpu_budget_enabled();uint64_t elapsed=nr_gpu_budget_now_us()-start_us;
  *out=(nr_gpu_budget_metrics_t){elapsed?100.0*atomic_load(&used_us)/elapsed:0,
    atomic_load(&yields),atomic_load(&wait_max),atomic_load(&fallbacks),atomic_load(&overruns)};
}
int nr_gpu_discovery_due(nr_gpu_discovery_duty_t *s,uint64_t now,uint64_t epoch,
                        int published,int verified,int unexplained,uint64_t acquire,
                        uint64_t maintain,uint64_t recheck) {
  if(s->epoch!=epoch || !s->acquired_us) {
    *s=(nr_gpu_discovery_duty_t){.epoch=epoch,.acquired_us=now?now:1};
  }
  if(unexplained && s->verified) {
    s->verified=0;s->next_us=now;
  }
  s->unexplained=unexplained;
  if(published)s->published=1;
  if(verified && published && !s->verified && !unexplained) {
    s->verified=1;s->mode=NR_GPU_DISC_PAUSED;s->next_us=now+recheck;return 0;
  }
  if(!verified && s->verified) {s->verified=0;s->next_us=now;}
  s->mode=verified && published && !unexplained?NR_GPU_DISC_PAUSED:
    (s->published || (now>=s->acquired_us && now-s->acquired_us>=acquire)?NR_GPU_DISC_MAINTAIN:NR_GPU_DISC_ACQUIRE);
  if(now<s->next_us)return 0;
  uint64_t interval=verified && published && !unexplained?recheck:
    (s->published || (now>=s->acquired_us && now-s->acquired_us>=acquire)?maintain:0);
  s->next_us=now+interval;
  return 1;
}
#ifdef NR_GPU_BUDGET_TEST
void nr_gpu_budget_test_reset(unsigned p) {
  nr_gpu_budget_enabled();enabled=1;pct=p;
  memset(buckets,0,sizeof(buckets));window_tick=window_used=0;
  atomic_store(&owner,0);atomic_store(&fault,0);atomic_store(&next_us,0);
  atomic_store(&used_us,0);atomic_store(&yields,0);atomic_store(&wait_max,0);
  atomic_store(&fallbacks,0);atomic_store(&overruns,0);
}
#endif
