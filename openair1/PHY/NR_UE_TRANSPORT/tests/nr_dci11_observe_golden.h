/* Deterministic DCI 1_1 resolver observe sequence + state hash. Header-only C99 so a standalone C harness (built against the
 * ORIGINAL nr_pdcch_dci11_layout_sweep.c to produce the golden value) and the gtest (built against the current one) run the
 * exact same code. Covers nr_dci11_resolver_observe_rnti(): HARQ-sequence bookkeeping, plausibility/impossibility counters,
 * histograms, hard refutation, Wilson drops, distribution ranking. */
#ifndef NR_DCI11_OBSERVE_GOLDEN_H
#define NR_DCI11_OBSERVE_GOLDEN_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nr_pdcch_dci11_layout_sweep.h"

static inline uint64_t g11_rng(uint64_t *s)
{
  *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
  return *s * 2685821657736338717ULL;
}
static inline void g11_poke(nr_dci_bits_t *p, uint16_t total, uint16_t off, uint8_t nbits, uint32_t v)
{
  if (nbits == 0 || off + nbits > total) return;
  const int lsb = total - off - nbits;
  for (int b = 0; b < nbits; b++) {
    const int pos = lsb + b;
    const uint64_t m = 1ULL << (pos & 63);
    if ((v >> b) & 1) p->w[pos >> 6] |= m; else p->w[pos >> 6] &= ~m;
  }
}
static inline uint64_t g11_fnv(uint64_t h, const void *d, size_t n)
{
  const uint8_t *b = (const uint8_t *)d;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
  return h;
}
/* bwp: BWP size; n_obs: payloads fed; out_info (optional) receives "n_hyp n_alive n_obs"; observe_us (optional) the mean cost of one
 * nr_dci11_resolver_observe_rnti() call. Returns the state hash. */
static inline nr_dci11_resolver_t *g11_observe_state(int bwp, int n_obs, int *best_len_out, double *observe_us)
{
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof(*r));
  const uint16_t span_bits = (uint16_t)(bwp > 200 ? 15 : bwp > 100 ? 13 : 12);
  int best_len = 0, best_n = 0;
  for (int len = 40; len <= 90; len++) {
    const int n = nr_dci11_resolver_init_fdra(r, 0, (uint16_t)bwp, span_bits, NR_DCI11_TDA_UNKNOWN, (uint16_t)len);
    if (n > best_n) { best_n = n; best_len = len; }
  }
  nr_dci11_resolver_init_fdra(r, 0, (uint16_t)bwp, span_bits, NR_DCI11_TDA_UNKNOWN, (uint16_t)best_len);
  const int truth = r->n_hyp / 2;
  uint64_t rng = 0x1234567ULL;
  uint16_t rnti_pool[20];
  uint8_t harq[20] = {0};
  for (int k = 0; k < 20; k++) rnti_pool[k] = (uint16_t)(0x4600 + 17 * k);
  double spent = 0.0;
  for (int n = 0; n < n_obs; n++) {
    nr_dci_bits_t p = {{g11_rng(&rng), g11_rng(&rng), g11_rng(&rng)}};
    const int u = (int)(g11_rng(&rng) % 20);
    const nr_dci11_offsets_t *o = &r->off[truth];
    harq[u] = (uint8_t)((harq[u] + 1) & 15);
    g11_poke(&p, o->total, 0, 1, 1);
    g11_poke(&p, o->total, o->mcs, 5, (uint32_t)(g11_rng(&rng) % 20));
    g11_poke(&p, o->total, o->rv, 2, (g11_rng(&rng) % 8) == 0 ? 1u : 0u);
    g11_poke(&p, o->total, o->rv + 2, 4, harq[u]);
    if (o->tda_bits) g11_poke(&p, o->total, o->tda, o->tda_bits, (uint32_t)(g11_rng(&rng) % 3));
    g11_poke(&p, o->total, o->ant_ports, o->ant_ports_bits, (uint32_t)(g11_rng(&rng) % 3));
    g11_poke(&p, o->total, o->riv, (uint8_t)(o->tda - o->riv), (uint32_t)(1 + g11_rng(&rng) % 900));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    nr_dci11_resolver_observe_rnti(r, p, rnti_pool[u]);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    spent += (double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
  }
  if (observe_us) *observe_us = spent / (n_obs > 0 ? n_obs : 1);
  if (best_len_out) *best_len_out = best_len;
  return r;
}
static inline uint64_t g11_observe_run(int bwp, int n_obs, char *out_info, size_t info_n, double *observe_us)
{
  int best_len = 0;
  nr_dci11_resolver_t *r = g11_observe_state(bwp, n_obs, &best_len, observe_us);
  uint64_t h = 1469598103934665603ULL;
  const int nh = r->n_hyp;
  h = g11_fnv(h, &nh, sizeof(nh));
  h = g11_fnv(h, &r->n_alive, sizeof(r->n_alive));
  h = g11_fnv(h, &r->n_obs, sizeof(r->n_obs));
  h = g11_fnv(h, &r->dropped_dist, sizeof(r->dropped_dist));
  h = g11_fnv(h, &r->winner, sizeof(r->winner));
  h = g11_fnv(h, &r->fdra_next, sizeof(r->fdra_next));
  h = g11_fnv(h, r->alive, (size_t)nh * sizeof(r->alive[0]));
  h = g11_fnv(h, r->seen, (size_t)nh * sizeof(r->seen[0]));
  h = g11_fnv(h, r->pass, (size_t)nh * sizeof(r->pass[0]));
  h = g11_fnv(h, r->impossible, (size_t)nh * sizeof(r->impossible[0]));
  h = g11_fnv(h, r->harq_pairs, (size_t)nh * sizeof(r->harq_pairs[0]));
  h = g11_fnv(h, r->harq_inc, (size_t)nh * sizeof(r->harq_inc[0]));
  h = g11_fnv(h, r->hist, (size_t)nh * sizeof(r->hist[0]));
  h = g11_fnv(h, r->harq_rnti, sizeof(r->harq_rnti));
  if (out_info) snprintf(out_info, info_n, "len=%d n_hyp=%d n_alive=%d n_obs=%u", best_len, nh, r->n_alive, r->n_obs);
  free(r);
  return h;
}
#endif
