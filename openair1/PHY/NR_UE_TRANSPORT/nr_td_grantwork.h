/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GrantWork-lite (levers spec 2026-10-01 section 9.1, plan Task R1; operator acceleration item 2).
 *
 * One nr_td_grantwork_t per grant (per decoded PDSCH slot): the hypothesis-invariant PHY work --
 * FEP, the full-slot DM-RS channel estimate, equalisation and the decoder-input LLRs -- computed
 * ONCE per geometry signature and kept IMMUTABLE, so that any number of candidate hypotheses
 * sharing that signature can be CB0-decoded from the same buffers (only TBS / segmentation / rate
 * de-matching / LDPC differ between them). A later task batches those CB0 decodes on the GPU.
 *
 * Key = nr_td_grantwork_key(): nr_td_signature() (Task 4c) with the DM-RS IE fields that the
 * effective DM-RS mask already fixes (mapping type, additional position, max length) canonicalised,
 * so two hypotheses that the decoder computes identically always share an entry.
 *
 * Entry life cycle: EMPTY -> COMPUTING (one owner thread) -> READY (immutable) | FAILED (sticky).
 * Several threads may read the same gw: a READY entry is read lock-free after the acquire; a reader
 * of a COMPUTING entry waits for the owner. Entry computation is serialised per gw (one compute lock,
 * recursive, because the lazy path re-enters through the decoder), so the shared FEP buffer is never
 * written by two threads at once.
 *
 * Memory: the LLR buffers come from nr_td_gw_alloc(): cudaMallocManaged (unified memory, GB10) when
 * the CUDA build is enabled (NR_TD_GW_CUDA, libcudart dlopen'd -- no link dependency) and the runtime
 * is present, else plain aligned malloc. Buffers are pooled: no CUDA allocation per grant.
 *
 * The decoder side (how an entry is computed from IQ, the hypothesis -> CB0 parameters) lives in
 * nr_pdsch_passive_decode.c; this unit is pure (pthreads + optional libcudart) and unit-tested by
 * test_nr_td_grantwork. */
#ifndef NR_TD_GRANTWORK_H
#define NR_TD_GRANTWORK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nr_td_legal.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_TD_GW_MAX_SIG 16    /* distinct signatures per grant (measured catalogues: a few) */
#define NR_TD_GW_META_MAX 256  /* opaque decoder-private bytes stored with an entry */

enum {
  NR_TD_GW_OK = 0,
  NR_TD_GW_E_ARG = -1,
  NR_TD_GW_E_FAILED = -2,    /* the entry's computation failed (sticky); see view->status */
  NR_TD_GW_E_FULL = -3,      /* NR_TD_GW_MAX_SIG distinct signatures already present */
  NR_TD_GW_E_STALE = -4,     /* the borrowed FEP buffer / IQ slot is no longer this grant's */
  NR_TD_GW_E_NOCOMPUTE = -5, /* no compute callback */
  NR_TD_GW_E_NOMEM = -6,
  NR_TD_GW_E_RM = -7,        /* rate de-matching refused the geometry (C/Foffset/Ncb) */
  NR_TD_GW_E_K0 = -8,        /* the signature's k0 is not the gw's: another PDSCH slot, another GrantWork */
  NR_TD_GW_E_NOTOWNER = -9,  /* lazy compute refused: not the owning job thread, or the job has ended */
};

/* Per-grant admissibility (fix round 1, I2): conditions under which the full TB of a hypothesis can pass while its
 * CB0 computed from shared work cannot (or vice versa), so a CB0 FAIL from this grant is not evidence. Sticky,
 * OR-ed into the gw by the decoder (main decode) and by this module (STALE / FULL). Read them after the job's main
 * decode. A CB0 result also carries its per-hypothesis bits (LBRM). */
#define NR_TD_GW_F_HARQ 0x01u     /* the main decode combined a HARQ retransmission / used a reserved-MCS record */
#define NR_TD_GW_F_LBRM 0x02u     /* LBRM n_L not latched for the RNTI and E0 > N_ref under some n_L (decode retries) */
#define NR_TD_GW_F_RV_RETRY 0x04u /* ISAC_RV_RETRY: the full decode re-tries rv 2/3/1 */
#define NR_TD_GW_F_ARM 0x08u      /* the main decode ran a PRG or PT-RS arm (the shared work has neither) */
#define NR_TD_GW_F_STALE 0x10u    /* some member could not be computed: borrowed FEP / IQ gone */
#define NR_TD_GW_F_FULL 0x20u     /* some member did not fit (NR_TD_GW_MAX_SIG) */
#define NR_TD_GW_F_GRANT_MASK (NR_TD_GW_F_HARQ | NR_TD_GW_F_RV_RETRY | NR_TD_GW_F_ARM | NR_TD_GW_F_STALE | NR_TD_GW_F_FULL)

/* Immutable view of one computed entry. `llr` stays valid until the gw is released. */
typedef struct {
  uint64_t sig;
  const int16_t *llr; /* demodulated LLRs, DESCRAMBLED (and ISAC_LLR_SCALE'd if set), NOT normalised: the consumer
                       * applies exactly the per-hypothesis K38 shift k_h = nr_llr_norm_shift(llr, ceil(G / C_h))
                       * (nr_llr_norm.h, C_h = nr_llr_norm_num_cb(TBS_h, BG_h); none when ISAC_LLR_NORM=0 or G < 64)
                       * -- nr_td_gw_cb0_input() does it */
  uint32_t G;
  uint8_t nl, qm;
  int status;         /* compute status (decoder-defined) when the entry FAILED */
  const void *meta;   /* decoder-private block published with the entry (NR_TD_GW_META_MAX bytes max) */
  uint32_t meta_len;
  uint64_t compute_ns; /* wall time of the shared (per-signature) computation */
} nr_td_gw_llr_view_t;

typedef struct nr_td_grantwork_s nr_td_grantwork_t;

/* Lazily invoked by nr_td_grantwork_get_llr() for a missing signature, on the calling thread, with the
 * entry already claimed by that thread. Must end with nr_td_grantwork_publish() (directly or through
 * the decoder); returning without a publish marks the entry FAILED with status `rc`. `hint` is the
 * caller's (e.g. an nr_pdsch_cfg_hypothesis_t). */
typedef int (*nr_td_gw_compute_fn)(void *ctx, nr_td_grantwork_t *gw, uint64_t sig, const void *hint);

typedef struct {
  long abs_slot;            /* IQ slot (producer clock) the grant was captured in */
  int slots_per_frame;      /* lifetime: the ring keeps a slot for spf - 2 slots (nr_passive_samples_valid) */
  void *fep;                /* FEP output buffer (borrowed; [ant][samples_per_slot_wCP] c16) */
  const uint64_t *fep_gen_src; /* owner's reuse generation of `fep` (read with __atomic_load_n); NULL = never reused */
  nr_td_gw_compute_fn compute;
  const void *ctx;          /* copied into the gw (ctx_len bytes); the callback gets the copy */
  size_t ctx_len;
} nr_td_gw_job_t;

/* Refcount 1. NULL on bad args / no memory. */
nr_td_grantwork_t *nr_td_grantwork_begin(const nr_td_gw_job_t *job);
nr_td_grantwork_t *nr_td_grantwork_retain(nr_td_grantwork_t *gw);
/* Drops one reference; the last one frees every entry buffer (back to the pool). NULL-safe. */
void nr_td_grantwork_release(nr_td_grantwork_t *gw);
int nr_td_grantwork_refcount(const nr_td_grantwork_t *gw);

/* The entry for `sig`: READY -> view (no compute); EMPTY -> computed once on this thread via the job's
 * callback; COMPUTING elsewhere -> waits. Returns NR_TD_GW_OK or an NR_TD_GW_E_* code. */
int nr_td_grantwork_get_llr(nr_td_grantwork_t *gw, uint64_t sig, const void *hint, nr_td_gw_llr_view_t *out);

/* Producer side (used by the decoder, which IS the compute). */
typedef enum { NR_TD_GW_ACQ_READY = 0, NR_TD_GW_ACQ_OWNER = 1, NR_TD_GW_ACQ_FAILED = 2, NR_TD_GW_ACQ_ERR = 3 } nr_td_gw_acq_t;
/* READY: `out` filled. OWNER: the caller computes the entry and must publish or abandon it; it then holds
 * the gw compute lock. FAILED: the entry failed before (out->status). ERR: full / stale / bad args. */
nr_td_gw_acq_t nr_td_grantwork_acquire(nr_td_grantwork_t *gw, uint64_t sig, nr_td_gw_llr_view_t *out);
/* Copies `llr[0..G)` and `meta` into the entry (unified buffer), marks it READY, wakes waiters. Only the owner. */
int nr_td_grantwork_publish(nr_td_grantwork_t *gw, uint64_t sig, const int16_t *llr, uint32_t G, uint8_t nl, uint8_t qm,
                            const void *meta, uint32_t meta_len);
/* Owner gives up: the entry becomes FAILED with `status` (sticky: the same inputs fail the same way). */
void nr_td_grantwork_abandon(nr_td_grantwork_t *gw, uint64_t sig, int status);

/* Shared FEP state (protected by the compute lock: call only between acquire(OWNER) and publish/abandon).
 * The owner transforms only the symbols it needs ([S, S+L) + DM-RS); later computes extend the mask lazily.
 * fep_mask(): symbols already transformed with this FEP frequency offset (0 when stale / another offset). */
void *nr_td_grantwork_fep(const nr_td_grantwork_t *gw);
uint16_t nr_td_grantwork_fep_mask(const nr_td_grantwork_t *gw, double fo_hz);
void nr_td_grantwork_fep_done(nr_td_grantwork_t *gw, double fo_hz, uint16_t mask);
/* Admissibility flags (NR_TD_GW_F_*). */
void nr_td_grantwork_flag(nr_td_grantwork_t *gw, uint32_t bits);
uint32_t nr_td_grantwork_flags(const nr_td_grantwork_t *gw);
/* LAZY-COMPUTE RULE (fix round 1, I3/I4): a missing entry is computed only on the thread that created the gw (the
 * job's consumer thread) and only until nr_td_grantwork_job_end(); anything else gets NR_TD_GW_E_NOTOWNER (counted).
 * READY entries can be read from any thread for as long as a reference is held. A batch (G4) is called
 * synchronously from the job thread, before job_end, and retains the gw until its stream sync. */
void nr_td_grantwork_job_end(nr_td_grantwork_t *gw);
uint64_t nr_td_grantwork_refused_count(void);
/* False once the borrowed FEP buffer was handed to another slot (generation moved). */
bool nr_td_grantwork_fep_alive(const nr_td_grantwork_t *gw);
long nr_td_grantwork_abs_slot(const nr_td_grantwork_t *gw);
int nr_td_grantwork_slots_per_frame(const nr_td_grantwork_t *gw);
const void *nr_td_grantwork_ctx(const nr_td_grantwork_t *gw);
/* Diagnostics / tests: computations started for `sig` (0 or 1 by construction), entries present. */
int nr_td_grantwork_compute_count(const nr_td_grantwork_t *gw, uint64_t sig);
int nr_td_grantwork_n_entries(const nr_td_grantwork_t *gw);

/* Geometry key: nr_td_signature() with mapping_type / dmrs_add_pos / dmrs_max_len zeroed when dmrs_mask
 * != 0 (the mask is what the decoder reads; those IEs only produce it). mcs_table enters through qm. */
uint64_t nr_td_grantwork_key(const nr_pdsch_cfg_hypothesis_t *h, int nl, int qm);

/* ---- CB0 extraction (per hypothesis, from an entry's LLRs) ----
 * Code block 0 sits at LLR offset 0 with E0 = nr_get_E(G, C, Qm, Nl, 0) bits. Output = the rate-de-matched
 * circular buffer the LDPC segment decoder builds for r = 0: nr_deinterleaving_ldpc(E0, Qm) then
 * nr_rate_matching_ldpc_rx(tbslbrm, BG, Z, d, e, C, rv, clear = 1, E0, F, K - F - 2Z) -- the same calls,
 * the same arguments as nrLDPC_coding_segment_decoder.c. */
typedef struct {
  uint32_t G, A, E, tbslbrm; /* A = TBS bits; E = E0 */
  int C, K, Z, F, BG, Qm, Nl, rv;
} nr_td_cb0_params_t;
/* From an entry's (un-normalised) LLRs to a hypothesis's CB0 input, exactly as a full decode of that hypothesis
 * does it: K38 shift k over llr[0, ceil(G / C)) with C = nr_llr_norm_num_cb(A, BG) when `norm` (ISAC_LLR_NORM on)
 * and G >= 64, applied to code block 0's E LLRs (-> e0, E entries), then nr_td_gw_cb0_extract(). *k_out = k
 * (-1 = normalisation off). */
int nr_td_gw_cb0_input(const int16_t *llr, uint32_t G, const nr_td_cb0_params_t *p, bool norm, int16_t *e0, int16_t *d,
                       int *k_out);
/* d must hold (BG == 1 ? 68 : 52) * Z int16 (the decoder's per-segment stride); it is fully written
 * (entries the bit selection does not reach are 0). Returns NR_TD_GW_OK, NR_TD_GW_E_ARG or NR_TD_GW_E_RM. */
int nr_td_gw_cb0_extract(const int16_t *llr, uint32_t n_llr, const nr_td_cb0_params_t *p, int16_t *d);
static inline int nr_td_gw_cb0_dlen(const nr_td_cb0_params_t *p) { return (p->BG == 1 ? 68 : 52) * p->Z; }

/* ---- Unified-memory allocator (pooled) ---- */
void *nr_td_gw_alloc(size_t bytes);
void nr_td_gw_free(void *p);
/* true when nr_td_gw_alloc() returns cudaMallocManaged memory (ISAC_TD_GW_UNIFIED=0 forces malloc). */
bool nr_td_gw_unified(void);
/* Allocation counters (tests): fresh backing allocations vs pool reuses. */
void nr_td_gw_alloc_stats(uint64_t *fresh, uint64_t *reused, uint64_t *unified);

/* ---- GWTIM: per-grant shared cost vs per-hypothesis incremental cost (ISAC_PDCCH_TIMING) ---- */
enum {
  NR_TD_GWTIM_SHARED = 0, /* one entry: FEP + chest + equalise + LLR (+ descramble/normalise), wall */
  NR_TD_GWTIM_FEP,        /* the FEP part of SHARED */
  NR_TD_GWTIM_CHEST,      /* the channel-estimation part of SHARED */
  NR_TD_GWTIM_DEMOD,      /* equalisation + LLR part of SHARED */
  NR_TD_GWTIM_HIT,        /* an entry served READY (no compute) */
  NR_TD_GWTIM_CB0_RM,     /* per hypothesis: segmentation + deinterleave + rate de-match of CB0 */
  NR_TD_GWTIM_CB0_LDPC,   /* per hypothesis: CPU LDPC of CB0 (+ CRC) */
  NR_TD_GWTIM_N
};
void nr_td_gw_tim_add(int stage, uint64_t ns);
/* Prints "SENSING: GWTIM ..." every 200 CB0 decodes (or when force) if ISAC_PDCCH_TIMING is set. */
void nr_td_gw_tim_report(bool force);
bool nr_td_gw_tim_on(void);
uint64_t nr_td_gw_now_ns(void);

#ifdef __cplusplus
}
#endif
#endif
