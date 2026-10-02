/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c
 * \brief Phase 3 Technique D. See the header for why the TB CRC is the required oracle and why
 *        hypotheses are interleaved per grant rather than tested in blocks.
 */

#include "nr_pdsch_config_sweep.h"
#include "nr_crc_evidence.h"
#include "nr_pdsch_qm_oracle.h"
#include "nr_td_order.h"
#include "nr_td_legal.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include "common/utils/LOG/log.h"

/* OBSERVABILITY (2026-09-17). This module had NO logging at all, which made its central claim --
 * "a cell-wide prior is only published once two DISTINCT RNTIs converge on the same fields" --
 * unfalsifiable from a run log: a live no-regression test could not tell "did not promote" from
 * "never executed". Every log below marks a state TRANSITION (a context opening, converging, being
 * reopened, a prior published or withdrawn), so the volume is bounded by how often the receiver
 * actually learns something, not by grant rate. The one event that can be driven by noise RNTIs --
 * context eviction -- is rate limited. */

/* Minimum trials before a hypothesis may be declared. At the measured working rate (~76 % TB CRC)
 * and a wrong-hypothesis rate of ~0, a few hundred trials is already overwhelming; this is set for
 * the case where the link itself is marginal and the true rate is only a few per cent. */
#define SWEEP_MIN_TRIALS 300
/* The winner must beat the runner-up by this ratio, not merely lead it. Two hypotheses that differ
 * only in a field the current traffic never exercises (e.g. a second TDRA entry the scheduler is
 * not using) will score IDENTICALLY -- and in that case the honest answer is "undecided", not a
 * coin flip between them. */
#define SWEEP_WIN_RATIO 3.0
/* Absolute floor: a hypothesis that decodes essentially nothing cannot win by ratio alone against
 * a runner-up that decodes nothing at all. */
#define SWEEP_MIN_RATE 0.02

/* Forward declaration: the per-(typeA,legality) catalog template is built much further down (final
 * review I8), but R30 item 1's mask_needs_typeb() below needs it as the type-A reference set. */
static const nr_pdsch_config_sweep_state_t *catalog_template(int typeA, nr_pdsch_legality_fn_t legality);

int nr_pdsch_config_sweep_init(nr_pdsch_config_sweep_state_t *st, int tda_count)
{
  return nr_pdsch_config_sweep_init_legal(st, tda_count, 0, NULL);
}

bool nr_pdsch_tda_legal(int mapping_type, int S, int L)
{
  if (S < 0 || L < 1 || S + L > 14)
    return false;
  if (mapping_type == 0)
    return S <= 3 && L >= 3;
  if (mapping_type == 1)
    return S <= 12 && L >= 2 && L <= 13;
  return false;
}

void nr_pdsch_k0_slot(int frame, int slot, int slots_per_frame, int k0, int *frame_out, int *slot_out)
{
  const int abs = slot + k0;
  *frame_out = (frame + abs / slots_per_frame) % 1024;
  *slot_out = abs % slots_per_frame;
}

/* Lever P evidence (geometry slots) follows the same rule as ok_unique. */
static inline void clear_geom_evidence(nr_pdsch_config_sweep_state_t *st)
{
  memset(st->geom_key, 0, sizeof(st->geom_key));
  memset(st->ok_geom, 0, sizeof(st->ok_geom));
  st->n_geom = 0;
  st->geom_blocked = false;
}
/* Appends every legal (S,L,k0,add_pos,max_len,mcs_table) entry of ONE mapping type to st, merging any
 * that duplicate an ALREADY-PRESENT effective PDU (S,L,k0,mask,table) anywhere in the catalog: the TB
 * CRC cannot tell two identical PDUs apart, so neither could ever win. Shared by init_legal (mapping
 * type A, always -- see R30 item 1 below) and add_typeb_layer (mapping type B, added later, on
 * evidence). Returns the number of entries added, or -1 if the catalog would have overflowed (whatever
 * already fit stays; the two callers differ only in what they do with that -1). */
static int catalog_add_mapping_type(nr_pdsch_config_sweep_state_t *st, int mt, int typeA,
                                    nr_pdsch_legality_fn_t legality)
{
  static const uint8_t kK0[]     = {0, 1};
  static const uint8_t kAddPos[] = {0, 1, 2, 3};
  static const uint8_t kMaxLen[] = {1, 2};
  static const uint8_t kMcsTab[] = {0, 1, 2};
  int added = 0;
  for (uint8_t S = 0; S <= 12; S++)
   for (uint8_t L = 2; S + L <= 14; L++) {
    if (!nr_pdsch_tda_legal(mt, S, L))
      continue;
    for (unsigned e = 0; e < sizeof(kK0); e++)
     for (unsigned b = 0; b < sizeof(kAddPos); b++)
      for (unsigned c = 0; c < sizeof(kMaxLen); c++)
       for (unsigned d = 0; d < sizeof(kMcsTab); d++) {
        int32_t mask = 0;
        if (legality) {
          mask = legality(typeA, L, S, mt, kAddPos[b], kMaxLen[c]);
          if (mask <= 0)
            continue;
          /* Equivalence is scoped to the SAME (S,L,k0): the data RE range comes from (S,L), so two
           * DIFFERENT (S,L) pairs whose absolute dmrs_mask bit pattern happens to coincide are NOT
           * the same effective PDU (different data REs either side of that mask) and must not be
           * merged -- only an entry of the SAME allocation that reaches the same (mask,table) really
           * is indistinguishable to the TB CRC. A whole-catalog scan without the (S,L) match silently
           * dropped real type-B entries whenever an unrelated (S,L) elsewhere in the real mask
           * generator's output happened to reuse the same mask value (measured: it ate every entry of
           * a live TypeBTruthIsPinnedByOneOracleObservationAndConverges-style truth). */
          bool equivalent = false;
          for (int i = 0; i < st->n_hyp && !equivalent; ++i)
            equivalent = st->hyp[i].tda_start == S && st->hyp[i].tda_length == L
                        && st->hyp[i].k0 == kK0[e] && st->hyp[i].dmrs_mask == mask
                        && st->hyp[i].mcs_table == kMcsTab[d];
          if (equivalent)
            continue;
        }
        if (st->n_hyp >= NR_PDSCH_SWEEP_MAX_HYP)
          return -1; /* fail closed: caller decides what "did not fit" means for it */
        nr_pdsch_cfg_hypothesis_t *h = &st->hyp[st->n_hyp];
        h->dmrs_mask    = (uint16_t)mask;
        h->tda_start    = S;
        h->tda_length   = L;
        h->k0           = kK0[e];
        h->dmrs_add_pos = kAddPos[b];
        h->dmrs_max_len = kMaxLen[c];
        h->mcs_table    = kMcsTab[d];
        h->mapping_type = mt;
        st->order[st->n_hyp] = st->n_hyp;
        st->ok_unique[st->n_hyp] = 0;
        st->fp_trials[st->n_hyp] = st->sib_trials[st->n_hyp] = 0;
        clear_geom_evidence(st);
        st->n_hyp++;
        added++;
       }
   }
  return added;
}

int nr_pdsch_config_sweep_init_legal(nr_pdsch_config_sweep_state_t *st, int tda_count,
                                   int typeA, nr_pdsch_legality_fn_t legality)
{
  if (st == NULL) {
    return 0;
  }
  memset(st, 0, sizeof(*st));
  st->winner = -1;
  st->sib_pmin = 0.05f;
  st->sib_eps = 1e-6f;
  (void)tda_count; /* Contexts are isolated by the observed index; list width is not inferred here. */

  /* R30 item 1 (2026-09-26, technique-d-regression.md): mapping type A only, unconditionally. Type B
   * used to be built in here too (env ISAC_PDSCH_TYPEB, default on) and grew every fresh catalog
   * 2.9x (750->2154 runtime, 2016->6336 pure) whether or not the cell even uses it -- diluting the
   * trial density on EVERY hypothesis, including this cell's true type-A one, by the same factor.
   * Measured: BASE (type-A only, pre-dilution) still occasionally decoded a TB on the phy-test rig
   * while the diluted catalog decoded 0/6 independent runs in the same wall time. Type B now enters a
   * context only once the air has shown it is needed -- see mask_needs_typeb()/add_typeb_layer()
   * below, hooked from nr_pdsch_config_sweep_observe(). ISAC_PDSCH_TYPEB=0 (pdsch_typeb_enabled())
   * still hard-disables it there, so the env knob keeps its old meaning as a kill switch. */
  catalog_add_mapping_type(st, 0, typeA, legality);
  return st->n_hyp;
}

/* ISAC_PDSCH_TYPEB=0 (read once): hard-disables mapping type B, even via the evidence-triggered path
 * below. Unset/nonzero (default): type B stays available, gated purely by observation. */
static bool pdsch_typeb_enabled(void)
{
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("ISAC_PDSCH_TYPEB");
    enabled = (e != NULL && atoi(e) == 0) ? 0 : 1;
  }
  return enabled != 0;
}

/* R30 item 1's admission test: true when NO mapping-type-A hypothesis of this cell's own catalog can
 * produce `mask`. Reuses the cached type-A template (init_legal's only output now) rather than a
 * bespoke classifier: "not explainable by type A" is exactly what "the first DM-RS is not at
 * dmrs-TypeA-Position" and "a type-B-only mask" both reduce to -- type A's own front-loaded DM-RS
 * symbol is pinned at that one fixed slot position for every (S,L), so no type-A entry's mask can
 * ever land anywhere else, and any other type-B-only pattern is equally absent from the template. */
static bool mask_needs_typeb(uint16_t mask, int typeA, nr_pdsch_legality_fn_t legality)
{
  if (!mask || !legality)
    return false;
  const nr_pdsch_config_sweep_state_t *t = catalog_template(typeA, legality);
  if (!t)
    return false; /* every template slot busy: do not guess */
  for (int i = 0; i < t->n_hyp; i++)
    if (t->hyp[i].dmrs_mask == mask)
      return false;
  return true;
}

/* Appends every legal mapping-type-B entry not already present as an equivalent effective PDU.
 * Mirrors add_k0_layer: only ADDS, never prunes the type-A incumbent, so a wrong trigger costs
 * catalog size, never correctness (the TB CRC still adjudicates). No-op once the catalog already has
 * a type-B entry (a sibling context already widened and this one inherited the same evidence). */
static int add_typeb_layer(nr_pdsch_config_sweep_state_t *st, int typeA, nr_pdsch_legality_fn_t legality)
{
  if (st == NULL || st->n_hyp <= 0 || st->winner >= 0 || !legality || !pdsch_typeb_enabled())
    return 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (st->hyp[i].mapping_type == 1)
      return 0;
  const int before = st->n_hyp;
  if (catalog_add_mapping_type(st, 1, typeA, legality) < 0)
    LOG_W(PHY, "SWEEP: type-B layer did not fully fit a context holding %d (max %d): kept what fit\n",
          before, NR_PDSCH_SWEEP_MAX_HYP);
  return st->n_hyp - before;
}

/* OBSERVED DM-RS SYMBOL MASK (2026-09-15). The DM-RS symbol pattern of a grant is directly
 * measurable (per-symbol DM-RS coherence over its PRBs), and it pins (S,L) x add_pos x max_len to the
 * one or two catalog entries whose effective mask matches -- leaving only mcs_table to the TB CRC.
 * Measured need: 809 live DCI layouts x ~233 hypotheses here = a joint space no probing converges on.
 * Nothing matched -> the catalog is left whole (the measurement may be wrong; a decode still can tell). */
/* Every prune compacts IN PLACE, keeping catalog order: a copy of NR_PDSCH_SWEEP_MAX_HYP hypotheses
 * is 80 KB, too much for a queue-consumer thread's stack. Only matching entries are ever written, so a
 * prune that matches nothing leaves the catalog untouched by construction. Returns 0 (nothing matched,
 * untouched), the unchanged count (everything matched, evidence kept), or the new count with the
 * evidence cleared -- indices have moved, and keeping it would score one hypothesis with another's. */
/* ---- DORMANT MASKS (spec 2026-10-01 section 4): the one place that knows what "active" means ---------------------------- */
static inline bool dorm_bit(const nr_pdsch_config_sweep_state_t *st, int c, int i)
{
  return (st->dormant[c][i >> 6] >> (i & 63)) & 1u;
}
/* active(i) = fail_open || no cause marks i. Every skip in this file goes through here. */
static inline bool active(const nr_pdsch_config_sweep_state_t *st, int i)
{
  if (st->fail_open)
    return true;
  for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++)
    if (dorm_bit(st, c, i))
      return false;
  return true;
}
/* Lever C evidence is only valid for one active set: every active-set change restarts it (as since_pass). */
static inline void lever_c_restart(nr_pdsch_config_sweep_state_t *st)
{
  memset(st->ok_unique, 0, sizeof(st->ok_unique));
  st->crc_accept_blocked = false;
  clear_geom_evidence(st);
  memset(st->fp_trials, 0, sizeof(st->fp_trials)); /* fast-path streams follow ok_unique */
  memset(st->sib_trials, 0, sizeof(st->sib_trials));
  st->sib_blocked = false;
  memset(st->sib_t, 0, sizeof(st->sib_t));
}
/* Bits of word w that belong to the live catalogue [0, n). */
static inline uint64_t live_word(int n, int w)
{
  const int lo = w * 64;
  return n >= lo + 64 ? ~UINT64_C(0) : n > lo ? (UINT64_C(1) << (n - lo)) - 1 : 0;
}
static int count_dormant_union(const nr_pdsch_config_sweep_state_t *st)
{
  int dormant = 0;
  for (int w = 0; w < (st->n_hyp + 63) / 64; w++) {
    uint64_t u = 0;
    for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++)
      u |= st->dormant[c][w];
    dormant += __builtin_popcountll(u & live_word(st->n_hyp, w));
  }
  return dormant;
}
/* Number of hypotheses the acceptance ranges over (also the union-bound class count). */
static inline int n_active_of(const nr_pdsch_config_sweep_state_t *st)
{
  return st->fail_open ? st->n_hyp : st->n_hyp - count_dormant_union(st);
}
/* Drops bits at indices >= n_hyp; an all-dormant result (a destructive prune kept only dormant entries) clears every mask
 * so the "at least one active" invariant holds. */
