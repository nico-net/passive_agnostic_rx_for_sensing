/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* iq_ddc (fused mixer + NEON quantiser) against the previous implementation (tests/iq_ddc_ref.c, frozen copy of the pre-MPCIFIX
 * code): same sc16 output to <= 1 LSB on isolated samples, same clip count, over decim 1/2/4/8, several shifts, block sizes that are
 * not multiples of the 4096 mixer block, state carried across blocks, a clipping input and a discontinuity reset. */
#include "../iq_ddc.h"
#include "iq_ddc_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int run(double rate, int decim, double shift, double pass, int amp, const int *blk, int nblk)
{
  iq_ddc_t a;
  iqref_t b;
  if (iq_ddc_init(&a, rate, decim, pass, 70) || iqref_init(&b, rate, decim, pass, 70)) return 1;
  iq_ddc_set_shift(&a, shift); iqref_set_shift(&b, shift);
  long maxd = 0, nd = 0, tot = 0;
  for (int k = 0; k < nblk; ++k) {
    const int n = blk[k];
    int16_t *in = malloc((size_t)n * 4), *oa = malloc((size_t)n * 4), *ob = malloc((size_t)n * 4);
    for (int i = 0; i < 2 * n; ++i) in[i] = (int16_t)((rand() % (2 * amp + 1)) - amp);
    if (k == nblk - 1) { iq_ddc_reset(&a); iqref_reset(&b); } /* discontinuity: last block starts clean */
    const int na = iq_ddc_process(&a, in, n, oa), nb = iqref_process(&b, in, n, ob);
    if (na != nb || na != n / decim) { fprintf(stderr, "FAIL count %d %d\n", na, nb); return 1; }
    for (int i = 0; i < 2 * na; ++i) { long d = labs((long)oa[i] - ob[i]); if (d > maxd) maxd = d; nd += d != 0; tot++; }
    free(in); free(oa); free(ob);
  }
  const long dc = (long)a.clipped - (long)b.clipped;
  const int ok = maxd <= 1 && nd * 500 <= tot && labs(dc) * 200 <= (long)(b.clipped + 200);
  printf("%s decim=%d shift=%.0f amp=%d: max_diff=%ld differing=%ld/%ld clipped %lu vs %lu\n", ok ? "PASS" : "FAIL", decim, shift, amp, maxd, nd, tot,
         (unsigned long)a.clipped, (unsigned long)b.clipped);
  iq_ddc_free(&a); iqref_free(&b);
  return !ok;
}

int main(void)
{
  const int blk[] = {65536, 4096 + 16, 20480, 65536, 8, 12288};
  const double shifts[] = {0, -19.81e6, 7.3e6, 3.6e9 /* aliases: any shift is legal */};
  int bad = 0;
  srand(7);
  for (int d = 1; d <= 8; d *= 2)
    for (int s = 0; s < 4; ++s) bad |= run(122.88e6, d, shifts[s], d == 1 ? 50e6 : 0.5 * 122.88e6 / d * 0.8, 1000, blk, 6);
  bad |= run(245.76e6, 2, -45e6, 50e6, 30000, blk, 6); /* near full scale: clipping */
  bad |= run(122.88e6, 1, 5e6, 50e6, 32767, blk, 6);
  printf(bad ? "FAIL\n" : "ALL OK\n");
  return bad;
}
