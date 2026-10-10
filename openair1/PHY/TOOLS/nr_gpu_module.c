/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_platform.h"
#include <pthread.h>
/* Every CUDA module records its actual cubin/PTX set for load diagnostics. */
const char nr_gpu_built_archs[] = NR_GPU_BUILT_ARCHS;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int ready;
/* check: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none found in tree.
 */
static void check(void) { ready = nr_gpu_module_check(NR_GPU_MODULE_NAME, nr_gpu_built_archs); }
/* nr_gpu_module_ready: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:750, openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:24, openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:367, openair1/PHY/CODING/nrLDPC_cuda/nr_ldpc_cuda.c:17 (+9 more).
 */
int nr_gpu_module_ready(void) { pthread_once(&once, check); return ready; }