static void normalize_masks(nr_pdsch_config_sweep_state_t *st)
{
  for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++)
    for (int w = 0; w < NR_TD_DWORDS; w++)
      st->dormant[c][w] &= live_word(st->n_hyp, w);
  if (st->n_hyp > 0 && count_dormant_union(st) >= st->n_hyp)
    memset(st->dormant, 0, sizeof(st->dormant));
}
/* The one compaction step of every destructive prune: keep entry i as entry n (n <= i, ascending), moving its mask bits with it. */
static inline void prune_move(nr_pdsch_config_sweep_state_t *st, int n, int i)
{
  if (n != i) {
    st->hyp[n] = st->hyp[i];
    for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++) {
      const uint64_t b = (st->dormant[c][i >> 6] >> (i & 63)) & 1u;
      st->dormant[c][n >> 6] = (st->dormant[c][n >> 6] & ~(UINT64_C(1) << (n & 63))) | (b << (n & 63));
    }
  }
}

/* Probe counters follow the KL evidence: indices move on every prune, so they are cleared with it. */
static void clear_probe_stats(nr_pdsch_config_sweep_state_t *st)
{
  memset(st->probe_pass, 0, sizeof(st->probe_pass));
  memset(st->probe_fail, 0, sizeof(st->probe_fail));
  memset(st->probe_inconclusive, 0, sizeof(st->probe_inconclusive));
  lever_c_restart(st); /* lever C and lever P evidence follow the KL evidence */
}
static int prune_commit(nr_pdsch_config_sweep_state_t *st, int n)
{
  if (n <= 0 || n == st->n_hyp)
    return n == st->n_hyp ? n : 0;
  st->n_hyp = n;
  normalize_masks(st); /* callers compacted hyp[] AND the masks through prune_move(); drop the stale tail */
  memset(st->trials, 0, sizeof(st->trials));
  memset(st->ok, 0, sizeof(st->ok));
  clear_probe_stats(st);
  st->since_pass = 0;
  for (int i = 0; i < n; i++)
    st->order[i] = i;
  st->cursor = 0;
  st->winner = -1;
  st->winner_by_crc = false;
  return n;
}

int nr_pdsch_config_sweep_prune_mask(nr_pdsch_config_sweep_state_t *st, uint16_t dmrs_mask)
{
  if (st == NULL || st->n_hyp <= 0 || dmrs_mask == 0)
    return 0;
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (st->hyp[i].dmrs_mask == dmrs_mask)
      prune_move(st, n++, i);
  return prune_commit(st, n);
}

static uint8_t qm_table_mask(uint8_t mcs, int qm)
{
  uint8_t mask = 0;
  for (uint8_t t = 0; t < 3; t++)
    if (nr_pdsch_qm_of_mcs(mcs, t) == qm)
      mask |= (uint8_t)(1u << t);
  return mask;
}

static int prune_tables(nr_pdsch_config_sweep_state_t *st, uint8_t mask)
{
  if (st == NULL || st->n_hyp <= 0 || mask == 0)
    return 0;
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (mask & (1u << st->hyp[i].mcs_table))
      prune_move(st, n++, i);
  return prune_commit(st, n);
}

int nr_pdsch_config_sweep_prune_qm(nr_pdsch_config_sweep_state_t *st, uint8_t mcs, int qm)
{
  return prune_tables(st, qm_table_mask(mcs, qm));
}

/* DM-RS symbol masks the oracle has MEASURED on this cell (per-symbol coherence, independent of
 * any hypothesis). A new context is pruned to catalog entries producing one of them. Without this
 * the observation only reached the one context it was made on, and the layout rotation churns
 * contexts (keyed by RNTI x observed TDA index) faster than any of them walks past the add_pos 0
 * entries at the head of the catalog: OTA 2026-09-16, every PARMSET dmrsmask was 0x4 while the
 * oracle read 0x884 on every slot it looked at. */
#define OBS_MASKS_MAX 8
typedef struct {
  uint16_t mask[OBS_MASKS_MAX];
  int8_t   last[OBS_MASKS_MAX]; /* last PDSCH symbol measured with that mask, -1 = unknown */
  int8_t   k0[OBS_MASKS_MAX];   /* k0 of the job the mask was measured on, -1 = unknown */
  int n;
} obs_set_t;
typedef struct {
  bool valid;
  uint64_t configuration;
  uint8_t mcs_table, dmrs_add_pos, dmrs_max_len;
  uint8_t mapping_type; /* the mapping type whose DM-RS IE dmrs_add_pos/max_len were learned on */
} prior_t;

/* PER-RNTI EVIDENCE. mcs-Table, dmrs-AdditionalPosition, maxLength and the DM-RS symbol set are
 * per-UE in the spec (dedicated RRC) and only cell-common in practice. Each RNTI therefore keeps its
 * own prior and observations, SEEDED from the cell-wide ones (the seed is what made sibling TDA
 * contexts converge 29x sooner -- see the CELL-WIDE PRIOR note in the header); the cell-wide ones
 * are PROMOTED only once two distinct RNTIs agree, so one UE with a private config can neither
 * poison the cell prior nor be forced onto it. Single-RNTI behaviour is unchanged by construction:
 * with one RNTI its own prior/observations are exactly what the cell-wide ones used to be, and the
 * (never-promoted) cell-wide ones are read only when the RNTI has nothing of its own. */
#define RNTI_CTX_MAX 64 /* was 16: measured OTA 2026-09-25, 16 real+noise RNTIs already thrashed it */
typedef struct {
  uint16_t rnti;
  uint64_t touched;
  prior_t prior;
  obs_set_t obs;
  uint64_t k0_seen; /* bit k: the k0 oracle saw this RNTI's PDSCH k slots after its DCI (k >= 2) */
  bool typeb_seen; /* R30 item 1: this RNTI's DM-RS oracle has shown a mask type A cannot explain */
} rnti_ctx_t;
static rnti_ctx_t g_rnti[RNTI_CTX_MAX];
static obs_set_t g_obs;   /* cell-wide: observations two RNTIs agree on */
static prior_t   g_prior; /* cell-wide: a prior two RNTIs converged on */
static uint64_t g_generation, g_clock;

/* MEASURED OTA 2026-09-25, lab cell: pure LRU-by-touch evicted the ONE real, continuously-scheduled
 * RNTI 5 times in a 200s run ("SWEEP: new per-RNTI context rnti=0x4768" x5), each eviction wiping its
 * prior/obs via the memset below -- directly contradicting the old comment's assumption that "an
 * active UE is touched every grant and so is never the victim". It CAN be the victim: a burst of
 * one-off noise-floor RNTIs (blind PDCCH false-accepts) touches this table between two of the real
 * RNTI's own grants, and with RNTI_CTX_MAX slots that is enough to make it briefly the
 * least-recently-touched. On a commercial gNB with many concurrently connected UEs this only gets
 * worse, and it directly undermines the cross-RNTI prior-promotion design above (which needs an
 * RNTI's obs/prior to SURVIVE long enough for a second RNTI to agree with it).
 * Fix: protect any slot carrying real evidence (a converged prior, or observations toward one) from
 * eviction by a zero-evidence slot, regardless of recency. Only when EVERY slot already carries
 * evidence do we fall back to evicting the least-recently-touched one of those -- the genuine
 * "burst of more real RNTIs than we have slots for" case, which still logs loudly below. */
static rnti_ctx_t *rnti_ctx(uint16_t rnti, bool create)
{
  int victim = -1;
  bool victim_has_evidence = true;
  for (int i = 0; i < RNTI_CTX_MAX; i++) {
    if (g_rnti[i].rnti == rnti) {
      g_rnti[i].touched = ++g_clock;
      return &g_rnti[i];
    }
    const bool has_evidence = g_rnti[i].prior.valid || g_rnti[i].obs.n > 0;
    const bool better = victim < 0 || (victim_has_evidence && !has_evidence)
                         || (has_evidence == victim_has_evidence && g_rnti[i].touched < g_rnti[victim].touched);
    if (better) {
      victim = i;
      victim_has_evidence = has_evidence;
    }
  }
  if (!create)
    return NULL;
  rnti_ctx_t *r = &g_rnti[victim];
  const uint16_t evicted = r->rnti;
  const bool had_prior = r->prior.valid;
  memset(r, 0, sizeof(*r));
  r->rnti = rnti;
  r->touched = ++g_clock;
  /* Rate limited: a burst of one-off noise-floor RNTIs churns this slot and must not flood. Losing
   * a context that had already CONVERGED is the case worth seeing, so it is logged separately and
   * louder -- that is the "a real UE got evicted by noise" failure the LRU comment warns about. */
  if (evicted && had_prior)
    LOG_W(PHY, "SWEEP: evicted CONVERGED context rnti=0x%04x to make room for rnti=0x%04x\n",
          evicted, rnti);
  else {
    static int s_left = 20;
    if (s_left > 0) {
      s_left--;
      LOG_I(PHY, "SWEEP: new per-RNTI context rnti=0x%04x%s%s\n", rnti,
            evicted ? " (evicted unconverged rnti=" : "", evicted ? "...)" : "");
    }
  }
  return r;
}

static int obs_find(const obs_set_t *o, uint16_t mask)
{
  for (int i = 0; i < o->n; i++)
    if (o->mask[i] == mask)
      return i;
  return -1;
}
/* Record (mask, last symbol, k0) into a set. A later, more specific observation refines the record; a
 * contradiction (a different last symbol under the same mask) relaxes it back to unknown: two TDRA
 * entries can share a mask. Returns the entry index, -1 when the set is full. */
static int obs_record(obs_set_t *o, uint16_t mask, int last_symbol, int k0)
{
  int k = obs_find(o, mask);
  if (k < 0 && o->n < OBS_MASKS_MAX) {
    k = o->n++;
    o->mask[k] = mask;
    o->last[k] = -1;
    o->k0[k] = -1;
  }
  if (k >= 0) {
    if (last_symbol >= 0) o->last[k] = (o->last[k] < 0 || o->last[k] == last_symbol) ? (int8_t)last_symbol : -1;
    if (k0 >= 0) o->k0[k] = (o->k0[k] < 0 || o->k0[k] == k0) ? (int8_t)k0 : -1;
  }
  return k;
}
/* An observation is (mask, last symbol, k0); an entry is consistent with it when its mask matches,
 * its S+L-1 equals the measured last symbol (when measured) and its k0 equals the job's (when the
 * mask was seen in the DCI's own slot the PDSCH is there: k0 of that job). */
static bool obs_admits(const nr_pdsch_cfg_hypothesis_t *h, const obs_set_t *o, int k)
{
  if (h->dmrs_mask != o->mask[k])
    return false;
  if (o->last[k] >= 0 && (int)h->tda_start + (int)h->tda_length - 1 != o->last[k])
    return false;
  if (o->k0[k] >= 0 && h->k0 != o->k0[k])
    return false;
  return true;
}
static bool obs_any_admits(const nr_pdsch_cfg_hypothesis_t *h, const obs_set_t *o)
{
  for (int k = 0; k < o->n; k++)
    if (obs_admits(h, o, k))
      return true;
  return false;
}
/* Keep the entries admitted by any observation of this RNTI or of the cell; untouched if none matches. */
static int prune_to_observed(nr_pdsch_config_sweep_state_t *st, const obs_set_t *own)
{
  if (st == NULL || st->n_hyp <= 0 || ((own ? own->n : 0) + g_obs.n) <= 0)
    return 0;
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if ((own && obs_any_admits(&st->hyp[i], own)) || obs_any_admits(&st->hyp[i], &g_obs))
      prune_move(st, n++, i);
  return prune_commit(st, n);
}

/* Append a k0 layer: every hypothesis of the lowest-k0 layer present, with k0 replaced. The lowest
 * layer holds every (S,L,mask,table) tuple of the catalog: prior/table prunes are k0-agnostic, and a
 * DM-RS observation fixes k0 only to 0 (the oracle measures in the DCI's own slot), so no other layer
 * can carry a tuple the lowest one lacks. Existing indices, evidence and outstanding tickets are
 * untouched; the new entries join the round-robin with zero trials. Fails closed (adds nothing) when
 * the layer would not fit. Returns the number added. */
