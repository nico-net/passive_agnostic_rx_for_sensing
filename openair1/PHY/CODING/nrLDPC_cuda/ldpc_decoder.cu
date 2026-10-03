/*
SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
*/
#include <cuda_runtime.h>
#include <vector>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <stdint.h>

#include "ldpc_tables_bg1.h"
#include "ldpc_tables_bg2.h"

#define TIMESTAMP_CLOCK_SOURCE CLOCK_MONOTONIC

// START marker-grid-blocks-util
inline __host__ __device__ uint32_t blocks_for(uint32_t elements, int block_size) {
    return int( uint32_t(elements + (block_size-1)) / uint32_t(block_size) );
}
// END marker-grid-blocks-util

/* Unified memory suits Jetson/DGX (the plugin's original targets). A discrete PCIe GPU wants
 * device buffers and explicit copies: build with LDPC_CUDA_DISCRETE (the adaptive-rx default). */
#ifndef LDPC_CUDA_DISCRETE
#define USE_UNIFIED_MEMORY
#endif
//#define ENABLE_DGX_OPTIMIZATIONS
//#define USE_GRAPHS
//#define PRINT_TIMES

#ifdef USE_UNIFIED_MEMORY

#ifndef ENABLE_DGX_OPTIMIZATIONS
#define cudaMallocStaging(pp, size, hostFlags) cudaHostAlloc(pp, size, hostFlags)
#define cudaFreeStaging cudaFreeHost
#else
#define cudaMallocStaging(pp, size, hostFlags) cudaMallocManaged(pp, size)
#define cudaFreeStaging cudaFree
#endif

#else

#define cudaMallocStaging(pp, size, hostFlags) cudaMalloc(pp, size)
#define cudaFreeStaging cudaFree

#endif

static uint32_t const* bg_cn_degree[2][8] = {};
static uint32_t const* bg_vn_degree[2][8] = {};
static uint32_t const* bg_cn[2][8] = {};
static uint32_t const* bg_vn[2][8] = {};
static uint32_t bg_cn_size[2][8] = {};
static uint32_t bg_vn_size[2][8] = {};

static uint32_t const MAX_BG_ROWS = 46;
static uint32_t const MAX_BG_COLS = 68;
static uint32_t const MAX_Z = 384; // lifting set according to 38.212 Tab 5.3.2-1
static uint32_t const MAX_BLOCK_LENGTH = MAX_BG_COLS * MAX_Z;

// START marker-dtypes
static const int MAX_LLR_ACCUMULATOR_VALUE = 127;
typedef int8_t llr_accumulator_t;
static const int MAX_LLR_MSG_VALUE = 127;
typedef int8_t llr_msg_t;
// END marker-dtypes

// START marker-damping-factor
#define APPLY_DAMPING_INT(x) (x*3/4)
// END marker-damping-factor

// START marker-thread-context
struct ThreadContext {
    cudaStream_t stream = 0;

    // Device memory declarations - use raw pointers instead of device symbols
    int8_t* llr_in_buffer = nullptr;
    uint8_t* llr_bits_out_buffer = nullptr;
    uint32_t* syndrome_buffer = nullptr;
#ifndef USE_UNIFIED_MEMORY
    uint32_t* host_syndrome_buffer = nullptr;
#endif
    llr_msg_t* llr_msg_buffer = nullptr;
    llr_accumulator_t* llr_total_buffer = nullptr;

    // batched decode (ldpc_batch_*): capacity in codewords, pinned host I/O, device work buffers
    uint32_t b_cap = 0;
    int8_t* b_llr_host = nullptr;
    uint8_t* b_bits_host = nullptr;
    int8_t* b_llr_dev = nullptr;
    uint8_t* b_bits_dev = nullptr;
    llr_msg_t* b_msg = nullptr;
    llr_accumulator_t* b_total = nullptr;
    uint32_t* b_done = nullptr;   // per-codeword converged flag (early termination)
    uint32_t* b_unsat = nullptr;  // per-codeword unsatisfied-check flag, this iteration

#ifdef USE_GRAPHS
    cudaGraphExec_t graphCtx = nullptr;
#endif

    // list of thread contexts for shutdown
    ThreadContext* next_initialized_context = nullptr;
};
static __thread ThreadContext thread_context = { };
// END marker-thread-context

static ThreadContext* initialized_thread_contexts = nullptr;

#ifdef PRINT_TIMES
struct TimeMeasurements {
    unsigned long long avg_ns;
    unsigned long long max_ns;
    size_t count;
};
static __thread struct TimeMeasurements input_copy_time = {};
static __thread struct TimeMeasurements output_copy_time = {};
static __thread struct TimeMeasurements decoding_time = {};

static unsigned add_measurement(struct TimeMeasurements* time, unsigned long long time_ns, size_t max_samples) {
    size_t samples = ++time->count;
    if (samples > max_samples)
        samples = max_samples;
    time->avg_ns = (unsigned long long)((long long)time->avg_ns + (long long)(time_ns - time->avg_ns) / (int) samples);
    if (time_ns > time->max_ns)
        time->max_ns = time_ns;
    return time->count;
}
#endif

#define CHECK_CUDA(call) do { \
        cudaError_t err = (call); \
        if (err) \
            printf("CUDA error %d: %s; in %s|%d|\n", (int) err, cudaGetErrorString(err), __FILE__, __LINE__); \
    } while (false)

ThreadContext& ldpc_decoder_init_context(int make_stream);

struct BaseGraph {
    uint32_t num_rows;
    uint32_t num_cols;
    uint32_t num_edges;
    uint32_t const* cn_degree;
    uint32_t const* vn_degree;
    uint32_t const* cn;
    uint32_t const* vn;
    uint32_t cn_stride;
    uint32_t vn_stride;
};

static BaseGraph get_basegraph(uint32_t BG, uint32_t Z) {
    BaseGraph bg;
    uint32_t num_nnz;
    // select base graph dimensions
    if (BG == 1) {
        bg.num_rows = 46;
        bg.num_cols = 68;
        num_nnz = 316; // num non-zero elements in BG
    }
    else {
        bg.num_rows = 42;
        bg.num_cols = 52;
        num_nnz = 197; // num non-zero elements in BG
    }

    // number of variable nodes
    // uint32_t num_vns = bg.num_cols * Z;
    // number of check nodes
    // uint32_t num_cns = bg.num_rows * Z;

    // number of edges/messages in the graph
    bg.num_edges = num_nnz * Z;

    // lifting set according to 38.212 Tab 5.3.2-1
    static uint32_t const s_val[8][8] = {{2, 4, 8, 16, 32, 64, 128, 256},
             {3, 6, 12, 24, 48, 96, 192, 384},
             {5, 10, 20, 40, 80, 160, 320},
             {7, 14, 28, 56, 112, 224},
             {9, 18, 36, 72, 144, 288},
             {11, 22, 44, 88, 176, 352},
             {13, 26, 52, 104, 208},
             {15, 30, 60, 120, 240}};

    // find lifting set index
    int ils = -1;
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (Z == s_val[i][j]) {
                ils = i;
                break;
            }
        }
        if (ils != -1)
            break;
    }
    // this case should not happen
    assert(ils != -1 && "Lifting factor not found in lifting set");

    bg.cn_degree = bg_cn_degree[BG-1][ils];
    bg.vn_degree = bg_vn_degree[BG-1][ils];
    bg.cn = bg_cn[BG-1][ils];
    bg.vn = bg_vn[BG-1][ils];
    bg.cn_stride = bg_cn_size[BG-1][ils] / (sizeof(bg.cn[0]) * bg.num_rows);
    bg.vn_stride = bg_vn_size[BG-1][ils] / (sizeof(bg.vn[0]) * bg.num_cols);

    return bg;
 }

//#define NODE_KERNEL_BLOCK 512
//#define UNROLL_NODES 1
#define NODE_KERNEL_BLOCK 128
#define UNROLL_NODES 4

