/* CSI-RS / TRS PDSCH rate-matching candidate family and its CRC arbitration (see nr_csirs_rm_arb.h). Pure functions plus per-cell vote tables. */
#include "nr_csirs_rm_arb.h"
#include "nr_re_pattern.h"
#include "common/utils/nr_cell.h"
#include <pthread.h>
#include <string.h>

typedef fapi_nr_dl_config_csirs_pdu_rel15_t pdu_t;

bool nr_rm_same_geometry(const pdu_t *a, const pdu_t *b)
{
  return a->row == b->row && a->freq_domain == b->freq_domain && a->symb_l0 == b->symb_l0 && a->symb_l1 == b->symb_l1
         && a->cdm_type == b->cdm_type && a->freq_density == b->freq_density && a->start_rb == b->start_rb
         && a->nr_of_rbs == b->nr_of_rbs;
}
bool nr_rm_dmrs_clash(const pdu_t *c, uint16_t dmrs_mask)
{
  static const uint8_t num_l0[18] = {1, 1, 1, 1, 2, 1, 2, 2, 1, 2, 2, 2, 2, 2, 4, 2, 2, 4};
  if (c->row < 1 || c->row > 18)
    return true;
  for (int k = 0; k < num_l0[c->row - 1]; k++)
    if ((dmrs_mask >> (c->symb_l0 + k)) & 1)
      return true;
  if (c->row == 13 || c->row == 14 || c->row == 16 || c->row == 17)
    for (int k = 0; k < 2; k++)
      if ((dmrs_mask >> (c->symb_l1 + k)) & 1)
        return true;
  return false;
}
bool nr_rm_rb_overlap_half(const pdu_t *a, const pdu_t *b)
{
  const int lo = a->start_rb > b->start_rb ? a->start_rb : b->start_rb;
  const int hi = a->start_rb + a->nr_of_rbs < b->start_rb + b->nr_of_rbs ? a->start_rb + a->nr_of_rbs : b->start_rb + b->nr_of_rbs;
  const int m = a->nr_of_rbs < b->nr_of_rbs ? a->nr_of_rbs : b->nr_of_rbs;
  return hi - lo > 0 && 2 * (hi - lo) >= m;
}

/* The same RE set: a CSI-RS row ignores the scramblingID (it is a sequence, not a position), a generic RE pattern carries its symbol mask there. */
static bool re_equal(const pdu_t *a, const pdu_t *b)
{
  const bool ra = nr_re_pattern_is(a), rb = nr_re_pattern_is(b);
  return ra == rb && nr_rm_same_geometry(a, b) && (!ra || a->scramb_id == b->scramb_id);
}
static bool list_has(const pdu_t *l, int n, const pdu_t *x)
{
  for (int i = 0; i < n; i++)
    if (re_equal(&l[i], x))
      return true;
  return false;
}
static bool list_equal(const pdu_t *a, int na, const pdu_t *b, int nb)
{
  if (na != nb)
    return false;
  for (int i = 0; i < na; i++)
    if (!list_has(b, nb, &a[i]))
      return false;
  return true;
}

void nr_rm_select_own(const pdu_t *conf, int n_conf, const pdu_t *all, int n_all, uint16_t pop, uint16_t dmrs_mask, pdu_t *sel, int *nsel,
                      int max_sel, pdu_t *er, int *ner, int max_er)
{
  if (nsel) *nsel = 0;
  if (ner) *ner = 0;
  for (int i = 0; i < n_all; i++) {
    if (nr_rm_dmrs_clash(&all[i], dmrs_mask))
      continue;
    bool in = all[i].scramb_id == pop;
    for (int j = 0; j < n_conf && !in; j++)
      in = conf[j].scramb_id == pop && conf[j].csi_type != 2 && nr_rm_rb_overlap_half(&conf[j], &all[i]);
    pdu_t *list = in ? sel : er;
    int *nl = in ? nsel : ner;
    const int mx = in ? max_sel : max_er;
    if (!list || !nl || (!in && all[i].csi_type == 2))
      continue;
    bool dup = false;
    for (int j = 0; j < *nl && !dup; j++)
      dup = nr_rm_same_geometry(&list[j], &all[i]);
    if (!dup && *nl < mx)
      list[(*nl)++] = all[i];
  }
}

