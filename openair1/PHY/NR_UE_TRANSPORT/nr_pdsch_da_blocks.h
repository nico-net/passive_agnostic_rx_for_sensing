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

/* Never-guess: a code block whose payload is all zero has a zero CRC24B, so the LDPC hard decision of erased / wrong-layout LLRs passes it. Clear such blocks from the bitmap and recount
 * p->ok. pay + r*stride is block r's payload (nbytes long); bytes at or beyond `limit` (the buffer end) are not read. */
static inline void nr_da_cb_clear_zero(nr_da_cb_pass_t *p, const uint8_t *pay, size_t stride, size_t nbytes, size_t limit)
{
  uint16_t ok = 0;
  for (uint32_t r = 0; r < p->C && r < NR_DA_CB_MAX; r++) {
    if (nr_da_cb_get(p, r)) {
      bool z = true;
      for (size_t i = 0; z && i < nbytes && (size_t)r * stride + i < limit; i++) z = pay[(size_t)r * stride + i] == 0;
      if (z) nr_da_cb_set(p, r, false);
    }
    ok += nr_da_cb_get(p, r);
  }
  p->ok = ok;
}

/* The per-RE decision of the data-aided walk. s0 = index of the RE's first modulation symbol d(s0..s0+nl-1) (TS 38.211 7.3.1.3: x_l(i) = d(nl*i + l)), nof_re = REs kept so far. Returns false when the RE is
 * carried by a failed or untried block (not exported). For nl > 1 a kept RE's nl symbols are moved down to mod_syms[nof_re*nl ..] (the windowed LS reads them by position in the exported list; the
 * write index never passes the read index). mod_syms holds int16 pairs (re, im). nl == 1 reads mod_syms[s0] in place (no compaction: the recon needs mod_idx == nof_re anyway). */
static inline bool nr_da_re_take(const uint8_t *re_valid, bool part_on, uint32_t s0, uint32_t nl, uint32_t nof_re, int16_t *mod_syms)
{
  if (!part_on) return true;
  if (!re_valid[s0 / nl]) return false;
  if (nl > 1 && nof_re * nl != s0)
    for (uint32_t l = 0; l < nl; l++) { mod_syms[2 * (nof_re * nl + l)] = mod_syms[2 * (s0 + l)]; mod_syms[2 * (nof_re * nl + l) + 1] = mod_syms[2 * (s0 + l) + 1]; }
  return true;
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