// START marker-cnp-kernel
__launch_bounds__(UNROLL_NODES*NODE_KERNEL_BLOCK, 3)
static __global__ void update_cn_kernel(llr_accumulator_t const* __restrict__ llr_total, llr_msg_t* __restrict__ llr_msg,
                                        uint32_t Z, uint32_t const* __restrict__ bg_cn, uint32_t const* __restrict__ bg_cn_degree, uint32_t max_degree, uint32_t num_rows,
                                        bool first_iter, uint32_t total_stride, uint32_t msg_stride,
                                        uint32_t const* __restrict__ done) {
    // batched decode: blockIdx.y selects the codeword (strides 0 = the single-codeword path)
    if (done && done[blockIdx.y])
        return; // converged: every check satisfied, messages frozen
    llr_total += blockIdx.y * total_stride;
    llr_msg += (size_t)blockIdx.y * msg_stride;
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;

    uint32_t i = tid % Z; // for i in range(Z)
    uint32_t idx_row = tid / Z; // for idx_row in range(num_rows)

    uint32_t cn_degree = bg_cn_degree[idx_row];

    // list of tuples (idx_col = idx_vn, s),
    // idx_row = idx_cn and msg_offset omitted,
    // msg spread out to idx_cn + idx_vn * num_cn
    uint32_t const* check_nodes = &bg_cn[idx_row * max_degree]; // len(cn) = cn_degree

    // search the "extrinsic" min of all incoming LLRs
    // this means we need to find the min and the second min of all incoming LLRs
    int min_1 = INT_MAX;
    int min_2 = INT_MAX;
    int idx_min = -1;
    int node_sign = 1;
    uint32_t msg_signs = 0; // bitset, 0 == positive; max degree is 19

#if UNROLL_NODES > 1
    __shared__ int mins1[UNROLL_NODES][NODE_KERNEL_BLOCK+1];
    __shared__ int mins2[UNROLL_NODES][NODE_KERNEL_BLOCK+1];
    __shared__ int idx_mins[UNROLL_NODES][NODE_KERNEL_BLOCK+1];
    __shared__ uint32_t signs[UNROLL_NODES][NODE_KERNEL_BLOCK+1];
    mins1[threadIdx.y][threadIdx.x] = INT_MAX;
    mins2[threadIdx.y][threadIdx.x] = INT_MAX;
    signs[threadIdx.y][threadIdx.x] = 0;
#endif

    __syncwarp();

    if (idx_row < num_rows) {
    for (uint32_t ii = threadIdx.y; ii < cn_degree; ii += UNROLL_NODES) {
        uint32_t cn = check_nodes[ii];

        // see packing layout above
        uint32_t idx_col = cn & 0xffffu; // note: little endian
        uint32_t s = cn >> 16;           // ...
        uint32_t msg_offset = idx_row + idx_col * num_rows;

        uint32_t msg_idx = msg_offset * Z + i;

        // total VN message
        int t = llr_total[idx_col*Z + (i+s)%Z];

        // make extrinsic by subtracting the previous msg
        if (!first_iter)
            t -= __ldg(&llr_msg[msg_idx]);

        // store sign for 2nd recursion
        // note: could be also used for syndrome-based check or early termination
        int sign = (t >= 0 ? 1 : -1);
        node_sign *= sign;
        msg_signs |= (t < 0) << ii; // for later sign calculation

        // find min and second min
        int t_abs = abs(t);
        if (t_abs < min_1) {
            min_2 = min_1;
            min_1 = t_abs;
            idx_min = msg_idx;
        } else if (t_abs < min_2)
            min_2 = t_abs;
    }
    }

#if UNROLL_NODES > 1
	mins1[threadIdx.y][threadIdx.x] = min_1;
	mins2[threadIdx.y][threadIdx.x] = min_2;
	idx_mins[threadIdx.y][threadIdx.x] = idx_min;
	signs[threadIdx.y][threadIdx.x] = msg_signs;

    __syncthreads();

    if (threadIdx.y == 0) {
        int min_1 = INT_MAX;
        int min_2 = INT_MAX;
        int idx_min = -1;
        uint32_t msg_signs = 0; // bitset, 0 == positive; max degree is 19
        for (int i = 0; i < UNROLL_NODES; ++i) {
            int t_abs = mins1[i][threadIdx.x];
            if (t_abs < min_1) {
                min_2 = min_1;
                min_1 = t_abs;
                idx_min = idx_mins[i][threadIdx.x];
            } else if (t_abs < min_2)
                min_2 = t_abs;
            min_2 = min(min_2, mins2[i][threadIdx.x]);
            msg_signs |= signs[i][threadIdx.x];
        }
        mins1[0][threadIdx.x] = min_1;
        mins2[0][threadIdx.x] = min_2;
        idx_mins[0][threadIdx.x] = idx_min;
        signs[0][threadIdx.x] = msg_signs;
    }

    __syncthreads();

    min_1 = mins1[0][threadIdx.x];
    min_2 = mins2[0][threadIdx.x];
    idx_min = idx_mins[0][threadIdx.x];
    msg_signs = signs[0][threadIdx.x];

    node_sign = (__popc(msg_signs) & 1) ? -1 : 1;
#endif

    // START marker-cnp-damping
    // apply damping factor
    min_1 = APPLY_DAMPING_INT(min_1); // min_1 * DAMPING_FACTOR, e.g. *3/4
    min_2 = APPLY_DAMPING_INT(min_2); // min_2 * DAMPING_FACTOR, e.g. *3/4
    // END marker-cnp-damping

    // clip msg magnitudes to MAX_LLR_VALUE
    min_1 = min(max(min_1, -MAX_LLR_MSG_VALUE), MAX_LLR_MSG_VALUE);
    min_2 = min(max(min_2, -MAX_LLR_MSG_VALUE), MAX_LLR_MSG_VALUE);
    // END marker-vnp-clipping

    __syncwarp();

    // apply min and second min to the outgoing LLR
    if (idx_row < num_rows) {
    for (uint32_t ii = threadIdx.y; ii < cn_degree; ii += UNROLL_NODES) {
         uint32_t cn = check_nodes[ii];

        // see packing layout above
        uint32_t idx_col = cn & 0xffffu; // note: little endian
        uint32_t msg_offset = idx_row + idx_col * num_rows;

        uint32_t msg_idx = msg_offset * Z + i;
        int min_val;
        if (msg_idx == idx_min)
            min_val = min_2;
        else
            min_val = min_1;

        int msg_sign = (msg_signs >> ii) & 0x1 ? -1 : 1;

        // and update outgoing msg including sign
        llr_msg[msg_idx] = llr_msg_t(min_val * node_sign * msg_sign);
    }
    }
}
// END marker-cnp-kernel

