/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_budget.h"
#include "nr_mpci_sched.h"
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
static _Atomic int prefer;
/* Cross-cell arbiter (ISAC_MPCI_SCHED); NULL = off, local policy only. */
static nr_mpci_t *mpci;
static _Atomic uint64_t next_us, used_us, yields, wait_max, fallbacks, overruns;
static _Thread_local uint64_t job_start;
/* nr_gpu_budget_rt_job_start: records the start time (us) of the real-time job on this thread; read later by nr_gpu_budget_rt_start_us() and rt_begin() for the deadline check.
 *   Thread-local. Callers: nr_pdsch_passive_queue.c (per job) and the budget header users.
 */
void nr_gpu_budget_rt_job_start(uint64_t start) {job_start=start;}
/* nr_gpu_budget_rt_start_us: returns the thread's real-time job start (us), or the current time when no job start was recorded. */
uint64_t nr_gpu_budget_rt_start_us(void) {return job_start?job_start:nr_gpu_budget_now_us();}
static uint64_t start_us, slice_start;
/* Discovery owner protects this conservative rolling second. Completion is
 * charged in full to its millisecond bucket; retain the boundary bucket too. */
static uint64_t buckets[1001], window_tick, window_used;
/* advance_window: advances the one-second rolling window of discovery GPU time (1001 millisecond buckets) to `now`, dropping buckets that left the window.
 *   Called only with the discovery owner held (discovery_begin / discovery_end). A timestamp older than the window is ignored.
 */
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
/* nr_gpu_budget_now_us: returns CLOCK_MONOTONIC time in microseconds. Used for all budget timestamps. */
uint64_t nr_gpu_budget_now_us(void) {
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
/* setting: reads an unsigned environment setting in [lo, hi]; returns def when unset, malformed or out of range. */
static unsigned setting(const char *name,unsigned def,unsigned lo,unsigned hi) {
  const char *s=getenv(name);if(!s || !*s)return def;
  char *end;long v=strtol(s,&end,10);return *end || v<(long)lo || v>(long)hi?def:(unsigned)v;
}
/* init: one-time setup (pthread_once): reads ISAC_GPU_BUDGET (enabled when "1"), ISAC_GPU_DISCOVERY_BUDGET_PCT (default 30) and ISAC_GPU_RT_DEADLINE_US (default 250), and records the start time. */
static void init(void) {
  const char *s=getenv("ISAC_GPU_BUDGET");enabled=s && !strcmp(s,"1");
  pct=setting("ISAC_GPU_DISCOVERY_BUDGET_PCT",30,1,100);
  deadline=setting("ISAC_GPU_RT_DEADLINE_US",250,1,5000);
  start_us=nr_gpu_budget_now_us();
  if(enabled)mpci=nr_mpci_default(pct);
}
/* nr_gpu_budget_enabled: returns 1 when the GPU time budget is enabled (ISAC_GPU_BUDGET=1), else 0; runs init() on first use. Every other entry point checks this and does nothing when disabled. */
int nr_gpu_budget_enabled(void) {pthread_once(&once,init);return enabled;}
/* nr_gpu_rt_deadline_us: returns the real-time GPU completion deadline in us (ISAC_GPU_RT_DEADLINE_US). Callers: nr_gpu_budget_rt_wait() and the GPU paths. */
unsigned nr_gpu_rt_deadline_us(void) {nr_gpu_budget_enabled();return deadline;}
/* nr_gpu_budget_rt_begin: admits one real-time GPU job: refused (counted as a fallback) when the job has already exceeded the deadline; otherwise registers it as an RT user of the device.
 *   While any RT user is active the next discovery slice cannot start. Returns 1 to proceed on the GPU, 0 to use the CPU fallback. Callers: nr_pdsch_passive_queue.c and the CUDA budget header.
 */
int nr_gpu_budget_rt_begin(void) {
  if(!nr_gpu_budget_enabled())return 1;
  if(job_start && nr_gpu_budget_now_us()-job_start>=deadline) {
    atomic_fetch_add(&fallbacks,1);return 0;
  }
  /* Do not reject RT merely because discovery has a kernel in flight.
   * CUDA priority applies at block admission; prevent the NEXT discovery slice
   * until all RT work retires. The absolute RT completion deadline still holds. */
  atomic_fetch_add(&owner,1);
  nr_mpci_rt_begin(mpci,nr_gpu_budget_now_us());
  return 1;
}
/* nr_gpu_budget_rt_end: releases one RT user registered by nr_gpu_budget_rt_begin(). No-op when the budget is disabled. */
void nr_gpu_budget_rt_end(void) {if(nr_gpu_budget_enabled()) {atomic_fetch_sub(&owner,1);nr_mpci_rt_end(mpci);}}
/* nr_gpu_budget_rt_result: records the outcome of one RT GPU call: its wait time updates the maximum wait; a fallback is counted when `fallback` is set. No-op when disabled. */
void nr_gpu_budget_rt_result(uint64_t us,int fallback) {
  if(!nr_gpu_budget_enabled())return;
  uint64_t v=atomic_load(&wait_max);
  while(us>v && !atomic_compare_exchange_weak(&wait_max,&v,us)) {}
  if(fallback)atomic_fetch_add(&fallbacks,1);
}
/* nr_gpu_budget_rt_wait: waits for an RT GPU job by polling query(job) until it completes, or until the deadline (measured with clock_us from start) expires (rc 600).
 *   Records the wait, and calls nr_gpu_budget_fault() on a real error (not a timeout). Returns the query's last result code.
 */
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
/* nr_gpu_budget_discovery_begin: asks for one discovery GPU slice at time now. Returns 1 when the slice may start (the discovery owner is then held), 0 when it must yield (pacing, RT in flight, or the rolling budget is full), -1 after a fault.
 *   Reserves the hard slice ceiling (5 ms) before launch against the pct% budget per second. Counts yields. Callers: nr_gpu_budget_cuda.h (the CUDA wrappers) and the discovery GPU code.
 */
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
  if(!nr_mpci_discovery_admit(mpci,now,atomic_load(&prefer))) {
    atomic_fetch_and(&owner,~DISC_OWNER);atomic_fetch_add(&yields,1);return 0;
  }
  slice_start=now;
  return 1;
}
/* nr_gpu_budget_discovery_delay_us: returns how many microseconds the discovery pacing still forbids a slice at time now (0 when a slice may start). */
uint64_t nr_gpu_budget_discovery_delay_us(uint64_t now) {
  uint64_t next=atomic_load(&next_us);
  return next>now?next-now:0;
}
/* nr_gpu_budget_discovery_end: completes a discovery slice that used `us` microseconds: charges it to the rolling window, paces the next slice to keep the duty cycle at pct%, counts overruns (above 5 ms), and releases the owner.
 *   The whole interval is charged even when host scheduling inflated it.
 */
