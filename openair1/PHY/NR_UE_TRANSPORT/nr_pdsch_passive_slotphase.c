/* SLOTPHASE: see nr_pdsch_passive_slotphase.h. */
#include "common/utils/nr_cell.h" /* mpci: per-cell state (NR_CELL_STATIC) */
#include "nr_pdsch_passive_slotphase.h"
#include "nr_re_pattern.h"
#include "nr_snr_gate.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"

#define SP_MAX_ENT 8
#define SP_NVAR 22       /* variant slots of one entry: 0 control, 1 union, 2 replace, 3 foot-union, 4 foot-union+confirmed, 5..7 trim 1..3, 8 est, 9 est+confirmed, 10.. one per seed / footprint id */
#define SV_UNION 1
#define SV_REPLACE 2
#define SV_FUNION 3
#define SV_FUNION_CONF 4
#define SV_TRIM(t) (4 + (t)) /* t = 1..NR_SLOTPHASE_MAX_TRIM */
#define SV_EST 8
#define SV_EST_CONF 9
#define SV_DYN0 10
#define SP_TAGS 1024     /* in-flight decodes of flagged phases, by solver token */
#define SP_LOGS 256      /* SLOTPHASE decision lines per cell (they are rare; the cap bounds a flapping phase) */
#define SP_STRONG_MARGIN_DB 6.0
static const uint16_t kPer[NR_SLOTPHASE_NPER] = {10, 20, 40, 80, 160};
static const uint16_t kBase[NR_SLOTPHASE_NPER] = {0, 10, 30, 70, 150};
#define SP_NBIN 310 /* 10 + 20 + 40 + 80 + 160 */

typedef struct { uint32_t n, ok; } sp_cnt_t;
static const uint16_t kLook[NR_SLOTPHASE_EST_LOOKS] = {16, 24, 32, 48, 64, 96};
typedef struct { uint16_t sym, sc, rb0, nrb; uint8_t dens, n; } sp_desc_t; /* what a footprint variant is, for the log line (n = footprints of a union) */
/* the residual estimator of one flagged phase (state NR_SP_EST_*): its FAIL table (heap, lazily) and the measured patterns */
typedef struct {
  uint8_t state, look, npat, have_cand, cand_npat;
  uint16_t planned;     /* control grants planned while collecting (bounds a phase that never delivers samples) */
  uint32_t pos_n;       /* FAIL grants at the first look that found foreign REs */
  nr_spr_tab_t *fail;
  fapi_nr_dl_config_csirs_pdu_rel15_t pat[NR_SPR_MAXPAT], cand[NR_SPR_MAXPAT];
} sp_est_t;
typedef struct {
  bool used;
  bool others_offered; /* the non-estimator variants have been offered at least once */
  sp_est_t est;
  uint8_t pi, state, adopted;
  uint16_t phase;
  uint8_t vkind[SP_NVAR];
  uint32_t vseed[SP_NVAR]; /* id of a dynamic (seed / footprint) variant, 0 = slot free */
  sp_desc_t vdesc[SP_NVAR];
  bool dead[SP_NVAR], offered[SP_NVAR];
  sp_cnt_t v[SP_NVAR];
  uint32_t rot;
} sp_ent_t;
typedef struct {
  sp_cnt_t bin[SP_NBIN];
  sp_cnt_t all;
  sp_ent_t ent[SP_MAX_ENT];
  struct { uint32_t token; uint8_t ent, var; } tag[SP_TAGS];
  nr_spr_tab_t *ref[2]; /* the cell's reference: strong grants whose CRC passed, alternately (heap, lazily) */
  uint32_t ref_n;
  bool capable;         /* residual tables have arrived at least once (the GPU demod delivers them): control-only collection is then meaningful */
  _Atomic int open;     /* lock-free: the residual accumulation may be wanted (a phase collects, or the reference is short) */
  uint32_t gen, logs;
} sp_t;
NR_CELL_STATIC(sp_t, g_sp);
#define g_sp NR_CELL_REF(g_sp)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_sp_mu, PTHREAD_MUTEX_INITIALIZER);
#define g_sp_mu NR_CELL_REF(g_sp_mu)
NR_CELL_STATIC(_Atomic int, g_sp_nent); /* flagged phases of the cell (read without the lock) */
#define g_sp_nent NR_CELL_REF(g_sp_nent)