int nr_pdsch_config_sweep_add_k0_layer(nr_pdsch_config_sweep_state_t *st, uint8_t k0)
{
  if (st == NULL || st->n_hyp <= 0 || st->winner >= 0 || k0 > 32)
    return 0;
  uint8_t lo = st->hyp[0].k0;
  for (int i = 0; i < st->n_hyp; i++) {
    if (st->hyp[i].k0 == k0)
      return 0;
    if (st->hyp[i].k0 < lo)
      lo = st->hyp[i].k0;
  }
  const int n0 = st->n_hyp;
  int layer = 0;
  for (int i = 0; i < n0; i++)
    layer += st->hyp[i].k0 == lo;
  if (n0 + layer > NR_PDSCH_SWEEP_MAX_HYP) {
    LOG_W(PHY, "SWEEP: k0=%u layer (%d hypotheses) does not fit a context holding %d (max %d): not added\n",
          (unsigned)k0, layer, n0, NR_PDSCH_SWEEP_MAX_HYP);
    return 0;
  }
  for (int i = 0; i < n0; i++)
    if (st->hyp[i].k0 == lo) {
      st->hyp[st->n_hyp] = st->hyp[i];
      st->hyp[st->n_hyp].k0 = k0;
      for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++) /* the new layer inherits each source entry's dormancy */
        if (dorm_bit(st, c, i))
          st->dormant[c][st->n_hyp >> 6] |= UINT64_C(1) << (st->n_hyp & 63);
      st->order[st->n_hyp] = st->n_hyp;
      st->ok_unique[st->n_hyp] = 0;
      st->fp_trials[st->n_hyp] = st->sib_trials[st->n_hyp] = 0;
      clear_geom_evidence(st); /* the active set grows: lever P evidence restarts */
      st->n_hyp++;
    }
  return layer;
}

/* dmrs_add_pos/max_len are constrained only on entries of the prior's own mapping type
 * (mapping_type 0xFF = every entry): dmrs-DownlinkForPDSCH-MappingTypeA and -MappingTypeB are separate
 * RRC IEs, so a type-A winner says nothing about a type-B entry's DM-RS. mcs-Table is shared. */
static int prune_prior(nr_pdsch_config_sweep_state_t *st, uint8_t mcs_table, uint8_t dmrs_add_pos,
                       uint8_t dmrs_max_len, uint8_t mapping_type)
{
  if (st == NULL || st->n_hyp <= 0) {
    return 0;
  }
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++) {
    const nr_pdsch_cfg_hypothesis_t *h = &st->hyp[i];
    const bool dmrs_free = mapping_type != 0xFF && h->mapping_type != mapping_type;
    if (h->mcs_table == mcs_table
        && (dmrs_free || (h->dmrs_add_pos == dmrs_add_pos && h->dmrs_max_len == dmrs_max_len))) {
      prune_move(st, n++, i);
    }
  }
  /* Nothing matched: the prior does not describe this catalog at all. The catalog is left in place
   * rather than emptied -- a context with no hypotheses can never converge. */
  return prune_commit(st, n);
}

int nr_pdsch_config_sweep_prune_to(nr_pdsch_config_sweep_state_t *st, uint8_t mcs_table,
                                   uint8_t dmrs_add_pos, uint8_t dmrs_max_len)
{
  return prune_prior(st, mcs_table, dmrs_add_pos, dmrs_max_len, 0xFF);
}

/* SCORE-ORDERED ROUNDS (spec 2026-10-01 §5.1). Side information only changes WHEN a hypothesis is tried
 * inside a balanced round, never whether: every hypothesis still gets exactly one slot per round, and
 * acceptance stays with the TB CRC. Stable descending sort of the freshly shuffled round by
 *   key(i) = nr_td_ordering_score(hyp[i], side) + (probe_pass[i] > 0 ? side->w_probe : 0)
 * so equal keys keep the shuffle's order: with every key 0 the round is exactly the shuffle (neutral side
 * information is bit-identical to no side information). The shuffle itself still runs, so RNG use is
 * unchanged. Bottom-up merge sort on a heap scratch (2 x 32 KB at the cap: too much for a consumer
 * thread's stack); the key array is always allocated, but an all-equal round (the neutral case) skips the
 * merge buffer and the sort. A non-finite key (NaN/Inf from a bad weight) counts as 0, so the comparison
 * stays a strict weak order and the round deterministic. If the scratch
 * cannot be allocated the round keeps the shuffle order -- ordering is a priority, never a correctness
 * input. */
static void score_order_round(nr_pdsch_config_sweep_state_t *st)
{
  const int n = st->n_hyp;
  if (n < 2)
    return;
  float *key = malloc(sizeof(*key) * (size_t)n);
  if (!key)
    return;
  bool varied = false;
  for (int i = 0; i < n; i++) {
    key[i] = nr_td_ordering_score(&st->hyp[i], st->side) + (st->probe_pass[i] > 0 ? st->side->w_probe : 0.0f);
    if (!isfinite(key[i]))
      key[i] = 0.0f;
    varied |= key[i] != key[0];
  }
  int *tmp = varied ? malloc(sizeof(*tmp) * (size_t)n) : NULL;
  if (tmp) {
    int *src = st->order, *dst = tmp;
    for (int w = 1; w < n; w *= 2) {
      for (int lo = 0; lo < n; lo += 2 * w) {
        const int mid = lo + w < n ? lo + w : n;
        const int hi = lo + 2 * w < n ? lo + 2 * w : n;
        int a = lo, b = mid, k = lo;
        /* ">=" takes the LEFT run on ties: stable */
        while (a < mid && b < hi)
          dst[k++] = key[src[a]] >= key[src[b]] ? src[a++] : src[b++];
        while (a < mid)
          dst[k++] = src[a++];
        while (b < hi)
          dst[k++] = src[b++];
      }
      int *swap = src;
      src = dst;
      dst = swap;
    }
    if (src != st->order)
      memcpy(st->order, src, sizeof(*src) * (size_t)n);
  }
  free(tmp);
  free(key);
}

/* ---- K0-sibling guard helpers (fix B, see the header) ---- */
int nr_pdsch_config_sweep_sib_n(int n_sib, double pmin, double eps)
{
  if (n_sib <= 0 || !(pmin > 0.0) || !(eps > 0.0))
    return 0;
  const double n = ceil(log((double)n_sib / eps) / pmin);
  return n > 65535.0 ? 65535 : (n < 1.0 ? 1 : (int)n);
}
/* geometry key without k0: the sibling class of a lead */
static inline uint64_t skey_of(const nr_pdsch_cfg_hypothesis_t *h)
{
  return nr_td_geom_key(h) & ~(UINT64_C(0x3F) << 8); /* k0 occupies bits 8..13; mapping_type (bit 14) stays in the key */
}
static inline bool is_sibling_of(const nr_pdsch_config_sweep_state_t *st, int i, uint64_t skey, uint8_t k0)
{
  return active(st, i) && st->hyp[i].k0 != k0 && skey_of(&st->hyp[i]) == skey;
}
static int count_siblings(const nr_pdsch_config_sweep_state_t *st, uint64_t skey, uint8_t k0)
{
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    n += is_sibling_of(st, i, skey, k0);
  return n;
}
/* true when every ACTIVE sibling has N_sib sibling-test trials (vacuously true with no sibling or the guard disabled) */
static bool siblings_clear(const nr_pdsch_config_sweep_state_t *st, uint64_t skey, uint8_t k0)
{
  if (!(st->sib_pmin > 0.0f))
    return true;
  const int n_sib = count_siblings(st, skey, k0);
  const int need = nr_pdsch_config_sweep_sib_n(n_sib, st->sib_pmin, st->sib_eps);
  for (int i = 0; n_sib > 0 && i < st->n_hyp; i++)
    if (is_sibling_of(st, i, skey, k0) && st->sib_trials[i] < need)
      return false;
  return true;
}
/* The sibling to test next: over the pending targets, the active sibling with the fewest sib_trials below N_sib (ties: lowest index). -1 = none. */
static int pick_sibling(const nr_pdsch_config_sweep_state_t *st)
{
  int best = -1;
  for (int t = 0; t < 2; t++) {
    if (!st->sib_t[t].valid)
      continue;
    const int need = nr_pdsch_config_sweep_sib_n(count_siblings(st, st->sib_t[t].skey, st->sib_t[t].k0), st->sib_pmin, st->sib_eps);
    for (int i = 0; i < st->n_hyp; i++)
      if (is_sibling_of(st, i, st->sib_t[t].skey, st->sib_t[t].k0) && st->sib_trials[i] < need
          && (best < 0 || st->sib_trials[i] < st->sib_trials[best]))
        best = i;
  }
  return best;
}

static int next_core(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out, nr_td_pick_t *kind, bool allow_sib)
{
  if (kind != NULL)
    *kind = NR_TD_PICK_EXPLORE;
  if (st == NULL || out == NULL || st->n_hyp <= 0) {
    return -1;
  }
  if (st->winner >= 0) {
    *out = st->hyp[st->winner];
    return st->winner;
  }
  if (allow_sib && (st->crc_accept || st->geom_pin) && !st->sib_blocked) {
    const int sib = pick_sibling(st);
    if (sib >= 0) {
      *out = st->hyp[sib];
      if (kind != NULL)
        *kind = NR_TD_PICK_SIBLING;
      return sib;
    }
  }
  /* EXPLOIT a hypothesis that has already passed a CRC: three trials in four go to the one with
   * the most passes, the fourth keeps the round-robin exploring. Pure round-robin spent 5/6 of
   * every probe on hypotheses already refuted by evidence, so a layout that hit once took ~1300
   * grants to hit again (rank-4 bed, 809 live layouts, 2026-09-16). Ties keep the lowest index. */
  int hot = -1;
  for (int i = 0; i < st->n_hyp; i++)
    if (active(st, i) && st->ok[i] > 0 && (hot < 0 || st->ok[i] > st->ok[hot])) /* a dormant hot hypothesis falls through to round-robin */
      hot = i;
  /* Only until the hot one has the 64 trials the separation test needs; after that the fair
   * round-robin resumes so a marginal link (5 % true rate) still reaches SWEEP_MIN_TRIALS on
   * every hypothesis. */
  if (hot >= 0 && st->trials[hot] < 64 && (st->exploit_tick++ & 3) != 3) {
    *out = st->hyp[hot];
    if (kind != NULL)
      *kind = NR_TD_PICK_EXPLOIT;
    return hot;
  }
  /* The shuffle always covers all n_hyp (RNG use independent of the masks). Dormant entries are skipped in the round order;
   * when the rest of the round is all dormant the next round starts exactly as a cursor wrap does today. The round bound
   * only guards a violated "one active" invariant (then the entry at the cursor is returned unfiltered). */
  for (int rounds = 0;; rounds++) {
    if (!st->cursor) {
      nr_crc_shuffle(st->order, st->n_hyp, &st->random_state);
      if (st->side)
        score_order_round(st);
    }
    while (st->cursor < st->n_hyp && !active(st, st->order[st->cursor]))
      st->cursor++;
    if (st->cursor < st->n_hyp)
      break;
    st->cursor = 0;
    if (rounds >= 1) /* a full fresh round found no active entry */
      break;
  }
  const int idx = st->order[st->cursor];
  st->cursor = (st->cursor + 1) % st->n_hyp;
  *out = st->hyp[idx];
  return idx;
}

int nr_pdsch_config_sweep_next(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out)
{
  return next_core(st, out, NULL, false);
}
int nr_pdsch_config_sweep_next_ex(nr_pdsch_config_sweep_state_t *st, nr_pdsch_cfg_hypothesis_t *out, nr_td_pick_t *kind)
{
  return next_core(st, out, kind, true);
}

static double rate_of(const nr_pdsch_config_sweep_state_t *st, int i)
{
  return (st->trials[i] > 0) ? ((double)st->ok[i] / (double)st->trials[i]) : 0.0;
}

/* The acceptance decision, shared by every feed path: the KL separation test when check_separation (a credited
 * hypothesis just reached a multiple of 16 trials), then the full-round fallback. Evidence must already be credited. */
