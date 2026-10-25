/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_MEMSTAT_H
#define NR_GPU_MEMSTAT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* GPU / pinned-host memory ledger (mpci plan Task G1, Stage 4 Task 1 GPUMEM).
 *
 * Every GPU allocation of the receiver is booked under a TAG = (module, scope, cell):
 *   module  one of nr_ms_module_t (the allocating source file / subsystem)
 *   scope   NR_MS_SHARED   class-sized or process-wide (pass instances, graph executables, LDPC tables, scratch, constant-size contexts)
 *           NR_MS_PER_CELL sized from one cell's grid / antennas, or holding one cell's state (rxF, HARQ soft store, CSI-RS ring, nID IQ window)
 *   cell    cell index for PER_CELL (callers pass nr_cell_idx() or the cell they are working for; SHARED ignores it)
 * and a KIND: NR_MS_DEV (cudaMalloc), NR_MS_PIN (page-locked / mapped host: cudaHostAlloc, cudaMallocHost, nr_gpu_alloc STAGING/SHARED_RW, the
 * DIRECT-model nr_gpu_buf), NR_MS_MGD (cudaMallocManaged / host-registered pageable memory). Per tag and kind: current bytes, peak bytes, allocation count.
 *
 * Where a call is booked: the wrappers (nr_gpu_alloc, nr_gpu_register_host, nr_gpu_buf_alloc[_dev] and their frees) book by themselves under the tag of the
 * calling thread's SCOPE (nr_gpu_memstat_enter/leave, or the C++ RAII NrMsScope); raw cudaMalloc/cudaHostAlloc sites call nr_gpu_memstat_note_to / the
 * nr_ms_* helpers in nr_gpu_memstat_cuda.cuh. A booking outside any scope lands under NR_MS_UNTAGGED/SHARED, which GPUMEM prints as untagged=<bytes>:
 * the ledger test and the orchestrator read that field, it must be 0 after a full init.
 * Thread safety: all counters are relaxed atomics (peak by CAS), no lock. Bookings happen at allocation time, before the RX fence; nothing runs on the hot path.
 *
 * Log line (nr_gpu_memstat_log, at the RX fence and at process exit), one line, key=value, bytes unless named otherwise:
 *   SENSING: GPUMEM at=<fence|exit> dev=<B> pin=<B> mgd=<B> peak_dev=<B> peak_pin=<B> peak_mgd=<B> allocs=<n> untagged=<B> rss_anon_kb=<kB|-1> cells=<n>
 *       shared=[<mod>:<dev>/<pin>/<mgd>/<n>[+<k>g] ...] cell0=[<mod>:<dev>/<pin>/<mgd>/<n> ...] cell1=[...]
 *   dev/pin/mgd are the sums over all tags (current), peak_* the process-wide high-water marks of those sums; <mod> is nr_gpu_memstat_name(); <n> counts
 *   bookings, +<k>g counts graph executables (bytes not measurable per call: they show in rss_anon_kb and in the membudget graph figure).
 *   Modules with nothing booked are omitted; cellK groups appear only for cells with PER_CELL bytes; rss_anon_kb is RssAnon of /proc/self/status (on the
 *   GB10 unified memory is counted there, not in dev); -1 when unreadable.
 */
typedef enum {
  NR_MS_UNTAGGED = 0,   /* a booking outside any scope (must stay 0) */
  NR_MS_PLATFORM,       /* nr_gpu_platform.c probe buffers */
  NR_MS_PASS,           /* nr_gpu_pass_graph.cu: instances, rings, HARQ store */
  NR_MS_PASS_GRAPH,     /* nr_gpu_pass_graph.cu: graph executables (count only) */
  NR_MS_PDSCH_FEP,      /* nr_pdsch_gpu_fep.cu */
  NR_MS_PDSCH_DEMOD,    /* nr_pdsch_gpu_demod.cu */
  NR_MS_PDSCH_LDPC,     /* nr_pdsch_gpu_ldpc.cu */
  NR_MS_PDCCH_FEP,      /* nr_pdcch_gpu_fep.cu */
  NR_MS_PUSCH_FEP,      /* nr_pusch_gpu_fep.cuh */
  NR_MS_LDPC_DEC,       /* ldpc_decoder.cu */
  NR_MS_LDPC_CB0,       /* ldpc_cb0.cu */
  NR_MS_TD_CB0,         /* nr_td_cb0_batch.cu */
  NR_MS_CSIRS,          /* nr_csirs_disc_gpu*.cu */
  NR_MS_PDCCH_NID,      /* nr_pdcch_nid_gpu.cu */
  NR_MS_POLAR,          /* nr_polar_sc_cuda.cu */
  NR_MS_EXACT,          /* nr_pdsch_gpu_exact_fep.cuh, nr_gpu_green.h probes */
  NR_MS_MOD_N
} nr_ms_module_t;
typedef enum { NR_MS_SHARED = 0, NR_MS_PER_CELL = 1 } nr_ms_scope_t;
typedef enum { NR_MS_DEV = 0, NR_MS_PIN = 1, NR_MS_MGD = 2, NR_MS_KIND_N } nr_ms_kind_t;
#define NR_GPU_MEMSTAT_CELLS 16 /* must equal NR_CELL_MAX (common/utils/nr_cell.h); checked in the ledger test */

