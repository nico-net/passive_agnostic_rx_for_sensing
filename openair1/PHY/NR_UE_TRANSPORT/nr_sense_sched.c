/* Sensing-mode unit scheduler decision (spec 2026-11-03-two-modes-design.md section 5, plan Task S6). Pure: no threads, no I/O, no allocation.
 * The caller (S7) snapshots the units, calls nr_sense_sched_decide() every SENSE_DECIDE_NS and applies st->enabled with nr_sense_unit_set_enabled(). */
#include "nr_sense_sched.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAXC (NR_SENSE_CELLS * 256)
#define INCUMBENT (1.0 / (1.0 - SENSE_HYST)) /* an enabled unit is ranked x1.25: a challenger must beat it by more than the margin (v_inc < (1-HYST) x v_challenger) */

typedef struct {
  int cell, slot;
  const nr_sense_unit_view_t *v;
  double val, rank, snr, dem[NR_RES_N];
  bool elig, cold, enabled, want, removed, viol, holdoff;
} cand_t;

static bool uses(nr_unit_src_t s, int r)
{
  switch (r) {
    case NR_RES_POOL: case NR_RES_SENSE_RING: return true;
    case NR_RES_GPU: case NR_RES_QUEUE_DL: return s == NR_UNIT_PDSCH_DATA;
    case NR_RES_QUEUE_UL: return s == NR_UNIT_SRS;
  }
  return false;
}
static bool is_queue(int r) { return r == NR_RES_QUEUE_DL || r == NR_RES_QUEUE_UL; }
/* the group a violation is charged to: the unit's own queue (per cell), else the shared pool (all cells) */
static int group_of(nr_unit_src_t s) { return s == NR_UNIT_PDSCH_DATA ? NR_RES_QUEUE_DL : s == NR_UNIT_SRS ? NR_RES_QUEUE_UL : NR_RES_POOL; }

bool nr_sense_unit_eligible(const nr_sense_unit_view_t *v)
{
  /* wb_rate_hz is a window quantity (messages with usable PRBs >= min_prb), so the size of the latest message does not matter */
  return v->wb_rate_hz > 0.f && (v->n_cfr == 0 || v->valid_ratio >= SENSE_ELIG_VALID_MIN);
}

double nr_sense_age_boost(bool eligible, uint64_t last_valid_ns, uint64_t now_ns)
{
  if (!eligible)
    return 1.0;
  const double f = now_ns > last_valid_ns ? (double)(now_ns - last_valid_ns) / (double)SENSE_AGE_FULL_NS : 0.0;
  return 1.0 + (SENSE_AGE_CAP - 1.0) * (f < 1.0 ? f : 1.0);
}

static int cmp_rank(const void *a, const void *b)
{
  const cand_t *x = a, *y = b;
  if (x->rank != y->rank) return x->rank > y->rank ? -1 : 1;
  if (x->snr != y->snr) return x->snr > y->snr ? -1 : 1;
  if (x->cell != y->cell) return x->cell - y->cell;
  if (x->v->rnti != y->v->rnti) return x->v->rnti < y->v->rnti ? -1 : 1;
  return (int)x->v->src - (int)y->v->src;
}

static bool fits(const double *use, const double *dem, const double *budget, nr_unit_src_t src, int *over)
{
  bool ok = true;
  double worst = 0;
  for (int r = 0; r < NR_RES_N; r++) {
    if (budget[r] == -1.0) continue; /* resource absent */
    if (budget[r] == -2.0) { /* invalid cap: nothing that uses the resource is admitted */
      if (uses(src, r)) { ok = false; if (over) { *over = r; worst = 1e19; } }
      continue;
    }
    const double next = is_queue(r) ? (dem[r] > use[r] ? dem[r] : use[r]) : use[r] + dem[r];
    if (next > budget[r] * (1.0 + 1e-9)) {
      ok = false;
      const double o = budget[r] > 0 ? next / budget[r] : 1e18;
      if (over && o > worst) { worst = o; *over = r; }
    }
  }
  return ok;
}
static void add_use(double *use, const double *dem)
{
  for (int r = 0; r < NR_RES_N; r++)
    use[r] = is_queue(r) ? (dem[r] > use[r] ? dem[r] : use[r]) : use[r] + dem[r];
}