static int sweep_decide(nr_pdsch_config_sweep_state_t *st, bool check_separation)
{
  if (check_separation) {
    /* Acceptance ranges over the ACTIVE set only; with no mask set first_active == 0 and n_act == n_hyp (today's rule). */
    const int n_act = n_active_of(st);
    int leader=-1;
    for(int i=0;i<st->n_hyp;i++) {
      if(!active(st,i)) continue;
      if(leader<0 || rate_of(st,i)>rate_of(st,leader)) leader=i;
    }
    if(leader<0) leader=0; /* unreachable (invariant); keeps indices valid */
    double lo,hi;
    /* Class count = the LIVE catalog, not the storage cap: every prune clears evidence and add_k0 only
     * raises n, so the union bound always covers the hypotheses actually competing. */
    nr_crc_interval(st->ok[leader],st->trials[leader],(unsigned)n_act,&lo,&hi);
    /* The absolute floor was 0.60, which silently assumed the TRUE config decodes at >=60 %.
     * MEASURED OTA 2026-09-13: the winning hypothesis decodes at 124/311 = 40 %, so its Wilson
     * lower bound can never reach 0.60 -- early separation could NEVER fire on this link and every
     * acquisition paid the full fallback of SWEEP_MIN_TRIALS x n_hyp (~300 x 233 = 70,000 grants,
     * ~11 min). The pairwise test below is the one that actually carries the evidence: the leader's
     * lower bound must clear EVERY other hypothesis's upper bound. Keep only a token floor so a
     * dead link (everything near zero) cannot "separate", and let the separation test decide. */
    bool separated=st->trials[leader]>=64 && lo>=SWEEP_MIN_RATE;
    for(int i=0;i<st->n_hyp && separated;i++) {
      if(i==leader || !active(st,i)) continue;
      double other_lo,other_hi;
      nr_crc_interval(st->ok[i],st->trials[i],(unsigned)n_act,&other_lo,&other_hi);
      if(other_hi>=lo) separated=false;
    }
    if(separated) { st->winner=leader; return leader; }
  }
  /* Decide only when EVERY hypothesis has had a fair shot -- otherwise the first one to reach the
   * threshold wins by being early in the rotation rather than by being right. */
  for (int i = 0; i < st->n_hyp; i++) {
    if (st->trials[i] < SWEEP_MIN_TRIALS && active(st, i)) {
      return -1;
    }
  }
  int best = -1, second = -1;
  for (int i = 0; i < st->n_hyp; i++) {
    if (active(st, i) && (best < 0 || rate_of(st, i) > rate_of(st, best))) {
      best = i;
    }
  }
  if (best < 0)
    return st->winner; /* unreachable (invariant) */
  for (int i = 0; i < st->n_hyp; i++) {
    if (active(st, i) && i != best && (second < 0 || rate_of(st, i) > rate_of(st, second))) {
      second = i;
    }
  }
  const double rb = rate_of(st, best);
  const double rs = (second >= 0) ? rate_of(st, second) : 0.0;
  if (rb >= SWEEP_MIN_RATE && (rs <= 0.0 || rb >= SWEEP_WIN_RATIO * rs)) {
    st->winner = best;
  }
  return st->winner;
}

/* since_pass bookkeeping, once per public feed CALL: +1 when it credited >= 1 active hypothesis, reset by a PASS credited to
 * an active one. */
static inline void since_pass_update(nr_pdsch_config_sweep_state_t *st, bool credited, bool pass_credited)
{
  if (!credited)
    return;
  if (st->since_pass < UINT32_MAX)
    st->since_pass++;
  if (pass_credited)
    st->since_pass = 0;
}

/* Credit one outcome to one hypothesis (no since_pass): a dormant or invalid index, or a decided state, credits nothing. */
static int feed_one(nr_pdsch_config_sweep_state_t *st, int idx, bool tb_crc_ok, bool *credited)
{
  if (idx < 0 || idx >= st->n_hyp) {
    return st->winner;
  }
  if (st->winner >= 0) {
    return st->winner;
  }
  if (!active(st, idx)) {
    return st->winner; /* feedback that arrived after the hypothesis went dormant: no evidence */
  }
  st->trials[idx]++;
  if (tb_crc_ok) {
    st->ok[idx]++;
  }
  *credited = true;
  return sweep_decide(st, (st->trials[idx] % 16) == 0);
}

int nr_pdsch_config_sweep_feed(nr_pdsch_config_sweep_state_t *st, int idx, bool tb_crc_ok)
{
  if (st == NULL) {
    return -1;
  }
  bool credited = false;
  const int w = feed_one(st, idx, tb_crc_ok, &credited);
  since_pass_update(st, credited, credited && tb_crc_ok);
  return w;
}

/* Smallest m >= 2 with n_alive * C(t_max, m) * 2^(-24 m) <= 1e-6, in the log domain. */
int nr_pdsch_config_sweep_crc_accept_m(int n_alive, uint32_t t_max)
{
  if (n_alive <= 1 || t_max == 0)
    return 2;
  const double budget = log(1e-6) - log((double)n_alive);
  const double t = (double)t_max;
  for (int m = 2;; m++) {
    if ((double)m > t) /* C(t, m) = 0 */
      return m;
    const double log_c = lgamma(t + 1.0) - lgamma((double)m + 1.0) - lgamma(t - (double)m + 1.0);
    if (log_c - 24.0 * (double)m * M_LN2 <= budget)
      return m;
  }
}

/* Lever P step of the shared feed function (see the header). Returns true when a pin was applied.
 * `count`: this grant is fast-path evidence (EXPLORE pick, new-data pass). The lead condition is re-evaluated on every call so the sibling
 * schedule (sib_t[1]) follows it. */
static bool geom_keep_g(const nr_pdsch_cfg_hypothesis_t *h, const void *arg)
{
  return nr_td_geom_key(h) == *(const uint64_t *)arg;
}
static uint32_t fp_tmax_active(const nr_pdsch_config_sweep_state_t *st)
{
  uint32_t t_max = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (active(st, i) && st->fp_trials[i] > t_max)
      t_max = st->fp_trials[i];
  return t_max;
}
int nr_pdsch_config_sweep_geom_groups(const nr_pdsch_config_sweep_state_t *st, int *n_groups, uint32_t *t_g_max)
{
  if (st == NULL || n_groups == NULL || t_g_max == NULL)
    return -1;
  uint64_t *keys = (uint64_t *)malloc((size_t)(st->n_hyp > 0 ? st->n_hyp : 1) * (sizeof(uint64_t) + sizeof(uint32_t)));
  if (keys == NULL)
    return -1;
  uint32_t *sum = (uint32_t *)(keys + (st->n_hyp > 0 ? st->n_hyp : 1));
  int ng = 0;
  for (int i = 0; i < st->n_hyp; i++) {
    if (!active(st, i))
      continue;
    const uint64_t k = nr_td_geom_key(&st->hyp[i]);
    int g = 0;
    while (g < ng && keys[g] != k)
      g++;
    if (g == ng) {
      keys[ng] = k;
      sum[ng++] = 0;
    }
    sum[g] += st->fp_trials[i];
  }
  uint32_t mx = 0;
  for (int g = 0; g < ng; g++)
    if (sum[g] > mx)
      mx = sum[g];
  free(keys);
  *n_groups = ng;
  *t_g_max = mx;
  return 0;
}
static bool lever_p(nr_pdsch_config_sweep_state_t *st, int idx0, const int *cls, int n_cls, bool count)
{
  if (count) {
    const uint64_t key = nr_td_geom_key(&st->hyp[idx0]);
    for (int k = 0; k < n_cls; k++) /* attribution: a class spanning two geometry groups is ambiguous -> no evidence */
      if (cls[k] >= 0 && cls[k] < st->n_hyp && nr_td_geom_key(&st->hyp[cls[k]]) != key)
        return false;
    int slot = -1;
    for (int g = 0; g < st->n_geom && slot < 0; g++)
      if (st->geom_key[g] == key)
        slot = g;
    if (slot < 0) {
      if (st->n_geom >= NR_TD_GEOM_SLOTS) {
        st->geom_blocked = true;
        return false;
      }
      slot = st->n_geom++;
      st->geom_key[slot] = key;
      st->ok_geom[slot] = 0;
    }
    if (st->ok_geom[slot] < UINT16_MAX)
      st->ok_geom[slot]++;
  }
  if (st->n_geom >= 2) { /* every slot holds >= 1 pass: a second geometry with a pass */
    st->geom_blocked = true;
    return false;
  }
  if (st->n_geom < 1 || st->ok_geom[0] < 2) /* crc_accept_m() >= 2: nothing to test yet */
    return false;
  /* m*(n_groups_active, T_g,max): ok_geom sums the explore passes of EVERY member of a group, so the trial count that bounds a wrong group
   * is the SUM of fp_trials over its active members (T_g), maximised over the active groups. */
  int n_groups = 0;
  uint32_t t_g_max = 0;
  if (nr_pdsch_config_sweep_geom_groups(st, &n_groups, &t_g_max) < 0)
    return false;
  if (n_groups < 2) /* already a single geometry: nothing to pin (also keeps a no-op re-pin from running on every pass) */
    return false;
  if (st->ok_geom[0] < (unsigned)nr_pdsch_config_sweep_crc_accept_m(n_groups, t_g_max))
    return false;
  const uint64_t g = st->geom_key[0];
  /* Lead condition holds. The k0 siblings of the group must have passed their sibling tests before the pin. */
  int rep = -1; /* any active member of G: its (skey, k0) define the siblings */
  for (int i = 0; i < st->n_hyp && rep < 0; i++)
    if (active(st, i) && nr_td_geom_key(&st->hyp[i]) == g)
      rep = i;
  if (rep >= 0 && !siblings_clear(st, skey_of(&st->hyp[rep]), st->hyp[rep].k0)) {
    st->sib_t[1].valid = true;
    st->sib_t[1].skey = skey_of(&st->hyp[rep]);
    st->sib_t[1].k0 = st->hyp[rep].k0;
    return false;
  }
  /* set_dormant restarts lever C/P evidence on an active-set change (all slots empty afterwards: no loop, no stale re-pin);
   * if it refuses (would empty the catalogue) nothing changes. */
  return nr_pdsch_config_sweep_set_dormant(st, NR_TD_DORMANT_GEOM, geom_keep_g, &g) > 0;
}

/* The one feed function. Crediting: idx[0..n) (each distinct, in-range, ACTIVE member gets one Bernoulli sample).
 * Attribution: cls[0..n_cls) (+ idx[0] itself), the FULL class incl. dormant members, for lever-C uniqueness and lever-P
 * attribution. Order: credit -> lever P -> lever C -> sweep_decide. `kind` is the pick kind of idx[0]: only EXPLORE picks add fast-path
 * evidence and fp_trials; SIBLING picks add sib_trials and a pass blocks the fast path (see the header); every kind is a normal KL trial. */
static int feed_shared(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, const int *cls, int n_cls, bool tb_crc_ok,
                       bool new_data, nr_td_pick_t kind)
{
  /* An invalid decoded index idx[0] credits nothing (mirrors _feed): a class defined relative to it is untrustworthy. */
  if (st == NULL || idx == NULL || n < 1 || idx[0] < 0 || idx[0] >= st->n_hyp
      || !active(st, idx[0])) { /* a dormant decoded hypothesis credits nothing (as _feed) */
    return (st != NULL) ? st->winner : -1;
  }
  if (st->winner >= 0) {
    return st->winner;
  }
  const bool explore = kind == NR_TD_PICK_EXPLORE;
  /* One crediting loop: every distinct in-range member gets exactly this grant's one Bernoulli sample. */
  bool check = false;
  bool credited = false; /* >= 1 ACTIVE member credited; with no mask set idx[0] always is */
  int n_credited = 0;
  for (int k = 0; k < n; k++) {
    const int h = idx[k];
    if (h < 0 || h >= st->n_hyp)
      continue;
    bool dup = false;
    for (int j = 0; j < k && !dup; j++)
      dup = idx[j] == h;
    if (dup)
      continue;
    if (!active(st, h))
      continue;
    st->trials[h]++;
    if (tb_crc_ok)
      st->ok[h]++;
    if (explore && st->fp_trials[h] < UINT16_MAX)
      st->fp_trials[h]++;
    credited = true;
    n_credited++;
    if ((st->trials[h] % 16) == 0)
      check = true;
  }
  if (!credited) /* every member dormant: no new evidence, no decision */
    return st->winner;
  since_pass_update(st, true, tb_crc_ok);
  if (kind == NR_TD_PICK_SIBLING) {
    if (st->sib_trials[idx[0]] < UINT16_MAX)
      st->sib_trials[idx[0]]++;
    if (tb_crc_ok) /* a sibling that passes may be the truth: the fast path is off until the next evidence restart */
      st->sib_blocked = true;
  }
  st->sib_t[0].valid = st->sib_t[1].valid = false; /* re-derived below from the current lead conditions */
  bool pinned = false;
  if (st->geom_pin && !st->geom_blocked && !st->fail_open && !st->sib_blocked)
    pinned = lever_p(st, idx[0], cls, n_cls, explore && tb_crc_ok && new_data);
  /* After a pin the active set changed and the evidence restarted: this grant's pass does not count for lever C. */
  if (st->crc_accept && !st->crc_accept_blocked && !st->sib_blocked && !pinned) {
    /* Lever C: a unique pass = new data, alone in its FULL class (dormant members counted) and alone among the ACTIVE
     * hypotheses credited by this grant; only exploration picks count. */
    int n_distinct = 1; /* idx[0] is always a member of its own class */
    for (int k = 0; k < n_cls; k++) {
      const int h = cls[k];
      if (h < 0 || h >= st->n_hyp || h == idx[0])
        continue;
      bool dup = false;
      for (int j = 0; j < k && !dup; j++)
        dup = cls[j] == h;
      n_distinct += !dup;
    }
    if (explore && tb_crc_ok && new_data && n_distinct == 1 && n_credited == 1 && st->ok_unique[idx[0]] < UINT16_MAX)
      st->ok_unique[idx[0]]++;
    int n_u = 0, lead = -1;
    for (int i = 0; i < st->n_hyp; i++) {
      if (active(st, i) && st->ok_unique[i] > 0) {
        n_u++;
        lead = i;
      }
    }
    if (n_u >= 2)
      st->crc_accept_blocked = true;
    else if (n_u == 1
             && st->ok_unique[lead] >= (unsigned)nr_pdsch_config_sweep_crc_accept_m(n_active_of(st), fp_tmax_active(st))) {
      if (siblings_clear(st, skey_of(&st->hyp[lead]), st->hyp[lead].k0)) {
        st->winner = lead;
        st->winner_by_crc = true;
        return lead;
      }
      st->sib_t[0].valid = true; /* lead without completed sibling tests: schedule them (next_ex) */
      st->sib_t[0].skey = skey_of(&st->hyp[lead]);
      st->sib_t[0].k0 = st->hyp[lead].k0;
    }
  }
  return sweep_decide(st, check);
}

