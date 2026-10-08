/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_EVIDENCE_INTERNAL_H
#define NR_CSIRS_DISC_EVIDENCE_INTERNAL_H
#include "nr_csirs_disc.h"
#include <string.h>

/* Full ID vectors, never a history of selected maxima. Exact geometry/lattice
 * keys prevent extent jitter or burst phases from sharing statistical credit.
 * Capacity matches the pipeline rejection cache: entries must survive cooldown.
 * This metadata is worker-owned (GPU callers already hold workspace_mutex). */
#define NR_CSIRS_EVIDENCE_BANK 256
#define NR_CSIRS_EVIDENCE_IDS 1024
#define NR_CSIRS_EVIDENCE_SLOTS 32
typedef struct {
  nr_csirs_disc_hyp_t h;
  uint32_t last_slot;
  uint16_t count;
  uint64_t touched;
  bool used;
} nr_csirs_disc_evidence_entry_t;
typedef struct {
  nr_csirs_disc_evidence_entry_t entry[NR_CSIRS_EVIDENCE_BANK];
  const int16_t *grid;
  uint64_t epoch, written, tick;
  int rb, ant, spf;
} nr_csirs_disc_evidence_t;

static inline bool nr_csirs_disc_evidence_key(const nr_csirs_disc_hyp_t *a, const nr_csirs_disc_hyp_t *b)
{
  return a->row==b->row && a->freq_domain==b->freq_domain && a->symb_l0==b->symb_l0
      && a->symb_l1==b->symb_l1 && a->cdm_type==b->cdm_type && a->density==b->density
      && a->rb_start==b->rb_start && a->n_rb==b->n_rb && a->period==b->period && a->offset==b->offset;
}

static inline int nr_csirs_disc_evidence_begin(nr_csirs_disc_evidence_t *bank,
    const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *h, const nr_csirs_disc_sweep_cfg_t *c)
{
  /* General delta/multiport searches preserve the original path. */
  if (!c->evidence_epoch || r->capacity<2 || h->row!=1 || c->id_first || c->id_count!=1024 || c->delta_max!=1
      || c->block_re!=32 || c->max_slots<1 || c->max_slots>4)
    return -1;
  if (bank->grid!=r->grid_base || bank->epoch!=c->evidence_epoch || r->written<bank->written
      || bank->rb!=r->n_rb || bank->ant!=r->n_ant || bank->spf!=r->slots_per_frame) {
    memset(bank,0,sizeof(*bank));
    bank->grid=r->grid_base; bank->epoch=c->evidence_epoch;
    bank->rb=r->n_rb; bank->ant=r->n_ant; bank->spf=r->slots_per_frame;
  }
  bank->written=r->written;
  uint32_t latest=0;
  for (int i=0;i<r->capacity;++i)
    if (r->meta[i].valid && r->meta[i].abs_slot>latest) latest=r->meta[i].abs_slot;
  int at=-1, oldest=0;
  for (int i=0;i<NR_CSIRS_EVIDENCE_BANK;++i) {
    if (bank->entry[i].used && nr_csirs_disc_evidence_key(h,&bank->entry[i].h)) {at=i;break;}
    if (bank->entry[i].touched<bank->entry[oldest].touched) oldest=i;
  }
  if (at<0) {
    at=oldest;
    memset(&bank->entry[at],0,sizeof(bank->entry[at]));
    bank->entry[at].h=*h; bank->entry[at].used=true;
  }
  nr_csirs_disc_evidence_entry_t *e=&bank->entry[at];
  /* Bounded evidence lifetime, including after long holes or clock rollback. */
  if (e->count && (latest<e->last_slot || latest-e->last_slot>512u*h->period)) {
    e->count=0; e->last_slot=0;
  }
  e->touched=++bank->tick;
  return at;
}

static inline bool nr_csirs_disc_evidence_fresh(const nr_csirs_disc_evidence_entry_t *e, uint32_t slot)
{
  return !e || !e->count || slot>e->last_slot;
}

/* Called only after fresh slots were selected. Full blocks restart rather than
 * giving an arbitrarily old signal an unbounded lifetime. Never reread old IQ. */
static inline int nr_csirs_disc_evidence_previous(nr_csirs_disc_evidence_entry_t *e, int fresh)
{
  return e && e->count+fresh<=NR_CSIRS_EVIDENCE_SLOTS ? e->count : 0;
}
#endif