#define SP_LOG(...) do { if (g_sp.logs < SP_LOGS) { g_sp.logs++; LOG_A(PHY, __VA_ARGS__); } } while (0)

static double wilson(uint32_t k, uint32_t n, int hi)
{
  if (n == 0) return hi ? 1.0 : 0.0;
  const double z = 1.96, p = (double)k / n, d = 1.0 + z * z / n, c = p + z * z / (2.0 * n);
  const double a = z * sqrt(p * (1.0 - p) / n + z * z / (4.0 * (double)n * n));
  const double r = (hi ? c + a : c - a) / d;
  return r < 0.0 ? 0.0 : r > 1.0 ? 1.0 : r;
}
double nr_slotphase_wilson_lo(uint32_t k, uint32_t n) { return wilson(k, n, 0); }
double nr_slotphase_wilson_hi(uint32_t k, uint32_t n) { return wilson(k, n, 1); }

bool nr_slotphase_strong(double comb_db, int mcs, int tbl, int nl, int rv, bool dedicated, bool probe)
{
  const double req = nr_required_snr_db(mcs, tbl);
  return !probe && dedicated && rv == 0 && nl >= 1 && nl <= 2 && isfinite(comb_db) && comb_db > -90.0 && isfinite(req)
         && comb_db >= req + SP_STRONG_MARGIN_DB;
}

static const char *kind_name(int k)
{
  switch (k) {
    case NR_SP_EST: return "est";
    case NR_SP_EST_CONF: return "est+confirmed";
    case NR_SP_UNION: return "union";
    case NR_SP_REPLACE: return "replace";
    case NR_SP_SEED: return "seed";
    case NR_SP_FOOT: return "foot";
    case NR_SP_FUNION: return "foot-union";
    case NR_SP_FUNION_CONF: return "foot-union+confirmed";
    case NR_SP_TRIM: return "trim";
    default: return "none";
  }
}
/* sp_desc: the variant for the log: pattern masks of a footprint, the end trim, the seed id */
static const char *sp_desc(const sp_ent_t *e, int var, char *b, size_t n)
{
  const sp_desc_t *d = &e->vdesc[var];
  switch (e->vkind[var]) {
    case NR_SP_FOOT:
      snprintf(b, n, "id=%u sym=0x%x sc=0x%x dens=%u rb=%u:%u", e->vseed[var], d->sym, d->sc, d->dens, d->rb0, d->nrb);
      break;
    case NR_SP_FUNION:
    case NR_SP_FUNION_CONF: snprintf(b, n, "footprints=%u", d->n); break;
    case NR_SP_EST:
    case NR_SP_EST_CONF: snprintf(b, n, "measured-patterns=%u", d->n); break;
    case NR_SP_TRIM: snprintf(b, n, "end-%d-symbols", var - 4); break;
    case NR_SP_SEED: snprintf(b, n, "id=%u", e->vseed[var]); break;
    default: b[0] = 0;
  }
  return b;
}

/* sp_reopen_ent: forgets what an undecided entry measured (the CSI-RS map or the seed set changed) */
static void sp_reopen_ent(sp_ent_t *e)
{
  memset(e->v, 0, sizeof(e->v));
  memset(e->dead, 0, sizeof(e->dead));
  memset(e->offered, 0, sizeof(e->offered));
  memset(e->vseed, 0, sizeof(e->vseed));
  memset(e->vdesc, 0, sizeof(e->vdesc));
  for (int j = SV_DYN0; j < SP_NVAR; j++) e->vkind[j] = 0;
  e->rot = 0;
  e->others_offered = false;
}
static void sp_ent_init(sp_ent_t *e, int pi, int phase)
{
  memset(e, 0, sizeof(*e));
  e->used = true;
  e->pi = (uint8_t)pi;
  e->phase = (uint16_t)phase;
  e->vkind[SV_UNION] = NR_SP_UNION;
  e->vkind[SV_REPLACE] = NR_SP_REPLACE;
  e->vkind[SV_FUNION] = NR_SP_FUNION;
  e->vkind[SV_FUNION_CONF] = NR_SP_FUNION_CONF;
  e->vkind[SV_EST] = NR_SP_EST;
  e->vkind[SV_EST_CONF] = NR_SP_EST_CONF;
  for (int t = 1; t <= NR_SLOTPHASE_MAX_TRIM; t++) e->vkind[SV_TRIM(t)] = NR_SP_TRIM;
}
/* sp_dyn: the variant slot keyed by (kind, id): found, or allocated in a free dynamic slot; -1 when none is free */
static int sp_dyn(sp_ent_t *e, int kind, uint32_t id)
{
  for (int q = SV_DYN0; q < SP_NVAR; q++)
    if (e->vseed[q] == id && e->vkind[q] == kind) return q;
  for (int q = SV_DYN0; q < SP_NVAR; q++)
    if (e->vseed[q] == 0) {
      e->vseed[q] = id;
      e->vkind[q] = (uint8_t)kind;
      return q;
    }
  return -1;
}
/* sp_sync: a new CSI-RS generation (confirmed set or seeds changed) voids the counters that were measured under the old one; an ADOPTED variant
 * keeps running (its own health check revokes it). Caller holds the lock. */
