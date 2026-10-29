/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_budget.h"
#include "nr_gpu_poll.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <stdio.h>
static pthread_once_t once=PTHREAD_ONCE_INIT;
static int enabled;
static unsigned pct=30, deadline=250;
/* High bit: one in-flight discovery slice; low bits: RT users.
 * RT may enter a high-priority stream while that slice retires. */
#define DISC_OWNER (1u << 31)
static _Atomic unsigned owner;
/* G4: wall time with at least one RT user in flight (rt_begin .. rt_end of the first/last), read by the discovery ticket to see how much of the device the RT work leaves. */
static _Atomic uint64_t rt_busy_total,rt_busy_since;
static _Atomic int fault;
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
/* init: one-time setup (pthread_once): reads ISAC_GPU_BUDGET (enabled when "1", set by the passive receiver's main) and records the start time; discovery share 30 %, RT deadline 250 us. */
static void init(void) {
  const char *s=getenv("ISAC_GPU_BUDGET");enabled=s && !strcmp(s,"1");
  start_us=nr_gpu_budget_now_us();
}
/* nr_gpu_budget_enabled: returns 1 when the GPU time budget is enabled (ISAC_GPU_BUDGET=1), else 0; runs init() on first use. Every other entry point checks this and does nothing when disabled. */
int nr_gpu_budget_enabled(void) {pthread_once(&once,init);return enabled;}
/* nr_gpu_rt_deadline_us: returns the real-time GPU completion deadline in us (250). Callers: nr_gpu_budget_rt_wait() and the GPU paths. */
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
  if((atomic_fetch_add(&owner,1)&~DISC_OWNER)==0)atomic_store(&rt_busy_since,nr_gpu_budget_now_us()); /* G4: first RT user: the device becomes RT-busy */
  return 1;
}
/* nr_gpu_budget_rt_end: releases one RT user registered by nr_gpu_budget_rt_begin(). No-op when the budget is disabled. */
void nr_gpu_budget_rt_end(void) {
  if(!nr_gpu_budget_enabled())return;
  const unsigned prev=atomic_fetch_sub(&owner,1);
  if((prev&~DISC_OWNER)==1) { /* G4: last RT user left: the interval since the first one is RT-busy time */
    const uint64_t now=nr_gpu_budget_now_us(),since=atomic_load(&rt_busy_since);
    if(now>since)atomic_fetch_add(&rt_busy_total,now-since);
  }
}
/* nr_gpu_budget_rt_active: number of RT GPU users in flight (0 when the budget is off). Read-only; best-effort GPU users (ISAC_UL_GPU_FEP) defer to the CPU while it is non-zero and they have no SM partition of their own. */
int nr_gpu_budget_rt_active(void) {return nr_gpu_budget_enabled()?(int)(atomic_load(&owner)&~DISC_OWNER):0;}
/* Dynamic discovery partition: large (> 96 PRB) real grants in flight. Independent of ISAC_GPU_BUDGET; end() never goes below 0. */
static _Atomic int large_active;
void nr_gpu_budget_rt_large_begin(void) {atomic_fetch_add(&large_active,1);}
void nr_gpu_budget_rt_large_end(void) {int v=atomic_load(&large_active);while(v>0 && !atomic_compare_exchange_weak(&large_active,&v,v-1));}
int nr_gpu_budget_rt_large_active(void) {return atomic_load(&large_active);}
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
  int rc;uint64_t elapsed,pt=0;
  do {
    rc=query(job);
    if(rc==600)nr_gpu_poll_relax(&pt);
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
  /* Pacing prevents catch-up bursts even after RT has held the device idle. */
  uint64_t paced=slice_start+(us*100+pct-1)/pct;
  atomic_store(&next_us,paced>now?paced:now);
  /* Event intervals can include host descheduling between event/launch calls.
   * Charge the entire interval and report it, but do not turn one slow slice
   * into a permanent synthetic CUDA error on the NEXT upload/kernel. */
  if(us>5000)atomic_fetch_add(&overruns,1);
  atomic_fetch_and(&owner,~DISC_OWNER);
}
/* G4: one prefer flag per cell (the nID sweep of that cell is in ACQUIRE); cell 0 is the single-cell receiver of today. */
#define TK_CELLS 16 /* = NR_CELL_MAX (common/utils/nr_cell.h); this file is in the platform DSO and does not include it */
static _Atomic int prefer[TK_CELLS];
void nr_gpu_budget_discovery_prefer_cell(int cell,int on) {if(cell>=0 && cell<TK_CELLS)atomic_store(&prefer[cell],on);}
void nr_gpu_budget_discovery_prefer(int on) {nr_gpu_budget_discovery_prefer_cell(0,on);}
int nr_gpu_budget_discovery_preferred(void) {
  for(int c=0;c<TK_CELLS;++c)if(atomic_load(&prefer[c]))return 1;
  return 0;
}
/* ---- G4: process-wide single-flight discovery ticket (see nr_gpu_budget.h) ----
 * One mutex/condvar for the whole table. A waiter is served when the ticket is free, the duty pacing allows its class, and it is the best waiter:
 *   1. short holders (FEP, copies, launches: they feed the observation ring and hold for under a chunk) before bulk holders (kernel chunks of the search);
 *   2. cells that prefer (nID ACQUIRE) first;
 *   3. the cell with the least recent discovery time (usage decayed with a one second half life);
 *   4. arrival order.
 * The duty pacing is on the bulk class only and only while two or more cells discovered in the last two seconds: with k such cells the summed bulk time is capped at
 * min(100, k*pct) % of wall time (pct = the discovery share of one cell, 30): after a bulk hold of h us the next bulk grant waits h*(100-cap)/cap us. */
