/* SCAN2 slot-direction filter for TDD cells whose pattern SIB1 does not give (SIB1-less NSA anchors): learn which slots
 * of the frame never carry a PDCCH from CRC-valid DCI evidence, skip those scan passes, keep verifying.
 * Header-only, no allocation, GCC __atomic builtins on plain integers so C and C++ both include it (N scan consumers). Never hard-codes a pattern: the evidence is per slot of
 * the frame, so any pattern (2.5 / 5 / 10 ms, dual period) is covered.
 *
 * Rule (recomputed every NR_TDD_LEARN_RECALC scanned passes):
 *  - the cell must be active: >= 1/4 of the frame's slots hold >= NR_TDD_LEARN_STRONG evidence events;
 *  - a slot is a UL candidate only if it has ZERO evidence although, at the weakest strong slot's rate, >= NR_TDD_LEARN_MISS
 *    events would have been expected in its observations (P[miss | DL] <= e^-20);
 *  - the first candidate after a strong (plain DL) slot stays scanned: it may be the special/flexible slot (see decide());
 *  - if more than 3/4 of the frame would be skipped the result is discarded (implausible pattern, idle cell);
 *  - one in NR_TDD_LEARN_VERIFY passes (hashed) of a skipped slot is scanned anyway; one evidence event un-skips it for good. */
#ifndef NR_TDD_LEARN_H
#define NR_TDD_LEARN_H
#include <stdbool.h>
#include <stdint.h>

#define NR_TDD_LEARN_MAX_SLOTS 160
#define NR_TDD_LEARN_STRONG 50
#define NR_TDD_LEARN_MISS 20.0
#define NR_TDD_LEARN_VERIFY 8
#define NR_TDD_LEARN_RECALC 512

typedef struct {
  uint32_t obs[NR_TDD_LEARN_MAX_SLOTS];   /* scanned passes (verification passes included) */
  uint32_t ev[NR_TDD_LEARN_MAX_SLOTS];    /* CRC-valid DCI events seen in those passes */
  uint32_t seen[NR_TDD_LEARN_MAX_SLOTS];  /* passes offered while skipped (verification counter) */
  uint8_t skip[NR_TDD_LEARN_MAX_SLOTS];
  uint32_t calls;
  uint64_t n_skipped, n_verify, n_unskip;
} nr_tdd_learn_t;

/* Pure: skip[] from the counters. Returns the number of skipped slots.
 * Classes (rate_ref = weakest strong slot's evidence rate): STRONG ev >= STRONG; ZERO ev == 0 with obs*rate_ref >= MISS;
 * SPECIAL ev > 0, rare (rate < rate_ref/4) with obs*rate_ref >= MISS; everything else UNDETERMINED (still learning).
 * A ZERO slot is skipped only when its predecessor in the frame (cyclic) is SPECIAL or ZERO; after a STRONG slot it stays
 * scanned (possible special / flexible slot), after an UNDETERMINED one it stays scanned (not enough evidence yet). */
static inline int nr_tdd_learn_decide(const uint32_t *obs, const uint32_t *ev, int spf, uint8_t *skip)
{
  enum { STRONG, ZERO, SPECIAL, UNDET };
  for (int s = 0; s < spf; s++) skip[s] = 0;
  int strong = 0;
  double rate_ref = 1e9;
  for (int s = 0; s < spf; s++)
    if (ev[s] >= NR_TDD_LEARN_STRONG && obs[s] > 0) {
      strong++;
      const double r = (double)ev[s] / (double)obs[s];
      if (r < rate_ref) rate_ref = r;
    }
  if (strong * 4 < spf) return 0;
  uint8_t cls[NR_TDD_LEARN_MAX_SLOTS];
  for (int s = 0; s < spf; s++) {
    const bool enough = (double)obs[s] * rate_ref >= NR_TDD_LEARN_MISS;
    cls[s] = ev[s] >= NR_TDD_LEARN_STRONG ? STRONG
           : (ev[s] == 0 && enough) ? ZERO
           : (ev[s] > 0 && enough && (double)ev[s] < 0.25 * rate_ref * (double)obs[s]) ? SPECIAL : UNDET;
  }
  int n = 0;
  for (int s = 0; s < spf; s++) {
    if (cls[s] != ZERO) continue;
    const int pc = cls[(s + spf - 1) % spf];
    if (pc == SPECIAL || pc == ZERO) {
      skip[s] = 1;
      n++;
    }
  }
  if (n * 4 > spf * 3) {
    for (int s = 0; s < spf; s++) skip[s] = 0;
    return 0;
  }
  return n;
}

/* Called once per scan pass with the slot of the frame; true = skip this pass. */
static inline bool nr_tdd_learn_skip(nr_tdd_learn_t *t, int spf, int slot)
{
  if (spf <= 0 || spf > NR_TDD_LEARN_MAX_SLOTS || slot < 0 || slot >= spf) return false;
  if (__atomic_load_n(&t->skip[slot], __ATOMIC_RELAXED)) {
    const uint32_t k = __atomic_fetch_add(&t->seen[slot], 1, __ATOMIC_RELAXED) + 1;
    /* hashed, not k % V: the pass kinds of a slot (CSS0, banks, dwell) cycle, a fixed stride could always land on one kind */
    if (((k * 0x9E3779B1u) >> 24) % NR_TDD_LEARN_VERIFY) {
      __atomic_fetch_add(&t->n_skipped, 1, __ATOMIC_RELAXED);
      return true;
    }
    __atomic_fetch_add(&t->n_verify, 1, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&t->obs[slot], 1, __ATOMIC_RELAXED);
  if ((__atomic_fetch_add(&t->calls, 1, __ATOMIC_RELAXED) + 1) % NR_TDD_LEARN_RECALC == 0) {
    uint32_t o[NR_TDD_LEARN_MAX_SLOTS], e[NR_TDD_LEARN_MAX_SLOTS];
    uint8_t d[NR_TDD_LEARN_MAX_SLOTS];
    for (int s = 0; s < spf; s++) {
      o[s] = __atomic_load_n(&t->obs[s], __ATOMIC_RELAXED);
      e[s] = __atomic_load_n(&t->ev[s], __ATOMIC_RELAXED);
    }
    nr_tdd_learn_decide(o, e, spf, d);
    for (int s = 0; s < spf; s++) __atomic_store_n(&t->skip[s], d[s] && e[s] == 0, __ATOMIC_RELAXED);
  }
  return false;
}

/* Evidence: a CRC-valid DCI was accepted in this slot of the frame. Un-skips the slot immediately. */
static inline void nr_tdd_learn_evidence(nr_tdd_learn_t *t, int spf, int slot)
{
  if (spf <= 0 || spf > NR_TDD_LEARN_MAX_SLOTS || slot < 0 || slot >= spf) return;
  __atomic_fetch_add(&t->ev[slot], 1, __ATOMIC_RELAXED);
  if (__atomic_exchange_n(&t->skip[slot], 0, __ATOMIC_RELAXED))
    __atomic_fetch_add(&t->n_unskip, 1, __ATOMIC_RELAXED);
}
#endif
