/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_gpu_platform.h"
#include <pthread.h>
/* Every CUDA module records its actual cubin/PTX set for load diagnostics. */
const char nr_gpu_built_archs[] = NR_GPU_BUILT_ARCHS;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int ready;
static void check(void) { ready = nr_gpu_module_check(NR_GPU_MODULE_NAME, nr_gpu_built_archs); }
int nr_gpu_module_ready(void) { pthread_once(&once, check); return ready; }