typedef struct tk_waiter {int cell,bulk;uint64_t seq;struct tk_waiter *next;} tk_waiter_t;
static pthread_mutex_t tk_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tk_cv;
static pthread_once_t tk_once=PTHREAD_ONCE_INIT;
static int tk_held,tk_hbulk,tk_hcell;
static uint64_t tk_seq,tk_next_us,tk_grant_us,tk_report_us;
static double tk_usage[TK_CELLS];
static uint64_t tk_usage_t[TK_CELLS],tk_last_use[TK_CELLS];
static tk_waiter_t *tk_wait;
static nr_gpu_disc_ticket_stats_t tk_st[TK_CELLS];
static uint64_t (*tk_clock)(void);
static _Thread_local int tk_depth;
static uint64_t tk_now(void) {return tk_clock?tk_clock():nr_gpu_budget_now_us();}
static void tk_init(void) {
  pthread_condattr_t a;pthread_condattr_init(&a);pthread_condattr_setclock(&a,CLOCK_MONOTONIC);
  pthread_cond_init(&tk_cv,&a);pthread_condattr_destroy(&a);
}
static double tk_usage_at(int c,uint64_t now) {
  /* half life one second: whole seconds halve, the remainder decays linearly (no libm in the platform DSO) */
  const uint64_t dt=now>tk_usage_t[c]?now-tk_usage_t[c]:0;
  double u=tk_usage[c];
  for(uint64_t n=dt/1000000ull;n>0 && u>1.0;--n)u*=0.5;
  return u>1.0?u*(1.0-0.5*(double)(dt%1000000ull)/1e6):0.0;
}
/* tk_before: true when waiter a is served before waiter b (tk_mu held). */
static int tk_before(const tk_waiter_t *a,const tk_waiter_t *b,uint64_t now) {
  if(a->bulk!=b->bulk)return !a->bulk;
  const int pa=atomic_load(&prefer[a->cell]),pb=atomic_load(&prefer[b->cell]);
  if(pa!=pb)return pa>pb;
  if(a->cell!=b->cell) {
    const double ua=tk_usage_at(a->cell,now),ub=tk_usage_at(b->cell,now);
    if(ua!=ub)return ua<ub;
  }
  return a->seq<b->seq;
}
static int tk_best(const tk_waiter_t *me,uint64_t now) {
  for(const tk_waiter_t *w=tk_wait;w;w=w->next)if(w!=me && tk_before(w,me,now))return 0;
  return 1;
}
static void tk_wait_until(uint64_t until_us,uint64_t now) {
  if(until_us>now) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    uint64_t ns=(uint64_t)t.tv_nsec+(until_us-now)*1000ull;
    t.tv_sec+=ns/1000000000ull;t.tv_nsec=ns%1000000000ull;
    pthread_cond_timedwait(&tk_cv,&tk_mu,&t);
  } else pthread_cond_wait(&tk_cv,&tk_mu);
}
/* tk_cap: the cap (percent of wall time) of the summed bulk discovery time of k discovering cells (tk_mu held). Each cell keeps the pct share it has alone (the ticket already
 * serialises them: the interference with the real-time graphs is that of ONE cell), so k cells get min(100, k*pct); and whatever the real-time work leaves idle is free: the
 * cap is never below 100 - (RT-busy share of wall time, measured over the last 250 ms windows, smoothed). RT idle: no pacing at all (replay, quiet cell). */
