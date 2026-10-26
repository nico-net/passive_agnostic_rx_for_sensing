/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Cell lifecycle of the receiver (task A8). See nr_cell_life.h. */
#include "nr_cell_life.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static void llog(nr_cell_life_t *L, const char *line)
{
  if (L->ops.log)
    L->ops.log(L->ops.ctx, line);
  else
    printf("%s\n", line);
}

int nr_cell_life_order(const nr_cell_desc_t *primary, const nr_cell_inventory_t *in, double tol_hz, nr_cell_inventory_t *out, int *matched)
{
  if (!primary || !out)
    return -1;
  int m = -1;
  if (in)
    for (int i = 0; i < in->n && m < 0; i++)
      if (in->cell[i].desc.state != NR_CELLD_LOST && fabs(in->cell[i].desc.centre_hz - primary->centre_hz) <= tol_hz)
        m = i;
  nr_cell_inventory_t o;
  memset(&o, 0, sizeof o);
  if (in) {
    o.window_centre_hz = in->window_centre_hz;
    o.window_rate_msps = in->window_rate_msps;
    o.window_n_rx = in->window_n_rx;
  }
  nr_inv_cell_t *p = &o.cell[o.n++];
  if (m >= 0) {
    *p = in->cell[m];
    /* the started tuning is what the receiver runs: it wins over the inventory's centre; unknowns of the started descriptor come from the inventory */
    nr_cell_desc_t d = *primary;
    if (d.pci < 0) d.pci = p->desc.pci;
    if (d.n_rb <= 0) d.n_rb = p->desc.n_rb;
    if (d.ssb_hz <= 0) d.ssb_hz = p->desc.ssb_hz;
    if (d.gscn <= 0) d.gscn = p->desc.gscn;
    if (d.scs_khz <= 0) d.scs_khz = p->desc.scs_khz;
    if (d.n_rx <= 0) d.n_rx = p->desc.n_rx;
    if (d.window_centre_hz == 0) d.window_centre_hz = p->desc.window_centre_hz;
    p->desc = d;
  } else {
    snprintf(p->id, sizeof p->id, "%s", NR_CELL_LIFE_PRIMARY_ID);
    p->desc = *primary;
  }
  p->desc.state = NR_CELLD_MAINTAIN;
  if (in)
    for (int i = 0; i < in->n && o.n < NR_ADMIT_PLAN_MAX; i++) {
      if (i == m)
        continue;
      if (m < 0 && !strcmp(in->cell[i].id, NR_CELL_LIFE_PRIMARY_ID))
        continue; /* a front-end id may not shadow the synthetic primary */
      o.cell[o.n] = in->cell[i];
      if (o.cell[o.n].desc.state == NR_CELLD_MAINTAIN)
        o.cell[o.n].desc.state = NR_CELLD_ACQUIRE; /* only the started cell keeps slot 0 */
      o.n++;
    }
  *out = o;
  if (matched)
    *matched = m;
  return o.n;
}


/* run-time ordering: the primary keeps the id it got at plan time whatever the inventory now says, so slot 0 is never retired */
static void order_fixed(const nr_cell_life_t *L, const nr_cell_inventory_t *inv, nr_cell_inventory_t *o)
{
  nr_cell_life_order(&L->primary, inv, L->match_tol_hz, o, NULL);
  snprintf(o->cell[0].id, sizeof o->cell[0].id, "%s", L->primary_id);
  int w = 1;
  for (int r = 1; r < o->n; r++)
    if (strcmp(o->cell[r].id, L->primary_id))
      o->cell[w++] = o->cell[r];
  o->n = w;
}

static int run_hook_start(nr_cell_life_t *L, int cell, int reactivation)
{
  const nr_mgr_slot_t *s = nr_cell_mgr_slot(&L->mgr, cell);
  const int prev = nr_cell_idx();
  nr_cell_bind(cell);
  const int rc = L->ops.start ? L->ops.start(L->ops.ctx, cell, s, reactivation) : 0;
  nr_cell_bind(prev);
  if (rc == 0)
    L->started[cell] = 1;
  return rc;
}

static void on_event(void *ctx, const nr_cell_mgr_event_t *ev)
{
  nr_cell_life_t *L = ctx;
  char line[256];
  const int k = ev->slot;
  switch (ev->kind) {
    case NR_MGR_EV_ADD:
      if (L->collecting) {
        if (L->n_pending < NR_CELL_MAX)
          L->pending_add[L->n_pending++] = k;
        return;
      }
      if (k < 0 || k >= NR_CELL_MAX || !L->prepared[k]) {
        snprintf(line, sizeof line, "MPCI_LIFE cell=%d id=%s event=add not_started reason=not_prepared_before_fence", k, ev->id);
        llog(L, line);
        return;
      }
      if (run_hook_start(L, k, 1) == 0) {
        snprintf(line, sizeof line, "MPCI_LIFE cell=%d id=%s event=reactivate", k, ev->id);
        llog(L, line);
      }
      return;
    case NR_MGR_EV_RETUNE:
      if (k >= 0 && k < NR_CELL_MAX && L->ops.retune) {
        const int prev = nr_cell_idx();
        nr_cell_bind(k);
        L->ops.retune(L->ops.ctx, k, ev);
        nr_cell_bind(prev);
      }
      return;
    case NR_MGR_EV_RETIRE:
      if (k >= 0 && k < NR_CELL_MAX) {
        if (L->started[k] && L->ops.retire) {
          const int prev = nr_cell_idx();
          nr_cell_bind(k);
          /* the manager frees the slot before it reports RETIRE: hand the hook the cell as it was (id and last applied descriptor) */
          nr_mgr_slot_t was = *nr_cell_mgr_slot(&L->mgr, k);
          snprintf(was.id, sizeof was.id, "%s", ev->id ? ev->id : "");
          was.desc = ev->desc;
          L->ops.retire(L->ops.ctx, k, &was);
          nr_cell_bind(prev);
        }
        L->started[k] = 0;
        snprintf(line, sizeof line, "MPCI_LIFE cell=%d id=%s event=retire cause=%s", k, ev->id, ev->cause ? ev->cause : "-");
        llog(L, line);
      }
      return;
    default: /* REFUSE: the manager logged it; a refused cell gets no hook */
      return;
  }
}

