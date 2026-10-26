/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_CELL_H
#define NR_GPU_CELL_H
/* GPU seam of the single-process multi-PCI receiver (mpci plan Task G1): the per-cell GPU / pinned-host bytes of a cell, by kind.
 *
 * Plain C, no CUDA, no OAI headers: it builds on the cloud x86 host (no GPU) exactly as on the DGX, so the admission model (common/utils/nr_cell_admit.c)
 * and the allocators use ONE set of sizing expressions. The static inline functions below are the expressions the GPU modules call at their allocation
 * sites (nr_pdsch_gpu_fep.cu, nr_pdcch_gpu_fep.cu, nr_pdcch_nid_gpu.cu, nr_gpu_pass_graph.cu), so the estimate cannot drift from the allocation; each
 * module static_asserts the constants it still owns against the ones here. The ledger test (tests/nr_gpu_cell_bytes_test.cc) allocates through the real
 * module inits and compares the GPUMEM ledger with nr_gpu_cell_bytes().
 *
 * Kinds (all PER_CELL unless stated) return {dev, pin}: dev = cudaMalloc / nr_gpu_alloc(DEVICE), pin = mapped pinned host (nr_gpu_alloc STAGING / SHARED_RW).
 * The split does not depend on the platform tier because these modules allocate through nr_gpu_allocate (tier independent); the DISCRETE-only staging of
 * nr_gpu_buf_t (demod / chest staging) is not a kind here (it is sized from the memory budget, not from the cell).
 * Class-sized memory (pass instances, LLR output buffers, LDPC tables, graph executables, FFT tables) is SHARED and is not in this file.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int n_rb;        /* N_RB_DL of the cell (0 = the largest grid of the SCS) */
  int n_rx;        /* RX antennas (0 = 4) */
  int scs_khz;     /* 15, 30, 60, 120 (0 = 30) */
  int max_layers;  /* reserved: the pass buffers are class-sized at 4 layers, no kind depends on it yet */
  int nid_dur;     /* PDCCH nID window CORESET duration 1..3 (0 = 3, policy default nr_pdcch_nid_gpu.c) */
  int nid_window;  /* PDCCH nID window batches (0 = 10, policy default nr_pdcch_nid_gpu.c) */
} nr_gpu_cell_params_t;

typedef enum {
  NR_GPU_CELL_HARQ = 0,        /* DL HARQ soft store of the pass (nr_gpu_pass_graph.cu g_hq): dev */
  NR_GPU_CELL_PDSCH_FEP,       /* nr_pdsch_gpu_fep.cu: IQ staging + device copy, FFT in/out, check, rotation */
  NR_GPU_CELL_PDSCH_FEP_EXACT, /* nr_pdsch_gpu_fep.cu: bit-exact FEP working set and the per-pass-slot IQ rings (when the twin covers the FFT size) */
  NR_GPU_CELL_PDCCH_FEP,       /* nr_pdcch_gpu_fep.cu: CORESET rxF device + staging */
  NR_GPU_CELL_NID_WINDOW,      /* nr_pdcch_nid_gpu.cu: the window buffers for nid_window batches (win_alloc) */
  NR_GPU_CELL_NID_STAGING,     /* nr_pdcch_nid_gpu.cu: the three pinned IQ staging batches */
  NR_GPU_CELL_CSIRS_FIXED,     /* CSI-RS discovery: resident detector, FEP workspace, sweep buffers, local-memory reservation: the MODEL nr_csirs_disc_mem_fixed (not asserted equal to the ledger) */
  NR_GPU_CELL_CSIRS_SLOT,      /* CSI-RS discovery: bytes per ring slot of capacity (model nr_csirs_disc_mem_per_slot) */
  NR_GPU_SHARED_NID_FIXED,     /* NOT per cell: the one nID context's fixed part (process-wide), here so admission has the exact figure */
  NR_GPU_CELL_KIND_N
} nr_gpu_cell_kind_t;

typedef struct { size_t dev, pin; } nr_gpu_cell_mem_t;

