/* Receiver-owned continuity state: acquisition/rebase consumes untracked samples,
 * so it starts a new epoch. Ordinary adjacent slot reads must stay contiguous. */
#ifndef NR_RX_CONTINUITY_H
#define NR_RX_CONTINUITY_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  uint64_t next_timestamp;
  bool valid;
} nr_rx_continuity_t;
static inline void nr_rx_continuity_reset(nr_rx_continuity_t *s)
{
  s->valid=false;
}
static inline bool nr_rx_continuity_check(const nr_rx_continuity_t *s, uint64_t timestamp)
{
  return !s->valid || timestamp==s->next_timestamp;
}
static inline void nr_rx_continuity_commit(nr_rx_continuity_t *s, uint64_t timestamp, long consumed)
{
  s->valid=consumed>0;
  s->next_timestamp=timestamp+(consumed>0?(uint64_t)consumed:0);
}
#endif