int nr_pdsch_config_sweep_feed_equiv_ex(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, bool tb_crc_ok, bool new_data,
                                        nr_td_pick_t kind)
{
  return feed_shared(st, idx, n, idx, n, tb_crc_ok, new_data, kind);
}
int nr_pdsch_config_sweep_feed_equiv(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, bool tb_crc_ok, bool new_data)
{
  return feed_shared(st, idx, n, idx, n, tb_crc_ok, new_data, NR_TD_PICK_EXPLOIT); /* legacy API: no pick kind => no fast-path evidence */
}
int nr_pdsch_config_sweep_feed_attr_ex(nr_pdsch_config_sweep_state_t *st, int idx0, const int *cls, int n_cls, bool tb_crc_ok,
                                       bool new_data, nr_td_pick_t kind)
{
  return feed_shared(st, &idx0, 1, cls, cls != NULL ? n_cls : 0, tb_crc_ok, new_data, kind);
}
int nr_pdsch_config_sweep_feed_attr(nr_pdsch_config_sweep_state_t *st, int idx0, const int *cls, int n_cls, bool tb_crc_ok,
                                    bool new_data)
{
  return feed_shared(st, &idx0, 1, cls, cls != NULL ? n_cls : 0, tb_crc_ok, new_data, NR_TD_PICK_EXPLOIT); /* legacy API (see feed_equiv) */
}

static int next_k_core(nr_pdsch_config_sweep_state_t *st, int K, int idx[], nr_pdsch_cfg_hypothesis_t out[], nr_td_pick_t *kind,
                       bool allow_sib)
{
  if (kind != NULL)
    *kind = NR_TD_PICK_EXPLORE;
  if (st == NULL || idx == NULL || out == NULL || K < 1)
    return 0;
  if (K > NR_TD_MAX_K)
    K = NR_TD_MAX_K;
  idx[0] = next_core(st, &out[0], kind, allow_sib);
  if (idx[0] < 0)
    return 0;
  if (st->winner >= 0)
    return 1;
  int n = 1;
  /* Probes: the rest of the current round from the cursor on (wrapping), read-only -- the cursor and
   * the RNG belong to the main selection alone, so K > 1 never changes the main hypothesis sequence. A
   * hypothesis already CLEARED by the KL evidence (full fallback trials, no pass) is not worth a probe. */
  for (int step = 0; step < st->n_hyp && n < K; step++) {
    const int h = st->order[(st->cursor + step) % st->n_hyp];
    if (h == idx[0] || !active(st, h) || (st->trials[h] >= SWEEP_MIN_TRIALS && st->ok[h] == 0))
      continue;
    bool dup = false;
    for (int j = 1; j < n; j++)
      dup |= idx[j] == h;
    if (dup)
      continue;
    idx[n] = h;
    out[n] = st->hyp[h];
    n++;
  }
  return n;
}
int nr_pdsch_config_sweep_next_k(nr_pdsch_config_sweep_state_t *st, int K, int idx[], nr_pdsch_cfg_hypothesis_t out[])
{
  return next_k_core(st, K, idx, out, NULL, false);
}
int nr_pdsch_config_sweep_next_k_ex(nr_pdsch_config_sweep_state_t *st, int K, int idx[], nr_pdsch_cfg_hypothesis_t out[], nr_td_pick_t *kind)
{
  return next_k_core(st, K, idx, out, kind, true);
}

static void probe_count(uint16_t *c)
{
  if (*c < UINT16_MAX)
    (*c)++;
}

int nr_pdsch_config_sweep_feed_k(nr_pdsch_config_sweep_state_t *st, const nr_td_outcome_t *outcomes, int n)
{
  if (st == NULL || outcomes == NULL || n < 1)
    return (st != NULL) ? st->winner : -1;
  const nr_td_outcome_t *m = &outcomes[0];
  /* Only a decided FULL-TB outcome is KL evidence; an inconclusive main decode is not fed. */
  bool credited = false, pass_credited = false; /* since_pass: one update per feed_k CALL */
  if (m->kind == NR_TD_FULL_TB && (m->result == NR_TD_PASS || m->result == NR_TD_FAIL)) {
    bool c = false;
    feed_one(st, m->hyp, m->result == NR_TD_PASS, &c);
    credited |= c;
    pass_credited = c && m->result == NR_TD_PASS;
  }
  for (int i = 1; i < n; i++) {
    const nr_td_outcome_t *o = &outcomes[i];
    if (o->hyp < 0 || o->hyp >= st->n_hyp || !active(st, o->hyp))
      continue;
    if (o->result == NR_TD_PASS) {
      probe_count(&st->probe_pass[o->hyp]); /* ordering only (P1): never a KL success */
    } else if (o->result == NR_TD_FAIL) {
      probe_count(&st->probe_fail[o->hyp]);
      /* P2: one KL failure per admissible failed probe, at most once per hypothesis per grant (a
       * hypothesis repeated in one batch, or equal to the main one, was already scored on this grant). */
      bool seen = o->hyp == m->hyp;
      for (int j = 1; j < i && !seen; j++)
        seen = outcomes[j].hyp == o->hyp;
      if (st->p2 && o->p2_admissible && !seen) {
        bool c = false; /* KL evidence: counts toward since_pass, a PASS never does */
        feed_one(st, o->hyp, false, &c);
        credited |= c;
      }
    } else {
      probe_count(&st->probe_inconclusive[o->hyp]);
    }
  }
  since_pass_update(st, credited, pass_credited);
  return st->winner;
}

int nr_pdsch_config_sweep_winner(const nr_pdsch_config_sweep_state_t *st)
{
  return (st != NULL) ? st->winner : -1;
}

/* ---- Dormant masks and fail-open public API ---------------------------------------------------------------------------- */
int nr_pdsch_config_sweep_set_dormant(nr_pdsch_config_sweep_state_t *st, int cause, nr_td_keep_fn_t keep, const void *arg)
{
  if (st == NULL || keep == NULL || cause < 0 || cause >= NR_TD_DORMANT_CAUSES || st->n_hyp <= 0)
    return -1;
  /* Build the candidate mask for this cause; commit only if some hypothesis stays active under ALL causes (fail_open ignored). */
  uint64_t cand[NR_TD_DWORDS];
  memcpy(cand, st->dormant[cause], sizeof(cand));
  for (int i = 0; i < st->n_hyp; i++)
    if (!keep(&st->hyp[i], arg))
      cand[i >> 6] |= UINT64_C(1) << (i & 63);
  bool any_active = false;
  int newly = 0;
  for (int w = 0; w < (st->n_hyp + 63) / 64; w++) {
    uint64_t u = cand[w];
    for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++)
      if (c != cause)
        u |= st->dormant[c][w];
    any_active |= (~u & live_word(st->n_hyp, w)) != 0;
    newly += __builtin_popcountll(cand[w] & ~st->dormant[cause][w]);
  }
  if (!any_active)
    return -1;
  if (memcmp(st->dormant[cause], cand, sizeof(cand)) != 0) {
    st->since_pass = 0; /* the active set changed */
    lever_c_restart(st);
  }
  memcpy(st->dormant[cause], cand, sizeof(cand));
  return newly;
}

int nr_pdsch_config_sweep_clear_dormant(nr_pdsch_config_sweep_state_t *st, int cause)
{
  if (st == NULL || cause < 0 || cause >= NR_TD_DORMANT_CAUSES)
    return 0;
  int reactivated = 0;
  for (int w = 0; w < NR_TD_DWORDS; w++) {
    uint64_t others = 0;
    for (int c = 0; c < NR_TD_DORMANT_CAUSES; c++)
      if (c != cause)
        others |= st->dormant[c][w];
    reactivated += __builtin_popcountll(st->dormant[cause][w] & ~others);
    if (st->dormant[cause][w]) {
      st->since_pass = 0; /* the active set changed */
      lever_c_restart(st);
    }
    st->dormant[cause][w] = 0;
  }
  return reactivated;
}

void nr_pdsch_config_sweep_set_fail_open(nr_pdsch_config_sweep_state_t *st, bool on)
{
  if (st != NULL && st->fail_open != on) {
    st->fail_open = on;
    st->since_pass = 0; /* the active set changed */
    if (on) /* the GEOM mask is derived from fast-path evidence: fail-open discards the evidence, so the pin must not outlive it */
      memset(st->dormant[NR_TD_DORMANT_GEOM], 0, sizeof(st->dormant[NR_TD_DORMANT_GEOM]));
    lever_c_restart(st);
  }
}

bool nr_pdsch_config_sweep_is_active(const nr_pdsch_config_sweep_state_t *st, int i)
{
  return st != NULL && i >= 0 && i < st->n_hyp && active(st, i);
}

int nr_pdsch_config_sweep_n_active(const nr_pdsch_config_sweep_state_t *st)
{
  return st != NULL ? n_active_of(st) : 0;
}

bool nr_pdsch_config_sweep_fail_open_due(const nr_pdsch_config_sweep_state_t *st, double alpha, double p_min)
{
  if (st == NULL || st->fail_open || st->since_pass == 0 || !(alpha > 0.0 && alpha < 1.0) || !(p_min > 0.0))
    return false;
  const double need = ceil((double)n_active_of(st) * log(1.0 / alpha) / p_min);
  return (double)st->since_pass >= need;
}

int nr_pdsch_config_sweep_prune_keep(nr_pdsch_config_sweep_state_t *st, nr_td_keep_fn_t keep, const void *arg)
{
  if (st == NULL || keep == NULL || st->n_hyp <= 0)
    return 0;
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (keep(&st->hyp[i], arg))
      prune_move(st, n++, i);
  return prune_commit(st, n);
}

/* All shared accesses, including winner publication and reset, use one short mutex. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
  uint64_t configuration, generation, touched;
  uint16_t rnti;
  uint8_t tda;
  int tda_count, typeA;
  bool reported;
  uint8_t qm_tables, qm_obs; /* Qm-oracle evidence: consistent-table bitmask, sightings */
  uint64_t outcomes, locked_trials, locked_passes;
  uint64_t failure_streak, reacquisitions;
  double reference_crc_lower;
  /* Kept so a context pruned by the cell-wide prior can rebuild its full catalog without the
   * caller having to hand the legality function back. */
  nr_pdsch_legality_fn_t legality;
  enum { PRIORED_NONE = 0, PRIORED_OWN, PRIORED_CELL } priored; /* which prior pruned this catalog */
  /* Heap, allocated when the slot is first used and kept across reuse: nr-uesoftmodem mlockall()s
   * (MCL_CURRENT|MCL_FUTURE) at startup, so 1024 inline states (180 KB each) would pin 185 MB of BSS
   * whether or not any context ever opens. Non-NULL whenever generation != 0. */
  nr_pdsch_config_sweep_state_t *state;
} sweep_context_t;

/* Outcomes a pruned context may spend before the prior is judged wrong for it. ~250 per hypothesis
 * at the pruned width -- ample to clear SWEEP_MIN_RATE if the prior is right, and 35x cheaper than
 * the ~70,000 the full catalog costs if it is not. */
#define PRIOR_PROBATION 2000

static sweep_context_t g_contexts[NR_PDSCH_SWEEP_MAX_CONTEXTS];
static nr_pdsch_sweep_reporter_t g_reporter;
/* Census (lane perf 2026-09-27), all under g_lock: whether evidence ACCUMULATES is the question every
 * "never converged" report needs answered first, and nothing in the log could answer it -- the
 * new-context line is capped at 20 and the evidence line fires only on a pass or every 10000 outcomes
 * of one context, i.e. never on a context that lives a few grants. */
static uint64_t g_st_created, g_st_scored, g_st_stale, g_st_reindexed;

/* A prune compacts st->hyp[] in place (or a probation restore rebuilds it), so every outstanding ticket
 * names an index that now belongs to another hypothesis. Retire them: a new generation makes
 * ticket_context() refuse them instead of crediting whatever moved onto their index. Appends (k0/type-B
 * layers) keep every index and need no bump. Call under g_lock whenever n_hyp changed by a prune. */
static void context_reindexed(sweep_context_t *c)
{
  c->generation = ++g_generation;
  g_st_reindexed++;
}
static uint32_t g_recovery_minimum_failures = 32;
static double g_recovery_probability_budget = 1e-6;
bool nr_pdsch_config_sweep_set_recovery_policy(uint32_t minimum_failures, double probability_budget)
{
  if (!minimum_failures || !isfinite(probability_budget)
      || probability_budget <= 0 || probability_budget >= 1)
    return false;
  pthread_mutex_lock(&g_lock);
  g_recovery_minimum_failures = minimum_failures;
  g_recovery_probability_budget = probability_budget;
  pthread_mutex_unlock(&g_lock);
  return true;
}

/* Compare a run of failures with the frozen conservative rate at convergence.
 * Use summable budgets over feedback positions and generations, shared across
 * contexts. Correlated fading can also trigger this: only reopen local search,
 * never claim that CRC evidence alone identified a configuration change. */