// START marker-vnp-kernel
__launch_bounds__(UNROLL_NODES*NODE_KERNEL_BLOCK, 3)
static __global__ void update_vn_kernel(llr_msg_t const* __restrict__ llr_msg, int8_t const* __restrict__ llr_ch, llr_accumulator_t* __restrict__ llr_total,
                                        uint32_t Z, uint32_t const* __restrict__ bg_vn, uint32_t const* __restrict__ bg_vn_degree, uint32_t max_degree, uint32_t num_cols, uint32_t num_rows,
                                        uint32_t total_stride, uint32_t msg_stride,
                                        uint32_t const* __restrict__ done) {
    if (done && done[blockIdx.y])
        return;
    llr_msg += (size_t)blockIdx.y * msg_stride;
    llr_ch += blockIdx.y * total_stride;
    llr_total += blockIdx.y * total_stride;
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;

    uint32_t i = tid % Z; // for i in range(Z)
    uint32_t idx_col = tid / Z; // for idx_col in range(num_cols)

    uint32_t vn_degree = bg_vn_degree[idx_col];

    // list of tuples (idx_row = index_cn, s)
    // idx_col = idx_vn and msg_offset omitted,
    // msg spread out to idx_cn + idx_vn * num_cn
    uint32_t const* variable_nodes = &bg_vn[idx_col * max_degree]; // len(vn) = vn_degree

#if UNROLL_NODES > 1
    __shared__ int msg_sums[UNROLL_NODES][NODE_KERNEL_BLOCK+1];
    msg_sums[threadIdx.y][threadIdx.x] = 0;
#endif

    __syncwarp();

    int msg_sum = 0;
    // accumulate all incoming LLRs
    if (idx_col < num_cols) {
    for (uint32_t j = threadIdx.y; j < vn_degree; j += UNROLL_NODES) {
        uint32_t vn = variable_nodes[j];

        // see packing layout above
        uint32_t idx_row = vn & 0xffffu; // note: little endian
        uint32_t s = vn >> 16;           // ...
        uint32_t msg_offset = idx_row + idx_col * num_rows;

        // index of the msg in the LLR array
        // it is the idx_col-th variable node, and the j-th message from the idx_row-th check node
        uint32_t msg_idx = msg_offset * Z + (i-s+(Z<<8))%Z;

        // accumulate all incoming LLRs
        msg_sum += llr_msg[msg_idx];
    }
    }

    // add the channel LLRs
    __syncwarp();
    if (threadIdx.y == 0) {
        if (idx_col < num_cols)
        msg_sum += llr_ch[idx_col*Z + i];
    }

    msg_sum = min(max(msg_sum, -MAX_LLR_ACCUMULATOR_VALUE), MAX_LLR_ACCUMULATOR_VALUE);

#if UNROLL_NODES > 1
	msg_sums[threadIdx.y][threadIdx.x] = msg_sum;

    __syncthreads();

    if (threadIdx.y == 0) {
        int msg_sum = 0;
        for (int i = 0; i < UNROLL_NODES; ++i) {
            msg_sum += msg_sums[i][threadIdx.x];
        }
        msg_sums[0][threadIdx.x] = msg_sum;
    }

    __syncthreads();

    msg_sum = msg_sums[0][threadIdx.x];
#endif

    msg_sum = min(max(msg_sum, -MAX_LLR_ACCUMULATOR_VALUE), MAX_LLR_ACCUMULATOR_VALUE);

    __syncwarp();
    if (idx_col < num_cols)
    llr_total[idx_col*Z + i] = llr_accumulator_t(msg_sum);
}
// END marker-vnp-kernel

__launch_bounds__(512, 3)
static __global__ void compute_syndrome_kernel(llr_accumulator_t const* __restrict__ llr_total, uint32_t* __restrict__ syndrome,
                                               uint32_t Z, uint32_t const* __restrict__ bg_cn, uint32_t const* __restrict__ bg_cn_degree, uint32_t max_degree, uint32_t num_rows) {
    const int tid = blockIdx.x * blockDim.x + threadIdx.x;

    uint32_t i = tid % Z; // for i in range(Z)
    uint32_t idx_row = tid / Z; // for idx_row in range(num_rows)
    if (idx_row >= num_rows) return;

    uint32_t cn_degree = bg_cn_degree[idx_row];

    // list of tuples (idx_col = idx_vn, s),
    // idx_row = idx_cn and msg_offset omitted,
    // msg spread out to idx_cn + idx_vn * num_cn
    uint32_t const* check_nodes = &bg_cn[idx_row * max_degree]; // len(cn) = cn_degree

    __syncwarp();

    uint32_t sign = 0;
    for (uint32_t ii = 0; ii < cn_degree; ++ii) {
        uint32_t cn = check_nodes[ii];

        // see packing layout above
        uint32_t idx_col = cn & 0xffffu; // note: little endian
        uint32_t s = cn >> 16;           // ...

        uint32_t t = llr_total[idx_col*Z + (i+s)%Z] < 0;
        sign = sign ^ t;
    }

    sign = __any_sync(0xffffffff, sign);
    if (threadIdx.x % 32 == 0)
        syndrome[tid / 32] = sign;
}

/* Batched syndrome for early termination: unsat[b] |= any unsatisfied check of codeword b. */
__launch_bounds__(512, 3)
static __global__ void batch_syndrome_kernel(llr_accumulator_t const* __restrict__ llr_total, uint32_t* __restrict__ unsat,
                                             uint32_t const* __restrict__ done, uint32_t Z,
                                             uint32_t const* __restrict__ bg_cn, uint32_t const* __restrict__ bg_cn_degree,
                                             uint32_t max_degree, uint32_t num_rows, uint32_t total_stride) {
    const uint32_t b = blockIdx.y;
    if (done[b])
        return;
    llr_total += b * total_stride;
    const uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    const uint32_t i = tid % Z, idx_row = tid / Z;
    uint32_t sign = 0;
    if (idx_row < num_rows) {
        uint32_t const* check_nodes = &bg_cn[idx_row * max_degree];
        for (uint32_t ii = 0; ii < bg_cn_degree[idx_row]; ++ii) {
            const uint32_t cn = check_nodes[ii];
            sign ^= llr_total[(cn & 0xffffu) * Z + (i + (cn >> 16)) % Z] < 0;
        }
    }
    if (__any_sync(0xffffffff, sign) && (threadIdx.x % 32) == 0)
        atomicOr(&unsat[b], 1u);
}
static __global__ void batch_done_kernel(uint32_t* __restrict__ unsat, uint32_t* __restrict__ done, uint32_t n) {
    const uint32_t b = blockIdx.x * blockDim.x + threadIdx.x;
    if (b < n) {
        if (!done[b] && !unsat[b])
            done[b] = 1;
        unsat[b] = 0;
    }
}

static const uint32_t PACK_BITS_KERNEL_THREADS = 256;

// START marker-pack-bits
__launch_bounds__(PACK_BITS_KERNEL_THREADS, 6)
static __global__ void pack_bits_kernel(llr_accumulator_t const* __restrict__ llr_total, uint8_t* __restrict__ bits, uint32_t block_length,
                                        uint32_t total_stride, uint32_t bits_stride) {
    llr_total += blockIdx.y * total_stride;
    bits += blockIdx.y * bits_stride;
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;

    uint32_t coop_byte = 0;
    // 1 bit per thread
    if (tid < block_length)
        coop_byte = (llr_total[tid] < 0) << (7 - (threadIdx.x & 7)); // note: highest to lowest bit

    // use fast lane shuffles to assemble one byte per group of 8 adjacent threads
    coop_byte += __shfl_xor_sync(0xffffffff, coop_byte, 1); // xxyyzzww
    coop_byte += __shfl_xor_sync(0xffffffff, coop_byte, 2); // xxxxyyyy
    coop_byte += __shfl_xor_sync(0xffffffff, coop_byte, 4); // xxxxxxxx

    // share bytes across thread group to allow one coalesced write by first N threads
    __shared__ uint32_t bit_block_shared[PACK_BITS_KERNEL_THREADS / 8];
    if ((threadIdx.x & 0x7) == 0)
        bit_block_shared[threadIdx.x / 8] = coop_byte;

    __syncthreads();

    // the first (PACK_BITS_KERNEL_THREADS / 8) threads pack 8 bits each
    if (threadIdx.x < PACK_BITS_KERNEL_THREADS / 8 && blockIdx.x * PACK_BITS_KERNEL_THREADS + threadIdx.x * 8 < block_length) {
        bits[blockIdx.x * PACK_BITS_KERNEL_THREADS / 8 + threadIdx.x] = bit_block_shared[threadIdx.x];
    }
}
// END marker-pack-bits

extern "C" uint32_t ldpc_decode(ThreadContext* context_, cudaStream_t stream, uint32_t BG, uint32_t Z,
                                int8_t const* llr_in, uint32_t block_length,
                                uint8_t* llr_bits, uint32_t num_iter,
                                uint32_t perform_syndrome_check) {
    auto& context = context_ ? *context_ : ldpc_decoder_init_context(0);
    if (context_ && stream == 0)
        stream = context_->stream;

    BaseGraph bg = get_basegraph(BG, Z);

    const uint32_t num_llrs = bg.num_cols * Z;
    const uint32_t num_cn = bg.num_rows * Z;
    const uint32_t num_out_bytes = blocks_for(block_length, 8);

#ifdef PRINT_TIMES
    struct timespec ts_begin, ts_cursor, ts_end;
    unsigned long long time_ns;
    size_t message_count;

    clock_gettime( TIMESTAMP_CLOCK_SOURCE, &ts_begin );
#endif

#ifdef USE_GRAPHS
    CHECK_CUDA(cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal));
