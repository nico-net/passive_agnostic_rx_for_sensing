/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Data-aided tap off the decode thread: the part of the consumer's rxdataF the tap reads, copied into a job-owned snapshot with the SAME layout (antenna stride, symbol row of
 * `fft` c16), so nr_isac_pdsch_data_aided_submit runs on the snapshot unchanged and produces the same CFR rows.
 * The tap reads rxdataF[a][l * fft + re] only for the symbols l of the grant's window [sym0, sym0 + nsym) and the REs of its contiguous allocation: num_sc subcarriers from
 * start_re, wrapping at fft (nr_pdsch_data_aided.c, "re = start_re + j; if (re >= fft) re -= fft"). Header-only, element-size generic (c16_t = 4 bytes), so that the unit test
 * (nr_pdsch_da_snap_test) checks the read set without the UE headers. */
#ifndef NR_PDSCH_DA_SNAP_H
#define NR_PDSCH_DA_SNAP_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 0 = copied; -1 = the region does not fit the buffers (the caller then runs the tap inline on the live buffer) */
static inline int nr_da_snap_copy(uint32_t *dst, const uint32_t *src, int nant, size_t ant_stride, int fft, int sym0, int nsym, int start_re, int num_sc)
{
  if (nant < 1 || fft < 1 || sym0 < 0 || nsym < 1 || start_re < 0 || start_re >= fft || num_sc < 1 || num_sc > fft || (size_t)(sym0 + nsym) * (size_t)fft > ant_stride)
    return -1;
  const int n1 = num_sc < fft - start_re ? num_sc : fft - start_re; /* up to the end of the symbol row, then the wrapped rest from re 0 */
  for (int a = 0; a < nant; a++)
    for (int l = sym0; l < sym0 + nsym; l++) {
      const size_t row = (size_t)a * ant_stride + (size_t)l * (size_t)fft;
      memcpy(dst + row + start_re, src + row + start_re, (size_t)n1 * sizeof(uint32_t));
      if (num_sc > n1)
        memcpy(dst + row, src + row, (size_t)(num_sc - n1) * sizeof(uint32_t));
    }
  return 0;
}
#endif