static bool recovery_needed(const sweep_context_t *c)
{
  if (c->failure_streak < g_recovery_minimum_failures || c->reference_crc_lower <= 0)
    return false;
  const double n = (double)c->locked_trials, generation = (double)c->generation;
  const double budget = log(g_recovery_probability_budget) - log(NR_PDSCH_SWEEP_MAX_CONTEXTS)
                        - log(n + 1) - log(n + 2) - log(generation + 1) - log(generation + 2);
  return (double)c->failure_streak * log1p(-c->reference_crc_lower) <= budget;
}

/* ---- CATALOG TEMPLATES (final review I8). nr_pdsch_config_sweep_init_legal() depends only on (typeA,
 * legality) -- tda_count is ignored -- and costs ~6.3k legality calls with type B. It used to run for
 * every new context UNDER g_lock, on the scan thread (the PHY receive thread when the scan is not
 * deferred). Now each distinct catalog is built once, under its own lock, and a context copies it
 * (memcpy of one state). Templates are immutable once published and never freed. */
#define NR_PDSCH_SWEEP_TEMPLATES 4
typedef struct {
  int typeA;
  nr_pdsch_legality_fn_t legality;
  nr_pdsch_config_sweep_state_t *st;
} catalog_template_t;
static catalog_template_t g_tmpl[NR_PDSCH_SWEEP_TEMPLATES];
static pthread_mutex_t g_tmpl_lock = PTHREAD_MUTEX_INITIALIZER; /* lock order: g_lock -> g_tmpl_lock, never reversed */
static const nr_pdsch_config_sweep_state_t *catalog_template(int typeA, nr_pdsch_legality_fn_t legality)
{
  const nr_pdsch_config_sweep_state_t *t = NULL;
  pthread_mutex_lock(&g_tmpl_lock);
  int free_slot = -1;
  for (int i = 0; i < NR_PDSCH_SWEEP_TEMPLATES && !t; i++) {
    if (g_tmpl[i].st && g_tmpl[i].typeA == typeA && g_tmpl[i].legality == legality)
      t = g_tmpl[i].st;
    else if (!g_tmpl[i].st && free_slot < 0)
      free_slot = i;
  }
  if (!t && free_slot >= 0) {
    nr_pdsch_config_sweep_state_t *st = malloc(sizeof(*st));
    if (st) {
      nr_pdsch_config_sweep_init_legal(st, 0, typeA, legality);
      g_tmpl[free_slot] = (catalog_template_t){.typeA = typeA, .legality = legality, .st = st};
      t = st;
    }
  }
  pthread_mutex_unlock(&g_tmpl_lock);
  return t; /* NULL only when every template slot holds another key (or malloc failed): caller enumerates */
}
/* Rebuilds the catalog and discards evidence; side/p2 are CONFIGURATION, not catalog: the whole-state
 * memcpy/memset below would revert them to the template's neutral values, so they are carried over. */
int nr_pdsch_config_sweep_rebuild(nr_pdsch_config_sweep_state_t *st, int tda_count, int typeA,
                                  nr_pdsch_legality_fn_t legality)
{
  if (st == NULL)
    return 0;
  const struct nr_td_side_info_s *side = st->side;
  const bool p2 = st->p2;
  const bool crc_accept = st->crc_accept;
  const bool geom_pin = st->geom_pin;
  const float sib_pmin = st->sib_pmin, sib_eps = st->sib_eps;
  const bool fail_open = st->fail_open;
  uint64_t dormant[NR_TD_DORMANT_CAUSES][NR_TD_DWORDS];
  memcpy(dormant, st->dormant, sizeof(dormant));
  memset(dormant[NR_TD_DORMANT_GEOM], 0, sizeof(dormant[NR_TD_DORMANT_GEOM])); /* GEOM is derived from evidence (cleared by rebuild) */
  const nr_pdsch_config_sweep_state_t *t = legality ? catalog_template(typeA, legality) : NULL;
  if (t)
    memcpy(st, t, sizeof(*st));
  else
    nr_pdsch_config_sweep_init_legal(st, tda_count, typeA, legality);
  st->side = side;
  st->p2 = p2;
  st->crc_accept = crc_accept;
  st->geom_pin = geom_pin;
  st->sib_pmin = sib_pmin;
  st->sib_eps = sib_eps;
  st->fail_open = fail_open;
  memcpy(st->dormant, dormant, sizeof(dormant));
  normalize_masks(st);
  return st->n_hyp;
}
static void catalog_fill(nr_pdsch_config_sweep_state_t *st, int tda_count, int typeA, nr_pdsch_legality_fn_t legality)
{
  nr_pdsch_config_sweep_rebuild(st, tda_count, typeA, legality);
}
/* One recycled context state (under g_lock): an evicted or race-lost state is kept for the next new
 * context instead of being freed, so a burst of new contexts does not mmap/munmap (and, under
 * mlockall(), fault-and-lock) a fresh 180 KB each time. */
static nr_pdsch_config_sweep_state_t *g_spare_state;

/* Full catalog for a context, plus every k0 layer the air has shown for its RNTI. */
static void context_catalog(sweep_context_t *c, const rnti_ctx_t *r)
{
  catalog_fill(c->state, c->tda_count, c->typeA, c->legality);
  /* Restoring the "full" catalog after a bad prior/probation must include type B once the air has
   * already shown it for this RNTI -- that is real evidence, not a prior that could be wrong. */
  if (r && r->typeb_seen)
    add_typeb_layer(c->state, c->typeA, c->legality);
  for (int k = 2; r && k <= 32; k++)
    if (r->k0_seen & (UINT64_C(1) << k))
      nr_pdsch_config_sweep_add_k0_layer(c->state, (uint8_t)k);
}

static void reopen_context(sweep_context_t *c)
{
  const uint64_t previous = c->generation;
  nr_pdsch_sweep_report_t report = {
      .configuration=c->configuration, .rnti=c->rnti, .tda=c->tda,
      .outcomes=c->outcomes, .passes=c->locked_passes, .trials=c->locked_trials,
      .winner=-1, .invalidated=true, .previous_generation=previous,
      .generation=++g_generation, .reacquisitions=++c->reacquisitions,
      .failure_streak=c->failure_streak, .reference_crc_lower=c->reference_crc_lower};
  c->generation = report.generation;
  c->reported = false;
  c->outcomes = c->locked_trials = c->locked_passes = c->failure_streak = 0;
  c->reference_crc_lower = 0;
  /* A reopen says this context's evidence is no longer trusted. If its catalog had been pruned by
   * the cell-wide prior, restore the full one: the prior is the most likely thing to be wrong when
   * a previously converged context starts failing. */
  if (c->priored && c->legality) {
    context_catalog(c, rnti_ctx(c->rnti, false));
    c->priored = PRIORED_NONE;
  }
  /* Keep the already checked legal catalog, but discard stale decoding evidence (and the GEOM pin derived from it). */
  memset(c->state->dormant[NR_TD_DORMANT_GEOM], 0, sizeof(c->state->dormant[NR_TD_DORMANT_GEOM]));
  memset(c->state->trials, 0, sizeof(c->state->trials));
  memset(c->state->ok, 0, sizeof(c->state->ok));
  clear_probe_stats(c->state);
  c->state->since_pass = 0;
  c->state->winner = -1;
  c->state->winner_by_crc = false;
  c->state->cursor = 0;
  for (int i=0; i<c->state->n_hyp; ++i) c->state->order[i] = i;
  /* A reopen is the signal that a CONVERGED context stopped working -- the most valuable thing in
   * this log, because it is how a wrong prior announces itself. reacquisitions rising steadily on
   * one RNTI means its catalog keeps being re-derived. */
  LOG_W(PHY,
        "SWEEP: rnti=0x%04x tda=%u REOPENED (reacquisition #%u, failure streak %u, "
        "locked %llu/%llu, reference lower bound %.3f)\n",
        c->rnti, (unsigned)c->tda, (unsigned)c->reacquisitions, (unsigned)report.failure_streak,
        (unsigned long long)report.passes, (unsigned long long)report.trials,
        report.reference_crc_lower);
  if (g_reporter) g_reporter(&report);
}

void nr_pdsch_config_sweep_set_reporter(nr_pdsch_sweep_reporter_t reporter)
{
  pthread_mutex_lock(&g_lock); g_reporter=reporter; pthread_mutex_unlock(&g_lock);
}

void nr_pdsch_config_sweep_prior_reset(void)
{
  pthread_mutex_lock(&g_lock);
  g_prior.valid = false;
  for (int i = 0; i < RNTI_CTX_MAX; i++)
    g_rnti[i].prior.valid = false;
  pthread_mutex_unlock(&g_lock);
}

static bool prior_get_locked(const prior_t *p, uint64_t *configuration, uint8_t *mcs_table,
                             uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len)
{
  if (!p || !p->valid)
    return false;
  if (configuration) *configuration = p->configuration;
  if (mcs_table)     *mcs_table     = p->mcs_table;
  if (dmrs_add_pos)  *dmrs_add_pos  = p->dmrs_add_pos;
  if (dmrs_max_len)  *dmrs_max_len  = p->dmrs_max_len;
  return true;
}
bool nr_pdsch_config_sweep_prior_get(uint64_t *configuration, uint8_t *mcs_table,
                                     uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len)
{
  pthread_mutex_lock(&g_lock);
  const bool v = prior_get_locked(&g_prior, configuration, mcs_table, dmrs_add_pos, dmrs_max_len);
  pthread_mutex_unlock(&g_lock);
  return v;
}
bool nr_pdsch_config_sweep_rnti_prior_get(uint16_t rnti, uint64_t *configuration, uint8_t *mcs_table,
                                          uint8_t *dmrs_add_pos, uint8_t *dmrs_max_len)
{
  pthread_mutex_lock(&g_lock);
  const rnti_ctx_t *r = rnti_ctx(rnti, false);
  const bool v = r && prior_get_locked(&r->prior, configuration, mcs_table, dmrs_add_pos, dmrs_max_len);
  pthread_mutex_unlock(&g_lock);
  return v;
}

static bool prior_same(const prior_t *a, const prior_t *b)
{
  return a->valid && b->valid && a->configuration == b->configuration && a->mcs_table == b->mcs_table
         && a->dmrs_add_pos == b->dmrs_add_pos && a->dmrs_max_len == b->dmrs_max_len
         && a->mapping_type == b->mapping_type;
}
/* Promote to the cell-wide prior once a SECOND distinct RNTI has converged on the same fields. The
 * first alone stays private: one UE's dedicated config is not evidence about the cell. */
static void prior_promote_locked(const rnti_ctx_t *just_set)
{
  if (g_prior.valid)
    return;
  for (int i = 0; i < RNTI_CTX_MAX; i++)
    if (g_rnti[i].rnti && g_rnti[i].rnti != just_set->rnti && prior_same(&g_rnti[i].prior, &just_set->prior)) {
      g_prior = just_set->prior;
      /* The load-bearing line: it names BOTH RNTIs, so "two distinct UEs agreed" is verifiable from
       * the log instead of asserted. A promotion that ever prints the same RNTI twice is a bug. */
      LOG_W(PHY,
            "SWEEP: cell prior PROMOTED by agreement rnti=0x%04x + rnti=0x%04x -- "
            "mcs_table=%u dmrs_add_pos=%u dmrs_max_len=%u cfg=0x%llx\n",
            just_set->rnti, g_rnti[i].rnti, (unsigned)g_prior.mcs_table,
            (unsigned)g_prior.dmrs_add_pos, (unsigned)g_prior.dmrs_max_len,
            (unsigned long long)g_prior.configuration);
      return;
    }
}

static sweep_context_t *ticket_context(const nr_pdsch_sweep_ticket_t *t)
{
  if (!t || !t->generation || t->context_slot >= NR_PDSCH_SWEEP_MAX_CONTEXTS)
    return NULL;
  sweep_context_t *c = &g_contexts[t->context_slot];
  /* generation+slot+tda identify the context (the slot's generation changes on every reuse). */
  return c->generation == t->generation && c->tda == t->tda_index
         && t->hypothesis >= 0 && t->hypothesis < c->state->n_hyp ? c : NULL;
}

/* under g_lock: the context for this key, or -1 (then *victim, if non-NULL, is the LRU slot) */
static int find_context(uint64_t configuration, uint16_t rnti, uint8_t tda_index, int tda_count, int typeA, int *victim)
{
  int v = 0;
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS; ++i) {
    const sweep_context_t *c = &g_contexts[i];
    if (c->generation && c->configuration == configuration && c->rnti == rnti
        && c->tda == tda_index && c->tda_count == tda_count && c->typeA == typeA)
      return i;
    if (c->touched < g_contexts[v].touched)
      v = i;
  }
  if (victim)
    *victim = v;
  return -1;
}