#endif

    int8_t const *mapped_llr_in = context.llr_in_buffer;
    // START marker-copy-input
    // copy input data to device-visible memory
#ifdef USE_UNIFIED_MEMORY
    memcpy(const_cast<int8_t*>(mapped_llr_in), llr_in, num_llrs * sizeof(*llr_in));
#else
    CHECK_CUDA(cudaMemcpyAsync(const_cast<int8_t*>(mapped_llr_in), llr_in, num_llrs * sizeof(*llr_in), cudaMemcpyHostToDevice, stream));
#endif
    // END marker-copy-input

#ifdef PRINT_TIMES
    clock_gettime( TIMESTAMP_CLOCK_SOURCE, &ts_cursor );

    time_ns = ts_cursor.tv_nsec - ts_begin.tv_nsec + 1000000000ll * (ts_cursor.tv_sec - ts_begin.tv_sec);
    message_count = add_measurement(&input_copy_time, time_ns, 500);
    if (message_count % 500 == 0) {
      time_ns = input_copy_time.avg_ns;
      printf("Input copy time: %llu us %llu ns\n", time_ns / 1000, time_ns - time_ns / 1000 * 1000);
      fflush(stdout);
    }
#endif

    int8_t const* llr_total = mapped_llr_in;

    for (uint32_t i = 0; i < num_iter; ++i) {
        dim3 threads(NODE_KERNEL_BLOCK, UNROLL_NODES);

        // check node update
        dim3 blocks_cn(blocks_for(bg.num_rows * Z, threads.x));
        // note: llr_msg not not read, only written to in first iteration; will be filled with outputs of this function
        update_cn_kernel<<<blocks_cn, threads, 0, stream>>>(
            llr_total, context.llr_msg_buffer,
            Z, bg.cn, bg.cn_degree, bg.cn_stride, bg.num_rows, i==0, 0, 0, nullptr);

        // variable node update
        dim3 blocks_vn(blocks_for(bg.num_cols * Z, threads.x));
        // note: llr_total only written to
        update_vn_kernel<<<blocks_vn, threads, 0, stream>>>(
            context.llr_msg_buffer, mapped_llr_in, context.llr_total_buffer,
            Z, bg.vn, bg.vn_degree, bg.vn_stride, bg.num_cols, bg.num_rows, 0, 0, nullptr);
        llr_total = context.llr_total_buffer;
    }

    uint8_t *mapped_llr_bits_out = context.llr_bits_out_buffer;

    // pack bits
    dim3 threads_pack(PACK_BITS_KERNEL_THREADS);
    dim3 blocks_pack(blocks_for(block_length, threads_pack.x));
    pack_bits_kernel<<<blocks_pack, threads_pack, 0, stream>>>(
        llr_total, mapped_llr_bits_out, block_length, 0, 0);
#ifndef USE_UNIFIED_MEMORY
    CHECK_CUDA(cudaMemcpyAsync(llr_bits, mapped_llr_bits_out, num_out_bytes, cudaMemcpyDeviceToHost, stream));
#endif

    // allow CPU access of output bits while computing syndrome
#if defined(USE_UNIFIED_MEMORY) && !defined(USE_GRAPHS)
    cudaStreamSynchronize(stream);
#endif

#ifdef PRINT_TIMES
    clock_gettime( TIMESTAMP_CLOCK_SOURCE, &ts_cursor );
#endif

    // check syndrome if additional testing is requested
    if (perform_syndrome_check) {
        dim3 threads(512);
        dim3 blocks_cn(blocks_for(num_cn, threads.x));
        compute_syndrome_kernel<<<blocks_cn, threads, 0, stream>>>(
            context.llr_total_buffer, context.syndrome_buffer,
            Z, bg.cn, bg.cn_degree, bg.cn_stride, bg.num_rows);
#ifndef USE_UNIFIED_MEMORY
      CHECK_CUDA(cudaMemcpyAsync(context.host_syndrome_buffer, context.syndrome_buffer, num_cn * sizeof(*context.syndrome_buffer) / 32, cudaMemcpyDeviceToHost, stream));
#endif
    }

#ifdef USE_GRAPHS
    cudaGraph_t graphUpdate = {};
    CHECK_CUDA(cudaStreamEndCapture(stream, &graphUpdate));
    if (context.graphCtx) {
        cudaGraphNode_t errorNode;
        cudaGraphExecUpdateResult updateResult;
        CHECK_CUDA(cudaGraphExecUpdate(context.graphCtx, graphUpdate, &errorNode, &updateResult));
    }
    else
         CHECK_CUDA(cudaGraphInstantiate(&context.graphCtx, graphUpdate, 0));
    cudaGraphDestroy(graphUpdate);
    CHECK_CUDA(cudaGraphLaunch(context.graphCtx, stream));
#endif

#if !defined(USE_UNIFIED_MEMORY) || defined(USE_GRAPHS)
    // allow CPU access of output bits and syndrome
    cudaStreamSynchronize(stream);
#endif

#ifdef USE_UNIFIED_MEMORY
    // note: GPU synchronized before async syndrome check
    memcpy(llr_bits, mapped_llr_bits_out, num_out_bytes);
#endif

#ifdef PRINT_TIMES
    clock_gettime( TIMESTAMP_CLOCK_SOURCE, &ts_end );

    time_ns = ts_end.tv_nsec - ts_cursor.tv_nsec + 1000000000ll * (ts_end.tv_sec - ts_cursor.tv_sec);
    message_count = add_measurement(&output_copy_time, time_ns, 500);
    if (message_count % 500 == 0) {
      time_ns = output_copy_time.avg_ns;
      printf("Output copy time: %llu us %llu ns\n", time_ns / 1000, time_ns - time_ns / 1000 * 1000);
      fflush(stdout);
    }
#endif

    if (perform_syndrome_check) {
      uint32_t* p_syndrome;
#ifdef USE_UNIFIED_MEMORY
    #ifndef USE_GRAPHS
      // allow reading syndrome
      cudaStreamSynchronize(stream);
    #endif
      p_syndrome = context.syndrome_buffer;
#else
      // note: already synchronized above
      p_syndrome = context.host_syndrome_buffer;
#endif

      // check any errors indicated by syndrome
      for (uint32_t i = 0; i < num_cn / 32; i++) {
          if (p_syndrome[i] != 0) {
              return num_iter+1;
          }
      }
    }

#ifdef PRINT_TIMES
    clock_gettime( TIMESTAMP_CLOCK_SOURCE, &ts_end );

    time_ns = ts_end.tv_nsec - ts_begin.tv_nsec + 1000000000ll * (ts_end.tv_sec - ts_begin.tv_sec);
    message_count = add_measurement(&decoding_time, time_ns, 500);
    if (message_count % 500 == 0) {
      time_ns = decoding_time.avg_ns;
      printf("CUDA sync runtime: %llu us %llu ns\n", time_ns / 1000, time_ns - time_ns / 1000 * 1000);
      fflush(stdout);
    }
#endif

    return num_iter-1; // note: now index of successful iteration
}

