/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Evidence that an RNTI's DM-RS is distinguishable from the noise floor under ANY decode hypothesis (PARK, 2026-10-30), and the cell-measured
 * no-chance margin of nr_snr_gate.h. Pure state + functions on explicit structs (no globals, no flags, no dB constants in the decision): the
 * solver keeps one nse_ref_t / nse_gate_t per cell and one nse_rnti_t per RNTI. Unit-tested in tests/nr_pdsch_solver_test.cc.
 *
 * Noise-floor reference (nse_ref_t): a histogram (0.5 dB) of the DM-RS comb SNR of every decode of RNTIs that are not solved (their hypotheses
 * are overwhelmingly wrong, so their values are noise draws). Empirical null (Efron): the null is centred on the lowest mode of the pool and its
 * upper tail is the LOWER half mirrored about that mode (the lower half cannot be contaminated by signal): p_single(x) = P0(X >= x), uniform on
 * (0,1) for a noise draw. Alternative H1 (a hypothesis sees the RNTI): p ~ Beta(a, 1) with a MEASURED on the cell from the weakest fifth of the
 * decodes that passed their TB CRC (solved RNTIs). One SPRT per hypothesis class, see nse_rnti_t. A parked RNTI is re-tested on 1 grant in 2^k
 * (k grows with each negative re-test, capped at NSE_THIN_MAX_SHIFT) and un-parked when a class climbs back: a UE that turns toward us is
 * recovered within about 2^NSE_THIN_MAX_SHIFT * ceil(-ln BETA / E_H1[LLR per draw]) of its grants. */
#ifndef NR_SIGNAL_EVIDENCE_H
#define NR_SIGNAL_EVIDENCE_H
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NSE_BINS 160
#define NSE_LO_DB (-40.0f)
#define NSE_BW_DB 0.5f
#define NSE_MIN_POOL 2048u     /* reference samples before it is trusted (sample size, not a decision level) */
#define NSE_BETA 0.01          /* error rate of both boundaries (the operator-visible false-park / false-un-park bound) */
#define NSE_THIN_MAX_SHIFT 4   /* re-test at most 1 grant in 16 */
#define NSE_PRING 128

typedef struct {
  uint32_t h[NSE_BINS];
  uint64_t n;
  int mu_bin;      /* -1: reference not ready */
  uint64_t left;   /* mass at or below the mode (mode bin counted half) */
  uint32_t since;
  float pr[NSE_PRING]; /* p of the decodes that passed their CRC (the H1 shape) */
  int pr_n, pr_pos;
} nse_ref_t;

static inline int nse_bin(float db)
{
  int b = (int)floorf((db - NSE_LO_DB) / NSE_BW_DB);
  return b < 0 ? 0 : b >= NSE_BINS ? NSE_BINS - 1 : b;
}

static inline void nse_ref_recompute(nse_ref_t *R)
{
  R->since = 0;
  if (R->n < NSE_MIN_POOL) { R->mu_bin = -1; return; }
  uint64_t cum = 0;
  int b50 = NSE_BINS - 1;
  for (int b = 0; b < NSE_BINS; b++) {
    cum += R->h[b];
    if (2 * cum >= R->n) { b50 = b; break; }
  }
  int mu = 0;
  uint64_t best = 0;
  for (int b = 0; b <= b50; b++) { /* mode of the smoothed lower half */
    const uint64_t s = (uint64_t)(b > 0 ? R->h[b - 1] : 0) + 2ull * R->h[b] + (b + 1 < NSE_BINS ? R->h[b + 1] : 0);
    if (s > best) { best = s; mu = b; }
  }
  uint64_t l = 0;
  for (int b = 0; b < mu; b++) l += R->h[b];
  R->mu_bin = mu;
  R->left = l + R->h[mu] / 2;
}

static inline void nse_ref_add(nse_ref_t *R, float db)
{
  R->h[nse_bin(db)]++;
  R->n++;
  if (R->n > (1u << 22)) { /* slow forgetting */
    R->n = 0;
    for (int b = 0; b < NSE_BINS; b++) { R->h[b] >>= 1; R->n += R->h[b]; }
    R->since = 1u << 30;
  }
  if (++R->since >= 64 || R->mu_bin < 0) nse_ref_recompute(R);
}

static inline bool nse_ref_ready(const nse_ref_t *R) { return R->mu_bin >= 0 && R->n >= NSE_MIN_POOL; }

/* Lmid(b): reference mass below bin b plus half of bin b */
static inline double nse_lmid(const nse_ref_t *R, int b)
{
  double l = 0;
  for (int i = 0; i < b && i < NSE_BINS; i++) l += R->h[i];
  return b >= 0 && b < NSE_BINS ? l + 0.5 * R->h[b] : l;
}

