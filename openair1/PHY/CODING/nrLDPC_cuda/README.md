# CUDA LDPC decoder (`libldpc_cuda.so`)

Vendored from NVIDIA's Sionna Research Kit, `plugins/ldpc_cuda` (Apache-2.0, SPDX headers kept in
each file): `nr_ldpc_cuda.c` (OAI `LDPCdecoder()` glue), `ldpc_decoder.cu` (layered min-sum
kernels), `ldpc_tables_bg{1,2}.h` (38.212 base graphs, all 8 lifting sets).

Built only with `-DENABLE_LDPC_CUDA=ON`; loaded instead of the CPU decoder with
`--loader.ldpc.shlibversion _cuda`. The segment/rate-matching layer is OAI's own
(`nrLDPC_coding_segment`), so only the per-segment decode runs on the GPU.

Local changes:
- `LDPC_CUDA_DISCRETE` (set by our CMake): device buffers + explicit copies instead of mapped host
  memory. The plugin's default (`USE_UNIFIED_MEMORY`) targets Jetson/DGX; on a PCIe GPU the kernels
  would read the input LLRs across PCIe on every iteration.
- `host_syndrome_buffer` allocation cast fixed (`uint8_t*` -> `uint32_t*`; the discrete path did
  not compile).
- Architecture: sm_89 (RTX 4060 Ti on sens6) unless `LDPC_CUDA_ARCH` is given. CUDA 12.4 needs
  `-DCMAKE_CUDA_HOST_COMPILER=g++-13` on Ubuntu 26.04 (gcc 15 is rejected).