ThreadContext& ldpc_decoder_init_context(int make_stream) {
    auto& context = thread_context;
    if (context.llr_in_buffer) // lazy
        return context;

    printf("Initializing LDPC context (TID %d)\n", (int) gettid());

    if (make_stream) {
        int highPriority = 0;
        if (cudaDeviceGetStreamPriorityRange(NULL, &highPriority))
            printf("CUDA stream priorities unsupported, %s:%d", __FILE__, __LINE__);
        CHECK_CUDA(cudaStreamCreateWithPriority(&context.stream, cudaStreamNonBlocking, highPriority));

        cudaStreamAttrValue attr = {};
        attr.syncPolicy = cudaSyncPolicyBlockingSync; /* the offload exists to free CPU */
        cudaStreamSetAttribute(context.stream, cudaStreamAttributeSynchronizationPolicy, &attr);
    }

    CHECK_CUDA(cudaMallocStaging(&context.llr_in_buffer, MAX_BG_COLS * MAX_Z * sizeof(int8_t), cudaHostAllocMapped | cudaHostAllocWriteCombined));
    CHECK_CUDA(cudaMallocStaging(&context.llr_bits_out_buffer, (MAX_BLOCK_LENGTH + 7) / 8 * sizeof(uint8_t), cudaHostAllocMapped));
    CHECK_CUDA(cudaMallocStaging(&context.syndrome_buffer, MAX_BG_ROWS * MAX_Z * sizeof(uint32_t) / 32, cudaHostAllocMapped));
#ifndef USE_UNIFIED_MEMORY
    context.host_syndrome_buffer = (uint32_t*) malloc(MAX_BG_ROWS * MAX_Z * sizeof(uint8_t));
#endif
    CHECK_CUDA(cudaMalloc(&context.llr_msg_buffer, MAX_BG_ROWS * MAX_BG_COLS * MAX_Z * sizeof(llr_msg_t)));
    CHECK_CUDA(cudaMalloc(&context.llr_total_buffer, MAX_BG_COLS * MAX_Z * sizeof(llr_accumulator_t)));

    // keep track of active thread contexts for shutdown
    ThreadContext* self = &context;
    __atomic_exchange(&initialized_thread_contexts, &self, &self->next_initialized_context, __ATOMIC_ACQ_REL);

    return context;
}

extern "C" ThreadContext* ldpc_decoder_init(int make_stream) {
    if (bg_cn[0][0])  // lazy, global
        return &ldpc_decoder_init_context(make_stream);

    printf("Initializing LDPC runtime %d\n", (int) gettid());
    /* Blocking sync: a thread waiting on the GPU sleeps instead of spinning. The point of the offload
     * is CPU time -- the passive receiver is CPU-bound -- so a spinning waiter would give it back.
     * Must precede the first CUDA call that creates the context. */
    cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);

    const uint32_t* table_bg_cn_degree[2][8] = { { BG1_CN_DEGREE_TABLE() }, { BG2_CN_DEGREE_TABLE() } };
    const uint32_t* table_bg_vn_degree[2][8] = { { BG1_VN_DEGREE_TABLE() }, { BG2_VN_DEGREE_TABLE() } };
    const uint32_t table_bg_cn_degree_size[2][8] = { { BG1_CN_DEGREE_TABLE(sizeof) }, { BG2_CN_DEGREE_TABLE(sizeof) } };
    const uint32_t table_bg_vn_degree_size[2][8] = { { BG1_VN_DEGREE_TABLE(sizeof) }, { BG2_VN_DEGREE_TABLE(sizeof) } };
    const void* table_bg_cn[2][8] = { { BG1_CN_TABLE() }, { BG2_CN_TABLE() } };
    const void* table_bg_vn[2][8] = { { BG1_VN_TABLE() }, { BG2_VN_TABLE() } };
    const uint32_t table_bg_cn_size[2][8] = { { BG1_CN_TABLE(sizeof) }, { BG2_CN_TABLE(sizeof) } };
    const uint32_t table_bg_vn_size[2][8] = { { BG1_VN_TABLE(sizeof) }, { BG2_VN_TABLE(sizeof) } };

    for (int b = 0; b < 2; ++b) {
        for (int ils = 0; ils < 8; ++ils) {
            CHECK_CUDA(cudaMalloc(&bg_cn_degree[b][ils], table_bg_cn_degree_size[b][ils]));
            CHECK_CUDA(cudaMemcpy(const_cast<uint32_t*>(bg_cn_degree[b][ils]), table_bg_cn_degree[b][ils], table_bg_cn_degree_size[b][ils], cudaMemcpyHostToDevice));
            CHECK_CUDA(cudaMalloc(&bg_vn_degree[b][ils], table_bg_vn_degree_size[b][ils]));
            CHECK_CUDA(cudaMemcpy(const_cast<uint32_t*>(bg_vn_degree[b][ils]), table_bg_vn_degree[b][ils], table_bg_vn_degree_size[b][ils], cudaMemcpyHostToDevice));

            CHECK_CUDA(cudaMalloc(&bg_cn[b][ils], table_bg_cn_size[b][ils]));
            CHECK_CUDA(cudaMemcpy(const_cast<uint32_t*>(bg_cn[b][ils]), table_bg_cn[b][ils], table_bg_cn_size[b][ils], cudaMemcpyHostToDevice));
            CHECK_CUDA(cudaMalloc(&bg_vn[b][ils], table_bg_vn_size[b][ils]));
            CHECK_CUDA(cudaMemcpy(const_cast<uint32_t*>(bg_vn[b][ils]), table_bg_vn[b][ils], table_bg_vn_size[b][ils], cudaMemcpyHostToDevice));

            bg_cn_size[b][ils] = table_bg_cn_size[b][ils];
            bg_vn_size[b][ils] = table_bg_vn_size[b][ils];
        }
    }

    return &ldpc_decoder_init_context(make_stream);
}

/* ---- Batched decode (adaptive-rx, 2026-09-15). One launch per iteration decodes every code block of
 * a TB: per-segment launches measured 88 us/segment at 5 flooding iterations on the RTX 4060 Ti --
 * slower than OAI's CPU decoder (19 us) -- because each launch is latency-bound. A rank-4, 273-PRB,
 * MCS-25 TB is ~115 segments, which only a batch keeps the GPU busy with. ---- */
static const uint32_t BATCH_LLR_STRIDE  = MAX_BG_COLS * MAX_Z;
static const uint32_t BATCH_BITS_STRIDE = (MAX_BLOCK_LENGTH + 7) / 8;
static const uint32_t BATCH_MSG_STRIDE  = MAX_BG_ROWS * MAX_BG_COLS * MAX_Z;

static void ldpc_batch_free(ThreadContext& c) {
    if (!c.b_cap)
        return;
    cudaFreeHost(c.b_llr_host);
    cudaFreeHost(c.b_bits_host);
    cudaFree(c.b_llr_dev);
    cudaFree(c.b_bits_dev);
    cudaFree(c.b_msg);
    cudaFree(c.b_total);
    cudaFree(c.b_done);
    cudaFree(c.b_unsat);
    c.b_cap = 0;
}

static void ldpc_batch_reserve(ThreadContext& c, uint32_t n) {
    if (n <= c.b_cap)
        return;
    uint32_t cap = c.b_cap ? c.b_cap : 16;
    while (cap < n)
        cap *= 2;
    ldpc_batch_free(c);
    CHECK_CUDA(cudaHostAlloc(&c.b_llr_host, (size_t)cap * BATCH_LLR_STRIDE, cudaHostAllocDefault));
    CHECK_CUDA(cudaHostAlloc(&c.b_bits_host, (size_t)cap * BATCH_BITS_STRIDE, cudaHostAllocDefault));
    CHECK_CUDA(cudaMalloc(&c.b_llr_dev, (size_t)cap * BATCH_LLR_STRIDE));
    CHECK_CUDA(cudaMalloc(&c.b_bits_dev, (size_t)cap * BATCH_BITS_STRIDE));
    CHECK_CUDA(cudaMalloc(&c.b_msg, (size_t)cap * BATCH_MSG_STRIDE * sizeof(llr_msg_t)));
    CHECK_CUDA(cudaMalloc(&c.b_total, (size_t)cap * BATCH_LLR_STRIDE * sizeof(llr_accumulator_t)));
    CHECK_CUDA(cudaMalloc(&c.b_done, (size_t)cap * sizeof(uint32_t)));
    CHECK_CUDA(cudaMalloc(&c.b_unsat, (size_t)cap * sizeof(uint32_t)));
    c.b_cap = cap;
}

