/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CPU (plain min-sum) LDPC decoder compiled into libldpc_cuda.so under private names (K34 CPU fallback), so it
 * cannot interpose on this plugin's own LDPCdecoder (the CUDA normalised min-sum one that ldpctest -v _cuda
 * measures) nor on libldpc.so. */
#define LDPCdecoder ldpc_cpu_LDPCdecoder
#define LDPCinit ldpc_cpu_LDPCinit
#define LDPCshutdown ldpc_cpu_LDPCshutdown
#include "../nrLDPC_decoder/nrLDPC_decoder.c"