static void sp_sync(uint32_t gen)
{
  if (gen == g_sp.gen) return;
  g_sp.gen = gen;
  memset(g_sp.bin, 0, sizeof(g_sp.bin));
  memset(&g_sp.all, 0, sizeof(g_sp.all));
  for (int i = 0; i < SP_MAX_ENT; i++)
    if (g_sp.ent[i].used && g_sp.ent[i].state != NR_SP_ADOPTED) sp_reopen_ent(&g_sp.ent[i]);
}
static int sp_find(uint32_t slot)
{
  int best = -1;
  for (int i = 0; i < SP_MAX_ENT; i++) {
    const sp_ent_t *e = &g_sp.ent[i];
    if (e->used && slot % kPer[e->pi] == e->phase && (best < 0 || e->pi < g_sp.ent[best].pi)) best = i;
  }
  return best;
}

bool nr_slotphase_active(void) { return atomic_load_explicit(&g_sp_nent, memory_order_relaxed) > 0; }
bool nr_slotphase_covers(uint32_t slot)
{
  if (!nr_slotphase_active()) return false;
  pthread_mutex_lock(&g_sp_mu);
  const bool r = sp_find(slot) >= 0;
  pthread_mutex_unlock(&g_sp_mu);
  return r;
}
int nr_slotphase_query(uint32_t slot, int *state, int *adopted)
{
  pthread_mutex_lock(&g_sp_mu);
  const int i = sp_find(slot);
  int per = 0;
  if (i >= 0) {
    per = kPer[g_sp.ent[i].pi];
    if (state) *state = g_sp.ent[i].state;
    if (adopted) *adopted = g_sp.ent[i].adopted;
  }
  pthread_mutex_unlock(&g_sp_mu);
  return per;
}
void nr_slotphase_reset(void)
{
  pthread_mutex_lock(&g_sp_mu);
  for (int i = 0; i < SP_MAX_ENT; i++) free(g_sp.ent[i].est.fail);
  free(g_sp.ref[0]);
  free(g_sp.ref[1]);
  memset(&g_sp, 0, sizeof(g_sp));
  atomic_store_explicit(&g_sp_nent, 0, memory_order_relaxed);
  pthread_mutex_unlock(&g_sp_mu);
}

static void sp_tag(uint32_t token, uint8_t ent, uint8_t var)
{
  if (!token) return;
  const unsigned i = token % SP_TAGS;
  g_sp.tag[i].token = token;
  g_sp.tag[i].ent = ent;
  g_sp.tag[i].var = var;
}