/* Pinned host input for this thread's batch: codeword k's int8 LLRs at k * ldpc_batch_llr_stride(). */
extern "C" int8_t* ldpc_batch_llr_buffer(uint32_t n) {
    ThreadContext& c = *ldpc_decoder_init(1);
    ldpc_batch_reserve(c, n);
    return c.b_llr_host;
}
extern "C" uint32_t ldpc_batch_llr_stride(void) { return BATCH_LLR_STRIDE; }
extern "C" uint32_t ldpc_batch_bits_stride(void) { return BATCH_BITS_STRIDE; }

/* Decode codewords [first, first+n) of the batch buffer (all the same BG and Z) for num_iter flooding
 * iterations; returns the pinned host bits, codeword k at k * ldpc_batch_bits_stride(). */
extern "C" uint8_t const* ldpc_batch_decode(uint32_t BG, uint32_t Z, uint32_t first, uint32_t n,
                                            uint32_t block_length, uint32_t num_iter) {
    ThreadContext& c = *ldpc_decoder_init(1);
    if (n == 0 || first + n > c.b_cap)
        return c.b_bits_host;
    cudaStream_t stream = c.stream;
    BaseGraph bg = get_basegraph(BG, Z);
    int8_t* llr_dev = c.b_llr_dev + (size_t)first * BATCH_LLR_STRIDE;
    llr_accumulator_t* total = c.b_total + (size_t)first * BATCH_LLR_STRIDE;
    llr_msg_t* msg = c.b_msg + (size_t)first * BATCH_MSG_STRIDE;
    uint8_t* bits_dev = c.b_bits_dev + (size_t)first * BATCH_BITS_STRIDE;
    static int bench = -1;
    if (bench < 0) { const char* e = getenv("LDPC_BENCH"); bench = e && atoi(e); }
    cudaEvent_t ev[4];
    if (bench) for (int e = 0; e < 4; e++) cudaEventCreate(&ev[e]);
    if (bench) cudaEventRecord(ev[0], stream);
    CHECK_CUDA(cudaMemcpyAsync(llr_dev, c.b_llr_host + (size_t)first * BATCH_LLR_STRIDE, (size_t)n * BATCH_LLR_STRIDE,
                               cudaMemcpyHostToDevice, stream));
    if (bench) cudaEventRecord(ev[1], stream);
    uint32_t* done = c.b_done + first;
    uint32_t* unsat = c.b_unsat + first;
    CHECK_CUDA(cudaMemsetAsync(done, 0, n * sizeof(uint32_t), stream));
    CHECK_CUDA(cudaMemsetAsync(unsat, 0, n * sizeof(uint32_t), stream));
    dim3 threads(NODE_KERNEL_BLOCK, UNROLL_NODES);
    dim3 blocks_cn(blocks_for(bg.num_rows * Z, threads.x), n);
    dim3 blocks_vn(blocks_for(bg.num_cols * Z, threads.x), n);
    dim3 blocks_syn(blocks_for(bg.num_rows * Z, 512), n);
    llr_accumulator_t const* llr_total = llr_dev;
    for (uint32_t i = 0; i < num_iter; ++i) {
        update_cn_kernel<<<blocks_cn, threads, 0, stream>>>(llr_total, msg, Z, bg.cn, bg.cn_degree, bg.cn_stride,
                                                           bg.num_rows, i == 0, BATCH_LLR_STRIDE, BATCH_MSG_STRIDE, done);
        update_vn_kernel<<<blocks_vn, threads, 0, stream>>>(msg, llr_dev, total, Z, bg.vn, bg.vn_degree, bg.vn_stride,
                                                           bg.num_cols, bg.num_rows, BATCH_LLR_STRIDE, BATCH_MSG_STRIDE, done);
        llr_total = total;
        /* Early termination: a codeword whose hard decisions satisfy every check stops iterating
         * (its kernels return at entry). Device-side only -- no host round trip per iteration. */
        if (i + 1 < num_iter) {
            batch_syndrome_kernel<<<blocks_syn, 512, 0, stream>>>(total, unsat, done, Z, bg.cn, bg.cn_degree,
                                                                  bg.cn_stride, bg.num_rows, BATCH_LLR_STRIDE);
            batch_done_kernel<<<blocks_for(n, 128), 128, 0, stream>>>(unsat, done, n);
        }
    }
    if (bench) cudaEventRecord(ev[2], stream);
    dim3 threads_pack(PACK_BITS_KERNEL_THREADS);
    dim3 blocks_pack(blocks_for(block_length, threads_pack.x), n);
    pack_bits_kernel<<<blocks_pack, threads_pack, 0, stream>>>(llr_total, bits_dev, block_length, BATCH_LLR_STRIDE,
                                                               BATCH_BITS_STRIDE);
    uint8_t* bits_host = c.b_bits_host + (size_t)first * BATCH_BITS_STRIDE;
    CHECK_CUDA(cudaMemcpyAsync(bits_host, bits_dev, (size_t)n * BATCH_BITS_STRIDE, cudaMemcpyDeviceToHost, stream));
    if (bench) cudaEventRecord(ev[3], stream);
    CHECK_CUDA(cudaStreamSynchronize(stream));
    if (bench) {
        static __thread double t_in, t_it, t_out; static __thread long nb, n_done, n_cw;
        float a, b, d;
        cudaEventElapsedTime(&a, ev[0], ev[1]); cudaEventElapsedTime(&b, ev[1], ev[2]); cudaEventElapsedTime(&d, ev[2], ev[3]);
        t_in += a; t_it += b; t_out += d;
        uint32_t h_done[1024]; const uint32_t m = n < 1024 ? n : 1024;
        cudaMemcpy(h_done, done, m * sizeof(uint32_t), cudaMemcpyDeviceToHost);
        for (uint32_t k = 0; k < m; k++) n_done += h_done[k];
        n_cw += m;
        for (int e = 0; e < 4; e++) cudaEventDestroy(ev[e]);
        if (++nb % 200 == 0)
            printf("LDPC_BENCH gpu kernel: copy-in %.1f us, %u iterations %.1f us, pack+copy-out %.1f us per batch; "
                   "early-terminated %.1f%% of code blocks\n", 1000.0 * t_in / nb, num_iter, 1000.0 * t_it / nb,
                   1000.0 * t_out / nb, 100.0 * n_done / (n_cw ? n_cw : 1));
    }
    return bits_host;
}

/* shared pool state (K34), defined here so shutdown can free it */
static ThreadContext g_pool_ctx;            /* launch buffers + stream, owned by the worker thread */
static int8_t* g_pool_llr_host;
static uint8_t* g_pool_bits_host;
static uint32_t g_pool_cap;
static void ldpc_batch_free(ThreadContext& c);

