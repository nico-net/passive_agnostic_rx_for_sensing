/* Bounded, measurement-driven ONLINE learning of the UCI-on-PUSCH footprint.
 *
 * nr_passive_uci_probe.h can invert one rate-matched UCI component, but its offline sweep tries
 * every RE count up to 1024 coded bits, which is far too expensive to run on a live consumer for
 * every failed grant. The observation that makes a live policy affordable: the footprint is set by
 * the UE's report configuration and beta offsets, so it REPEATS. Measured offline on this cell,
 * only a handful of distinct values appeared (ACK 17 and 22 RE, CSI 38 and 33 RE).
 *
 * So: remember what worked, try those first, and pay for a wide search only occasionally. A cached
 * footprint costs a couple of LDPC attempts; exploration is rate-limited so its cost is amortised
 * over many grants rather than paid per failure.
 *
 * What this does NOT claim: a recovered RE count is an inferred footprint, not a known O_ACK, CSI
 * report size, beta offset, or any proof about the DCI widths. Combined ACK+CSI layouts, small-ACK
 * puncturing and PTRS are outside the single-component inverse this builds on.
 */
#ifndef NR_PASSIVE_UCI_LEARN_H
#define NR_PASSIVE_UCI_LEARN_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#define NR_UCI_LEARN_UES        16 /* distinct RNTIs tracked */
#define NR_UCI_LEARN_FOOTPRINTS  4 /* remembered per RNTI, best-first */

typedef struct {
  uint16_t re;
  uint8_t  csi;
  uint32_t hits;
} nr_uci_footprint_t;

typedef struct {
  uint16_t rnti;
  bool     used;
  uint64_t failures;                 /* CRC failures seen, drives the exploration rate limit */
  uint64_t touched;
  nr_uci_footprint_t fp[NR_UCI_LEARN_FOOTPRINTS];
  int      n_fp;
} nr_uci_learn_ue_t;

static nr_uci_learn_ue_t g_uci_learn[NR_UCI_LEARN_UES];
static pthread_mutex_t   g_uci_learn_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t          g_uci_learn_clock;

static inline nr_uci_learn_ue_t *nr_uci_learn_slot(uint16_t rnti)
{
  nr_uci_learn_ue_t *oldest = &g_uci_learn[0];
  for (int i = 0; i < NR_UCI_LEARN_UES; i++) {
    if (g_uci_learn[i].used && g_uci_learn[i].rnti == rnti) return &g_uci_learn[i];
    if (g_uci_learn[i].touched < oldest->touched) oldest = &g_uci_learn[i];
  }
  memset(oldest, 0, sizeof(*oldest));
  oldest->used = true;
  oldest->rnti = rnti;
  return oldest;
}

/** Copy this RNTI's known footprints, most successful first. Returns how many were written. */
static inline int nr_passive_uci_learn_get(uint16_t rnti, nr_uci_footprint_t *out, int max)
{
  if (!out || max <= 0) return 0;
  pthread_mutex_lock(&g_uci_learn_lock);
  nr_uci_learn_ue_t *u = nr_uci_learn_slot(rnti);
  u->touched = ++g_uci_learn_clock;
  int n = u->n_fp < max ? u->n_fp : max;
  for (int i = 0; i < n; i++) out[i] = u->fp[i];
  pthread_mutex_unlock(&g_uci_learn_lock);
  return n;
}

/** Record a footprint that produced a verified transport-block CRC, and keep the list best-first. */
static inline void nr_passive_uci_learn_record(uint16_t rnti, uint16_t re, bool csi)
{
  pthread_mutex_lock(&g_uci_learn_lock);
  nr_uci_learn_ue_t *u = nr_uci_learn_slot(rnti);
  u->touched = ++g_uci_learn_clock;
  int at = -1;
  for (int i = 0; i < u->n_fp; i++)
    if (u->fp[i].re == re && u->fp[i].csi == (csi ? 1 : 0)) { at = i; break; }
  if (at < 0) {
    /* Evict the least successful entry rather than the oldest: a footprint that keeps working is
     * worth more than a recent one that worked once. */
    if (u->n_fp < NR_UCI_LEARN_FOOTPRINTS) {
      at = u->n_fp++;
    } else {
      at = 0;
      for (int i = 1; i < u->n_fp; i++) if (u->fp[i].hits < u->fp[at].hits) at = i;
    }
    u->fp[at] = (nr_uci_footprint_t){.re = re, .csi = (uint8_t)(csi ? 1 : 0), .hits = 0};
  }
  u->fp[at].hits++;
  for (int i = at; i > 0 && u->fp[i].hits > u->fp[i - 1].hits; i--) {
    nr_uci_footprint_t t = u->fp[i]; u->fp[i] = u->fp[i - 1]; u->fp[i - 1] = t;
  }
  pthread_mutex_unlock(&g_uci_learn_lock);
}

/** Rate limiter: true on one in `every` CRC failures for this RNTI, so a wide sweep is amortised.
 *  Always true for the first failure, so a UE with no cache can acquire one immediately. */
static inline bool nr_passive_uci_learn_should_explore(uint16_t rnti, unsigned every)
{
  if (!every) return false;
  pthread_mutex_lock(&g_uci_learn_lock);
  nr_uci_learn_ue_t *u = nr_uci_learn_slot(rnti);
  u->touched = ++g_uci_learn_clock;
  /* An identity must EARN a sweep. The first version gave every unknown RNTI a free exploration,
   * which is ruinous when the UL scan runs a wide RNTI range: transient noise-accepted identities
   * each bought a full sweep. Measured live: 354,119 demux+LDPC attempts for 9 rescues, ~95 per
   * grant against the ~4 intended, which starved the UL consumers and dragged health from 36.5 %
   * down to 16.1 %. Requiring `every` failures first means only a persistent identity -- a real
   * UE -- ever pays for exploration. */
  const bool go = u->failures >= every && (u->failures % every) == 0;
  u->failures++;
  pthread_mutex_unlock(&g_uci_learn_lock);
  return go;
}

static inline void nr_passive_uci_learn_reset(void)
{
  pthread_mutex_lock(&g_uci_learn_lock);
  memset(g_uci_learn, 0, sizeof(g_uci_learn));
  g_uci_learn_clock = 0;
  pthread_mutex_unlock(&g_uci_learn_lock);
}
#endif
