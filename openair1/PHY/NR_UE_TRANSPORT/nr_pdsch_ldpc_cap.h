/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* RANK2-ITER: the one LDPC iteration cap of every PDSCH decode path (CPU TB decode, GPU exact LDPC, GPU graph pass, CB0 mirrors).
 * Product value: NR_PDSCH_LDPC_CAP_DEFAULT, one value for all cells, no flag.
 * Test-only override: environment NR_TEST_PDSCH_LDPC_ITERS (1..NR_PDSCH_LDPC_CAP_MAX), read once; used by the replay A/B of
 * docs/superpowers/reports/csd/RANK2-ITER-20261023.md. Caps above NR_PDSCH_LDPC_GPU_MAX run on the CPU decoder (the GPU LDPC keeps 16 hard-decision snapshots). */
#ifndef NR_PDSCH_LDPC_CAP_H
#define NR_PDSCH_LDPC_CAP_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define NR_PDSCH_LDPC_CAP_DEFAULT 8
#define NR_PDSCH_LDPC_CAP_MAX 30
#define NR_PDSCH_LDPC_GPU_MAX 17

/* 0 = unset or invalid (the default applies), else 1..NR_PDSCH_LDPC_CAP_MAX */
static inline int nr_pdsch_ldpc_cap_parse(const char *s)
{
  if (s == NULL || *s == '\0')
    return 0;
  char *e = NULL;
  const long v = strtol(s, &e, 10);
  if (e == s || *e != '\0' || v < 1 || v > NR_PDSCH_LDPC_CAP_MAX)
    return 0;
  return (int)v;
}

static inline int nr_pdsch_ldpc_cap(void)
{
  static int cap = 0;
  if (cap == 0) {
    const int t = nr_pdsch_ldpc_cap_parse(getenv("NR_TEST_PDSCH_LDPC_ITERS"));
    cap = t > 0 ? t : NR_PDSCH_LDPC_CAP_DEFAULT;
  }
  return cap;
}

/* Per-TB summary of the code blocks' OAI numIter (decodeIters: 0 = not decoded / aborted): max over all reported blocks (a block that hit the cap reports the
 * cap) and the sum over the blocks that decoded. Returns the number of blocks that reported an iteration count. */
static inline int nr_pdsch_ldpc_iters_summary(const uint8_t *its, const bool *ok, int n, uint8_t *it_max, uint16_t *ok_sum)
{
  int rep = 0;
  uint8_t m = 0;
  uint32_t s = 0;
  for (int r = 0; r < n; r++) {
    if (its[r] == 0)
      continue;
    rep++;
    if (its[r] > m)
      m = its[r];
    if (ok[r])
      s += its[r];
  }
  *it_max = m;
  *ok_sum = (uint16_t)(s > 65535u ? 65535u : s);
  return rep;
}
#endif