/* One resource: same pattern AND scramblingID (two cells' TRS are different resources even when their RB thirds touch). */
static bool same_resource(const pdu_t *a, const pdu_t *b)
{
  return a->csi_type == b->csi_type && a->row == b->row && a->freq_domain == b->freq_domain && a->symb_l0 == b->symb_l0
         && a->symb_l1 == b->symb_l1 && a->cdm_type == b->cdm_type && a->freq_density == b->freq_density && a->scramb_id == b->scramb_id;
}
int nr_rm_consolidate(pdu_t *p, int n, uint8_t *merged)
{
  if (merged)
    for (int i = 0; i < n; i++)
      merged[i] = 1;
  for (bool again = true; again;) {
    again = false;
    for (int i = 0; i < n && !again; i++)
      for (int j = i + 1; j < n && !again; j++) {
        if (!same_resource(&p[i], &p[j]))
          continue;
        const int s1 = p[i].start_rb, e1 = s1 + p[i].nr_of_rbs, s2 = p[j].start_rb, e2 = s2 + p[j].nr_of_rbs;
        const int gap = (s1 > s2 ? s1 : s2) - (e1 < e2 ? e1 : e2);
        const int w = p[i].nr_of_rbs < p[j].nr_of_rbs ? p[i].nr_of_rbs : p[j].nr_of_rbs;
        if (gap > 0 && gap >= w)
          continue;
        p[i].start_rb = (uint16_t)(s1 < s2 ? s1 : s2);
        p[i].nr_of_rbs = (uint16_t)((e1 > e2 ? e1 : e2) - p[i].start_rb);
        if (merged)
          merged[i] = (uint8_t)(merged[i] + merged[j]);
        for (int k = j + 1; k < n; k++) {
          p[k - 1] = p[k];
          if (merged)
            merged[k - 1] = merged[k];
        }
        n--;
        again = true;
      }
  }
  return n;
}

static uint32_t res_hash(const pdu_t *c)
{
  uint64_t h = 1469598103934665603ull;
  const uint32_t key[] = {c->csi_type, c->row, c->freq_domain, c->symb_l0, c->symb_l1, c->cdm_type, c->freq_density, c->scramb_id, c->start_rb,
                          c->nr_of_rbs};
  for (unsigned i = 0; i < sizeof(key) / sizeof(key[0]); i++) {
    h ^= key[i];
    h *= 1099511628211ull;
  }
  return (uint32_t)(h ^ (h >> 32));
}

static void cand_add(nr_rm_cands_t *cs, uint8_t kind, const pdu_t *sel, int n_sel, const pdu_t *er, int n_er)
{
  if (n_sel > NR_RM_MAXL) n_sel = NR_RM_MAXL;
  if (n_er > NR_RM_MAXL) n_er = NR_RM_MAXL;
  for (int i = 0; i < cs->n; i++)
    if (list_equal(cs->c[i].sel, cs->c[i].n_sel, sel, n_sel) && list_equal(cs->c[i].er, cs->c[i].n_er, er, n_er))
      return;
  nr_rm_cand_t *c = &cs->c[cs->n++];
  c->kind = kind;
  c->n_sel = n_sel;
  c->n_er = n_er;
  if (n_sel) memcpy(c->sel, sel, sizeof(pdu_t) * (size_t)n_sel);
  if (n_er) memcpy(c->er, er, sizeof(pdu_t) * (size_t)n_er);
}