void nr_gpu_budget_discovery_end(uint64_t now,uint64_t us) {
  if(!nr_gpu_budget_enabled())return;
  advance_window(now);window_used+=us;buckets[window_tick%1001]+=us;
  atomic_fetch_add(&used_us,us);
  nr_mpci_discovery_charge(mpci,now,us);
  /* Pacing prevents catch-up bursts even after RT has held the device idle. */
  uint64_t paced=slice_start+(us*100+pct-1)/pct;
  atomic_store(&next_us,paced>now?paced:now);
  /* Event intervals can include host descheduling between event/launch calls.
   * Charge the entire interval and report it, but do not turn one slow slice
   * into a permanent synthetic CUDA error on the NEXT upload/kernel. */
  if(us>5000)atomic_fetch_add(&overruns,1);
  atomic_fetch_and(&owner,~DISC_OWNER);
}
void nr_gpu_budget_discovery_prefer(int on) {atomic_store(&prefer,on);}
int nr_gpu_budget_discovery_preferred(void) {return atomic_load(&prefer);}
/* nr_gpu_budget_fault: latches the GPU budget fault: every later discovery_begin returns -1 until the process restarts. No-op when disabled. */
void nr_gpu_budget_fault(void) {if(nr_gpu_budget_enabled())atomic_store(&fault,1);}
/* nr_gpu_budget_metrics: fills *out with the discovery share of GPU time since start (percent), yields, maximum RT wait, fallbacks and overruns. */
void nr_gpu_budget_metrics(nr_gpu_budget_metrics_t *out) {
  nr_gpu_budget_enabled();uint64_t elapsed=nr_gpu_budget_now_us()-start_us;
  *out=(nr_gpu_budget_metrics_t){elapsed?100.0*atomic_load(&used_us)/elapsed:0,
    atomic_load(&yields),atomic_load(&wait_max),atomic_load(&fallbacks),atomic_load(&overruns)};
}
/* nr_gpu_discovery_due: decides whether a discovery duty (acquire, maintain or paused) is due at time now for one scheduler state s, and advances s->next_us.
 *   The state is reset when the epoch changes. A published and verified duty pauses and rechecks at the recheck interval; an unexplained result restarts it. Returns 1 when work is due now.
 */
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
/* nr_gpu_budget_test_reset: test-only (NR_GPU_BUDGET_TEST): enables the budget with the given percentage and clears the window, owner, fault and counters. Caller: openair1/PHY/TOOLS/tests/nr_gpu_budget_test.cc. */
void nr_gpu_budget_test_reset(unsigned p) {
  nr_gpu_budget_enabled();enabled=1;pct=p;
  memset(buckets,0,sizeof(buckets));window_tick=window_used=0;
  atomic_store(&owner,0);atomic_store(&fault,0);atomic_store(&next_us,0);
  atomic_store(&used_us,0);atomic_store(&yields,0);atomic_store(&wait_max,0);
  atomic_store(&fallbacks,0);atomic_store(&overruns,0);
}
#endif