/* ---- constants the sizing depends on (each module static_asserts its own copy against these) ---- */
#define NR_GPU_CELL_HQ_BLK 2048          /* nr_gpu_pass.h NR_GPU_HQ_BLK */
#define NR_GPU_CELL_HQ_SEG (66 * 384)    /* nr_gpu_pass.h NR_GPU_HQ_SEG */
#define NR_GPU_CELL_MAX_SYM 14           /* nr_pdsch_gpu_fep.cu MAX_SYM */
#define NR_GPU_CELL_PASS_SLOTS 8         /* nr_pdsch_gpu_fep.cu PASS_SLOTS = NR_GPU_PASS_RING (4 + 2 + 1) + 1 */
#define NR_GPU_CELL_PDCCH_MAX_DUR 3      /* nr_pdcch_gpu_fep.cu MAX_DUR */
#define NR_GPU_CELL_NID_BATCH 336        /* nr_pdcch_nid_gpu.h NR_PDCCH_NID_BATCH */
#define NR_GPU_CELL_NID_STAT (6 * 14 * 65536) /* nr_pdcch_nid_gpu.cu STAT = 6 * CELLS, CELLS = 14 * IDS, IDS = 65536 */
#define NR_GPU_CELL_NID_SNAP_BYTES 56    /* sizeof(st2_snap_t): 6 pointers + int, padded (nr_pdcch_nid_algorithm.h) */
#define NR_GPU_CELL_NID_DEVICE_BYTES 8425776   /* sizeof(Device) of nr_pdcch_nid_gpu.cu (static_asserted there) */
#define NR_GPU_CELL_NID_IDS 65536
#define NR_GPU_CELL_NID_CHUNK 4          /* CHUNK */
#define NR_GPU_CELL_NID_HYP_CAP (1 << 20) /* NID_HYP_CAP */
#define NR_GPU_CELL_NID_HYP_BYTES 56     /* sizeof(st2_hyp_t) */
#define NR_GPU_CELL_NID_SUMMARY_BYTES 12 /* sizeof(Summary) */
#define NR_GPU_CELL_CSIRS_BATCH 100      /* nr_csirs_disc.h NR_CSIRS_DISC_DEFAULT_BATCH */
#define NR_GPU_CELL_NID_DUR_DEFAULT 3    /* nr_pdcch_nid_gpu.c policy.dur */
#define NR_GPU_CELL_NID_WINDOW_DEFAULT 10 /* nr_pdcch_nid_gpu.c policy.window */

/* ---- geometry of a grid (the OAI numerology tables, B3 geom_of) ---- */
typedef struct { int n_rb, n_rx, scs_khz, mu, fft, cp, cp0, ns; size_t sps; } nr_gpu_cell_geom_t;
static inline int nr_gpu_cell_max_grid(int scs) { return scs == 15 ? 270 : scs == 60 ? 135 : scs == 120 ? 264 : 273; }
static inline nr_gpu_cell_geom_t nr_gpu_cell_geom(const nr_gpu_cell_params_t *p)
{
  nr_gpu_cell_geom_t g;
  g.scs_khz = p->scs_khz > 0 ? p->scs_khz : 30;
  g.n_rb = p->n_rb > 0 ? p->n_rb : nr_gpu_cell_max_grid(g.scs_khz);
  g.n_rx = p->n_rx > 0 ? (p->n_rx > 4 ? 4 : p->n_rx) : 4;
  g.mu = g.scs_khz == 15 ? 0 : g.scs_khz == 30 ? 1 : g.scs_khz == 60 ? 2 : 3;
  int f = 128;
  while (f <= g.n_rb * 12) f <<= 1; /* smallest power of two above the occupied subcarriers (get_samplerate_and_bw at 51, 106, 133, 162, 189, 217, 273) */
  g.fft = f;
  g.cp = f / 128 * 9;
  g.cp0 = f / 128 * (9 + (1 << g.mu));
  g.ns = NR_GPU_CELL_MAX_SYM;
  g.sps = (size_t)g.cp0 + (size_t)f + (size_t)(g.ns - 1) * ((size_t)f + (size_t)g.cp); /* samples_per_slot as nr_pdsch_gpu_fep.cu allocate_init */
  return g;
}