void nr_rm_build(const pdu_t *conf, int n_conf, const pdu_t *all, int n_all, const pdu_t *foot, int n_foot, uint16_t pop, uint16_t dmrs_mask,
                 uint16_t bwp_start, uint16_t bwp_size, nr_rm_cands_t *cs)
{
  static __thread pdu_t pool[2 * NR_RM_MAXL], wide[2 * NR_RM_MAXL], w2[2 * NR_RM_MAXL], fp[NR_RM_MAXL];
  uint8_t pm[2 * NR_RM_MAXL], fm[NR_RM_MAXL];
  memset(cs, 0, sizeof(*cs));
  nr_rm_cand_t *c0 = &cs->c[0];
  nr_rm_select_own(conf, n_conf, all, n_all, pop, dmrs_mask, c0->sel, &c0->n_sel, NR_RM_MAXL, c0->er, &c0->n_er, NR_RM_MAXL);
  c0->kind = NR_RM_OWN;
  cs->n = 1;
  if (n_all <= 0 && n_foot <= 0)
    return;
  /* raw slot class: order independent over everything the slot offers */
  uint32_t cls = 0;
  for (int i = 0; i < n_all; i++)
    cls += res_hash(&all[i]);
  for (int i = 0; i < n_foot; i++)
    cls += res_hash(&foot[i]) * 3u;
  /* ALL_CONF: every non-clashing resource, fragments merged */
  int np = 0;
  for (int i = 0; i < n_all && np < NR_RM_MAXL; i++)
    if (!nr_rm_dmrs_clash(&all[i], dmrs_mask) && !list_has(pool, np, &all[i]))
      pool[np++] = all[i];
  np = nr_rm_consolidate(pool, np, pm);
  cand_add(cs, NR_RM_ALL_CONF, pool, np, NULL, 0);
  /* ALL_FOOT: plus the footprints (their DM-RS symbols are DM-RS energy, not a resource) */
  int nf = 0;
  for (int i = 0; i < n_foot && nf < NR_RM_MAXL; i++) {
    pdu_t c = foot[i];
    if (nr_re_pattern_is(&c)) {
      c.scramb_id = (uint16_t)(nr_re_pattern_sym(&c) & ~(uint32_t)dmrs_mask & 0x3fffu);
      if (c.scramb_id == 0)
        continue;
    } else if (nr_rm_dmrs_clash(&c, dmrs_mask))
      continue;
    fp[nf++] = c;
  }
  nf = nr_rm_consolidate(fp, nf, fm);
  uint8_t wm[2 * NR_RM_MAXL];
  int nt = 0;
  for (; nt < np; nt++) {
    wide[nt] = pool[nt];
    wm[nt] = pm[nt];
  }
  for (int i = 0; i < nf && nt < 2 * NR_RM_MAXL; i++, nt++) {
    wide[nt] = fp[i];
    wm[nt] = fm[i];
  }
  cand_add(cs, NR_RM_ALL_FOOT, wide, nt, NULL, 0);
  /* ALL_WIDE: a resource detected in >= 2 fragments is one wideband resource */
  bool any = false;
  for (int i = 0; i < nt; i++) {
    w2[i] = wide[i];
    if (wm[i] >= 2 && bwp_size) {
      w2[i].start_rb = bwp_start;
      w2[i].nr_of_rbs = bwp_size;
      any = true;
    }
  }
  if (any)
    cand_add(cs, NR_RM_ALL_WIDE, w2, nt, NULL, 0);
  cand_add(cs, NR_RM_NONE, NULL, 0, NULL, 0);
  cs->cls = cs->n > 1 ? (cls | 1u) : 0;
}

/* ---- per-cell votes ------------------------------------------------------------------------------------------------------------------------ */
#define RM_TAB 256
typedef struct {
  bool used;
  uint8_t rm, zp, streak;
  int8_t adopted;
  uint16_t rnti;
  uint32_t cls;
  uint16_t votes[NR_RM_NKIND];
  uint64_t touched;
} rm_ent_t;
typedef struct {
  rm_ent_t e[RM_TAB];
  uint64_t clock, tries, passes, adopts;
} rm_tab_t;
NR_CELL_STATIC(rm_tab_t, g_rm);
#define g_rm NR_CELL_REF(g_rm)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_rm_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_rm_lock NR_CELL_REF(g_rm_lock)

static rm_ent_t *ent_find(uint16_t rnti, uint32_t cls, uint8_t rm, uint8_t zp, bool create)
{
  rm_tab_t *t = &g_rm;
  rm_ent_t *free_e = NULL, *old = &t->e[0];
  for (int i = 0; i < RM_TAB; i++) {
    rm_ent_t *e = &t->e[i];
    if (!e->used) {
      if (!free_e) free_e = e;
      continue;
    }
    if (e->rnti == rnti && e->cls == cls && e->rm == rm && e->zp == zp) {
      e->touched = ++t->clock;
      return e;
    }
    if (e->touched < old->touched) old = e;
  }
  if (!create)
    return NULL;
  rm_ent_t *e = free_e ? free_e : old;
  memset(e, 0, sizeof(*e));
  e->used = true;
  e->rnti = rnti;
  e->cls = cls;
  e->rm = rm;
  e->zp = zp;
  e->adopted = -1;
  e->touched = ++t->clock;
  return e;
}
static int idx_of_kind(const nr_rm_cands_t *cs, int kind)
{
  for (int i = 0; i < cs->n; i++)
    if (cs->c[i].kind == kind)
      return i;
  return -1;
}
static void adopt_eval(rm_ent_t *e, int thr)
{
  int best = 0;
  for (int k = 1; k < NR_RM_NKIND; k++)
    if (e->votes[k] > e->votes[best]) best = k;
  if (e->votes[best] < thr || best == e->adopted)
    return;
  for (int k = 0; k < NR_RM_NKIND; k++)
    if (k != best && e->votes[k] >= e->votes[best])
      return;
  e->adopted = (int8_t)best;
  g_rm.adopts++;
}

