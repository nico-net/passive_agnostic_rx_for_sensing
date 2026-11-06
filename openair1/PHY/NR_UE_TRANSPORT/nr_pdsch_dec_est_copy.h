/* Copy of the decoder's DM-RS channel estimate for the sensing CFR export (see struct nr_pdsch_dec_est in nr_pdsch_passive_decode.h). Header only so it is unit-tested alone.
 * row l*nant + a (layer l, RX a) of the decoder's estimate starts at base + (l*nant + a)*row_stride (int32 = packed c16; allocCast2D layout, one contiguous 2-D array), `sym_stride` samples per OFDM symbol; the DM-RS symbol m starts at m*sym_stride; the grant's first
 * subcarrier is `off` samples into the symbol (a shared-slot estimate band starts before the grant). dst[(l*nant + a)*nsc + j] = base[(l*nant+a)*row_stride + m*sym_stride + off + j], j < nsc. */
#ifndef NR_PDSCH_DEC_EST_COPY_H
#define NR_PDSCH_DEC_EST_COPY_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
static inline void nr_dec_est_copy(int32_t *dst, const int32_t *base, size_t row_stride, int nl, int nant, int m, int sym_stride, int off, int nsc)
{
  for (int l = 0; l < nl; l++)
    for (int a = 0; a < nant; a++)
      memcpy(&dst[((size_t)l * nant + a) * nsc], &base[(size_t)(l * nant + a) * row_stride + (size_t)m * sym_stride + off], (size_t)nsc * sizeof(int32_t));
}
#endif