/* P0(X >= db) under the empirical null (mid-p, so uniform on (0,1) when db is a noise draw); 1 when the reference is not ready */
static inline double nse_p_single(const nse_ref_t *R, float db)
{
  if (!nse_ref_ready(R)) return 1.0;
  const int b = nse_bin(db);
  const double tot = 2.0 * (double)R->left + 1.0;
  double p;
  if (b <= R->mu_bin) p = 1.0 - nse_lmid(R, b) / tot;                    /* the measured lower half */
  else p = (nse_lmid(R, 2 * R->mu_bin - b) + 0.5) / tot;                  /* the upper tail: the lower half mirrored about the mode */
  return p < 1e-9 ? 1e-9 : p > 1.0 ? 1.0 : p;
}

/* a decode that passed its TB CRC (solved RNTI): its p is evidence of what a real signal looks like in this cell */
static inline void nse_ref_note_pass(nse_ref_t *R, float comb_db)
{
  if (!nse_ref_ready(R)) return;
  R->pr[R->pr_pos] = (float)nse_p_single(R, comb_db);
  R->pr_pos = (R->pr_pos + 1) % NSE_PRING;
  if (R->pr_n < NSE_PRING) R->pr_n++;
}

#define NSE_MIN_PASSES 8
static inline bool nse_h1_ready(const nse_ref_t *R) { return nse_ref_ready(R) && R->pr_n >= NSE_MIN_PASSES; }

/* H1 shape a of Beta(a, 1) (p of a decode that sees the RNTI's DM-RS): the maximum-likelihood fit to the WEAKEST fifth of the cell's CRC-passing
 * decodes (largest p), so the alternative is the weak real signal, not the typical one. a = 1 / mean(-ln p), clamped to [0.1, 0.9]. */