int nr_rm_arb_first(uint16_t rnti, uint8_t rm, uint8_t zp, const nr_rm_cands_t *cs)
{
  if (!cs || cs->n <= 1 || !cs->cls)
    return 0;
  pthread_mutex_lock(&g_rm_lock);
  int k = -1;
  const rm_ent_t *e = ent_find(rnti, cs->cls, rm, zp, false);
  if (e && e->adopted >= 0)
    k = idx_of_kind(cs, e->adopted);
  if (k < 0 && (e = ent_find(0, cs->cls, rm, zp, false)) != NULL && e->adopted >= 0)
    k = idx_of_kind(cs, e->adopted);
  pthread_mutex_unlock(&g_rm_lock);
  return k < 0 ? 0 : k;
}
int nr_rm_arb_next(uint16_t rnti, uint8_t rm, uint8_t zp, const nr_rm_cands_t *cs, unsigned tried)
{
  (void)rnti;
  int best = -1, bv = -1;
  pthread_mutex_lock(&g_rm_lock);
  const rm_ent_t *cell = ent_find(0, cs->cls, rm, zp, false);
  for (int i = 0; i < cs->n; i++) {
    if ((tried >> i) & 1u)
      continue;
    const int v = cell ? cell->votes[cs->c[i].kind] : 0;
    if (v > bv) {
      bv = v;
      best = i;
    }
  }
  pthread_mutex_unlock(&g_rm_lock);
  return best;
}
static void note_one(rm_ent_t *e, int kind, int from_kind, bool ok, bool strong, int thr)
{
  if (ok) {
    if (e->votes[kind] < UINT16_MAX) e->votes[kind]++;
    if (from_kind >= 0 && e->votes[from_kind] > 0) e->votes[from_kind]--;
    if (kind == e->adopted) e->streak = 0;
    adopt_eval(e, thr);
  } else if (strong && kind == e->adopted) {
    if (++e->streak >= NR_RM_RETEST_STREAK) {
      e->votes[kind] = 0;
      e->adopted = -1;
      e->streak = 0;
    }
  }
}
void nr_rm_arb_note(uint16_t rnti, uint8_t rm, uint8_t zp, const nr_rm_cands_t *cs, int k, int from, bool ok, bool strong)
{
  if (!cs || cs->n <= 1 || !cs->cls || k < 0 || k >= cs->n)
    return;
  const int kind = cs->c[k].kind, fk = from >= 0 && from < cs->n ? cs->c[from].kind : -1;
  pthread_mutex_lock(&g_rm_lock);
  if (from >= 0) {
    g_rm.tries++;
    if (ok) g_rm.passes++;
  }
  note_one(ent_find(rnti, cs->cls, rm, zp, true), kind, fk, ok, strong, NR_RM_ADOPT_RNTI);
  note_one(ent_find(0, cs->cls, rm, zp, true), kind, fk, ok, strong, NR_RM_ADOPT_CELL);
  pthread_mutex_unlock(&g_rm_lock);
}
int nr_rm_arb_adopted(uint16_t rnti, uint32_t cls, uint8_t rm, uint8_t zp)
{
  pthread_mutex_lock(&g_rm_lock);
  const rm_ent_t *e = ent_find(rnti, cls, rm, zp, false);
  const int a = e ? e->adopted : -1;
  pthread_mutex_unlock(&g_rm_lock);
  return a;
}
void nr_rm_arb_stats(uint64_t *tries, uint64_t *passes, uint64_t *adopts)
{
  pthread_mutex_lock(&g_rm_lock);
  if (tries) *tries = g_rm.tries;
  if (passes) *passes = g_rm.passes;
  if (adopts) *adopts = g_rm.adopts;
  pthread_mutex_unlock(&g_rm_lock);
}
void nr_rm_arb_reset(void)
{
  pthread_mutex_lock(&g_rm_lock);
  memset(&g_rm, 0, sizeof(g_rm));
  pthread_mutex_unlock(&g_rm_lock);
}
