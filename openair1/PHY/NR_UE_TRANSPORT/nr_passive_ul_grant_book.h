/* Bounded multi-UE grant storage. Caller serializes access. */
#ifndef NR_PASSIVE_UL_GRANT_BOOK_H
#define NR_PASSIVE_UL_GRANT_BOOK_H
#include "nr_pdcch_blind_monitor.h"
#include "nr_passive_sample_lifetime.h"
#include <string.h>
#define NR_PASSIVE_UL_BOOK_CAPACITY 256
typedef struct {
  nr_pdcch_blind_ul_result_t grant;
  long target;
  bool valid;
} nr_passive_ul_book_entry_t;
typedef struct { nr_passive_ul_book_entry_t entry[NR_PASSIVE_UL_BOOK_CAPACITY]; } nr_passive_ul_book_t;
/* 1=stored, 0=duplicate, -1=full/invalid. Never overwrite another UE silently. */
static inline int nr_passive_ul_book_put(nr_passive_ul_book_t *b,
    const nr_pdcch_blind_ul_result_t *g, long source) {
  if(!b || !g || !g->plausible || source<0) return -1;
  long target=source+g->k2;
  int free_slot=-1;
  for(int i=0;i<NR_PASSIVE_UL_BOOK_CAPACITY;i++) {
    if(!b->entry[i].valid) {if(free_slot<0) free_slot=i; continue;}
    const nr_passive_ul_book_entry_t *e=&b->entry[i];
    if(e->target==target && e->grant.rnti==g->rnti
        && nr_dci_bits_eq(&e->grant.raw_payload, &g->raw_payload)) return 0;
  }
  if(free_slot<0) return -1;
  b->entry[free_slot]=(nr_passive_ul_book_entry_t){.grant=*g,.target=target,.valid=true};
  return 1;
}
/* Return all due grants, including late arrivals while their IQ still exists.
 * Expired jobs disappear without becoming negative CRC evidence. */
static inline bool nr_passive_ul_book_take(nr_passive_ul_book_t *b, long now, long spf,
    nr_passive_ul_book_entry_t *out, unsigned *expired) {
  int chosen=-1;
  for(int i=0;i<NR_PASSIVE_UL_BOOK_CAPACITY;i++) {
    nr_passive_ul_book_entry_t *e=&b->entry[i];
    if(!e->valid || e->target>now) continue;
    if(!nr_passive_samples_valid(now,e->target,spf)) {
      e->valid=false; if(expired) ++*expired; continue;
    }
    if(chosen<0 || e->target<b->entry[chosen].target) chosen=i;
  }
  if(chosen<0) return false;
  *out=b->entry[chosen];b->entry[chosen].valid=false;
  return true;
}
#endif