extern "C" void ldpc_decoder_shutdown() {
    cudaDeviceSynchronize();

    ThreadContext* active_context = nullptr;
    __atomic_exchange(&initialized_thread_contexts, &active_context, &active_context, __ATOMIC_ACQ_REL);
    while (active_context) {
        cudaFreeStaging(active_context->llr_in_buffer);
        cudaFree(active_context->llr_msg_buffer);
        cudaFreeStaging(active_context->llr_bits_out_buffer);
        cudaFree(active_context->llr_total_buffer);
        cudaFreeStaging(active_context->syndrome_buffer);
        ldpc_batch_free(*active_context);
#ifndef USE_UNIFIED_MEMORY
        free(active_context->host_syndrome_buffer);
#endif
        if (active_context->stream)
            cudaStreamDestroy(active_context->stream);

#ifdef USE_GRAPHS
        cudaGraphExecDestroy(active_context->graphCtx);
#endif

        active_context = active_context->next_initialized_context;
    }

    /* shared pool (K34): free the real pointers, then forget them so a re-init starts clean */
    if (g_pool_cap) {
        ldpc_batch_free(g_pool_ctx);
        cudaFreeHost(g_pool_llr_host);
        cudaFreeHost(g_pool_bits_host);
        if (g_pool_ctx.stream)
            cudaStreamDestroy(g_pool_ctx.stream);
        g_pool_ctx = ThreadContext();
        g_pool_llr_host = nullptr;
        g_pool_bits_host = nullptr;
        g_pool_cap = 0;
    }

    /* K34 (7): the tables are device pointers; the old code passed the ADDRESS of the table slot
     * (cudaFree(&bg_cn[b][ils])), which freed nothing and returned an error. */
    for (int b = 0; b < 2; ++b) {
        for (int ils = 0; ils < 8; ++ils) {
            cudaFree(const_cast<uint32_t*>(bg_cn_degree[b][ils])); bg_cn_degree[b][ils] = nullptr;
            cudaFree(const_cast<uint32_t*>(bg_vn_degree[b][ils])); bg_vn_degree[b][ils] = nullptr;
            cudaFree(const_cast<uint32_t*>(bg_cn[b][ils]));        bg_cn[b][ils] = nullptr;
            cudaFree(const_cast<uint32_t*>(bg_vn[b][ils]));        bg_vn[b][ils] = nullptr;
        }
    }
}

#ifdef ENABLE_NANOBIND

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

namespace nb = nanobind;

NB_MODULE(ldpc_decoder, m) {
    m.def("decode", [](uint32_t BG, uint32_t Z,
                       const nb::ndarray<int8_t, nb::shape<-1>, nb::device::cpu>& llrs,
                       uint32_t block_length, uint32_t num_iter) {
        auto* context = ldpc_decoder_init(1); // lazy

        size_t num_bytes = (block_length + 7) / 8 * 8;
        uint8_t *data = new uint8_t[num_bytes];
        memset(data, 0, num_bytes);
        nb::capsule owner(data, [](void *p) noexcept { delete[] (uint8_t*) p; });

        ldpc_decode(context, 0, BG, Z, llrs.data(),
                    block_length, data,
                    num_iter, true);

        return nb::ndarray<nb::numpy, uint8_t>(data, {num_bytes}, owner);
    });
}

#endif

/* ---- Shared GPU pool for dynamic batching (adaptive-rx, 2026-09-15) ------------------------------------
 * The per-thread batch path is launch-bound: ~40 launches per TB (10 iterations x CN/VN/syndrome/done),
 * measured 537 us of iterations for 24 already-converged code blocks. Here ONE worker thread decodes the
 * code blocks of every queued TB in one launch sequence. Host side: a pinned pool of slots (a TB's code
 * blocks are contiguous slots). Device side: per-launch buffers indexed by launch position b; each
 * request's slot range is copied to [b0, b0+count) and back, so the batched kernels run unchanged.
 *
 * Safety contract (K34, 2026-10-03): ldpc_pool_decode NEVER returns success for a slot whose bits were
 * not produced by this call. Any CUDA error, skipped or failed launch leaves the affected slots filled
 * with LDPC_POOL_POISON and returns non-zero; the caller must also treat a non-zero status as failure
 * (the poison pattern is defence in depth, not the guarantee: the CPU CRC cannot be proven to reject it
 * for every K). Requests above POOL_MAX_LAUNCH code blocks are split over several launches. */
static const uint32_t POOL_MAX_LAUNCH = 512; /* code blocks per launch: bounds msg memory (~615 MB) */
static uint64_t g_pool_errors, g_pool_poisoned; /* ldpc_pool_counters() */

/* CUDA error inside the pool: counted, logged (rate limited), returned -- never swallowed. */
#define POOL_CHECK(call) do { \
        cudaError_t e_ = (call); \
        if (e_) { rc = (int)e_; pool_log_error(e_, __LINE__); goto fail; } \
    } while (false)

static void pool_log_error(cudaError_t e, int line) {
    __atomic_fetch_add(&g_pool_errors, 1, __ATOMIC_RELAXED);
    static uint64_t logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8) /* one-shot-ish: first 8 only */
        fprintf(stderr, "LDPC_CUDA pool error %d (%s) at ldpc_decoder.cu:%d -> slots poisoned, CPU fallback\n", (int)e,
                cudaGetErrorString(e), line);
}

extern "C" int ldpc_pool_init(uint32_t cap) {
    if (g_pool_cap)
        return 0;
    if (!ldpc_decoder_init(1)) /* global base-graph tables */
        return -1;
    int rc = 0;
    POOL_CHECK(cudaStreamCreateWithFlags(&g_pool_ctx.stream, cudaStreamNonBlocking));
    /* GB10 / unified-memory hosts: pinned host memory is already coherent with the GPU; the explicit
     * copies below are kept (they are device-to-device-local on GB10 and cheap) -- see K34 note. */
    POOL_CHECK(cudaHostAlloc(&g_pool_llr_host, (size_t)cap * BATCH_LLR_STRIDE, cudaHostAllocDefault));
    POOL_CHECK(cudaHostAlloc(&g_pool_bits_host, (size_t)cap * BATCH_BITS_STRIDE, cudaHostAllocDefault));
    ldpc_batch_reserve(g_pool_ctx, POOL_MAX_LAUNCH);
    if (!g_pool_ctx.b_cap || cudaGetLastError() != cudaSuccess) { rc = -1; pool_log_error(cudaErrorMemoryAllocation, __LINE__); goto fail; }
    g_pool_cap = cap;
    return 0;
fail:
    return rc ? rc : -1;
}
extern "C" int8_t* ldpc_pool_host_llr(void) { return g_pool_llr_host; }
extern "C" uint8_t* ldpc_pool_host_bits(void) { return g_pool_bits_host; }
extern "C" uint32_t ldpc_pool_max_launch(void) { return POOL_MAX_LAUNCH; }
extern "C" void ldpc_pool_counters(uint64_t* errors, uint64_t* poisoned) {
    *errors = __atomic_load_n(&g_pool_errors, __ATOMIC_RELAXED);
    *poisoned = __atomic_load_n(&g_pool_poisoned, __ATOMIC_RELAXED);
}

/* Output pattern of a slot that was not decoded. Not all-zero: zero data with a zero CRC would PASS. */
#define LDPC_POOL_POISON 0xA5

static void pool_poison(uint32_t first, uint32_t count) {
    memset(g_pool_bits_host + (size_t)first * BATCH_BITS_STRIDE, LDPC_POOL_POISON, (size_t)count * BATCH_BITS_STRIDE);
    __atomic_fetch_add(&g_pool_poisoned, count, __ATOMIC_RELAXED);
}

/* One launch of up to POOL_MAX_LAUNCH code blocks; chunk i = host slots [first[i], first[i]+count[i]),
 * block length K[i]. Returns 0 or a cudaError_t / negative code; on non-zero the caller poisons. */
