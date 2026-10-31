/* SLOTPHASE: cell-level evidence that PDSCH grants fail at a periodic slot phase although their DM-RS is strong, and the CRC arbitration of
 * a provisional rate-match set at that phase. PCI 923 OTA 2026-10-30: grants at abs slot % 40 in {8, 18} failed 0 of 530..1731 strong grants per
 * run (all RNTIs, RBs, Qm), while the periodic multi-port NZP CSI-RS resources discovery logged as WINNER there never reached CONFIRMED, so the
 * PDSCH was not rate-matched around them and its bit stream was misaligned. Nothing in the solver ever tried a rate-match hypothesis in a slot
 * with no confirmed resource.
 *
 * F1 evidence: per cell, first-transmission dedicated CRC outcomes of strong-DM-RS grants (nr_slotphase_strong) are counted per abs_slot % P,
 * P in {10, 20, 40, 80, 160}; a phase is flagged when n >= 30 and the Wilson upper bound of its CRC rate is below half the cell's overall strong
 * CRC rate (n >= 200). Nothing is assumed about WHICH phase: any period that the data shows is flagged.
 * F2/F3 arbitration: at a flagged phase the grants themselves are the probes (no extra decode). The variants are: none (control), the union of
 * the provisional CSI-RS seeds (discovery WINNERs, confirmed or not) occurring in the slot, that union INSTEAD of the confirmed set, and each
 * seed alone. Planning rotates them over the phase's grants, completion counts the CRC per variant; a variant is ADOPTED for the phase (all
 * RNTIs inherit it) when it passed >= 3 times and the lower Wilson bound of its rate exceeds the control's upper bound, and dropped when it
 * stays below 3 passes in 16 trials (or its upper bound is below the control's lower bound). Adopted variants are revoked when their own rate
 * falls below half the cell's. Everything is a function of the data of one cell; per-cell state, cleared with the cell identity.
 * TASK A variants (generic RE patterns, nr_re_pattern.h): the discovery energy FOOTPRINTs seen at the phase's exact (period, offset) are offered as
 * rate-match entries too: each footprint alone INSTEAD of the confirmed set (NR_SP_FOOT), all of them (NR_SP_FUNION), all of them added to the
 * confirmed set (NR_SP_FUNION_CONF). TASK B variants (TDD special / partial slot, or a phase no seed / footprint explains): the grant's PDSCH ends
 * 1, 2 or 3 symbols earlier (NR_SP_TRIM, S kept, L - t). Same CRC arbitration for all.
 * TASK C variants (measured RE pattern, nr_slotphase_resid.h): the flagged phase first collects, on its control grants (decoded as always, CRC failed) and on a bounded
 * reference of CRC-passing strong grants of the cell, the position-keyed decision-directed residual of the GPU equaliser output; nr_spr_estimate turns the excess into RE
 * patterns, offered first as NR_SP_EST (those patterns instead of the confirmed set) and NR_SP_EST_CONF (on top of it). The accumulation runs only for the control grants
 * of a phase in the collecting state and until the reference holds NR_SLOTPHASE_REF_TARGET grants; nowhere else.
 * Thread-safe (one mutex per cell): plan() on the scan thread, note() on the PDSCH decode threads. */
#ifndef NR_PDSCH_PASSIVE_SLOTPHASE_H
#define NR_PDSCH_PASSIVE_SLOTPHASE_H
#include <stdbool.h>
#include <stdint.h>
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
#include "nr_slotphase_resid.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NR_SLOTPHASE_MAX_SEED 4 /* seeds offered per slot (the planner's input bound) */
#define NR_SLOTPHASE_MAX_FOOT 6 /* footprints offered per slot */
#define NR_SLOTPHASE_MAX_TRIM 3 /* end-symbol variants: L-1 .. L-3 */
#define NR_SLOTPHASE_MIN_NSYM 3 /* a trimmed PDSCH keeps at least this many symbols */
#define NR_SLOTPHASE_MAX_N 48     /* trials after which a variant that was not shown better than the control is dropped */
#define NR_SLOTPHASE_NPER 5
#define NR_SLOTPHASE_MIN_N 30     /* strong grants of a phase before it can be flagged */
#define NR_SLOTPHASE_MIN_ALL 200  /* strong grants of the cell before any phase can be flagged */
#define NR_SLOTPHASE_ADOPT_OK 3   /* passes of a variant before it can be adopted */
#define NR_SLOTPHASE_DROP_N 16    /* trials a variant gets to reach NR_SLOTPHASE_ADOPT_OK passes */

#define NR_SLOTPHASE_REF_MIN 32     /* reference grants (CRC passed) before the estimator may look */
#define NR_SLOTPHASE_REF_TARGET 128 /* reference grants after which no more are collected */
#define NR_SLOTPHASE_EST_LOOKS 6    /* sequential looks at 16, 24, 32, 48, 64, 96 failed control grants of the phase */
#define NR_SLOTPHASE_EST_ALPHA 0.01 /* family-wise level over all looks and bins */

enum { NR_SP_CTRL = 0, NR_SP_UNION = 1, NR_SP_REPLACE = 2, NR_SP_SEED = 3, NR_SP_FOOT = 4, NR_SP_FUNION = 5, NR_SP_FUNION_CONF = 6, NR_SP_TRIM = 7,
       NR_SP_EST = 8, NR_SP_EST_CONF = 9 };