bool nr_pdsch_config_sweep_select(uint64_t configuration, uint16_t rnti, uint8_t tda_index,
                                 int tda_count, int typeA, nr_pdsch_legality_fn_t legality,
                                 nr_pdsch_sweep_ticket_t *ticket, nr_pdsch_cfg_hypothesis_t *out)
{
  if (ticket)
    memset(ticket, 0, sizeof(*ticket));
  if (!ticket || !out || !legality || !rnti || tda_index >= 16
      || tda_count < 0 || tda_count > 16 || (tda_count && tda_index >= tda_count))
    return false;
  /* Final review I8: the common case (the context exists) takes g_lock ONCE. A new context is allocated
   * and its catalog copied from a prebuilt template OUTSIDE g_lock, then installed under it (a thread
   * that lost the race hands its buffer back to the spare slot). */
  pthread_mutex_lock(&g_lock);
  int found = find_context(configuration, rnti, tda_index, tda_count, typeA, NULL);
  nr_pdsch_config_sweep_state_t *fresh = NULL;
  if (found < 0) {
    fresh = g_spare_state;
    g_spare_state = NULL;
    pthread_mutex_unlock(&g_lock);
    if (!fresh)
      fresh = malloc(sizeof(*fresh));
    if (!fresh) {
      LOG_E(PHY, "SWEEP: cannot allocate a %zu-byte context state\n", sizeof(*fresh));
      return false;
    }
    /* A new context starts neutral: a malloc'd or recycled buffer holds garbage or a previous context's
     * configuration, which catalog_fill() would otherwise carry over. */
    fresh->side = NULL;
    fresh->p2 = false;
    fresh->crc_accept = false;
    fresh->geom_pin = false;
    fresh->sib_pmin = 0.05f;
    fresh->sib_eps = 1e-6f;
    fresh->winner_by_crc = false;
    fresh->fail_open = false;
    memset(fresh->dormant, 0, sizeof(fresh->dormant));
    catalog_fill(fresh, tda_count, typeA, legality);
    pthread_mutex_lock(&g_lock);
  }
  rnti_ctx_t *r = rnti_ctx(rnti, true);
  nr_pdsch_config_sweep_state_t *to_free = NULL;
  int victim = 0;
  if (fresh && (found = find_context(configuration, rnti, tda_index, tda_count, typeA, &victim)) >= 0) {
    to_free = fresh; /* another thread created it meanwhile */
  } else if (fresh) {
    found = victim;
    sweep_context_t *c = &g_contexts[found];
    to_free = c->state; /* the evicted context's state (NULL for a slot never used) */
    memset(c, 0, sizeof(*c));
    c->state = fresh;
    c->configuration = configuration;
    c->generation = ++g_generation;
    g_st_created++;
    c->rnti = rnti;
    c->tda = tda_index;
    c->tda_count = tda_count;
    c->typeA = typeA;
    c->legality = legality;
    /* R30 item 1: widen to type B BEFORE the prior/observed prunes below, not after -- so those prunes
     * (which already know how to treat a different mapping type, see prune_prior's dmrs_free) apply to
     * the type-B entries too instead of leaving them unfiltered alongside an already-narrowed type-A
     * set. Mirrors the k0-layer mechanism: only ever adds, seeded from this RNTI's own evidence. */
    if (r->typeb_seen)
      add_typeb_layer(fresh, typeA, legality);
    /* Seed: this RNTI's own prior first (its other TDA contexts already converged on these fields),
     * else the cell-wide one. Scoped to the same configuration key either way: a different cell
     * config is a different DM-RS/PDSCH setup and its prior says nothing here. */
    const prior_t *seed = NULL;
    int from = PRIORED_NONE;
    if (r->prior.valid && r->prior.configuration == configuration) {
      seed = &r->prior; from = PRIORED_OWN;
    } else if (g_prior.valid && g_prior.configuration == configuration) {
      seed = &g_prior; from = PRIORED_CELL;
    }
    if (seed && prune_prior(c->state, seed->mcs_table, seed->dmrs_add_pos, seed->dmrs_max_len,
                            seed->mapping_type) > 0)
      c->priored = from;
    prune_to_observed(c->state, &r->obs);
    /* k0 values the air has shown for this RNTI (k0 oracle), so each new context does not re-probe. */
    for (int k = 2; k <= 32; k++)
      if (r->k0_seen & (UINT64_C(1) << k))
        nr_pdsch_config_sweep_add_k0_layer(c->state, (uint8_t)k);
  }
  if (to_free && !g_spare_state) {
    g_spare_state = to_free;
    to_free = NULL;
  }
  sweep_context_t *c = &g_contexts[found];
  c->touched = ++g_clock;
  const int h = nr_pdsch_config_sweep_next(c->state, out);
  if (h >= 0)
    *ticket = (nr_pdsch_sweep_ticket_t){.generation=c->generation, .context_slot=found,
                                       .rnti=rnti, .tda_index=tda_index, .hypothesis=h, .settled=c->state->winner >= 0,
                                       .k0=out->k0};
  pthread_mutex_unlock(&g_lock);
  free(to_free);
  return h >= 0;
}

/* An earlier TDA can seed this context with its mask, but cannot prove that this TDA has the
 * same duration. Restore type-A entries when a newly measured footprint has no representative
 * left after that seed prune. Append only: pending tickets and existing CRC evidence stay valid.
 * Type B has its separate observation-gated expansion below. Called with g_lock held. */
static int restore_observed_typea(sweep_context_t *c, const rnti_ctx_t *r,
                                 uint16_t mask, int last_symbol, int k0)
{
  if (last_symbol < -1 || last_symbol >= 14 || k0 < -1 || k0 > 32)
    return 0;
  nr_pdsch_config_sweep_state_t *st = c->state;
  obs_set_t observation = {.n = 1, .mask = {mask}, .last = {last_symbol}, .k0 = {k0}};
  for (int i = 0; i < st->n_hyp; i++)
    if (obs_admits(&st->hyp[i], &observation, 0))
      return 0;
  const nr_pdsch_config_sweep_state_t *catalog = catalog_template(c->typeA, c->legality);
  nr_pdsch_config_sweep_state_t *scratch = NULL;
  if (!catalog) {
    scratch = malloc(sizeof(*scratch));
    if (!scratch)
      return 0;
    nr_pdsch_config_sweep_init_legal(scratch, c->tda_count, c->typeA, c->legality);
    catalog = scratch;
  }
  const int before = st->n_hyp;
  const prior_t *prior = r->prior.valid && r->prior.configuration == c->configuration ? &r->prior
                        : g_prior.valid && g_prior.configuration == c->configuration ? &g_prior : NULL;
  for (int i = 0; i < catalog->n_hyp && st->n_hyp < NR_PDSCH_SWEEP_MAX_HYP; i++) {
    nr_pdsch_cfg_hypothesis_t h = catalog->hyp[i];
    if (k0 >= 0)
      h.k0 = (uint8_t)k0;
    if (h.mapping_type != 0 || !obs_admits(&h, &observation, 0))
      continue;
    if (c->qm_obs >= 2 && !(c->qm_tables & (1u << h.mcs_table)))
      continue;
    if (prior && (h.mcs_table != prior->mcs_table
                  || (prior->mapping_type == h.mapping_type
                      && (h.dmrs_add_pos != prior->dmrs_add_pos || h.dmrs_max_len != prior->dmrs_max_len))))
      continue;
    bool duplicate = false;
    for (int j = before; j < st->n_hyp; j++) {
      const nr_pdsch_cfg_hypothesis_t *old = &st->hyp[j];
      duplicate |= old->tda_start == h.tda_start && old->tda_length == h.tda_length
                   && old->k0 == h.k0 && old->dmrs_mask == h.dmrs_mask && old->mcs_table == h.mcs_table;
    }
    if (duplicate)
      continue;
    const int at = st->n_hyp++;
    st->hyp[at] = h;
    st->trials[at] = st->ok[at] = 0;
    st->probe_pass[at] = st->probe_fail[at] = st->probe_inconclusive[at] = 0;
    st->ok_unique[at] = 0;
    st->fp_trials[at] = st->sib_trials[at] = 0;
    clear_geom_evidence(st);
    st->order[at] = at;
  }
  free(scratch);
  const int added = st->n_hyp - before;
  if (added)
    LOG_I(PHY, "SWEEP: ORACLE_RESTORE rnti=0x%04x tda=%u mask=0x%x last=%d k0=%d added=%d\n",
          c->rnti, c->tda, mask, last_symbol, k0, added);
  return added;
}

int nr_pdsch_config_sweep_observe_mask(const nr_pdsch_sweep_ticket_t *ticket, uint16_t dmrs_mask)
{
  return nr_pdsch_config_sweep_observe(ticket, dmrs_mask, -1, -1);
}