/* slot of an RNTI in a cell: slot 0 is rnti 0 (the per-cell SRS unit); -1 when the state is full */
static int slot_of(nr_sense_sched_state_t *st, int c, uint16_t rnti)
{
  if (rnti == 0) return 0;
  int freeslot = -1;
  for (int i = 1; i < NR_SENSE_SLOTS; i++) {
    if (st->rnti[c][i] == rnti) return i;
    if (st->rnti[c][i] == 0 && freeslot < 0) freeslot = i;
  }
  if (freeslot >= 0) st->rnti[c][freeslot] = rnti;
  return freeslot;
}

void nr_sense_sched_decide(const nr_sense_sched_in_t *in, nr_sense_sched_state_t *st, nr_sense_sched_out_t *out)
{
  memset(out, 0, sizeof *out);
  cand_t C[MAXC];
  int n = 0;
  bool present[NR_SENSE_CELLS][NR_SENSE_SLOTS][NR_UNIT_SRC_N];
  memset(present, 0, sizeof present);
  const int nc = in->n_cells < NR_SENSE_CELLS ? in->n_cells : NR_SENSE_CELLS;
  double budget[NR_RES_N];
  for (int r = 0; r < NR_RES_N; r++) {
    const double av = (double)in->res[r].cap_ns - (double)in->res[r].base_p99_ns;
    if (r == NR_RES_GPU && !(in->res[r].cap_ns > 0.f)) budget[r] = -1.0; /* absent */
    else if (!(in->res[r].cap_ns > 0.f)) { budget[r] = -2.0; out->bad_cap++; } /* invalid */
    else budget[r] = av > 0 ? av * (1.0 - SENSE_HEADROOM) : 0.0;
  }

  for (int c = 0; c < nc; c++) {
    /* PCI level: last valid wideband CFR of any unit of the cell, kept in the state because units come and go */
    uint64_t mx = 0, first = 0;
    bool pci_elig = false;
    for (int i = 0; i < in->n_units[c]; i++) {
      const nr_sense_unit_view_t *v = &in->units[c][i];
      if (v->last_valid_wb_ns > mx) mx = v->last_valid_wb_ns;
      if (v->first_ns && (!first || v->first_ns < first)) first = v->first_ns;
      pci_elig = pci_elig || nr_sense_unit_eligible(v);
    }
    if (mx > st->last_pci_valid_wb_ns[c]) st->last_pci_valid_wb_ns[c] = mx;
    if (!st->last_pci_valid_wb_ns[c]) st->last_pci_valid_wb_ns[c] = first ? first : in->now_ns; /* never valid: age runs from the first sight */
    if (pci_elig && st->pci_seen[c] && !st->pci_was_elig[c]) st->pci_elig_since[c] = in->now_ns; /* age restarts when it becomes eligible again */
    st->pci_seen[c] = true; st->pci_was_elig[c] = pci_elig;
    const uint64_t pref = st->last_pci_valid_wb_ns[c] > st->pci_elig_since[c] ? st->last_pci_valid_wb_ns[c] : st->pci_elig_since[c];
    const double pci_boost = nr_sense_age_boost(pci_elig, pref, in->now_ns);

    for (int i = 0; i < in->n_units[c] && n < MAXC; i++) {
      const nr_sense_unit_view_t *v = &in->units[c][i];
      const int sl = slot_of(st, c, v->rnti);
      if (sl < 0 || present[c][sl][v->src]) continue; /* full state (never with the 64-row table) or a duplicate row */
      present[c][sl][v->src] = true;
      cand_t *k = &C[n++];
      memset(k, 0, sizeof *k);
      k->cell = c; k->slot = sl; k->v = v;
      k->enabled = v->enabled;
      if (st->enabled[c][sl][v->src] != v->enabled) st->streak[c][sl][v->src] = 0; /* the caller has not applied (or has overridden) the last decision */
      st->enabled[c][sl][v->src] = v->enabled;
      k->elig = nr_sense_unit_eligible(v);
      if (k->elig && st->known[c][sl][v->src] && !st->was_elig[c][sl][v->src]) st->elig_since[c][sl][v->src] = in->now_ns;
      st->was_elig[c][sl][v->src] = k->elig;
      if (k->elig) {
        /* a wideband message had >= min_prb usable PRBs by construction; est_usable_prbs (latest) only raises the size */
        double prbs = v->est_usable_prbs > in->min_prb ? v->est_usable_prbs : in->min_prb;
        if (prbs < 1) prbs = 1;
        const double ratio = v->n_cfr ? v->valid_ratio : SENSE_ELIG_VALID_MIN; /* unknown ratio: the floor */
        uint64_t ref = v->last_valid_wb_ns ? v->last_valid_wb_ns : v->first_ns;
        if (st->elig_since[c][sl][v->src] > ref) ref = st->elig_since[c][sl][v->src];
        double boost = nr_sense_age_boost(true, ref, in->now_ns) * pci_boost;
        if (boost > SENSE_AGE_CAP) boost = SENSE_AGE_CAP; /* the unit and PCI boosts together are capped */
        k->val = (double)v->wb_rate_hz * ratio * prbs * boost;
      }
      if (st->holdoff[c][sl][v->src]) { k->holdoff = true; st->holdoff[c][sl][v->src]--; }
      k->snr = v->snr_db;
      k->rank = k->val * (k->enabled ? INCUMBENT : 1.0);
      /* violations since the last decision; the counters restart from 0 when the row was re-created */
      uint32_t dv = v->violations;
      if (st->known[c][sl][v->src] && st->gen[c][sl][v->src] == v->gen && v->violations >= st->viol[c][sl][v->src]) dv = v->violations - st->viol[c][sl][v->src];
      st->known[c][sl][v->src] = true; st->gen[c][sl][v->src] = v->gen; st->viol[c][sl][v->src] = v->violations;
      k->viol = dv > 0 && k->enabled;
    }
  }
  /* release the state of rows that are gone */
  for (int c = 0; c < NR_SENSE_CELLS; c++)
    for (int sl = 0; sl < NR_SENSE_SLOTS; sl++) {
      bool any = false;
      for (int s = 0; s < NR_UNIT_SRC_N; s++) {
        if (present[c][sl][s]) { any = true; continue; }
        st->known[c][sl][s] = false; st->was_elig[c][sl][s] = false; st->elig_since[c][sl][s] = 0; st->holdoff[c][sl][s] = 0; st->enabled[c][sl][s] = false; st->streak[c][sl][s] = 0; st->gen[c][sl][s] = 0; st->viol[c][sl][s] = 0;
      }
      if (!any && sl > 0) st->rnti[c][sl] = 0;
    }

  /* demand: measured p99 of the unit; a unit with no history (disabled and idle in the window) takes its source's per-cell p99 (the largest of its units) */
  for (int i = 0; i < n; i++) {
    cand_t *k = &C[i];
    const nr_sense_unit_view_t *v = k->v;
    k->cold = !k->enabled && v->cpu_p99_ns <= 0.f && v->wait_p99_ns <= 0.f;
    double cpu = v->cpu_p99_ns, wait = v->wait_p99_ns;
    if (k->cold) {
      for (int j = 0; j < n; j++)
        if (C[j].cell == k->cell && C[j].v->src == v->src) {
          if (C[j].v->cpu_p99_ns > cpu) cpu = C[j].v->cpu_p99_ns;
          if (C[j].v->wait_p99_ns > wait) wait = C[j].v->wait_p99_ns;
        }
    }
    for (int r = 0; r < NR_RES_N; r++) k->dem[r] = !uses(v->src, r) ? 0.0 : is_queue(r) ? wait : cpu;
  }

  /* violations act at once: the lowest-value enabled unit of the violating unit's group goes now (one removal per group per decision) */
  bool grp_done[NR_SENSE_CELLS + 1][NR_RES_N];
  memset(grp_done, 0, sizeof grp_done);
  for (int i = 0; i < n; i++) {
    if (!C[i].viol) continue;
    const int g = group_of(C[i].v->src);
    const int gc = is_queue(g) ? C[i].cell : NR_SENSE_CELLS;
    if (grp_done[gc][g]) continue;
    grp_done[gc][g] = true;
    int lo = -1;
    for (int j = 0; j < n; j++) {
      if (!C[j].enabled || group_of(C[j].v->src) != g || (is_queue(g) && C[j].cell != C[i].cell)) continue;
      if (lo < 0 || C[j].val < C[lo].val || (C[j].val == C[lo].val && C[j].snr < C[lo].snr)) lo = j;
    }
    if (lo >= 0) {
      C[lo].removed = true;
      C[lo].enabled = false;
      st->enabled[C[lo].cell][C[lo].slot][C[lo].v->src] = false;
      st->streak[C[lo].cell][C[lo].slot][C[lo].v->src] = 0;
      st->holdoff[C[lo].cell][C[lo].slot][C[lo].v->src] = SENSE_VIOL_HOLDOFF;
      out->disables++;
    }
  }

  qsort(C, (size_t)n, sizeof C[0], cmp_rank);

  /* admission in rank order against every resource; at most one unit with no history per decision */
  double use[NR_RES_N] = {0};
  bool rejected = false, cold_taken = false;
  for (int i = 0; i < n; i++) {
    cand_t *k = &C[i];
    if (!k->elig || k->val <= 0 || k->removed) continue;
    if (k->cold && cold_taken) continue;
    int over = NR_RES_POOL;
    if (fits(use, k->dem, budget, k->v->src, &over)) {
      add_use(use, k->dem);
      k->want = true;
      if (!out->value_last_admitted || k->val < out->value_last_admitted) out->value_last_admitted = (float)k->val;
      if (k->cold) cold_taken = true;
    } else if (!rejected) {
      rejected = true;
      out->binding = (nr_res_id_t)over;
    }
  }
  if (!rejected) {
    double best = -1;
    for (int r = 0; r < NR_RES_N; r++)
      if (budget[r] > 0 && use[r] / budget[r] > best) { best = use[r] / budget[r]; out->binding = (nr_res_id_t)r; }
  }

  /* changes: a streak of SENSE_CONFIRM agreeing decisions; disables first (any number), then enables (at most SENSE_MAX_ENABLES, only if they fit
   * next to the units that stay on, so a pending disable never lets the set exceed the headroom) */
  double now_use[NR_RES_N] = {0};
  for (int i = 0; i < n; i++) {
    cand_t *k = &C[i];
    if (k->removed) continue;
    uint8_t *sk = &st->streak[k->cell][k->slot][k->v->src];
    if (k->want == k->enabled) { *sk = 0; continue; }
    if (*sk < 255) (*sk)++;
    if (!k->enabled || *sk < SENSE_CONFIRM) continue;
    k->enabled = false; *sk = 0; out->disables++;
    st->enabled[k->cell][k->slot][k->v->src] = false;
  }
  for (int i = 0; i < n; i++)
    if (C[i].enabled) add_use(now_use, C[i].dem);
  for (int i = 0; i < n; i++) {
    cand_t *k = &C[i];
    if (k->removed || k->enabled || !k->want || k->holdoff) continue;
    uint8_t *sk = &st->streak[k->cell][k->slot][k->v->src];
    if (*sk < SENSE_CONFIRM || out->enables >= SENSE_MAX_ENABLES || !fits(now_use, k->dem, budget, k->v->src, NULL)) continue;
    add_use(now_use, k->dem);
    k->enabled = true; *sk = 0; out->enables++;
    st->enabled[k->cell][k->slot][k->v->src] = true;
  }
}