static inline double nse_h1_shape(const nse_ref_t *R)
{
  if (R->pr_n < NSE_MIN_PASSES) return 0.5;
  float v[NSE_PRING];
  const int n = R->pr_n;
  memcpy(v, R->pr, sizeof(float) * (size_t)n);
  for (int i = 1; i < n; i++) /* insertion sort, n <= 128 */
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { const float t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
  int k = n / 5 < 2 ? 2 : n / 5;
  double sum = 0;
  for (int i = n - k; i < n; i++) sum += -log(v[i] < 1e-9f ? 1e-9 : v[i]);
  const double a = (double)k / (sum > 1e-9 ? sum : 1e-9);
  return a < 0.1 ? 0.1 : a > 0.9 ? 0.9 : a;
}

/* Per RNTI, one sequential test per hypothesis class (a hash of DM-RS ports, CDM groups, layers and scrambling identity of the decode: every
 * hypothesis class the search tries). The RNTI is parked when EVERY class it was decoded under has reached the noise boundary (and not climbed back to 0 since); it is un-parked when any class
 * climbs back. Per class a decode is one independent draw (the same grant/class is counted once) with p = p_single; LLR += ln a + (a - 1) ln p,
 * clipped to [ln BETA, -ln BETA]. If a class carries the RNTI's DM-RS (the H1 of the cell), P(its LLR ever reaches ln BETA) <= BETA (Wald), so
 * P(park | some hypothesis sees the RNTI) <= BETA; weak signals that stay on one class accumulate across grants, which the per-grant maximum over
 * hypotheses cannot do. */
#define NSE_CLASSES 32
typedef struct {
  float llr[NSE_CLASSES];
  uint64_t cabs[NSE_CLASSES]; /* last grant counted per class */
  uint32_t seen;              /* bitmask of classes decoded */
  uint32_t floor_hit;         /* classes whose test reached the noise boundary and has not climbed back to 0 since */
  uint64_t last_abs;          /* last grant while parked (re-test counter) */
  bool parked, proven;        /* proven: the RNTI is solved (its decodes are not noise draws) */
  uint8_t pk_neg;             /* negative re-test grants while parked */
  uint32_t n_obs, n_park;
} nse_rnti_t;

typedef enum { NSE_NONE = 0, NSE_PARKED, NSE_UNPARKED } nse_event_t;

/* One decode (probe or full) of the RNTI's grant at abs under hypothesis class cls (any integer; hashed). real_pass: a TB CRC pass of a SOLVED RNTI (a
 * probe's CRC16 pass is chance 2^-16 per try and says nothing about the signal: RNTI 0x9f52 had 15 in 164k tries at the noise floor). solved: the
 * RNTI's DM-RS is proven. cover_ok: the search has spent enough probes on this RNTI that a miss means "no DM-RS", not "not tried yet". */
static inline nse_event_t nse_note(nse_rnti_t *s, nse_ref_t *R, uint64_t abs, uint32_t cls, float comb_db, bool real_pass, bool solved, bool cover_ok)
{
  s->proven = solved;
  if (real_pass) nse_ref_note_pass(R, comb_db);
  else if (!solved) nse_ref_add(R, comb_db);
  if (!nse_h1_ready(R)) return NSE_NONE;
  const unsigned c = (cls * 2654435761u) >> 27; /* 0..31 */
  const double lo = log(NSE_BETA), hi = -lo;
  const bool fresh = !((s->seen >> c) & 1u);
  if (!fresh && s->cabs[c] == abs) return NSE_NONE; /* the same grant under the same class: the same IQ, not a new draw */
  if (fresh) { s->seen |= 1u << c; s->llr[c] = s->parked ? (float)lo : 0.f; }
  s->cabs[c] = abs;
  s->n_obs++;
  const double a = nse_h1_shape(R);
  const double p = nse_p_single(R, comb_db);
  double l = (double)s->llr[c] + log(a) + (a - 1.0) * log(p);
  l = l > hi ? hi : l < lo ? lo : l;
  s->llr[c] = (float)l;
  if (l <= lo) s->floor_hit |= 1u << c;
  else if (l >= 0.0) s->floor_hit &= ~(1u << c);
  nse_event_t ev = NSE_NONE;
  if (s->parked) {
    if (abs != s->last_abs) { s->last_abs = abs; if (s->pk_neg < 255) s->pk_neg++; }
    if (l >= 0.0) { /* a class climbed back from the floor by |ln BETA| nats: start over */
      s->parked = false;
      s->pk_neg = 0;
      memset(s->llr, 0, sizeof s->llr);
      s->floor_hit = 0;
      ev = NSE_UNPARKED;
    }
  } else if (l <= lo && cover_ok) {
    if ((s->floor_hit & s->seen) == s->seen) { s->parked = true; s->pk_neg = 0; s->last_abs = abs; s->n_park++; ev = NSE_PARKED; }
  }
  return ev;
}

/* whether this RNTI's grant number `grants` is decoded: always when active, 1 in 2^k while parked */
static inline bool nse_due(const nse_rnti_t *s, uint32_t grants)
{
  if (!s->parked) return true;
  const unsigned sh = s->pk_neg < NSE_THIN_MAX_SHIFT ? s->pk_neg : NSE_THIN_MAX_SHIFT;
  return (grants & ((1u << sh) - 1u)) == 0;
}

/* ---- cell-measured no-chance margin (nr_snr_gate.h) ----
 * margin m = comb SNR - required SNR of the real, ungated decodes of the cell, 1 dB bins from NSE_G_LO. The gate (decode skipped below req - M)
 * starts at the design margin M0 and is tightened to the largest edge below which the Wilson upper bound (one-sided, z = NSE_G_Z) of the CRC
 * pass rate of all ungated tries is <= NSE_G_EPS: bounded expected CRC loss, measured on this cell and direction (DL/UL never pooled). */
#define NSE_G_BINS 40
#define NSE_G_LO (-5.0)
#define NSE_G_Z 2.576
#define NSE_G_EPS 0.002
typedef struct { uint32_t tries[NSE_G_BINS], pass[NSE_G_BINS]; } nse_gate_t;

static inline void nse_gate_note(nse_gate_t *G, double margin_db, bool crc_ok)
{
  if (!isfinite(margin_db) || margin_db < NSE_G_LO) return;
  int b = (int)floor(margin_db - NSE_G_LO);
  if (b >= NSE_G_BINS) b = NSE_G_BINS - 1;
  if (G->tries[b] < UINT32_MAX) G->tries[b]++;
  if (crc_ok && G->pass[b] < UINT32_MAX) G->pass[b]++;
}

static inline double nse_wilson_upper(double k, double n, double z)
{
  if (n <= 0) return 1.0;
  const double ph = k / n, z2 = z * z;
  return (ph + z2 / (2 * n) + z * sqrt(ph * (1 - ph) / n + z2 / (4 * n * n))) / (1 + z2 / n);
}

/* gate margin M (dB below the requirement at which a decode is skipped): m0 until the cell's own data allow a tighter one; never above m0 */
static inline double nse_gate_margin(const nse_gate_t *G, double m0)
{
  double tr = 0, ps = 0;
  int edge = 0;
  for (int e = 1; e <= NSE_G_BINS; e++) {
    tr += G->tries[e - 1];
    ps += G->pass[e - 1];
    if (tr > 0 && nse_wilson_upper(ps, tr, NSE_G_Z) <= NSE_G_EPS) edge = e;
    else if (ps > 0) break; /* real passes: the safe region ends here (too few tries without a pass is only "not yet") */
  }
  const double m = -(NSE_G_LO + edge); /* the safe region ends at margin NSE_G_LO + edge (dB vs the requirement): gate below req - m */
  return m < m0 ? m : m0;
}
#endif
