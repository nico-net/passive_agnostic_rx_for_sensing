/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_MEM_H
#define NR_CSIRS_DISC_MEM_H
/* T6b: device memory model of the CSI-RS discovery GPU path and the ring capacity picked from the shared budget share (PHY/TOOLS/nr_gpu_membudget.h).
 * Pure functions (tested by test_nr_gpu_membudget). Bytes, for rb PRB, ant RX antennas.
 *   fixed: the resident detector (history sums over the 640-slot period, 100-slot batch state), the FEP workspace and its cuFFT batch buffers, the sweep's small buffers;
 *   per slot of capacity: the sweep's packed samples + grid + score table, and the ring's device grid.
 * The capacity (slots of the observation ring) is 640 at full size; 160 is the floor. A smaller ring shortens the observation window of one sweep
 * (sensitivity / time to detection); the detector history (period 640) does not depend on it. Below the floor the GPU discovery is not ready (CPU). */
#include <stddef.h>
#include "PHY/TOOLS/nr_gpu_membudget.h"
#define NR_CSIRS_DISC_CAPACITY_MIN 160
static inline size_t nr_csirs_disc_mem_fixed(int rb, int ant, int fft, size_t stride, int batch)
{
  const size_t tiles = (size_t)(rb + 15) / 16, pooled = 168 * tiles, nr = (size_t)14 * 12 * rb + pooled;
  size_t b = 640 * nr * 8 + 640 * pooled * 8 + 80 * nr * 8 + 64 * nr * 8 + 2 * nr * 8 + 2 * 640 * pooled * 8 + 640 * 8; /* sum sq s80 s64 total totalq ps pq dc */
  b += 100 * nr * 4 + 100 * (size_t)ant * 14 * 24 * rb * 2;                                                           /* x, grid */
  b += (size_t)14 * 12 * rb + 640 * 14 * tiles * 6 * 2;                                                              /* exclude, masks */
  b += (size_t)batch * ant * 14 * fft * 8 + (size_t)batch * ant * stride * 4;                                          /* FEP work (cufftComplex), td */
  b += (size_t)1024 * 160 * 4 + (size_t)rb * 16 * 8 * 2 * 4;                                                          /* sweep: dmean, mapping */
  b += (size_t)2048 * (39 * (size_t)rb + ((size_t)rb + 7) / 8 + 1) * 8;                                               /* segment_scratch() thread slots (2 blocks per SM of a 32-SM card; scratch mode only) */
  b += (size_t)512 << 20;                                                                                              /* local memory the driver reserves for refine_kernel (9248 B frame x 34 SMs x 1536 threads = 483 MB [MEASURED, sens6]) */
  return b;
}
static inline size_t nr_csirs_disc_mem_per_slot(int rb, int ant)
{
  return (size_t)rb * 32 * 8 * ant * 4          /* packed samples (short2) */
       + (size_t)ant * 14 * 12 * rb * 4         /* sweep grid (short2) */
       + (size_t)1024 * 160 * 4                 /* score table (float) */
       + (size_t)ant * 14 * 24 * rb * 2         /* ring device grid (int16) */
       + 64;                                    /* ds, di, meta */
}
/* capacity in [CAPACITY_MIN, full]; 0 = the share does not hold even the floor (GPU discovery not ready). full = NR_CSIRS_DISC_DEFAULT_CAPACITY. */
static inline int nr_csirs_disc_mem_capacity(size_t share, int rb, int ant, int fft, size_t stride, int batch, int full)
{
  return (int)nr_gpu_membudget_fit(share, nr_csirs_disc_mem_fixed(rb, ant, fft, stride, batch), nr_csirs_disc_mem_per_slot(rb, ant), NR_CSIRS_DISC_CAPACITY_MIN, full);
}
#endif