/* ---- nr_pdsch_gpu_fep.cu allocate_init ---- */
static inline size_t nr_gpu_cell_fep_rx_bytes(int nant, size_t sps, int N) { return (size_t)nant * (sps + (size_t)N) * 4; }  /* short2: h_rx, d_rx, one pass ring slot */
static inline size_t nr_gpu_cell_fep_fft_bytes(int nant, int ns, int N) { return (size_t)nant * (size_t)ns * (size_t)N * 8; } /* float2: d_fft_in, d_rxF */
static inline size_t nr_gpu_cell_fep_check_bytes(int ns, int N) { return (size_t)ns * (size_t)N * 8; }                          /* float2: h_check */
static inline size_t nr_gpu_cell_fep_rot_bytes(void) { return (size_t)NR_GPU_CELL_MAX_SYM * 4; }                                 /* short2: h_rot, d_rot */
static inline size_t nr_gpu_cell_fep_exact_bytes(int nant, int N) { return (size_t)nant * NR_GPU_CELL_MAX_SYM * (size_t)N * 4; } /* short2: d_exact, h_exact */
/* ---- nr_pdcch_gpu_fep.cu allocate_init ---- */
static inline size_t nr_gpu_cell_pdcch_rxf_bytes(int nant, int N) { return (size_t)nant * NR_GPU_CELL_PDCCH_MAX_DUR * (size_t)N * 8; } /* float2: g_d_rxF, g_h_rxF_stage */
/* ---- nr_pdcch_nid_gpu.cu ---- */
static inline size_t nr_gpu_cell_nid_window_dev_bytes(int rb, int dur, int window) /* win_alloc(): iq, slots, hh, hz, lit, active, sn, scratch */
{
  const size_t n = (size_t)window * NR_GPU_CELL_NID_BATCH, sc = 12 * (size_t)rb;
  return n * (size_t)dur * sc * 4 + 2 * n * 4 + n * 4 * 2 + (4 * n + 1) * NR_GPU_CELL_NID_SNAP_BYTES + ((4 * n + 1) * 6 * 276 + 2 * n) * 8
         + (size_t)window * NR_GPU_CELL_NID_STAT * (2 + 4);
}
/* the one nID context's window-independent device bytes (fixed_bytes() of the module; the Device struct + chunk workspaces + history + hypotheses) */
static inline size_t nr_gpu_cell_nid_fixed_bytes(void)
{
  const size_t cells = 14 * (size_t)NR_GPU_CELL_NID_IDS, chunk_ids = (size_t)NR_GPU_CELL_NID_CHUNK * NR_GPU_CELL_NID_IDS, stat = NR_GPU_CELL_NID_STAT;
  return NR_GPU_CELL_NID_DEVICE_BYTES + chunk_ids * 4 + 2 * chunk_ids * 4 + 4 * chunk_ids * 4 + 6 * cells * (4 + 2) + stat * (2 + 8) + stat / 256 * NR_GPU_CELL_NID_SUMMARY_BYTES
         + (size_t)NR_GPU_CELL_NID_HYP_CAP * NR_GPU_CELL_NID_HYP_BYTES;
}
static inline size_t nr_gpu_cell_nid_staging_bytes(int rb, int dur) { return (size_t)3 * NR_GPU_CELL_NID_BATCH * (size_t)dur * 12 * (size_t)rb * 4; }

/* The per-kind {dev, pin} bytes of a cell. Unknown kind: {0, 0}. */
nr_gpu_cell_mem_t nr_gpu_cell_mem(const nr_gpu_cell_params_t *p, int kind);
/* dev + pin: the seam of the plan (nr_gpu_cell_bytes(p, kind)). */
size_t nr_gpu_cell_bytes(const nr_gpu_cell_params_t *p, int kind);
const char *nr_gpu_cell_kind_name(int kind);

#ifdef __cplusplus
}
#endif
#endif
