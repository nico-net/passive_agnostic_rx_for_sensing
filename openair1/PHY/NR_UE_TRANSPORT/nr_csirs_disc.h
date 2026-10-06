/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Fully blind CSI-RS discovery (spec docs/superpowers/specs/2026-10-06-csirs-fully-blind-discovery-design.md).
 * Contract shared by the units U1 (feed/ring), U2 (footprint detector), U3 (hypothesis builder), U4 (sequence sweep)
 * and U5 (confirm/export). Units are implemented in separate files; this header is the only coupling.
 * All grids are CRB-aligned: subcarrier k of `grid` is subcarrier k of CRB 0 of the started grid
 * (k = 12*rb + sc), i.e. rxdataF[(first_carrier_offset + k) % ofdm_symbol_size]. */
#ifndef NR_CSIRS_DISC_H
#define NR_CSIRS_DISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_CSIRS_DISC_MAX_ANT 4
#define NR_CSIRS_DISC_NSYM 14
#define NR_CSIRS_DISC_MAX_RB 275
#define NR_CSIRS_DISC_MAX_FP 32       /* footprints per detector pass */
#define NR_CSIRS_DISC_MAX_HYP 256     /* hypotheses per builder pass */
#define NR_CSIRS_DISC_NUM_PERIODS 13  /* {4,5,8,10,16,20,32,40,64,80,160,320,640} */

extern const uint16_t nr_csirs_disc_periods[NR_CSIRS_DISC_NUM_PERIODS];

/* ---------------- U1: slot ring ---------------- */
typedef struct {
  uint32_t abs_slot;   /* receiver absolute slot counter (monotonic) */
  uint16_t frame;      /* SFN */
  uint8_t slot;        /* slot in frame */
  uint8_t valid;       /* 1 when the entry holds a complete slot */
} nr_csirs_disc_slot_meta_t;

typedef struct {
  int n_ant;                 /* antennas stored (<= NR_CSIRS_DISC_MAX_ANT) */
  int n_rb;                  /* grid width in RB */
  int slots_per_frame;
  int capacity;              /* ring length in slots */
  /* grid of ring entry i: grid_base + i * slot_stride; layout [ant][sym][12*n_rb] of int16 I/Q pairs */
  int16_t *grid_base;
  size_t slot_stride;        /* int16 elements per ring entry */
  nr_csirs_disc_slot_meta_t *meta;   /* [capacity] */
  uint64_t written;          /* entries ever written (ring position = written % capacity) */
  void *private_state;       /* U1 synchronization/allocation state; consumers must not inspect */
} nr_csirs_disc_ring_t;

/* Pointer to the I/Q pairs (int16 re,im interleaved) of entry `idx`, antenna `ant`, symbol `sym`. */
static inline int16_t *nr_csirs_disc_ring_sym(const nr_csirs_disc_ring_t *r, int idx, int ant, int sym)
{
  return r->grid_base + (size_t)idx * r->slot_stride + ((size_t)ant * NR_CSIRS_DISC_NSYM + sym) * (size_t)(24 * r->n_rb);
}

/* ---------------- U2: footprints ---------------- */
typedef struct {
  uint16_t sym_mask;     /* bit l set: symbol l of the slot carries the comb */
  uint16_t sc_mask;      /* bit s set: subcarrier s (0..11) within the RB carries energy */
  uint8_t density;       /* 0 = dot5 even RB, 1 = dot5 odd RB, 2 = one, 3 = three (as nr_csirs_candidate_t) */
  uint16_t rb_start;     /* first RB of the contiguous extent */
  uint16_t n_rb;         /* extent in RB */
  uint16_t period;       /* slots */
  uint16_t offset;       /* abs_slot % period of the occasions */
  float strength;        /* lattice contrast (on-lattice mean contrast / off-lattice), >= 1 */
} nr_csirs_disc_fp_t;

/* ---------------- U3: hypotheses ---------------- */
typedef struct {
  uint8_t row;            /* TS 38.211 Table 7.4.1.5.3-1 row 1..18 */
  uint16_t freq_domain;   /* bitmap as used by nr_generate_csi_rs / get_csi_mapping_parms */
  uint8_t symb_l0, symb_l1;
  uint8_t cdm_type;       /* as fapi: 0 noCDM, 1 fd-CDM2, 2 cdm4-FD2-TD2, 3 cdm8-FD2-TD4 */
  uint8_t density;        /* as nr_csirs_disc_fp_t */
  uint16_t rb_start, n_rb;
  uint16_t period, offset;
  uint8_t fp_index;       /* footprint it explains */
  float fit;              /* pattern IoU vs the footprint, 0..1 */
} nr_csirs_disc_hyp_t;