int nr_cell_life_plan(nr_cell_life_t *L, const nr_cell_mgr_cfg_t *cfg, const nr_host_caps_t *caps, const nr_cell_life_ops_t *ops,
                      const nr_cell_desc_t *primary, const nr_cell_inventory_t *inv, double tol_hz)
{
  if (!L || !cfg || !caps || !primary)
    return -1;
  memset(L, 0, sizeof *L);
  if (ops)
    L->ops = *ops;
  L->primary = *primary;
  L->match_tol_hz = tol_hz;
  nr_cell_inventory_t o;
  int matched = -1;
  if (nr_cell_life_order(primary, inv, tol_hz, &o, &matched) < 1)
    return -1;
  snprintf(L->primary_id, sizeof L->primary_id, "%s", o.cell[0].id);
  nr_cell_desc_t wanted[NR_ADMIT_PLAN_MAX];
  int nw = 0;
  for (int i = 0; i < o.n; i++)
    if (o.cell[i].desc.state != NR_CELLD_LOST)
      wanted[nw++] = o.cell[i].desc;
  nr_cell_mgr_cfg_t c = *cfg;
  c.cb = on_event;
  c.ctx = L;
  if (!c.log && L->ops.log) {
    c.log = L->ops.log;
    c.log_ctx = L->ops.ctx;
  }
  const int n = nr_cell_mgr_plan(&L->mgr, &c, caps, wanted, nw);
  if (n < 1)
    return -1;
  L->n = n;
  return n;
}

int nr_cell_life_admit(nr_cell_life_t *L, const nr_cell_inventory_t *inv, int64_t now_ns)
{
  if (!L || L->n < 1 || L->phase != 0)
    return -1;
  nr_cell_inventory_t o;
  order_fixed(L, inv, &o);
  L->collecting = 1;
  L->n_pending = 0;
  nr_cell_mgr_apply(&L->mgr, &o, now_ns);
  L->collecting = 0;
  L->phase = 1;
  return L->n_pending;
}

int nr_cell_life_prepare(nr_cell_life_t *L)
{
  if (!L || L->phase != 1)
    return -1;
  char line[256];
  for (int i = 0; i < L->n_pending; i++) {
    const int k = L->pending_add[i];
    const nr_mgr_slot_t *s = nr_cell_mgr_slot(&L->mgr, k);
    const int prev = nr_cell_idx();
    nr_cell_bind(k);
    const int rc = L->ops.prealloc ? L->ops.prealloc(L->ops.ctx, k, s) : 0;
    nr_cell_bind(prev);
    L->prepared[k] = rc == 0;
    if (rc) {
      snprintf(line, sizeof line, "MPCI_LIFE cell=%d id=%s event=prealloc_failed rc=%d", k, s ? s->id : "-", rc);
      llog(L, line);
    }
  }
  if (L->ops.fence)
    L->ops.fence(L->ops.ctx);
  /* the count every LOG line and the per-cell budgets see: the highest prepared slot + 1 (slots are filled from 0) */
  int top = 0, n_prep = 0;
  for (int k = 0; k < NR_CELL_MAX; k++)
    if (L->prepared[k]) {
      top = k + 1;
      n_prep++;
    }
  if (top >= 1)
    nr_cell_set_count(top);
  L->phase = 2;
  return n_prep;
}

int nr_cell_life_start(nr_cell_life_t *L)
{
  if (!L || L->phase != 2)
    return -1;
  char line[256];
  int started = 0, n_prep = 0;
  for (int k = 0; k < NR_CELL_MAX; k++)
    n_prep += L->prepared[k];
  for (int i = 0; i < L->n_pending; i++) {
    const int k = L->pending_add[i];
    if (!L->prepared[k])
      continue;
    if (run_hook_start(L, k, 0) == 0)
      started++;
    else {
      snprintf(line, sizeof line, "MPCI_LIFE cell=%d event=start_failed", k);
      llog(L, line);
    }
  }
  snprintf(line, sizeof line, "MPCI_LIFE activated cells=%d prepared=%d of nmax=%d primary=%s", started, n_prep, L->n, L->primary_id);
  llog(L, line);
  L->phase = 3;
  L->activated = 1;
  return started;
}

int nr_cell_life_activate(nr_cell_life_t *L, const nr_cell_inventory_t *inv, int64_t now_ns)
{
  if (nr_cell_life_admit(L, inv, now_ns) < 0 || nr_cell_life_prepare(L) < 0)
    return -1;
  return nr_cell_life_start(L);
}

int nr_cell_life_poll(nr_cell_life_t *L, const nr_cell_inventory_t *inv, int64_t now_ns)
{
  if (!L || !L->activated)
    return 0;
  int ev = 0;
  if (inv) {
    nr_cell_inventory_t o;
    order_fixed(L, inv, &o);
    ev += nr_cell_mgr_apply(&L->mgr, &o, now_ns);
  }
  ev += nr_cell_mgr_tick(&L->mgr, now_ns);
  return ev;
}

int nr_cell_life_started(const nr_cell_life_t *L, int cell)
{
  return L && cell >= 0 && cell < NR_CELL_MAX ? L->started[cell] : 0;
}