nr_slotphase_choice_t nr_slotphase_plan(uint32_t slot, uint32_t gen, uint32_t token, const nr_slotphase_in_t *in)
{
  nr_slotphase_choice_t ch = {0, 0, NR_SP_CTRL, 0, 0, 0};
  if (!nr_slotphase_active() || !in) return ch;
  int n_seeds = in->n_seeds, n_foot = in->n_foot;
  if (n_seeds > NR_SLOTPHASE_MAX_SEED) n_seeds = NR_SLOTPHASE_MAX_SEED;
  if (n_seeds < 0 || (n_seeds > 0 && !in->seed_ids)) n_seeds = 0;
  if (n_foot > NR_SLOTPHASE_MAX_FOOT) n_foot = NR_SLOTPHASE_MAX_FOOT;
  if (n_foot < 0 || (n_foot > 0 && !in->foot_ids)) n_foot = 0;
  const uint32_t *seed_ids = in->seed_ids, *foot_ids = in->foot_ids;
  const int nsym = in->nsym;
  /* end-symbol variants: for a slot that may be partial (TDD special), and for a phase nothing else explains */
  const bool trim_on = nsym > 0 && (in->trim_ok || (n_seeds == 0 && n_foot == 0));
  pthread_mutex_lock(&g_sp_mu);
  sp_sync(gen);
  const int ei = sp_find(slot);
  if (ei < 0) {
    pthread_mutex_unlock(&g_sp_mu);
    return ch;
  }
  sp_ent_t *e = &g_sp.ent[ei];
  ch.ent = (uint8_t)(ei + 1);
  const bool est_ok = e->est.state == NR_SP_EST_POS && e->est.npat > 0;
  if (e->state == NR_SP_EXPLORE && e->est.state == NR_SP_EST_COLLECT && g_sp.capable && e->est.fail) {
    /* the estimator is measuring: every grant of the phase is decoded as always (control), its failed outcome and residual are the FAIL sample; bounded in plans so that a
     * phase whose grants never deliver residual tables (CPU demod) goes on to the other variants */
    if (++e->est.planned >= 4 * kLook[NR_SLOTPHASE_EST_LOOKS - 1] && e->est.fail->grants < kLook[0]) {
      e->est.state = NR_SP_EST_NEG;
      SP_LOG("SENSING: SLOTPHASE resid P/phase=%d/%u none: only %u residual samples after %u grants (ref %u); trying the other variants\n", kPer[e->pi], e->phase,
             e->est.fail->grants, e->est.planned, g_sp.ref_n);
    } else {
      sp_tag(token, ch.ent, 0);
      pthread_mutex_unlock(&g_sp_mu);
      return ch;
    }
  }
  /* variants this slot can express: seed / footprint variants are keyed by their id, so a variant means the same RE set in every slot of the phase */
  int cand[SP_NVAR], idx_of[SP_NVAR], nc = 0;
  if (e->state == NR_SP_ADOPTED) {
    const int a = e->adopted, k = e->vkind[a];
    if ((k == NR_SP_UNION || k == NR_SP_REPLACE) && n_seeds > 0) { ch.var = (uint8_t)a; ch.kind = (uint8_t)k; }
    else if ((k == NR_SP_FUNION || k == NR_SP_FUNION_CONF) && n_foot > 0) { ch.var = (uint8_t)a; ch.kind = (uint8_t)k; }
    else if (k == NR_SP_TRIM && nsym - (a - 4) >= NR_SLOTPHASE_MIN_NSYM) { ch.var = (uint8_t)a; ch.kind = NR_SP_TRIM; ch.trim = (uint8_t)(a - 4); }
    else if (k == NR_SP_SEED)
      for (int q = 0; q < n_seeds; q++)
        if (e->vseed[a] == seed_ids[q]) { ch.var = (uint8_t)a; ch.kind = NR_SP_SEED; ch.seed = (uint8_t)q; }
    if (k == NR_SP_FOOT)
      for (int q = 0; q < n_foot; q++)
        if (e->vseed[a] == foot_ids[q]) { ch.var = (uint8_t)a; ch.kind = NR_SP_FOOT; ch.foot = (uint8_t)q; }
    if ((k == NR_SP_EST || k == NR_SP_EST_CONF) && est_ok) { ch.var = (uint8_t)a; ch.kind = (uint8_t)k; }
  } else {
#define SP_OFFER(v, ix) do { e->offered[v] = true; if (!e->dead[v]) { cand[nc] = (v); idx_of[nc++] = (ix); } } while (0)
    cand[nc] = 0; idx_of[nc++] = 0;
    /* measured patterns first: while one of them is still undecided only they (and the control) are offered; the other variants follow when both are dead */
    if (est_ok) {
      SP_OFFER(SV_EST, 0);
      if (in->has_conf) SP_OFFER(SV_EST_CONF, 0);
      e->vdesc[SV_EST].n = e->vdesc[SV_EST_CONF].n = e->est.npat;
    }
    const bool est_first = nc > 1;
    if (!est_first) e->others_offered = true;
    if (!est_first && n_seeds > 0) SP_OFFER(SV_UNION, 0);
    if (!est_first && n_seeds > 0 && in->has_conf) SP_OFFER(SV_REPLACE, 0);
    if (!est_first && n_foot > 0) {
      SP_OFFER(SV_FUNION, 0);
      if (in->has_conf) SP_OFFER(SV_FUNION_CONF, 0);
      e->vdesc[SV_FUNION].n = e->vdesc[SV_FUNION_CONF].n = (uint8_t)n_foot;
    }
    for (int q = 0; !est_first && n_seeds > 1 && q < n_seeds; q++) { /* a single seed IS the union */
      const int j = sp_dyn(e, NR_SP_SEED, seed_ids[q]);
      if (j >= 0) SP_OFFER(j, q);
    }
    for (int q = 0; !est_first && n_foot > 1 && q < n_foot; q++) { /* a single footprint IS the footprint union */
      const int j = sp_dyn(e, NR_SP_FOOT, foot_ids[q]);
      if (j < 0) continue;
      if (in->foot) {
        sp_desc_t *d = &e->vdesc[j];
        d->sym = (uint16_t)nr_re_pattern_sym(&in->foot[q]);
        d->sc = (uint16_t)nr_re_pattern_sc(&in->foot[q]);
        d->dens = in->foot[q].freq_density;
        d->rb0 = in->foot[q].start_rb;
        d->nrb = in->foot[q].nr_of_rbs;
      }
      SP_OFFER(j, q);
    }
    for (int t = 1; !est_first && trim_on && t <= NR_SLOTPHASE_MAX_TRIM; t++)
      if (nsym - t >= NR_SLOTPHASE_MIN_NSYM) SP_OFFER(SV_TRIM(t), t);
#undef SP_OFFER
    const int pick = (int)(e->rot++ % (uint32_t)nc);
    ch.var = (uint8_t)cand[pick];
    ch.kind = ch.var == 0 ? NR_SP_CTRL : e->vkind[ch.var];
    ch.seed = ch.kind == NR_SP_SEED ? (uint8_t)idx_of[pick] : 0;
    ch.foot = ch.kind == NR_SP_FOOT ? (uint8_t)idx_of[pick] : 0;
    ch.trim = ch.kind == NR_SP_TRIM ? (uint8_t)idx_of[pick] : 0;
  }
  sp_tag(token, ch.ent, ch.var);
  pthread_mutex_unlock(&g_sp_mu);
  return ch;
}