enum { NR_SP_EST_COLLECT = 0, NR_SP_EST_POS = 1, NR_SP_EST_NEG = 2 };
enum { NR_SP_EXPLORE = 0, NR_SP_ADOPTED = 1 };

typedef struct {
  uint8_t ent;  ///< 1-based phase entry the slot belongs to; 0 = no flagged phase (decode as always)
  uint8_t var;  ///< variant slot of that entry (0 = control)
  uint8_t kind; ///< NR_SP_*: CTRL, UNION (confirmed + all seeds), REPLACE (all seeds, no confirmed), SEED (one seed), FOOT (one footprint, no confirmed),
                ///< FUNION (all footprints, no confirmed), FUNION_CONF (all footprints + confirmed), TRIM (PDSCH ends .trim symbols earlier),
                ///< EST (the measured RE patterns of nr_slotphase_est_patterns, no confirmed), EST_CONF (measured patterns + confirmed)
  uint8_t seed; ///< NR_SP_SEED: index into the caller's seed_ids[]
  uint8_t foot; ///< NR_SP_FOOT: index into the caller's foot_ids[]
  uint8_t trim; ///< NR_SP_TRIM: symbols removed from the end (1..NR_SLOTPHASE_MAX_TRIM)
} nr_slotphase_choice_t;

/** What the caller knows about the slot being planned. Seeds / footprints: ids unique, stable and nonzero (a variant means the same RE set in every
 *  slot of the phase); @p foot (optional, may be NULL) are the footprints' RE-pattern PDUs, used only for the log line. */
typedef struct {
  const uint32_t *seed_ids;
  int n_seeds;
  const uint32_t *foot_ids;
  int n_foot;
  const fapi_nr_dl_config_csirs_pdu_rel15_t *foot;
  bool has_conf; ///< the slot has confirmed rate-match entries
  int nsym;      ///< the grant's PDSCH length (symbols), 0 = unknown (no end-symbol variants)
  bool trim_ok;  ///< the slot is, or may be, a TDD special / partial slot (end-symbol variants are offered; also offered when no seed / footprint exists)
} nr_slotphase_in_t;

/** Whether a decode is evidence for the phase counters: a real (not layout-probe) first transmission of a dedicated grant with at most two
 *  layers whose DM-RS comb SNR reached the SNR its MCS needs (nr_required_snr_db, nr_snr_gate.h) plus 6 dB. */
bool nr_slotphase_strong(double comb_db, int mcs, int tbl, int nl, int rv, bool dedicated, bool probe);
/** Lock-free: at least one flagged phase exists in this cell (the planner is skipped otherwise). */
bool nr_slotphase_active(void);
/** True when @p slot belongs to a flagged phase (the caller then gathers the slot's seeds for plan()). */
bool nr_slotphase_covers(uint32_t slot);
/** Plan one full decode of PDSCH slot @p slot (abs slot, any origin congruent mod 160). @p gen = nr_csirs_blind_rt_slotphase_gen() (a new value
 *  reopens the undecided phases: the CSI-RS map they were measured under changed). @p token (nonzero) is the decode's solver
 *  token: it tags the job so that nr_slotphase_note() credits the outcome to the variant. */
nr_slotphase_choice_t nr_slotphase_plan(uint32_t slot, uint32_t gen, uint32_t token, const nr_slotphase_in_t *in);
/** Whether the decode tagged with @p token should accumulate the position-keyed residual (nr_pdsch_resid): a control grant of a phase that is collecting, or a grant of
 *  a slot no phase covers while the cell's reference is still short. Lock-free false when no phase is flagged. */
bool nr_slotphase_resid_want(uint32_t token);
/** nr_slotphase_note() plus the grant's residual tables (e / n: [14][NR_SPR_N], NULL = none): a failed control grant of a collecting phase feeds its FAIL table, a passed
 *  grant the cell's reference; the estimator looks when the tables are large enough. */
void nr_slotphase_note_ex(uint32_t slot, uint32_t gen, uint32_t token, bool crc_ok, const float (*e)[NR_SPR_N], const uint32_t (*n)[NR_SPR_N]);
/** The measured RE patterns of the flagged phase covering @p slot (state POS), as RE-pattern PDUs; returns their number (0 = none). */
int nr_slotphase_est_patterns(uint32_t slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max);
/** Diagnostics / tests: estimator state (NR_SP_EST_*) of the phase covering @p slot (-1 = none), grants in its FAIL table and in the reference. */
int nr_slotphase_est_state(uint32_t slot, uint32_t *fail_grants, uint32_t *ref_grants);
/** CRC outcome of a decode that passed nr_slotphase_strong(). @p token as given to plan() (0 / unknown = untagged, counted as control). */
void nr_slotphase_note(uint32_t slot, uint32_t gen, uint32_t token, bool crc_ok);
/** New cell identity: forget everything. */
void nr_slotphase_reset(void);
/** Diagnostics / tests: the entry covering @p slot with the smallest period: returns its period (0 = none), *state NR_SP_*, *adopted variant slot. */
int nr_slotphase_query(uint32_t slot, int *state, int *adopted);
double nr_slotphase_wilson_lo(uint32_t k, uint32_t n);
double nr_slotphase_wilson_hi(uint32_t k, uint32_t n);

#ifdef __cplusplus
}
#endif
#endif
