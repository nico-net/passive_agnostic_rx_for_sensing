/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DECGPU exact LDPC host entry (nr_pdsch_gpu_ldpc_host.c). */
#ifndef NR_PDSCH_GPU_LDPC_HOST_H
#define NR_PDSCH_GPU_LDPC_HOST_H
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"

/* 1 when ISAC_PDSCH_GPU_EXACT_LDPC=1 and the GPU module loaded (first call loads it). */
int nr_pdsch_gpu_ldpc_enabled(void);
/* Drop-in for cpu(p): the same outputs (c, decodeSuccess, processedSegments, d, abort) from the GPU port of the CPU
 * decoder; calls cpu(p) itself when off, refused or unsure. */
int32_t nr_pdsch_gpu_ldpc_decode(nrLDPC_slot_decoding_parameters_t *p, nrLDPC_coding_decoder_t *cpu);
#endif