static double tk_rt_frac;
static uint64_t tk_rt_t,tk_rt_busy;
static unsigned tk_cap(uint64_t now,int active) {
  if(now-tk_rt_t>=250000ull) {
    uint64_t busy=atomic_load(&rt_busy_total);
    if(atomic_load(&owner)&~DISC_OWNER) {const uint64_t since=atomic_load(&rt_busy_since);if(now>since)busy+=now-since;} /* the open interval */
    if(tk_rt_t) {
      uint64_t d=busy>tk_rt_busy?busy-tk_rt_busy:0;
      double f=(double)d/(double)(now-tk_rt_t);if(f>1.0)f=1.0;
      tk_rt_frac=0.5*tk_rt_frac+0.5*f;
    }
    tk_rt_t=now;tk_rt_busy=busy;
  }
  unsigned cap=(unsigned)active*pct>100u?100u:(unsigned)active*pct;
  const unsigned idle=tk_rt_frac>=1.0?0u:(unsigned)(100.0*(1.0-tk_rt_frac));
  return idle>cap?idle:cap;
}
static void tk_acquire(int cell,int bulk) {
  if(tk_depth>0) {++tk_depth;return;}
  if(cell<0 || cell>=TK_CELLS)cell=0;
  pthread_once(&tk_once,tk_init);
  tk_waiter_t w={cell,bulk,0,NULL};
  pthread_mutex_lock(&tk_mu);
  const uint64_t t0=tk_now();
  w.seq=++tk_seq;
  tk_waiter_t **tail=&tk_wait;while(*tail)tail=&(*tail)->next;*tail=&w;
  int waited=0;
  for(;;) {
    const uint64_t now=tk_now();
    uint64_t until=0;
    if(!tk_held) {
      if(bulk && now<tk_next_us)until=tk_next_us;
      else if(tk_best(&w,now))break;
    }
    waited=1;
    tk_wait_until(until,now);
  }
  for(tk_waiter_t **p=&tk_wait;*p;p=&(*p)->next)if(*p==&w) {*p=w.next;break;}
  tk_held=1;tk_hbulk=bulk;tk_hcell=cell;tk_grant_us=tk_now();
  nr_gpu_disc_ticket_stats_t *st=&tk_st[cell];
  const uint64_t wait=tk_grant_us-t0;
  st->grants++;st->wait_us+=wait;if(wait>st->wait_max_us)st->wait_max_us=wait;
  if(waited)st->contended++;
  pthread_mutex_unlock(&tk_mu);
  tk_depth=1;
}
void nr_gpu_disc_ticket_acquire(int cell) {tk_acquire(cell,0);}
/* nr_gpu_disc_ticket_acquire_bulk: as acquire, for a kernel chunk of the search (subject to the duty pacing, served after short holders). */
void nr_gpu_disc_ticket_acquire_bulk(int cell) {tk_acquire(cell,1);}
void nr_gpu_disc_ticket_release(void) {
  if(tk_depth<=0)return;
  if(--tk_depth>0)return;
  pthread_mutex_lock(&tk_mu);
  const uint64_t now=tk_now(),held=now>tk_grant_us?now-tk_grant_us:0;
  const int c=tk_hcell;
  nr_gpu_disc_ticket_stats_t *st=&tk_st[c];
  st->held_us+=held;if(held>st->held_max_us)st->held_max_us=held;
  tk_usage[c]=tk_usage_at(c,now)+(double)held;tk_usage_t[c]=now;tk_last_use[c]=now?now:1;
  int active=0;
  for(int k=0;k<TK_CELLS;++k)if(tk_last_use[k] && now-tk_last_use[k]<2000000ull)++active;
  const unsigned cap=tk_cap(now,active);
  if(tk_hbulk && active>=2 && cap<100 && cap>0) {
    const uint64_t pace=held*(100-cap)/cap;
    tk_next_us=now+pace;st->pace_us+=pace;
  }
  tk_held=0;
  if(active>=2 && now-tk_report_us>=10000000ull) {
    /* always on, only with two or more discovering cells (a single cell prints nothing: N=1 log unchanged) */
    if(tk_report_us) {
      fprintf(stderr,"[GPU] DISC_TICKET active_cells=%d pct=%u rt_busy_pct=%u",active,pct,(unsigned)(100.0*tk_rt_frac));
      for(int k=0;k<TK_CELLS;++k)if(tk_st[k].grants)
        fprintf(stderr," c%d[grants=%lu contended=%lu wait_ms=%lu wait_max_ms=%lu held_ms=%lu held_max_ms=%lu pace_ms=%lu]",k,
                (unsigned long)tk_st[k].grants,(unsigned long)tk_st[k].contended,(unsigned long)(tk_st[k].wait_us/1000),
                (unsigned long)(tk_st[k].wait_max_us/1000),(unsigned long)(tk_st[k].held_us/1000),(unsigned long)(tk_st[k].held_max_us/1000),
                (unsigned long)(tk_st[k].pace_us/1000));
      fputc('\n',stderr);
    }
    tk_report_us=now;
  }
  pthread_cond_broadcast(&tk_cv);
  pthread_mutex_unlock(&tk_mu);
}
void nr_gpu_disc_ticket_stats(int cell,nr_gpu_disc_ticket_stats_t *out) {
  memset(out,0,sizeof(*out));
  if(cell<0 || cell>=TK_CELLS)return;
  pthread_mutex_lock(&tk_mu);*out=tk_st[cell];pthread_mutex_unlock(&tk_mu);
}
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
/* nr_gpu_disc_ticket_test_rt_frac: test-only: pins the measured RT-busy share (0..1) for the next 250 ms window. */
void nr_gpu_disc_ticket_test_rt_frac(double f) {pthread_mutex_lock(&tk_mu);tk_rt_frac=f;tk_rt_t=tk_now();pthread_mutex_unlock(&tk_mu);}
/* nr_gpu_disc_ticket_test_reset: test-only: clears the ticket (no holder, no waiters), the usage, the stats and the prefer flags; pct as the budget's; clock NULL = the real one. */
void nr_gpu_disc_ticket_test_reset(unsigned p,uint64_t (*clock_us)(void)) {
  pthread_once(&tk_once,tk_init);
  pthread_mutex_lock(&tk_mu);
  tk_held=0;tk_next_us=0;tk_report_us=0;tk_wait=NULL;tk_clock=clock_us;pct=p;
  tk_rt_frac=0;tk_rt_t=0;tk_rt_busy=0;atomic_store(&rt_busy_total,0);atomic_store(&rt_busy_since,0);
  memset(tk_usage,0,sizeof(tk_usage));memset(tk_usage_t,0,sizeof(tk_usage_t));memset(tk_last_use,0,sizeof(tk_last_use));memset(tk_st,0,sizeof(tk_st));
  for(int c=0;c<TK_CELLS;++c)atomic_store(&prefer[c],0);
  pthread_mutex_unlock(&tk_mu);
}
/* nr_gpu_budget_test_reset: test-only (NR_GPU_BUDGET_TEST): enables the budget with the given percentage and clears the window, owner, fault and counters. Caller: openair1/PHY/TOOLS/tests/nr_gpu_budget_test.cc. */
void nr_gpu_budget_test_reset(unsigned p) {
  nr_gpu_budget_enabled();enabled=1;pct=p;
  memset(buckets,0,sizeof(buckets));window_tick=window_used=0;
  atomic_store(&owner,0);atomic_store(&fault,0);atomic_store(&next_us,0);
  atomic_store(&used_us,0);atomic_store(&yields,0);atomic_store(&wait_max,0);
  atomic_store(&fallbacks,0);atomic_store(&overruns,0);
}
#endif