/* sp_flag_check: creates the entry of the smallest period whose bin of this slot is flagged (F1); a bin that an existing entry already covers
 * creates nothing. Caller holds the lock. */
static void sp_flag_check(uint32_t slot)
{
  if (g_sp.all.n < NR_SLOTPHASE_MIN_ALL) return;
  const double cell = (double)g_sp.all.ok / g_sp.all.n;
  for (int pi = 0; pi < NR_SLOTPHASE_NPER; pi++) {
    const uint32_t ph = slot % kPer[pi];
    const sp_cnt_t *c = &g_sp.bin[kBase[pi] + ph];
    if (c->n < NR_SLOTPHASE_MIN_N || !(wilson(c->ok, c->n, 1) < 0.5 * cell)) continue;
    for (int i = 0; i < SP_MAX_ENT; i++) {
      const sp_ent_t *e = &g_sp.ent[i];
      if (e->used && kPer[pi] % kPer[e->pi] == 0 && ph % kPer[e->pi] == e->phase) return; /* covered */
    }
    for (int i = 0; i < SP_MAX_ENT; i++) {
      if (g_sp.ent[i].used) continue;
      sp_ent_init(&g_sp.ent[i], pi, (int)ph);
      g_sp.ent[i].v[0] = *c; /* what the phase did without any hypothesis */
      g_sp.ent[i].est.fail = (nr_spr_tab_t *)calloc(1, sizeof(nr_spr_tab_t));
      if (!g_sp.ent[i].est.fail) g_sp.ent[i].est.state = NR_SP_EST_NEG; /* no memory: the other variants only */
      atomic_store_explicit(&g_sp.open, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_sp_nent, 1, memory_order_relaxed);
      SP_LOG("SENSING: SLOTPHASE flag P/phase=%u/%u strong CRC %u/%u (cell %u/%u): fails far below the cell; testing provisional CSI-RS rate matching\n",
             kPer[pi], ph, c->ok, c->n, g_sp.all.ok, g_sp.all.n);
      return;
    }
    return;
  }
}


