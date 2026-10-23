/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_mpci_sched.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Lock-free (not a robust mutex): a process killed mid-update can never wedge the others, and every field is a
 * single-writer counter or a CAS-claimed slot. Cost: readers see slightly torn aggregates, harmless for a policy
 * that is re-evaluated every slice. */
#define MAGIC 0x4d504349u
#define MAINTAIN_DIV 4
#define NB 100                 /* 10 ms buckets = 1 s rolling window */
#define BUCKET_US 10000
#define DEMAND_US 200000       /* a cell asking for discovery within this is "demanding" */
#define HB_STALE_US 2000000    /* RT in flight older than this without a heartbeat is a hung cell: ignored */
typedef struct {
  _Atomic uint64_t tick, us;
} bucket_t;
typedef struct {
  _Atomic uint32_t pid, share, acquire, rt_inflight;
  char tag[24];
  _Atomic uint64_t hb_us, last_req_us, total_us, yields, rt_jobs;
  _Atomic uint64_t drops[7];
  bucket_t b[NB];
} slot_t;
typedef struct {
  _Atomic uint32_t magic;
  uint32_t disc_pct;
  slot_t s[NR_MPCI_MAX_CELLS];
} seg_t;
struct nr_mpci {
  seg_t *seg;
  int me; /* -1: read-only */
};