static int pool_launch(uint32_t BG, uint32_t Z, uint32_t num_iter, int n_chunk, const uint32_t* first, const uint32_t* count,
                       const uint32_t* K) {
    ThreadContext& c = g_pool_ctx;
    cudaStream_t stream = c.stream;
    int rc = 0;
    uint32_t n = 0;
    for (int r = 0; r < n_chunk; r++)
        n += count[r];
    if (n == 0 || n > POOL_MAX_LAUNCH || !g_pool_cap) {
        __atomic_fetch_add(&g_pool_errors, 1, __ATOMIC_RELAXED);
        return -2; /* never a silent skip */
    }
    const char* skip = getenv("LDPC_CUDA_TEST_SKIP_LAUNCH"); /* test hook (K34 test a) */
    if (skip && atoi(skip)) {
        __atomic_fetch_add(&g_pool_errors, 1, __ATOMIC_RELAXED);
        return -3;
    }
    const char* stall = getenv("LDPC_CUDA_TEST_STALL_MS");   /* test hook (K34 test d) */
    if (stall && atoi(stall) > 0)
        usleep((useconds_t)atoi(stall) * 1000);
    n = 0;
    for (int r = 0; r < n_chunk; r++) {
        POOL_CHECK(cudaMemcpyAsync(c.b_llr_dev + (size_t)n * BATCH_LLR_STRIDE, g_pool_llr_host + (size_t)first[r] * BATCH_LLR_STRIDE,
                                   (size_t)count[r] * BATCH_LLR_STRIDE, cudaMemcpyHostToDevice, stream));
        n += count[r];
    }
    {
    BaseGraph bg = get_basegraph(BG, Z);
    /* The ~4*num_iter iteration launches are replayed from a CUDA graph per (BG, Z, nb, num_iter), nb = n
     * rounded up to a power of two; codewords [n, nb) are pre-marked done (any non-zero word) so their
     * blocks exit at once. LDPC_CUDA_GRAPH=0 launches them directly (A/B). */
    static const bool use_graph = !getenv("LDPC_CUDA_GRAPH") || atoi(getenv("LDPC_CUDA_GRAPH")) != 0;
    uint32_t nb = n;
    if (use_graph)
        for (nb = 1; nb < n; nb <<= 1)
            ;
    POOL_CHECK(cudaMemsetAsync(c.b_done, 0, n * sizeof(uint32_t), stream));
    if (nb > n)
        POOL_CHECK(cudaMemsetAsync(c.b_done + n, 1, (nb - n) * sizeof(uint32_t), stream));
    POOL_CHECK(cudaMemsetAsync(c.b_unsat, 0, nb * sizeof(uint32_t), stream));
    static std::map<uint64_t, cudaGraphExec_t> graphs; /* only the pool worker thread gets here */
    const uint64_t key = (uint64_t)BG << 40 | (uint64_t)Z << 24 | (uint64_t)nb << 8 | num_iter;
    cudaGraphExec_t& ge = graphs[key];
    if (use_graph && ge) {
        POOL_CHECK(cudaGraphLaunch(ge, stream));
    } else {
        if (use_graph)
            POOL_CHECK(cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal));
        dim3 threads(NODE_KERNEL_BLOCK, UNROLL_NODES);
        dim3 blocks_cn(blocks_for(bg.num_rows * Z, threads.x), nb);
        dim3 blocks_vn(blocks_for(bg.num_cols * Z, threads.x), nb);
        dim3 blocks_syn(blocks_for(bg.num_rows * Z, 512), nb);
        llr_accumulator_t const* it_total = c.b_llr_dev;
        for (uint32_t i = 0; i < num_iter; ++i) {
            update_cn_kernel<<<blocks_cn, threads, 0, stream>>>(it_total, c.b_msg, Z, bg.cn, bg.cn_degree, bg.cn_stride,
                                                               bg.num_rows, i == 0, BATCH_LLR_STRIDE, BATCH_MSG_STRIDE, c.b_done);
            update_vn_kernel<<<blocks_vn, threads, 0, stream>>>(c.b_msg, c.b_llr_dev, c.b_total, Z, bg.vn, bg.vn_degree,
                                                               bg.vn_stride, bg.num_cols, bg.num_rows, BATCH_LLR_STRIDE,
                                                               BATCH_MSG_STRIDE, c.b_done);
            it_total = c.b_total;
            if (i + 1 < num_iter) {
                batch_syndrome_kernel<<<blocks_syn, 512, 0, stream>>>(c.b_total, c.b_unsat, c.b_done, Z, bg.cn, bg.cn_degree,
                                                                      bg.cn_stride, bg.num_rows, BATCH_LLR_STRIDE);
                batch_done_kernel<<<blocks_for(nb, 128), 128, 0, stream>>>(c.b_unsat, c.b_done, nb);
            }
            POOL_CHECK(cudaGetLastError()); /* launch-configuration errors surface here */
        }
        if (use_graph) {
            cudaGraph_t g;
            POOL_CHECK(cudaStreamEndCapture(stream, &g));
            cudaError_t ei = cudaGraphInstantiate(&ge, g, 0);
            cudaGraphDestroy(g);
            POOL_CHECK(ei);
            POOL_CHECK(cudaGraphLaunch(ge, stream));
        }
    }
    llr_accumulator_t const* llr_total = num_iter ? c.b_total : c.b_llr_dev;
    uint32_t b0 = 0;
    for (int r = 0; r < n_chunk; r++) {
        dim3 blocks_pack(blocks_for(K[r], PACK_BITS_KERNEL_THREADS), count[r]);
        pack_bits_kernel<<<blocks_pack, PACK_BITS_KERNEL_THREADS, 0, stream>>>(
            llr_total + (size_t)b0 * BATCH_LLR_STRIDE, c.b_bits_dev + (size_t)b0 * BATCH_BITS_STRIDE, K[r],
            BATCH_LLR_STRIDE, BATCH_BITS_STRIDE);
        POOL_CHECK(cudaGetLastError());
        POOL_CHECK(cudaMemcpyAsync(g_pool_bits_host + (size_t)first[r] * BATCH_BITS_STRIDE,
                                   c.b_bits_dev + (size_t)b0 * BATCH_BITS_STRIDE, (size_t)count[r] * BATCH_BITS_STRIDE,
                                   cudaMemcpyDeviceToHost, stream));
        b0 += count[r];
    }
    POOL_CHECK(cudaStreamSynchronize(stream));
    POOL_CHECK(cudaGetLastError());
    }
    return 0;
fail:
    {   /* leave the stream usable: abandon a capture in progress, drain what was queued, clear sticky state */
        cudaStreamCaptureStatus st = cudaStreamCaptureStatusNone;
        if (cudaStreamIsCapturing(stream, &st) == cudaSuccess && st != cudaStreamCaptureStatusNone) {
            cudaGraph_t g = nullptr;
            cudaStreamEndCapture(stream, &g);
            if (g) cudaGraphDestroy(g);
        }
        cudaStreamSynchronize(stream);
        cudaGetLastError();
    }
    return rc ? rc : -1;
}

/* Decode n_req requests sharing BG/Z/num_iter: request r = slots [first[r], first[r]+count[r]) with
 * block length K[r]. Any count; requests above POOL_MAX_LAUNCH blocks are split over several launches.
 * Bits land in the host pool at the same slots. req_rc[r] (optional) is 0 only if every block of request r
 * was decoded by a launch that completed without error; otherwise its slots hold LDPC_POOL_POISON.
 * Returns 0 if all requests succeeded, else the first error. */
extern "C" int ldpc_pool_decode(uint32_t BG, uint32_t Z, uint32_t num_iter, int n_req, const uint32_t* first,
                                const uint32_t* count, const uint32_t* K, int* req_rc) {
    int overall = 0;
    for (int r = 0; r < n_req; r++)
        if (req_rc) req_rc[r] = 0;
    /* greedy pack of (request, sub-range) chunks into launches of <= POOL_MAX_LAUNCH blocks */
    std::vector<uint32_t> cf, cc, ck;
    std::vector<int> cr;
    uint32_t in_launch = 0;
    auto flush = [&]() {
        if (cf.empty()) return;
        int rc = pool_launch(BG, Z, num_iter, (int)cf.size(), cf.data(), cc.data(), ck.data());
        if (rc) {
            if (!overall) overall = rc;
            for (size_t i = 0; i < cf.size(); i++) {
                pool_poison(cf[i], cc[i]);
                if (req_rc) req_rc[cr[i]] = rc;
            }
        }
        cf.clear(); cc.clear(); ck.clear(); cr.clear(); in_launch = 0;
    };
    for (int r = 0; r < n_req; r++) {
        uint32_t done = 0;
        if (count[r] == 0) continue;
        while (done < count[r]) {
            uint32_t take = count[r] - done;
            if (take > POOL_MAX_LAUNCH - in_launch) take = POOL_MAX_LAUNCH - in_launch;
            cf.push_back(first[r] + done); cc.push_back(take); ck.push_back(K[r]); cr.push_back(r);
            in_launch += take;
            done += take;
            if (in_launch == POOL_MAX_LAUNCH) flush();
        }
    }
    flush();
    return overall;
}
