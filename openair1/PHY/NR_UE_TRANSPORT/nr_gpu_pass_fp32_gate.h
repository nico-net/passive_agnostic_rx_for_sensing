/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 5: the rate gate of the FP32 shadow checks. Every FP32-shadowed grant is noted with whether pass and old path disagree on the CRC verdict
 * (a flip, in either direction, or a different count of decoded blocks); once NR_FP32_GATE_MIN_N grants were shadowed, flips above 1 % of them trip the gate.
 * Header-only, C and C++, lock-free (GCC __atomic builtins). */
#ifndef NR_GPU_PASS_FP32_GATE_H
#define NR_GPU_PASS_FP32_GATE_H
#include <stdint.h>
#define NR_FP32_GATE_MIN_N 64
typedef struct {
  uint64_t n, flips;
} nr_fp32_gate_t;
/* pure decision: n shadowed grants, `flips` of them with a verdict difference */
static inline int nr_fp32_gate_bad(uint64_t n, uint64_t flips)
{
  return n >= NR_FP32_GATE_MIN_N && flips * 100 > n;
}
/* notes one shadowed grant; 1 = the gate is tripped (the caller disables the pass) */
static inline int nr_fp32_gate_note(nr_fp32_gate_t *g, int flip)
{
  const uint64_t n = __atomic_add_fetch(&g->n, 1, __ATOMIC_RELAXED);
  const uint64_t f = flip ? __atomic_add_fetch(&g->flips, 1, __ATOMIC_RELAXED) : __atomic_load_n(&g->flips, __ATOMIC_RELAXED);
  return nr_fp32_gate_bad(n, f);
}
#endif