int nr_pdsch_config_sweep_observe(const nr_pdsch_sweep_ticket_t *ticket, uint16_t dmrs_mask, int last_symbol, int k0)
{
  if (ticket == NULL || ticket->generation == 0 || dmrs_mask == 0)
    return 0;
  pthread_mutex_lock(&g_lock);
  rnti_ctx_t *r = rnti_ctx(ticket->rnti, true);
  obs_record(&r->obs, dmrs_mask, last_symbol, k0);
  /* Promote to the cell-wide set once a second distinct RNTI has seen the same mask. */
  if (obs_find(&g_obs, dmrs_mask) < 0) {
    for (int i = 0; i < RNTI_CTX_MAX; i++)
      if (g_rnti[i].rnti && g_rnti[i].rnti != r->rnti && obs_find(&g_rnti[i].obs, dmrs_mask) >= 0) {
        const int j = obs_find(&g_rnti[i].obs, dmrs_mask);
        obs_record(&g_obs, dmrs_mask, g_rnti[i].obs.last[j], g_rnti[i].obs.k0[j]);
        break;
      }
  }
  if (obs_find(&g_obs, dmrs_mask) >= 0)
    obs_record(&g_obs, dmrs_mask, last_symbol, k0);
  sweep_context_t *c = ticket_context(ticket);
  /* R30 item 1: a mask no mapping-type-A hypothesis of this cell can produce is direct evidence the
   * cell is not type A only. Record it against the RNTI regardless of whether this ticket's own
   * context is still open to react to it (a converged context's evidence must still seed its later
   * siblings), mirroring how the k0 oracle records k0_seen unconditionally. */
  if (c != NULL && pdsch_typeb_enabled() && mask_needs_typeb(dmrs_mask, c->typeA, c->legality))
    r->typeb_seen = true;
  int n = 0;
  if (c != NULL && c->state->winner < 0) {
    restore_observed_typea(c, r, dmrs_mask, last_symbol, k0);
    if (r->typeb_seen)
      add_typeb_layer(c->state, c->typeA, c->legality);
    const int before = c->state->n_hyp;
    n = prune_to_observed(c->state, &r->obs);
    if (c->state->n_hyp != before)
      context_reindexed(c);
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

int nr_pdsch_config_sweep_add_k0(const nr_pdsch_sweep_ticket_t *t, uint8_t k0)
{
  if (t == NULL || t->generation == 0 || k0 < 2 || k0 > 32)
    return 0;
  pthread_mutex_lock(&g_lock);
  rnti_ctx_t *r = rnti_ctx(t->rnti, true);
  const bool first = !(r->k0_seen & (UINT64_C(1) << k0));
  r->k0_seen |= UINT64_C(1) << k0;
  sweep_context_t *c = ticket_context(t);
  const int n = c ? nr_pdsch_config_sweep_add_k0_layer(c->state, k0) : 0;
  static int s_left = 50; /* noise-floor RNTIs can drive this too: bounded, like context eviction */
  if ((first || n > 0) && s_left > 0 && s_left--)
    LOG_W(PHY, "SWEEP: rnti=0x%04x k0=%u observed on air -- %d hypotheses added to tda=%u\n", t->rnti,
          (unsigned)k0, n, (unsigned)t->tda_index);
  pthread_mutex_unlock(&g_lock);
  return n;
}

int nr_pdsch_config_sweep_observe_qm(const nr_pdsch_sweep_ticket_t *ticket, uint8_t mcs, int qm)
{
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("ISAC_QM_ORACLE");
    enabled = (e != NULL && atoi(e) == 0) ? 0 : 1;
  }
  if (!enabled || ticket == NULL || ticket->generation == 0 || qm <= 0)
    return 0;
  const uint8_t mask = qm_table_mask(mcs, qm);
  if (mask == 0 || mask == 0x7)
    return 0; /* impossible for this MCS, or every table agrees: no information */
  pthread_mutex_lock(&g_lock);
  sweep_context_t *c = ticket_context(ticket);
  int n = 0;
  if (c != NULL && c->state->winner < 0) {
    const uint8_t inter = c->qm_obs ? (uint8_t)(c->qm_tables & mask) : mask;
    if (inter == 0) {
      c->qm_obs = 0;
      c->qm_tables = 0;
    } else {
      c->qm_tables = inter;
      if (c->qm_obs < 255) c->qm_obs++;
      if (c->qm_obs >= 2) {
        const int before = c->state->n_hyp;
        n = prune_tables(c->state, inter);
        if (n == before) n = 0;
        else if (n > 0) context_reindexed(c);
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

/* One line per 4096 scored outcomes (under g_lock): how many contexts exist and how long they live
 * (created vs scored), how much feedback arrives stale, and for the context just fed how far it is from
 * the separation test -- the leader's lower bound against the widest bound still overlapping it. */
static void census_log(const sweep_context_t *c)
{
  const nr_pdsch_config_sweep_state_t *s = c->state;
  int live = 0;
  for (int i = 0; i < NR_PDSCH_SWEEP_MAX_CONTEXTS; i++)
    live += g_contexts[i].generation != 0;
  int lead = 0;
  for (int i = 1; i < s->n_hyp; i++)
    if (rate_of(s, i) > rate_of(s, lead)) lead = i;
  double lo, hi, max_hi = 0;
  nr_crc_interval(s->ok[lead], s->trials[lead], (unsigned)s->n_hyp, &lo, &hi);
  int unrefuted = 0, twins = 0;
  uint32_t tmin = UINT32_MAX, tmax = 0;
  for (int i = 0; i < s->n_hyp; i++) {
    if (s->trials[i] < tmin) tmin = s->trials[i];
    if (s->trials[i] > tmax) tmax = s->trials[i];
    if (i == lead) continue;
    double l2, h2;
    nr_crc_interval(s->ok[i], s->trials[i], (unsigned)s->n_hyp, &l2, &h2);
    if (h2 >= lo) unrefuted++;
    if (h2 > max_hi) max_hi = h2;
    if (s->ok[i] * 2 > s->ok[lead] && s->ok[lead] >= 4) twins++; /* a runner-up passing at >= half the leader's count */
  }
  const nr_pdsch_cfg_hypothesis_t *h = &s->hyp[lead];
  LOG_A(PHY, "SENSING: SWEEPSTAT scored=%llu stale=%llu created=%llu reindexed=%llu live=%d | rnti=0x%04x tda=%u "
             "n_hyp=%d outcomes=%llu trials[min=%u max=%u] leader=%d S=%u L=%u k0=%u tbl=%u ok=%u/%u lo=%.3f "
             "max_other_hi=%.3f unrefuted=%d near_twins=%d winner=%d\n",
        (unsigned long long)g_st_scored, (unsigned long long)g_st_stale, (unsigned long long)g_st_created,
        (unsigned long long)g_st_reindexed, live, c->rnti, (unsigned)c->tda, s->n_hyp,
        (unsigned long long)c->outcomes, tmin, tmax, lead, h->tda_start, h->tda_length, h->k0, h->mcs_table,
        s->ok[lead], s->trials[lead], lo, max_hi, unrefuted, twins, s->winner);
}

bool nr_pdsch_config_sweep_feedback(const nr_pdsch_sweep_ticket_t *ticket, bool crc_ok,
                                   nr_pdsch_cfg_hypothesis_t *winner)
{
  pthread_mutex_lock(&g_lock);
  sweep_context_t *c = ticket_context(ticket);
  bool announced = false;
  if (!c && ticket && ticket->generation)
    g_st_stale++;
  if (c) {
    int w = nr_pdsch_config_sweep_feed(c->state, ticket->hypothesis, crc_ok);
    ++c->outcomes;
    if ((++g_st_scored % 4096) == 0)
      census_log(c);
    if (c->priored && c->state->winner < 0 && c->outcomes >= PRIOR_PROBATION) {
      double best_rate = 0.0;
      for (int i = 0; i < c->state->n_hyp; i++) {
        const double r = c->state->trials[i]
                             ? (double)c->state->ok[i] / (double)c->state->trials[i]
                             : 0.0;
        if (r > best_rate) {
          best_rate = r;
        }
      }
      if (best_rate < SWEEP_MIN_RATE) {
        /* The prior does not hold for this context. Restore the full search and stop applying the
         * prior that seeded it -- publishing it was the error, and leaving it valid would make every
         * later context pay the same probation. */
        if (c->legality) {
          context_catalog(c, rnti_ctx(c->rnti, false));
          context_reindexed(c);
        }
        if (c->priored == PRIORED_CELL) {
          g_prior.valid = false;
          LOG_W(PHY, "SWEEP: cell prior WITHDRAWN -- it did not hold for rnti=0x%04x tda=%u "
                     "(best rate %.3f < %.3f)\n", c->rnti, (unsigned)c->tda, best_rate, SWEEP_MIN_RATE);
        } else {
          rnti_ctx_t *r = rnti_ctx(c->rnti, false);
          if (r)
            r->prior.valid = false;
          LOG_W(PHY, "SWEEP: rnti=0x%04x prior WITHDRAWN for tda=%u (best rate %.3f < %.3f)\n",
                c->rnti, (unsigned)c->tda, best_rate, SWEEP_MIN_RATE);
        }
        c->priored = PRIORED_NONE;
        c->outcomes = 0;
        w = -1;
      }
    }
    if (ticket->settled && ticket->hypothesis == w) {
      ++c->locked_trials; c->locked_passes += crc_ok;
      c->failure_streak = crc_ok ? 0 : c->failure_streak + 1;
      if (recovery_needed(c)) {
        reopen_context(c);
        pthread_mutex_unlock(&g_lock);
        return false;
      }

      if (g_reporter && c->locked_trials % 1000 == 0) {
        nr_pdsch_sweep_report_t r={.configuration=c->configuration,.rnti=c->rnti,.tda=c->tda,
          .operational=true,.passes=c->locked_passes,.trials=c->locked_trials};
        g_reporter(&r);
      }
    }
    if (c->outcomes % 10000 == 0 || (crc_ok && !c->reported)) {
      const nr_pdsch_config_sweep_state_t *s=c->state;
      int best=0; uint32_t minimum=UINT32_MAX;
      for(int i=0;i<s->n_hyp;i++) {
        if(s->trials[i]<minimum) minimum=s->trials[i];
        if((double)s->ok[i]/(s->trials[i]?s->trials[i]:1)
            >(double)s->ok[best]/(s->trials[best]?s->trials[best]:1)) best=i;
      }
      const nr_pdsch_cfg_hypothesis_t *h=&s->hyp[best];
      if (g_reporter) {
        nr_pdsch_sweep_report_t r={.configuration=c->configuration,.rnti=c->rnti,.tda=c->tda,
          .outcomes=c->outcomes,.minimum=minimum,.passes=s->ok[best],.trials=s->trials[best],
          .hypothesis=*h,.winner=w};
        g_reporter(&r);
      }
    }
    if (w >= 0 && !c->reported) {
      c->reported = true;
      double reference_upper;
      nr_crc_interval(c->state->ok[w], c->state->trials[w], (unsigned)c->state->n_hyp,
                      &c->reference_crc_lower, &reference_upper);

      announced = true;
      /* Publish the UE-wide fields so this RNTI's sibling TDA contexts do not re-derive them. Only
       * the first converged context of the RNTI publishes: later ones are already cheap, and
       * re-publishing would let a context that converged under a prior reinforce that same prior.
       * The cell-wide prior is only ever set by agreement between two RNTIs. */
      rnti_ctx_t *r = rnti_ctx(c->rnti, true);
      /* A k0 layer this context tried and did not win on was a false k0-oracle hit (another UE's PDSCH
       * on those PRBs): stop seeding it into this RNTI's later contexts. */
      const uint64_t lost = r->k0_seen & ~(UINT64_C(1) << c->state->hyp[w].k0);
      if (lost) {
        r->k0_seen &= ~lost;
        LOG_I(PHY, "SWEEP: rnti=0x%04x tda=%u converged on k0=%u -- dropped k0-oracle layers 0x%llx\n", c->rnti,
              (unsigned)c->tda, (unsigned)c->state->hyp[w].k0, (unsigned long long)lost);
      }
      if (!r->prior.valid) {
        r->prior = (prior_t){.valid = true, .configuration = c->configuration,
                             .mcs_table = c->state->hyp[w].mcs_table,
                             .dmrs_add_pos = c->state->hyp[w].dmrs_add_pos,
                             .dmrs_max_len = c->state->hyp[w].dmrs_max_len,
                             .mapping_type = c->state->hyp[w].mapping_type};
        LOG_W(PHY,
              "SWEEP: rnti=0x%04x CONVERGED tda=%u mapping=%c k0=%u mcs_table=%u dmrs_add_pos=%u dmrs_max_len=%u "
              "(%u/%u trials on the winner, cfg=0x%llx) -- private to this RNTI until a second agrees\n",
              c->rnti, (unsigned)c->tda, c->state->hyp[w].mapping_type ? 'B' : 'A',
              (unsigned)c->state->hyp[w].k0, (unsigned)c->state->hyp[w].mcs_table,
              (unsigned)c->state->hyp[w].dmrs_add_pos, (unsigned)c->state->hyp[w].dmrs_max_len,
              c->state->ok[w], c->state->trials[w], (unsigned long long)c->configuration);
        prior_promote_locked(r);
      }
      if (winner)
        *winner = c->state->hyp[w];
    }
  }
  pthread_mutex_unlock(&g_lock);
  return announced;
}

void nr_pdsch_config_sweep_context_stats(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA,
                                         uint32_t *passes, uint32_t *trials)
{
  *passes = *trials = 0;
  pthread_mutex_lock(&g_lock);
  for (int i=0;i<NR_PDSCH_SWEEP_MAX_CONTEXTS;++i) {
    const sweep_context_t *c=&g_contexts[i];
    /* tda 0xFF = every TDA context of this configuration: a layout hypothesis reads the TDA
     * index at its own offset, so its evidence is spread over the contexts that index created. */
    if (c->generation && c->configuration==configuration && c->rnti==rnti && (tda == 0xFF || c->tda==tda) && c->typeA==typeA) {
      for (int h=0; h<c->state->n_hyp; ++h) { *passes += c->state->ok[h]; *trials += c->state->trials[h]; }
      if (tda != 0xFF)
        break;
    }
  }
  pthread_mutex_unlock(&g_lock);
}
bool nr_pdsch_config_sweep_is_settled(uint64_t configuration, uint16_t rnti, uint8_t tda, int typeA)
{
  pthread_mutex_lock(&g_lock);
  bool settled=false;
  for (int i=0;i<NR_PDSCH_SWEEP_MAX_CONTEXTS;++i) {
    const sweep_context_t *c=&g_contexts[i];
    if (c->generation && c->configuration==configuration && c->rnti==rnti
        && c->tda==tda && c->tda_count==0 && c->typeA==typeA && c->state->winner>=0) {
      settled=true;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return settled;
}
/* Diagnostic only: how many live keyed contexts currently hold a winner. Read by the acquisition
 * state tracker (nr_passive_acq_state.c) at the RT periodic summary; not a decision input. */
int nr_pdsch_config_sweep_settled_count(void)
{
  pthread_mutex_lock(&g_lock);
  int n=0;
  for (int i=0;i<NR_PDSCH_SWEEP_MAX_CONTEXTS;++i)
    if (g_contexts[i].generation && g_contexts[i].state->winner>=0) ++n;
  pthread_mutex_unlock(&g_lock);
  return n;
}

void nr_pdsch_config_sweep_reset_all(void)
{
  pthread_mutex_lock(&g_lock);
  /* Every lookup/feedback path requires a live generation. Invalidate the small
   * identity fields now; select() clears the full state before reusing a slot.
   * Clearing all 1024 hypothesis arrays here used ~1.2 ms even for an empty bank.
   * touched=0 makes invalid slots eligible for reuse ahead of live contexts. */
  for (int i=0; i<NR_PDSCH_SWEEP_MAX_CONTEXTS; ++i) {
    g_contexts[i].generation=0;
    g_contexts[i].touched=0;
  }
  /* Priors and observations are evidence derived from those contexts; keeping them across a reset
   * would let a cleared run inherit conclusions it can no longer justify. */
  memset(g_rnti, 0, sizeof(g_rnti));
  memset(&g_obs, 0, sizeof(g_obs));
  g_prior.valid = false;
  /* Do not rewind generation: in-flight jobs from before reset must remain invalid. */
  pthread_mutex_unlock(&g_lock);
}

bool nr_pdsch_config_sweep_snapshot(const nr_pdsch_sweep_ticket_t *ticket,
                                   nr_pdsch_config_sweep_state_t *out)
{
  if (!out)
    return false;
  pthread_mutex_lock(&g_lock);
  sweep_context_t *c = ticket_context(ticket);
  if (c)
    *out = *c->state;
  pthread_mutex_unlock(&g_lock);
  return c != NULL;
}

/* Compatibility for pure legacy tests; not used by the receive pipeline. */
static nr_pdsch_config_sweep_state_t g_sweep;
static bool g_sweep_on;
void nr_pdsch_config_sweep_enable_global(int tda_count)
{
  pthread_mutex_lock(&g_lock);
  nr_pdsch_config_sweep_init(&g_sweep, tda_count);
  g_sweep_on = true;
  pthread_mutex_unlock(&g_lock);
}
int nr_pdsch_config_sweep_next_global(nr_pdsch_cfg_hypothesis_t *out)
{
  pthread_mutex_lock(&g_lock);
  int r = g_sweep_on ? nr_pdsch_config_sweep_next(&g_sweep, out) : -1;
  pthread_mutex_unlock(&g_lock);
  return r;
}
int nr_pdsch_config_sweep_feed_global(int idx, bool tb_crc_ok)
{
  pthread_mutex_lock(&g_lock);
  int r = g_sweep_on ? nr_pdsch_config_sweep_feed(&g_sweep, idx, tb_crc_ok) : -1;
  pthread_mutex_unlock(&g_lock);
  return r;
}
int nr_pdsch_config_sweep_winner_global(void)
{
  pthread_mutex_lock(&g_lock);
  int r = g_sweep_on ? g_sweep.winner : -1;
  pthread_mutex_unlock(&g_lock);
  return r;
}
bool nr_pdsch_config_sweep_result_global(nr_pdsch_cfg_hypothesis_t *out)
{
  pthread_mutex_lock(&g_lock);
  bool ok = out && g_sweep_on && g_sweep.winner >= 0;
  if (ok)
    *out = g_sweep.hyp[g_sweep.winner];
  pthread_mutex_unlock(&g_lock);
  return ok;
}