/* ---------------- U4: sweep results ---------------- */
typedef struct {
  int hyp_index;
  uint16_t scramb_id;     /* best scramblingID */
  int16_t slot_delta;     /* best slot-number offset (0 = our numbering is the cell's) */
  float z;                /* best normalised score (noise ~1, like nr_csirs_blind_correlate_blocks_shift) */
  float z_second;         /* runner-up (different scramb_id) for the ambiguity margin */
  uint16_t n_slots;       /* lattice slots averaged */
} nr_csirs_disc_win_t;

/* ---------------- API ---------------- */
/* U1 (nr_csirs_disc_feed.c) */
bool nr_csirs_disc_enabled(void);                                  /* ISAC_CSIRS_DISC=1 */
int nr_csirs_disc_ring_init(nr_csirs_disc_ring_t *r, int n_ant, int n_rb, int slots_per_frame, int capacity, bool unified);
void nr_csirs_disc_ring_free(nr_csirs_disc_ring_t *r);
/* Copy one FEP'd slot (rxdataF layout of the receiver: [ant][sym*ofdm_symbol_size]) into the ring.
 * Returns ring index written, or -1 when dropped. Never blocks. */
int nr_csirs_disc_ring_push(nr_csirs_disc_ring_t *r, const int16_t *const *rxdataF, int ofdm_symbol_size,
                            int first_carrier_offset, uint32_t abs_slot, uint16_t frame, uint8_t slot);
/* The worker calls its process hook with this lock already held. Exposed for standalone consumers/tests. */
bool nr_csirs_disc_ring_read_lock(nr_csirs_disc_ring_t *r);
void nr_csirs_disc_ring_read_unlock(nr_csirs_disc_ring_t *r);

typedef struct NR_DL_FRAME_PARMS_s NR_DL_FRAME_PARMS;
extern void *(*nr_csirs_disc_alloc_unified)(size_t bytes);
extern void (*nr_csirs_disc_free_unified)(void *ptr);
extern void (*nr_csirs_disc_process_hook)(nr_csirs_disc_ring_t *ring);
extern uint64_t csirs_disc_feed_drops;
int nr_csirs_disc_start(const NR_DL_FRAME_PARMS *fp, int n_ant);
void nr_csirs_disc_stop(void);
bool nr_csirs_disc_running(void);
nr_csirs_disc_ring_t *nr_csirs_disc_worker_ring(void);

/* U2 (nr_csirs_disc_fp.c CPU reference; GPU in nr_csirs_disc_gpu.cu) */
typedef struct {
  uint16_t excl_sym_mask_any;           /* symbols excluded in every slot (e.g. CORESET#0 symbols) */
  const uint8_t *excl_re;               /* optional [14][12*n_rb] mask of REs to ignore (SSB etc.), may be NULL */
  float min_strength;                   /* default 2.0 */
} nr_csirs_disc_fp_cfg_t;
int nr_csirs_disc_footprints_cpu(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_fp_cfg_t *cfg,
                                 nr_csirs_disc_fp_t *out, int max_out);

/* U3 (nr_csirs_disc_hyp.c) */
int nr_csirs_disc_hypotheses(const nr_csirs_disc_fp_t *fps, int n_fp, nr_csirs_disc_hyp_t *out, int max_out);

/* U4 (nr_csirs_disc_sweep.c CPU reference; GPU in nr_csirs_disc_gpu.cu) */
typedef struct {
  int id_first, id_count;               /* scramblingID range, default 0..1023 */
  int delta_max;                        /* slot-number offsets 0..delta_max-1 tried (1 = none) */
  int block_re;                         /* coherent block length in occupied REs, default 32 */
} nr_csirs_disc_sweep_cfg_t;
/* Returns n_hyp on success, 0 while ISAC_CSIRS_DISC is not exactly "1", -1 on error.
 * NULL cfg selects {0,1024,1,32}; explicit cfg values must be positive (id_first may be zero).
 * Noise ~1 refers to an individual sequence score; maximizing a sweep raises the noise maximum.
 * Caller keeps the ring stable during a sweep and initializes OAI modulation tables on startup. */
int nr_csirs_disc_sweep_cpu(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int n_hyp,
                            const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out);
/* Separate U4 MODULE entry, with the same signature as gpu_ops.sweep. */
int nr_csirs_disc_gpu_sweep_entry(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int n_hyp,
                                 const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out);

/* GPU backends (dlopen'd module like pdsch_gpu; NULL/negative when absent). Same semantics as the CPU versions. */
typedef struct {
  int (*footprints)(const nr_csirs_disc_ring_t *, const nr_csirs_disc_fp_cfg_t *, nr_csirs_disc_fp_t *, int);
  int (*sweep)(const nr_csirs_disc_ring_t *, const nr_csirs_disc_hyp_t *, int, const nr_csirs_disc_sweep_cfg_t *,
               nr_csirs_disc_win_t *);
} nr_csirs_disc_gpu_ops_t;
const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_ops(void);   /* NULL when no GPU module */

#ifdef __cplusplus
}
#endif
#endif