/* sp_update_open: whether residual accumulation can still be wanted (caller holds the lock) */
static void sp_update_open(void)
{
  int open = g_sp.ref_n < NR_SLOTPHASE_REF_TARGET;
  for (int i = 0; i < SP_MAX_ENT; i++)
    if (g_sp.ent[i].used && g_sp.ent[i].state == NR_SP_EXPLORE && g_sp.ent[i].est.state == NR_SP_EST_COLLECT && g_sp.ent[i].est.fail) open = 1;
  atomic_store_explicit(&g_sp.open, open && atomic_load_explicit(&g_sp_nent, memory_order_relaxed) > 0, memory_order_relaxed);
}
static void sp_log_est(const sp_ent_t *e, const nr_spr_result_t *r, const fapi_nr_dl_config_csirs_pdu_rel15_t *pat, int npat)
{
  SP_LOG("SENSING: SLOTPHASE resid P/phase=%d/%u FOUND fail=%u ref=%u seeds=%d bins=%d z>=%.2f (null %.2f, hysteresis %.2f) excess>=%.3f patterns=%d rb=%d:%d full-symbols=0x%x mod4-left=%d undersampled=0x%x\n",
         kPer[e->pi], e->phase, e->est.fail->grants, g_sp.ref_n, r->nseed, r->nsig, r->zthr, r->znull, r->zthr2, r->otsu, npat, r->rb_lo, r->rb_hi - r->rb_lo, r->sym_full,
         r->mod4_left, r->undersampled);
  for (int q = 0; q < npat; q++)
    SP_LOG("SENSING: SLOTPHASE resid P/phase=%d/%u pattern %d sym=0x%x sc=0x%x dens=%u rb=%u:%u\n", kPer[e->pi], e->phase, q, nr_re_pattern_sym(&pat[q]),
           nr_re_pattern_sc(&pat[q]), pat[q].freq_density, pat[q].start_rb, pat[q].nr_of_rbs);
}
/* sp_est_look: the sequential look of a collecting phase (caller holds the lock). Looks at kLook[] FAIL grants once the reference is large enough; the first look that finds foreign
 * REs waits for the FAIL sample to double (or the last look) and finalises with that estimate; the last look without a finding closes the phase as NEG. */
static void sp_est_look(sp_ent_t *e)
{
  sp_est_t *x = &e->est;
  if (g_sp.ref_n < NR_SLOTPHASE_REF_MIN || !g_sp.ref[0] || !g_sp.ref[1] || g_sp.ref[0]->grants < 8 || g_sp.ref[1]->grants < 8) return;
  if (x->look >= NR_SLOTPHASE_EST_LOOKS || x->fail->grants < kLook[x->look]) return;
  while (x->look < NR_SLOTPHASE_EST_LOOKS && kLook[x->look] <= x->fail->grants) x->look++;
  nr_spr_result_t r;
  const int np = nr_spr_estimate(x->fail, g_sp.ref[0], g_sp.ref[1], NR_SLOTPHASE_EST_ALPHA / NR_SLOTPHASE_EST_LOOKS, &r);
  const bool last = x->look >= NR_SLOTPHASE_EST_LOOKS;
  if (np > 0) {
    memcpy(x->cand, r.pat, sizeof(x->cand));
    x->cand_npat = (uint8_t)np;
    x->have_cand = 1;
    if (!x->pos_n) x->pos_n = x->fail->grants;
    if ((x->fail->grants >= 2 * x->pos_n && !r.undersampled) || last) { /* wait while a symbol is still covered by too few grants to be judged */
      memcpy(x->pat, x->cand, sizeof(x->pat));
      x->npat = x->cand_npat;
      x->state = NR_SP_EST_POS;
      sp_log_est(e, &r, x->pat, x->npat);
    }
  } else if (x->have_cand && ((x->fail->grants >= 2 * x->pos_n && !r.undersampled) || last)) {
    memcpy(x->pat, x->cand, sizeof(x->pat));
    x->npat = x->cand_npat;
    x->state = NR_SP_EST_POS;
    SP_LOG("SENSING: SLOTPHASE resid P/phase=%d/%u FOUND (earlier look) fail=%u ref=%u patterns=%d\n", kPer[e->pi], e->phase, x->fail->grants, g_sp.ref_n, x->npat);
    for (int q = 0; q < x->npat; q++)
      SP_LOG("SENSING: SLOTPHASE resid P/phase=%d/%u pattern %d sym=0x%x sc=0x%x dens=%u rb=%u:%u\n", kPer[e->pi], e->phase, q, nr_re_pattern_sym(&x->pat[q]),
             nr_re_pattern_sc(&x->pat[q]), x->pat[q].freq_density, x->pat[q].start_rb, x->pat[q].nr_of_rbs);
  } else if (last) {
    x->state = NR_SP_EST_NEG;
    SP_LOG("SENSING: SLOTPHASE resid P/phase=%d/%u none: no foreign REs in %u failed grants vs reference %u (z cut %.2f, null %.2f, full-symbols=0x%x); trying the other variants\n",
           kPer[e->pi], e->phase, x->fail->grants, g_sp.ref_n, r.zthr, r.znull, r.sym_full);
  }
  if (x->state != NR_SP_EST_COLLECT) sp_update_open();
}
static void sp_ref_add(const float (*re)[NR_SPR_N], const uint32_t (*rn)[NR_SPR_N])
{
  if (g_sp.ref_n >= NR_SLOTPHASE_REF_TARGET) return;
  const int h = (int)(g_sp.ref_n & 1);
  if (!g_sp.ref[h]) g_sp.ref[h] = (nr_spr_tab_t *)calloc(1, sizeof(nr_spr_tab_t));
  if (!g_sp.ref[h]) return;
  g_sp.capable = true;
  if (nr_spr_add(g_sp.ref[h], re, rn)) g_sp.ref_n++;
  if (g_sp.ref_n >= NR_SLOTPHASE_REF_TARGET) sp_update_open();
}

