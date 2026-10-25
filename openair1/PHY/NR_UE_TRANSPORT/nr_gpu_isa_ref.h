/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* The host CPU's own ISA-dependent primitives, as references for the GPU twin self-test (nr_gpu_isa_twin.cuh).
 * nr_gpu_isa_ref.c is compiled with the project C flags (the same -march / SIMDE_* defines as the receiver), so its
 * rotate_cpx_vector / mult_cpx_vector are the receiver's; the DFT is the process's dft()/idft() pointer, or the
 * dft_implementation()/idft_implementation() of the libdfts.so the CPU loads. */
#ifndef NR_GPU_ISA_REF_H
#define NR_GPU_ISA_REF_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* out = dft (inverse = 0) or idft (inverse = 1) of n int16 complex samples, scale flag 1, as nr_slot_fep / nr_est_delay call it.
 * 0, or -1 when no CPU DFT is reachable or n is not a size of this build's DFT library. */
int nr_gpu_isa_cpu_dft(int n, int inverse, const int16_t *in, int16_t *out);
/* where the CPU DFT came from (for the log line) */
const char *nr_gpu_isa_cpu_dft_origin(void);
/* apply_nr_rotation_symbol_RX() on one range: y = mult_cpx_vector(rotate_cpx_vector(x, conj(rot), n, 15), ts, n, 15);
 * n a multiple of 4 (the ranges are multiples of 6 RE pairs) */
void nr_gpu_isa_cpu_rot(const int16_t *x, int16_t rot_r, int16_t rot_i, const int16_t *ts, int16_t *y, int n);
#ifdef __cplusplus
}
#endif
#endif
