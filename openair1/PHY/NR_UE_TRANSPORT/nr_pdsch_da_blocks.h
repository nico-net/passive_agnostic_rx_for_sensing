/* Per-code-block bookkeeping of a partially decoded PDSCH TB for the data-aided CFR (sensing mode, operator 2026-11-06: "export all the code blocks that are
 * decoded in the TB, even if the TB fails (CB that pass CRC of course)"). Header only, no dependencies, so it is unit-tested alone.
 *
 * Which resource elements does a set of passing code blocks pin down? TS 38.212 5.4.2.1 gives code block r of C the E_r rate-matched bits; 5.5 concatenates them in order r = 0..C-1
 * into the codeword f (G bits). TS 38.211 7.3.1.1-7.3.1.3: the codeword is scrambled and Qm bits make one modulation symbol d(n); with Nl layers, x_l(i) = d(Nl*i + l), so RE ordinal i
 * (in transmit order, 7.3.1.5/6.3.1.6 VRB-to-PRB and the increasing-k-then-l sweep, which the caller's RE walk already follows) carries the symbols d(Nl*i .. Nl*i+Nl-1), i.e. the codeword bits
 * [i*Nl*Qm, (i+1)*Nl*Qm). The RE is known only if EVERY block that holds one of those bits passed its CRC24B. E_r is a multiple of Nl*Qm (5.4.2.1), so in practice an RE sits in one block;
 * the walk below does not rely on it. */
#ifndef NR_PDSCH_DA_BLOCKS_H
#define NR_PDSCH_DA_BLOCKS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_DA_CB_MAX 288 /* = MAX_NUM_NR_DLSCH_SEGMENTS (36 per layer x 8); asserted in nr_pdsch_data_aided.c */

/* Per-block CRC result of one TB: bit r of bits = block r passed its CRC24B. Not limited to 64 blocks (EVMGRANT cbmap= is). C = blocks in the TB, ok = blocks that passed. */
typedef struct {
  uint16_t C, ok;
  uint8_t bits[(NR_DA_CB_MAX + 7) / 8];
} nr_da_cb_pass_t;

static inline bool nr_da_cb_get(const nr_da_cb_pass_t *p, uint32_t r) { return r < p->C && r < NR_DA_CB_MAX && ((p->bits[r >> 3] >> (r & 7)) & 1u); }
static inline void nr_da_cb_set(nr_da_cb_pass_t *p, uint32_t r, bool v)
{
  if (r >= NR_DA_CB_MAX) return;
  if (v) p->bits[r >> 3] |= (uint8_t)(1u << (r & 7)); else p->bits[r >> 3] &= (uint8_t)~(1u << (r & 7));
}

/* E_r of TS 38.212 5.4.2.1 (G = codeword bits, a multiple of Nl*Qm). */
static inline uint32_t nr_da_block_E(uint32_t G, uint32_t C, uint32_t Qm, uint32_t Nl, uint32_t r)
{
  const uint32_t Gp = G / (Nl * Qm);
  return r <= C - (Gp % C) - 1 ? Nl * Qm * (Gp / C) : Nl * Qm * ((Gp + C - 1) / C);
}

/* valid[i] (i < n_re) = 1 when RE ordinal i is carried by passing blocks only. E[r] = bits of block r (r < p->C). Returns the number of valid REs. Bits beyond sum(E) are never valid. */
static inline uint32_t nr_da_re_valid(const uint32_t *E, const nr_da_cb_pass_t *p, uint32_t Qm, uint32_t Nl, uint32_t n_re, uint8_t *valid)
{
  uint32_t r = 0, cnt = 0;
  uint64_t end = p->C ? E[0] : 0; /* bit just past block r */
  const uint64_t span = (uint64_t)Nl * Qm;
  for (uint32_t i = 0; i < n_re; i++) {
    const uint64_t lo = (uint64_t)i * span, hi = lo + span;
    while (r < p->C && lo >= end) {
      r++;
      if (r < p->C) end += E[r];
    }
    bool ok = r < p->C && nr_da_cb_get(p, r);
    uint64_t e = end; /* the span may leave block r: the blocks it touches next must pass too */
    for (uint32_t q = r; ok && hi > e;) {
      if (++q >= p->C) { ok = false; break; }
      e += E[q];
      ok = nr_da_cb_get(p, q);
    }
    valid[i] = ok;
    cnt += ok;
  }
  return cnt;
}

#ifdef __cplusplus
}
#endif
#endif