bool nr_slotphase_resid_want(uint32_t token)
{
  if (!atomic_load_explicit(&g_sp.open, memory_order_relaxed)) return false;
  pthread_mutex_lock(&g_sp_mu);
  bool w = false;
  const unsigned i = token ? token % SP_TAGS : 0;
  if (token && g_sp.tag[i].token == token && g_sp.tag[i].ent >= 1 && g_sp.tag[i].ent <= SP_MAX_ENT) {
    const sp_ent_t *e = &g_sp.ent[g_sp.tag[i].ent - 1];
    w = e->used && g_sp.tag[i].var == 0 && e->state == NR_SP_EXPLORE && e->est.state == NR_SP_EST_COLLECT && e->est.fail;
  } else
    w = g_sp.ref_n < NR_SLOTPHASE_REF_TARGET;
  pthread_mutex_unlock(&g_sp_mu);
  return w;
}
int nr_slotphase_est_patterns(uint32_t slot, fapi_nr_dl_config_csirs_pdu_rel15_t *out, int max)
{
  int n = 0;
  pthread_mutex_lock(&g_sp_mu);
  const int i = sp_find(slot);
  if (i >= 0 && g_sp.ent[i].est.state == NR_SP_EST_POS)
    for (; n < g_sp.ent[i].est.npat && n < max; n++) out[n] = g_sp.ent[i].est.pat[n];
  pthread_mutex_unlock(&g_sp_mu);
  return n;
}
int nr_slotphase_est_state(uint32_t slot, uint32_t *fail_grants, uint32_t *ref_grants)
{
  pthread_mutex_lock(&g_sp_mu);
  const int i = sp_find(slot);
  int st = -1;
  if (i >= 0) {
    st = g_sp.ent[i].est.state;
    if (fail_grants) *fail_grants = g_sp.ent[i].est.fail ? g_sp.ent[i].est.fail->grants : 0;
  }
  if (ref_grants) *ref_grants = g_sp.ref_n;
  pthread_mutex_unlock(&g_sp_mu);
  return st;
}

void nr_slotphase_note(uint32_t slot, uint32_t gen, uint32_t token, bool crc_ok)
{
  nr_slotphase_note_ex(slot, gen, token, crc_ok, NULL, NULL);
}