static uint64_t slot_used(slot_t *s,uint64_t now) {
  uint64_t tk=now/BUCKET_US,sum=0;
  for(int i=0;i<NB;i++) {
    uint64_t t=atomic_load(&s->b[i].tick);
    if(t+NB>tk && t<=tk)sum+=atomic_load(&s->b[i].us);
  }
  return sum;
}
static int alive(slot_t *s,uint32_t pid) {return pid && (kill((pid_t)pid,0)==0 || errno==EPERM);}
/* Frees the slot if its process is gone; returns the pid still owning it (0 = free). */
static uint32_t reap(slot_t *s) {
  uint32_t pid=atomic_load(&s->pid);
  if(pid && !alive(s,pid) && atomic_compare_exchange_strong(&s->pid,&pid,0)) {
    atomic_store(&s->rt_inflight,0);atomic_store(&s->acquire,0);return 0;
  }
  return atomic_load(&s->pid);
}
static nr_mpci_t *map(const char *name,int create,unsigned disc_pct,int *created) {
  char path[96];snprintf(path,sizeof(path),"/%s",name);
  int fd=-1;*created=0;
  if(create) {
    fd=shm_open(path,O_RDWR|O_CREAT|O_EXCL,0666);
    if(fd>=0) {*created=1;if(ftruncate(fd,sizeof(seg_t))) {close(fd);shm_unlink(path);return NULL;}}
  }
  if(fd<0)fd=shm_open(path,create?O_RDWR:O_RDONLY,0666);
  if(fd<0)return NULL;
  seg_t *seg=mmap(NULL,sizeof(seg_t),create?PROT_READ|PROT_WRITE:PROT_READ,MAP_SHARED,fd,0);
  close(fd);
  if(seg==MAP_FAILED)return NULL;
  if(*created) {
    seg->disc_pct=disc_pct;atomic_store(&seg->magic,MAGIC);
  } else for(int i=0;i<1000 && atomic_load(&seg->magic)!=MAGIC;i++)usleep(1000);
  nr_mpci_t *h=calloc(1,sizeof(*h));
  if(!h || atomic_load(&seg->magic)!=MAGIC) {munmap(seg,sizeof(seg_t));free(h);return NULL;}
  h->seg=seg;h->me=-1;return h;
}
nr_mpci_t *nr_mpci_attach(const char *name,const char *tag,unsigned share,unsigned disc_pct) {
  int created;nr_mpci_t *h=map(name,1,disc_pct,&created);
  if(!h)return NULL;
  uint32_t pid=(uint32_t)getpid();
  for(int i=0;i<NR_MPCI_MAX_CELLS;i++) {
    slot_t *s=&h->seg->s[i];reap(s);
    uint32_t z=0;
    if(!atomic_compare_exchange_strong(&s->pid,&z,0xffffffffu))continue; /* claimed while we initialise */
    memset(s->tag,0,sizeof(s->tag));snprintf(s->tag,sizeof(s->tag),"%s",tag&&*tag?tag:"cell");
    atomic_store(&s->share,share?share:1);atomic_store(&s->acquire,0);atomic_store(&s->rt_inflight,0);
    atomic_store(&s->hb_us,0);atomic_store(&s->last_req_us,0);atomic_store(&s->total_us,0);
    atomic_store(&s->yields,0);atomic_store(&s->rt_jobs,0);
    for(int k=0;k<7;k++)atomic_store(&s->drops[k],0);
    for(int k=0;k<NB;k++) {atomic_store(&s->b[k].tick,0);atomic_store(&s->b[k].us,0);}
    atomic_store(&s->pid,pid);h->me=i;return h;
  }
  munmap(h->seg,sizeof(seg_t));free(h);return NULL; /* segment full */
}
nr_mpci_t *nr_mpci_attach_ro(const char *name) {int c;return map(name,0,0,&c);}
void nr_mpci_detach(nr_mpci_t *h) {
  if(!h)return;
  if(h->me>=0) {
    slot_t *s=&h->seg->s[h->me];
    atomic_store(&s->rt_inflight,0);atomic_store(&s->pid,0);
  }
  munmap(h->seg,sizeof(seg_t));free(h);
}
static unsigned envu(const char *n,unsigned def,unsigned lo,unsigned hi) {
  const char *s=getenv(n);if(!s||!*s)return def;
  char *e;long v=strtol(s,&e,10);return *e||v<(long)lo||v>(long)hi?def:(unsigned)v;
}
static nr_mpci_t *g_default;
static pthread_once_t g_once=PTHREAD_ONCE_INIT;
static void default_init(void) {
  const char *name=getenv("ISAC_MPCI_SCHED");
  if(!name||!*name)return;
  const char *tag=getenv("ISAC_CELL_INSTANCE");
  if(!tag||!*tag)tag=getenv("ISAC_SHM_IQ_CELL");
  char buf[24];
  if(!tag||!*tag) {snprintf(buf,sizeof(buf),"pid%d",(int)getpid());tag=buf;}
  g_default=nr_mpci_attach(name,tag,envu("ISAC_MPCI_GPU_SHARE_PCT",100,1,1000),
                           /* default = the local budget's own setting, read here so the first caller (budget or metrics) cannot change it */
                           envu("ISAC_MPCI_GPU_DISCOVERY_PCT",envu("ISAC_GPU_DISCOVERY_BUDGET_PCT",30,1,100),1,100));
  if(!g_default)fprintf(stderr,"MPCI_SCHED_ATTACH_FAILED name=%s\n",name);
  else fprintf(stderr,"MPCI_SCHED_ATTACHED name=%s tag=%s slot=%d\n",name,tag,g_default->me);
}
nr_mpci_t *nr_mpci_default(unsigned def_pct) {
  if(!getenv("ISAC_MPCI_SCHED"))return NULL;
  (void)def_pct;pthread_once(&g_once,default_init);return g_default;
}
int nr_mpci_discovery_admit(nr_mpci_t *h,uint64_t now,int acquire) {
  if(!h||h->me<0)return 1;
  seg_t *g=h->seg;slot_t *me=&g->s[h->me];
  atomic_store(&me->hb_us,now);atomic_store(&me->last_req_us,now);atomic_store(&me->acquire,acquire!=0);
  uint64_t G=(uint64_t)g->disc_pct*10000,total=0,sum_share=0,mine=0;
  uint32_t my_share=atomic_load(&me->share);
  struct {uint64_t used,share;int acq,dem,self;} c[NR_MPCI_MAX_CELLS];
  for(int i=0;i<NR_MPCI_MAX_CELLS;i++) {
    slot_t *s=&g->s[i];c[i].dem=0;
    uint32_t pid=atomic_load(&s->pid);
    if(!pid||pid==0xffffffffu)continue;
    if(i!=h->me) {
      uint64_t hb=atomic_load(&s->hb_us);
      int stale=now>hb && now-hb>HB_STALE_US;
      if(stale && !reap(s))continue;
      /* Another cell's RT in flight blocks new discovery everywhere (cross-cell RT priority). */
      if(!stale && atomic_load(&s->rt_inflight)){atomic_fetch_add(&me->yields,1);return 0;}
    }
    c[i].used=slot_used(s,now);total+=c[i].used;
    uint64_t lr=atomic_load(&s->last_req_us);
    c[i].self=i==h->me;
    c[i].dem=c[i].self||(lr && now>=lr && now-lr<DEMAND_US);
    c[i].share=atomic_load(&s->share);c[i].acq=atomic_load(&s->acquire);
    if(c[i].dem)sum_share+=c[i].share;
  }
  mine=c[h->me].used;
  uint64_t reserve=0,acq_others=0;
  for(int i=0;i<NR_MPCI_MAX_CELLS;i++) {
    if(!c[i].dem||c[i].self)continue;
    uint64_t ent=G*c[i].share/sum_share;
    /* Work-conserving borrowing: only the still-unused share of cells that really ask is held back.
     * An ACQUIRE cell does not hold back for MAINTAIN cells; a MAINTAIN cell leaves one slice per ACQUIRE cell. */
    if(c[i].acq)acq_others++;
    if(acquire && !c[i].acq)continue;
    if(c[i].used<ent)reserve+=ent-c[i].used;
  }
  uint64_t ent_me=G*my_share/sum_share;
  int ok=total+NR_MPCI_SLICE_US<=G;
  /* While another cell is in ACQUIRE a MAINTAIN cell is confined to 1/MAINTAIN_DIV of its entitlement, no borrowing. */
  if(ok && !acquire && acq_others)ok=mine+NR_MPCI_SLICE_US<=ent_me/MAINTAIN_DIV;
  else if(ok)ok=mine+NR_MPCI_SLICE_US<=ent_me||total+NR_MPCI_SLICE_US+reserve<=G;
  if(!ok)atomic_fetch_add(&me->yields,1);
  return ok;
}
void nr_mpci_discovery_charge(nr_mpci_t *h,uint64_t now,uint64_t us) {
  if(!h||h->me<0)return;
  slot_t *s=&h->seg->s[h->me];uint64_t tk=now/BUCKET_US;bucket_t *b=&s->b[tk%NB];
  if(atomic_load(&b->tick)!=tk) {atomic_store(&b->us,0);atomic_store(&b->tick,tk);}
  atomic_fetch_add(&b->us,us);atomic_fetch_add(&s->total_us,us);atomic_store(&s->hb_us,now);
}
void nr_mpci_rt_begin(nr_mpci_t *h,uint64_t now) {
  if(!h||h->me<0)return;
  slot_t *s=&h->seg->s[h->me];atomic_fetch_add(&s->rt_inflight,1);atomic_fetch_add(&s->rt_jobs,1);atomic_store(&s->hb_us,now);
}
void nr_mpci_rt_end(nr_mpci_t *h) {
  if(!h||h->me<0)return;
  slot_t *s=&h->seg->s[h->me];
  uint32_t v=atomic_load(&s->rt_inflight);
  while(v && !atomic_compare_exchange_weak(&s->rt_inflight,&v,v-1)) {}
}
void nr_mpci_publish_drops(nr_mpci_t *h,const nr_mpci_drops_t *d) {
  if(!h||h->me<0)return;
  slot_t *s=&h->seg->s[h->me];const uint64_t v[7]={d->scanq_full,d->scanq_stale,d->scanq_epoch,d->pdschq_full,d->pdschq_stale,d->pdschq_epoch,d->pdschq_narrow};
  for(int i=0;i<7;i++)atomic_store(&s->drops[i],v[i]);
}
int nr_mpci_snapshot(nr_mpci_t *h,uint64_t now,nr_mpci_cell_stat_t *out,int max) {
  int n=0;
  for(int i=0;i<NR_MPCI_MAX_CELLS && n<max;i++) {
    slot_t *s=&h->seg->s[i];
    uint32_t pid=h->me>=0?reap(s):atomic_load(&s->pid);
    if(h->me<0 && pid && !alive(s,pid))pid=0; /* read-only: report only */
    if(!pid||pid==0xffffffffu)continue;
    nr_mpci_cell_stat_t *o=&out[n++];
    o->slot=i;o->pid=(int)pid;o->share=atomic_load(&s->share);o->acquire=atomic_load(&s->acquire);
    o->rt_inflight=atomic_load(&s->rt_inflight);memcpy(o->tag,s->tag,sizeof(o->tag));o->tag[sizeof(o->tag)-1]=0;
    o->window_used_us=slot_used(s,now);o->total_used_us=atomic_load(&s->total_us);
    o->yields=atomic_load(&s->yields);o->rt_jobs=atomic_load(&s->rt_jobs);
    uint64_t hb=atomic_load(&s->hb_us);o->heartbeat_age_us=now>hb?now-hb:0;
    o->drops=(nr_mpci_drops_t){atomic_load(&s->drops[0]),atomic_load(&s->drops[1]),atomic_load(&s->drops[2]),
      atomic_load(&s->drops[3]),atomic_load(&s->drops[4]),atomic_load(&s->drops[5]),atomic_load(&s->drops[6])};
  }
  return n;
}
