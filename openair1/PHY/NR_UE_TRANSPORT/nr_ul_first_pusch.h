#ifndef NR_UL_FIRST_PUSCH_H
#define NR_UL_FIRST_PUSCH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nr_passive_ue_ctx.h"

static inline bool nr_ul_chain_on(void)
{
  const char *v = getenv("ISAC_UL_PUSCH_CHAIN");
  const char *s = getenv("ISAC_UL_PUSCH_SOLVER");
  return (v && strcmp(v, "1") == 0) || (s && strcmp(s, "1") == 0);
}

/* Caller serializes access. Bounded per-UE milestone state, never a CRC oracle. */
#define NR_UL_FIRST_UE_CAP 64
typedef struct {
  nr_ue_ul_ticket_t ticket;
  int64_t first_slot;
  bool crc_ok;
} nr_ul_first_entry_t;
typedef struct {
  nr_ul_first_entry_t ue[NR_UL_FIRST_UE_CAP];
  uint64_t seen;
} nr_ul_first_state_t;

static inline bool nr_ul_first_same(const nr_ue_ul_ticket_t *a, const nr_ue_ul_ticket_t *b)
{
  return a->rnti == b->rnti && a->identity_gen == b->identity_gen &&
         a->incarnation == b->incarnation && a->epoch == b->epoch;
}

static inline bool nr_ul_first_note(nr_ul_first_state_t *s, const nr_ue_ul_ticket_t *t, int64_t slot)
{
  if (!s || !t || !t->rnti || slot < 0) return false;
  int free_idx = -1;
  for (int i = 0; i < NR_UL_FIRST_UE_CAP; ++i) {
    if (nr_ul_first_same(&s->ue[i].ticket, t)) return false;
    if (free_idx < 0 && (!s->ue[i].ticket.rnti || s->ue[i].ticket.rnti == t->rnti)) free_idx = i;
  }
  if (free_idx < 0) free_idx = (int)(s->seen % NR_UL_FIRST_UE_CAP);
  s->ue[free_idx] = (nr_ul_first_entry_t){.ticket=*t, .first_slot=slot};
  ++s->seen;
  return true;
}

static inline bool nr_ul_first_crc_ok(nr_ul_first_state_t *s, const nr_ue_ul_ticket_t *t,
                                       int64_t target_slot, int64_t *elapsed_slots)
{
  if (!s || !t || !elapsed_slots) return false;
  for (int i = 0; i < NR_UL_FIRST_UE_CAP; ++i) {
    nr_ul_first_entry_t *e = &s->ue[i];
    if (!e->ticket.rnti || !nr_ul_first_same(&e->ticket, t)) continue;
    if (e->crc_ok || target_slot < e->first_slot) return false;
    e->crc_ok = true;
    *elapsed_slots = target_slot - e->first_slot;
    return true;
  }
  return false;
}
#endif