void nr_slotphase_note_ex(uint32_t slot, uint32_t gen, uint32_t token, bool crc_ok, const float (*rs_e)[NR_SPR_N], const uint32_t (*rs_n)[NR_SPR_N])
{
  pthread_mutex_lock(&g_sp_mu);
  sp_sync(gen);
  int ent = 0, var = 0;
  if (token) {
    const unsigned i = token % SP_TAGS;
    if (g_sp.tag[i].token == token) {
      ent = g_sp.tag[i].ent;
      var = g_sp.tag[i].var;
      g_sp.tag[i].token = 0;
    }
  }
  sp_ent_t *e = ent >= 1 && ent <= SP_MAX_ENT && g_sp.ent[ent - 1].used ? &g_sp.ent[ent - 1] : NULL;
  if (!e) var = 0;
  if (rs_e && rs_n && atomic_load_explicit(&g_sp_nent, memory_order_relaxed) > 0) { /* RESID: failed control grant of a collecting phase = FAIL sample; passed grant = reference */
    if (crc_ok)
      sp_ref_add(rs_e, rs_n);
    else if (e && var == 0 && e->state == NR_SP_EXPLORE && e->est.state == NR_SP_EST_COLLECT && e->est.fail) {
      g_sp.capable = true;
      if (nr_spr_add(e->est.fail, rs_e, rs_n)) sp_est_look(e);
    }
  }
  if (var == 0) { /* decoded as always: the F1 evidence */
    for (int pi = 0; pi < NR_SLOTPHASE_NPER; pi++) {
      sp_cnt_t *c = &g_sp.bin[kBase[pi] + slot % kPer[pi]];
      c->n++;
      if (crc_ok) c->ok++;
    }
    g_sp.all.n++;
    if (crc_ok) g_sp.all.ok++;
    if (e) {
      e->v[0].n++;
      if (crc_ok) e->v[0].ok++;
    } else
      sp_flag_check(slot);
  } else {
    sp_cnt_t *c = &e->v[var];
    c->n++;
    if (crc_ok) c->ok++;
    const sp_cnt_t ctl = e->v[0];
    const int per = kPer[e->pi];
    if (e->state == NR_SP_EXPLORE) {
      if (c->ok >= NR_SLOTPHASE_ADOPT_OK && wilson(c->ok, c->n, 0) > wilson(ctl.ok, ctl.n, 1)) {
        e->state = NR_SP_ADOPTED;
        e->adopted = (uint8_t)var;
        char db[96];
        SP_LOG("SENSING: SLOTPHASE adopt P/phase=%d/%u variant=%s %s CRC %u/%u vs none %u/%u: applied at this phase for all RNTIs\n",
               per, e->phase, kind_name(e->vkind[var]), sp_desc(e, var, db, sizeof(db)), c->ok, c->n, ctl.ok, ctl.n);
      } else if (!e->dead[var] && ((c->n >= NR_SLOTPHASE_DROP_N && c->ok < NR_SLOTPHASE_ADOPT_OK) || c->n >= NR_SLOTPHASE_MAX_N
                                   || (c->n >= 8 && wilson(c->ok, c->n, 1) < wilson(ctl.ok, ctl.n, 0)))) {
        e->dead[var] = true;
        char db[96];
        SP_LOG("SENSING: SLOTPHASE drop P/phase=%d/%u variant=%s %s CRC %u/%u vs none %u/%u\n", per, e->phase, kind_name(e->vkind[var]),
               sp_desc(e, var, db, sizeof(db)), c->ok, c->n, ctl.ok, ctl.n);
        bool all_dead = e->others_offered || e->est.state != NR_SP_EST_POS;
        for (int j = 1; j < SP_NVAR; j++)
          if (e->offered[j] && !e->dead[j]) all_dead = false;
        if (all_dead)
          SP_LOG("SENSING: SLOTPHASE exhausted P/phase=%d/%u: no provisional rate-match set / end trim explains it (reopens when the CSI-RS map changes)\n", per, e->phase);
      }
    } else if (var == e->adopted && c->n >= 32) { /* health of the adopted variant on its recent grants (halved window) */
      if (g_sp.all.n >= NR_SLOTPHASE_MIN_ALL && wilson(c->ok, c->n, 1) < 0.5 * ((double)g_sp.all.ok / g_sp.all.n)) {
        e->state = NR_SP_EXPLORE;
        e->dead[var] = true;
        char db[96];
        SP_LOG("SENSING: SLOTPHASE revoke P/phase=%d/%u variant=%s %s CRC %u/%u (cell %u/%u)\n", per, e->phase, kind_name(e->vkind[var]),
               sp_desc(e, var, db, sizeof(db)), c->ok, c->n, g_sp.all.ok, g_sp.all.n);
      }
      c->n /= 2;
      c->ok /= 2;
    }
  }
  pthread_mutex_unlock(&g_sp_mu);
}