/* A tag packed in 32 bits (stored in buffers so a free books against the allocating tag). */
static inline uint32_t nr_ms_pack(int mod, int scope, int cell) { return (uint32_t)(mod & 0xff) | (uint32_t)(scope & 0xff) << 8 | (uint32_t)(cell & 0xffff) << 16; }

typedef struct {
  uint64_t cur[NR_MS_KIND_N], peak[NR_MS_KIND_N], n[NR_MS_KIND_N]; /* current bytes, peak bytes, allocations ever booked */
  uint64_t ngraph;                                                 /* graph executables (bytes not measurable) */
} nr_gpu_memstat_t;

/* Thread scope: books of the calling thread without an explicit tag go under (mod, scope, cell). Nestable; leave() restores the previous tag.
 * enter returns the previous packed tag, to be passed to leave. cell is ignored for SHARED; a PER_CELL cell outside 0..CELLS-1 books as UNTAGGED.
 * PER_CELL with cell < 0 (the default of NrMsScope) takes the cell of the calling thread, nr_cell_idx() of the host process (resolved by name, the GPU modules do not
 * link nr_cell.c): the module init must run on the cell's own thread, or the caller passes the cell explicitly. */
uint32_t nr_gpu_memstat_enter(int mod, int scope, int cell);
void nr_gpu_memstat_leave(uint32_t prev);
int nr_gpu_memstat_host_cell(void); /* the cell a PER_CELL scope without a cell would take: nr_cell_idx() of the calling thread, 0 when unknown */
uint32_t nr_gpu_memstat_current(void); /* the packed tag of the calling thread (UNTAGGED/SHARED when no scope) */

void nr_gpu_memstat_note(int kind, size_t bytes);                                  /* book under the thread scope */
void nr_gpu_memstat_note_to(int mod, int scope, int cell, int kind, size_t bytes); /* explicit tag */
void nr_gpu_memstat_note_tag(uint32_t tag, int kind, size_t bytes);
void nr_gpu_memstat_release_tag(uint32_t tag, int kind, size_t bytes);             /* a free: current drops, peak stays */
void nr_gpu_memstat_note_graph(void);                                              /* one graph executable under the thread scope (count only) */

/* Readers (relaxed). get(): the counters of one tag; scope SHARED ignores cell. Returns 0, -1 for a bad module. */
int nr_gpu_memstat_get(int mod, int scope, int cell, nr_gpu_memstat_t *out);
uint64_t nr_gpu_memstat_cell_bytes(int cell, int kind); /* sum over modules of the PER_CELL bytes of one cell (current) */
uint64_t nr_gpu_memstat_total(int scope, int kind);     /* sum over modules (and cells) of current bytes by scope */
uint64_t nr_gpu_memstat_total_peak(int kind);           /* process-wide peak of the sum over every tag */
uint64_t nr_gpu_memstat_total_count(void);              /* bookings */
const char *nr_gpu_memstat_name(int mod);
long nr_gpu_memstat_rss_anon_kb(void);                  /* RssAnon of /proc/self/status, -1 when unreadable */

/* Formats the GPUMEM line (grammar above, without the "SENSING: " prefix) into out; returns the length that would be written (snprintf semantics). */
int nr_gpu_memstat_format(char *out, size_t n, const char *at);
/* Emits it through the platform logger (LOG_I in the receiver, stderr otherwise); at == "exit" always writes to stderr (the logger may be gone). */
void nr_gpu_memstat_log(const char *at);
void nr_gpu_memstat_reset_for_test(void);
void nr_gpu_memstat_set_cell_fn_for_test(int (*fn)(void)); /* test hook: replaces the host cell lookup (NULL = by name) */
#ifdef __cplusplus
}
/* RAII scope for C++/CUDA modules: NrMsScope ms(NR_MS_PASS, NR_MS_SHARED); */
struct NrMsScope {
  uint32_t prev;
  NrMsScope(int mod, int scope, int cell = -1) : prev(nr_gpu_memstat_enter(mod, scope, cell)) {}
  void set(int mod, int scope, int cell = -1) { nr_gpu_memstat_enter(mod, scope, cell); } /* switch the tag within the scope; the destructor restores the original */
  ~NrMsScope() { nr_gpu_memstat_leave(prev); }
  NrMsScope(const NrMsScope &) = delete;
  NrMsScope &operator=(const NrMsScope &) = delete;
};
#endif
#endif
