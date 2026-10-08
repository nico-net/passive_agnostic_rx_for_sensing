/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef USRP_RING_VIEW_H
#define USRP_RING_VIEW_H
#include <stddef.h>
#include <stdint.h>
#include <simde/x86/avx2.h>

/* Convert a circular sc16 ring window directly into the PHY destination. This is
 * one mandatory format-shift pass, with no intermediate full-bandwidth copy. */
static inline void usrp_ring_shift_window(const int32_t *src, size_t cap, size_t start,
                                          int32_t *dst, int count, int shift)
{
  int j = 0;
  for (; j + 8 <= count; j += 8) {
    const size_t at = (start + (size_t)j) % cap;
    simde__m256i v;
    if (at + 8 <= cap)
      v = simde_mm256_loadu_si256((const simde__m256i *)(src + at));
    else {
      int32_t wrap[8];
      for (int k = 0; k < 8; k++) wrap[k] = src[(at + k) % cap];
      v = simde_mm256_loadu_si256((const simde__m256i *)wrap);
    }
    v = simde_mm256_srai_epi16(v, shift);
    simde_mm256_storeu_si256((simde__m256i *)(dst + j), v);
  }
  for (; j < count; j++) {
    const int32_t v = src[(start + (size_t)j) % cap];
    const int16_t *p = (const int16_t *)&v;
    int16_t *q = (int16_t *)(dst + j);
    q[0] = p[0] >> shift;
    q[1] = p[1] >> shift;
  }
}
#endif
