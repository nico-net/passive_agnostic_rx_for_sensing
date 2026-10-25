/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* iq_ddc (fused mixer + quantiser) against tests/iq_ddc_ref.c (frozen copy of the pre-MPCIFIX implementation): the sc16 output must be IDENTICAL (0 LSB, 0
 * differing samples, same clip count) over decim 1/2/4/8, shifts 0 / -19.81 MHz / +7.3 MHz / 3.6 GHz (aliases: any shift is legal) at 122.88 and 245.76 MS/s
 * input, block sizes that are not multiples of the 4096 mixer block, state carried across blocks, a discontinuity reset, clipping inputs.
 * Second axis: the portable (scalar quantiser) path against the NEON path in the same binary, forced with iq_ddc_set_force_scalar(): 0 LSB, same clip count.
 * (The x86 build is the portable path: checked by compiling iq_ddc.c with __aarch64__ undefined, see the report.) */
#include "../iq_ddc.h"
#include "iq_ddc_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int run(double rate, int decim, double shift, double pass, int amp, const int *blk, int nblk)
{
  iq_ddc_t a, s;
  iqref_t b;
  if (iq_ddc_init(&a, rate, decim, pass, 70) || iq_ddc_init(&s, rate, decim, pass, 70) || iqref_init(&b, rate, decim, pass, 70)) return 1;
  iq_ddc_set_shift(&a, shift); iq_ddc_set_shift(&s, shift); iqref_set_shift(&b, shift);
  long nd_ref = 0, nd_sc = 0, tot = 0, maxd = 0;
  for (int k = 0; k < nblk; ++k) {
    const int n = blk[k];
    int16_t *in = malloc((size_t)n * 4), *oa = malloc((size_t)n * 4), *os = malloc((size_t)n * 4), *ob = malloc((size_t)n * 4);
    for (int i = 0; i < 2 * n; ++i) in[i] = (int16_t)((rand() % (2 * amp + 1)) - amp);
    if (k == nblk - 1) { iq_ddc_reset(&a); iq_ddc_reset(&s); iqref_reset(&b); } /* discontinuity: last block starts clean */
    iq_ddc_set_force_scalar(0);
    const int na = iq_ddc_process(&a, in, n, oa);
    iq_ddc_set_force_scalar(1);
    const int ns = iq_ddc_process(&s, in, n, os);
    iq_ddc_set_force_scalar(0);
    const int nb = iqref_process(&b, in, n, ob);
    if (na != nb || ns != nb || na != n / decim) { fprintf(stderr, "FAIL count %d %d %d\n", na, ns, nb); return 1; }
    for (int i = 0; i < 2 * na; ++i) {
      long d = labs((long)oa[i] - ob[i]);
      if (d > maxd) maxd = d;
      nd_ref += d != 0;
      nd_sc += oa[i] != os[i];
      tot++;
    }
    free(in); free(oa); free(os); free(ob);
  }
  const int ok = nd_ref == 0 && nd_sc == 0 && a.clipped == b.clipped && s.clipped == a.clipped;
  printf("%s rate=%.2f decim=%d shift=%.0f amp=%d: vs frozen ref differing=%ld/%ld max=%ld; portable vs %s differing=%ld; clipped %lu/%lu/%lu\n", ok ? "PASS" : "FAIL",
         rate / 1e6, decim, shift, amp, nd_ref, tot, maxd, iq_ddc_neon_compiled() ? "NEON" : "(same build, no NEON)", nd_sc, (unsigned long)a.clipped,
         (unsigned long)s.clipped, (unsigned long)b.clipped);
  iq_ddc_free(&a); iq_ddc_free(&s); iqref_free(&b);
  return !ok;
}

static int test_passband(void)
{
  int bad = 0;
  /* H3: num_rb_dl x 12 x SCS (MPCI-DASH-REVIEW), explicit rx_bw wins, 95 % of the rate without grid info */
  bad |= !(iq_ddc_cell_passband(51, 30e3, 30.72e6, 0) == 0.5 * 51 * 12 * 30e3);
  bad |= !(iq_ddc_cell_passband(106, 30e3, 61.44e6, 0) == 0.5 * 106 * 12 * 30e3);
  bad |= !(iq_ddc_cell_passband(273, 30e3, 122.88e6, 0) == 0.5 * 273 * 12 * 30e3);
  bad |= !(iq_ddc_cell_passband(51, 30e3, 30.72e6, 20e6) == 10e6);
  bad |= !(iq_ddc_cell_passband(0, 30e3, 30.72e6, 0) == 0.5 * 0.95 * 30.72e6);
  bad |= !(iq_ddc_cell_passband(106, 15e3, 61.44e6, 0) == 0.5 * 106 * 12 * 15e3);
  printf("%s passband (H3 rule)\n", bad ? "FAIL" : "PASS");
  return bad;
}

int main(void)
{
  const int blk[] = {65536, 4096 + 16, 20480, 65536, 8, 12288, 4104, 6144};
  const int nblk = (int)(sizeof(blk) / sizeof(blk[0]));
  const double shifts[] = {0, -19.81e6, 7.3e6, 3.6e9};
  const double rates[] = {122.88e6, 245.76e6};
  int bad = 0;
  srand(7);
  for (int r = 0; r < 2; ++r)
    for (int d = 1; d <= 8; d *= 2)
      for (int s = 0; s < 4; ++s) bad |= run(rates[r], d, shifts[s], d == 1 ? 50e6 : 0.5 * rates[r] / d * 0.8, 1000, blk, nblk);
  bad |= run(245.76e6, 2, -45e6, 50e6, 30000, blk, nblk); /* near full scale: clipping */
  bad |= run(122.88e6, 1, 5e6, 50e6, 32767, blk, nblk);
  bad |= run(245.76e6, 8, 53e6, 10e6, 32767, blk, nblk);
  bad |= test_passband();
  printf("neon_compiled=%d\n", iq_ddc_neon_compiled());
  printf(bad ? "FAIL\n" : "ALL OK\n");
  return bad;
}
