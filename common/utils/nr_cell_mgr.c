/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Cell manager (task D1). See nr_cell_mgr.h. */
#include "nr_cell_mgr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mlog(nr_cell_mgr_t *m, const char *line)
{
  if (m->cfg.log)
    m->cfg.log(m->cfg.log_ctx, line);
  else
    printf("%s\n", line);
}

static void emit(nr_cell_mgr_t *m, int kind, int slot, const char *id, const nr_cell_desc_t *d, double prev_centre, double prev_cfo, int ssb,
                 int prev_ssb, int reason, const char *cause, void *state)
{
  char line[320];
  const char *ev = kind == NR_MGR_EV_ADD ? "add" : kind == NR_MGR_EV_RETUNE ? "update" : kind == NR_MGR_EV_RETIRE ? "retire" : "refuse";
  int n = snprintf(line, sizeof line, "MPCI_CELL id=%s slot=%d event=%s reason=%s pci=%d centre_hz=%.0f ssb=%d cfo_hz=%.0f", id, slot, ev, cause, d->pci,
                   d->centre_hz, ssb, d->cfo_hz);
  if (kind == NR_MGR_EV_RETUNE && n > 0 && (size_t)n < sizeof line)
    snprintf(line + n, sizeof line - (size_t)n, " prev_centre_hz=%.0f prev_ssb=%d prev_cfo_hz=%.0f", prev_centre, prev_ssb, prev_cfo);
  mlog(m, line);
  if (m->cfg.cb) {
    nr_cell_mgr_event_t e = {.kind = kind, .slot = slot, .id = id, .desc = *d, .prev_centre_hz = prev_centre, .prev_cfo_hz = prev_cfo,
                             .ssb = ssb, .prev_ssb = prev_ssb, .reason = reason, .cause = cause, .slot_state = state};
    m->cfg.cb(m->cfg.ctx, &e);
  }
}

int nr_cell_mgr_plan(nr_cell_mgr_t *m, const nr_cell_mgr_cfg_t *cfg, const nr_host_caps_t *caps, const nr_cell_desc_t *wanted, int n_wanted)
{
  if (!m || !caps || (n_wanted > 0 && !wanted) || n_wanted < 1 || n_wanted > NR_ADMIT_PLAN_MAX)
    return -1;
  memset(m, 0, sizeof *m);
  if (cfg)
    m->cfg = *cfg;
  if (m->cfg.retire_hysteresis_ns <= 0)
    m->cfg.retire_hysteresis_ns = NR_MGR_RETIRE_HYST_DEFAULT_NS;
  if (m->cfg.grid_n_rb <= 0 || m->cfg.pass1_ssb <= 0 || m->cfg.pass1_k_ssb < 0)
    return -1; /* the started grid is an input */
  if (nr_cell_alloc_fenced())
    return -1; /* planning allocates the slot objects: only legal before the RX fence */
  int served = nr_cell_admit_plan(caps, wanted, n_wanted, &m->plan);
  char line[640];
  if (served >= 0)
    nr_admit_plan_format(&m->plan, line, sizeof line);
  else
    snprintf(line, sizeof line, "MPCI_ADMIT cells=0 of %d reason=bad_arguments", n_wanted);
  mlog(m, line);
  if (served < 1 || m->plan.refuse)
    return -1;
  if (served > NR_CELL_MAX)
    served = NR_CELL_MAX;
  m->nmax = served;
  m->full_reason = m->plan.binding != NR_ADMIT_OK ? m->plan.binding : NR_ADMIT_MAX;
  if (m->cfg.slot_state_bytes)
    for (int i = 0; i < m->nmax; i++)
      (void)nr_cell_heap_get(&m->slot[i].state, m->cfg.slot_state_bytes, "nr_cell_mgr.slot_state");
  m->planned = 1;
  return m->nmax;
}

int nr_cell_mgr_nmax(const nr_cell_mgr_t *m) { return m->nmax; }
int nr_cell_mgr_active(const nr_cell_mgr_t *m) { return m->n_active; }
int nr_cell_mgr_find(const nr_cell_mgr_t *m, const char *id)
{
  for (int i = 0; i < m->nmax; i++)
    if (m->slot[i].active && !strcmp(m->slot[i].id, id))
      return i;
  return -1;
}
const nr_mgr_slot_t *nr_cell_mgr_slot(const nr_cell_mgr_t *m, int slot)
{
  return (slot >= 0 && slot < m->nmax) ? &m->slot[slot] : NULL;
}

static int free_slot(const nr_cell_mgr_t *m)
{
  for (int i = 0; i < m->nmax; i++)
    if (!m->slot[i].active)
      return i;
  return -1;
}
static int refused_idx(const nr_cell_mgr_t *m, const char *id)
{
  for (int i = 0; i < m->n_refused; i++)
    if (!strcmp(m->refused[i].id, id))
      return i;
  return -1;
}
static void refused_drop(nr_cell_mgr_t *m, int i)
{
  m->refused[i] = m->refused[--m->n_refused];
}
static int prio(int state)
{
  return state == NR_CELLD_MAINTAIN ? 0 : state == NR_CELLD_ACQUIRE ? 1 : 2;
}

int nr_cell_mgr_apply(nr_cell_mgr_t *m, const nr_cell_inventory_t *inv, int64_t now_ns)
{
  if (!m->planned || !inv)
    return 0;
  int events = 0;
  /* 1. cells holding a slot: update / lost / removed */
  for (int s = 0; s < m->nmax; s++) {
    nr_mgr_slot_t *sl = &m->slot[s];
    if (!sl->active)
      continue;
    const nr_inv_cell_t *ic = NULL;
    for (int i = 0; i < inv->n; i++)
      if (!strcmp(inv->cell[i].id, sl->id)) {
        ic = &inv->cell[i];
        break;
      }
    if (!ic || ic->desc.state == NR_CELLD_LOST) {
      if (!sl->pending_retire) {
        sl->pending_retire = ic ? 1 : 2;
        sl->pending_since_ns = now_ns;
      } else if (sl->pending_retire == 2 && ic) {
        sl->pending_retire = 1; /* now explicitly lost: keep the original start of the hysteresis */
      }
      continue;
    }
    sl->pending_retire = 0; /* back (or never gone): cancel */
    const nr_cell_desc_t *nd = &ic->desc;
    int changed = nd->centre_hz != sl->inv_desc.centre_hz || nd->ssb_hz != sl->inv_desc.ssb_hz || nd->cfo_hz != sl->inv_desc.cfo_hz;
    double pc = sl->desc.centre_hz, pf = sl->desc.cfo_hz;
    sl->inv_desc = *nd;
    sl->desc = *nd;
    if (!changed)
      sl->desc.centre_hz = pc; /* an unchanged inventory does not undo a pass-2 retune */
    if (changed) {
      emit(m, NR_MGR_EV_RETUNE, s, sl->id, &sl->desc, pc, pf, sl->ssb, sl->ssb, 0, "inventory", sl->state);
      events++;
    }
  }
  /* 2. refused list: forget ids that left the inventory */
  for (int r = 0; r < m->n_refused;) {
    int found = 0;
    for (int i = 0; i < inv->n; i++)
      found |= !strcmp(inv->cell[i].id, m->refused[r].id) && inv->cell[i].desc.state != NR_CELLD_LOST;
    if (found)
      r++;
    else
      refused_drop(m, r);
  }
  /* 3. adds in admission priority order (stable) */
  for (int cls = 0; cls < 3; cls++)
    for (int i = 0; i < inv->n; i++) {
      const nr_inv_cell_t *ic = &inv->cell[i];
      if (ic->desc.state == NR_CELLD_LOST || prio(ic->desc.state) != cls || nr_cell_mgr_find(m, ic->id) >= 0)
        continue;
      int s = free_slot(m);
      int ri = refused_idx(m, ic->id);
      if (s < 0) {
        if (ri < 0 || m->refused[ri].reason != m->full_reason) {
          if (ri < 0 && m->n_refused < NR_ADMIT_PLAN_MAX) {
            ri = m->n_refused++;
            snprintf(m->refused[ri].id, sizeof m->refused[ri].id, "%s", ic->id);
          }
          if (ri >= 0)
            m->refused[ri].reason = m->full_reason;
          emit(m, NR_MGR_EV_REFUSE, -1, ic->id, &ic->desc, 0, 0, m->cfg.pass1_ssb, 0, m->full_reason, nr_admit_reason_name(m->full_reason), NULL);
          events++;
        }
        continue;
      }
      if (ri >= 0)
        refused_drop(m, ri);
      nr_mgr_slot_t *sl = &m->slot[s];
      void *st = sl->state; /* pre-allocated at plan time: only cleared here */
      if (st && m->cfg.slot_state_bytes)
        memset(st, 0, m->cfg.slot_state_bytes);
      memset(sl, 0, sizeof *sl);
      sl->state = st;
      sl->active = 1;
      snprintf(sl->id, sizeof sl->id, "%s", ic->id);
      sl->desc = sl->inv_desc = ic->desc;
      sl->ssb = m->cfg.pass1_ssb;
      m->n_active++;
      emit(m, NR_MGR_EV_ADD, s, sl->id, &sl->desc, 0, 0, sl->ssb, 0, 0, "new", st);
      events++;
    }
  return events;
}

int nr_cell_mgr_tick(nr_cell_mgr_t *m, int64_t now_ns)
{
  int n = 0;
  for (int s = 0; m->planned && s < m->nmax; s++) {
    nr_mgr_slot_t *sl = &m->slot[s];
    if (!sl->active || !sl->pending_retire || now_ns - sl->pending_since_ns < m->cfg.retire_hysteresis_ns)
      continue;
    char id[NR_INV_ID_MAX];
    snprintf(id, sizeof id, "%s", sl->id);
    nr_cell_desc_t d = sl->desc;
    int ssb = sl->ssb;
    const char *cause = sl->pending_retire == 1 ? "lost" : "removed";
    void *st = sl->state;
    sl->active = 0;
    sl->pending_retire = 0;
    sl->id[0] = 0;
    m->n_active--;
    emit(m, NR_MGR_EV_RETIRE, s, id, &d, 0, 0, ssb, 0, 0, cause, st);
    n++;
  }
  return n;
}

/* first occurrence of `needle` in [log, log+len) */
static const char *find(const char *log, size_t len, const char *needle)
{
  size_t nl = strlen(needle);
  if (nl == 0 || len < nl)
    return NULL;
  for (size_t i = 0; i + nl <= len; i++)
    if (log[i] == needle[0] && memcmp(log + i, needle, nl) == 0)
      return log + i;
  return NULL;
}
static int read_int_after(const char *p, const char *end, int *v)
{
  int neg = 0;
  if (p < end && *p == '-') {
    neg = 1;
    p++;
  }
  if (p >= end || *p < '0' || *p > '9')
    return -1;
  long x = 0;
  while (p < end && *p >= '0' && *p <= '9' && x < 100000000)
    x = x * 10 + (*p++ - '0');
  *v = (int)(neg ? -x : x);
  return 0;
}

int nr_cell_mgr_pass2_from_log(nr_cell_mgr_t *m, const char *id, const char *log, size_t len, int64_t now_ns, int *ssb_out, double *centre_out)
{
  (void)now_ns;
  int s = id ? nr_cell_mgr_find(m, id) : -1;
  if (s < 0 || !log)
    return -1;
  nr_mgr_slot_t *sl = &m->slot[s];
  const char *end = log + len;
  /* same lines `autofront.py refine_from_log` reads (live_chain.sh pass2) */
  const char *mib = find(log, len, "MIB fields sfn=");
  int k;
  if (!mib)
    return -1;
  const char *kp = find(mib, (size_t)(end - mib), " k_ssb=");
  if (!kp || read_int_after(kp + 7, end, &k) || k < 0 || k > 31)
    return -1;
  const char *sy = find(log, len, "Initial sync successful, PCI: ");
  int pci;
  if (sy && read_int_after(sy + 30, end, &pci) == 0 && sl->desc.pci >= 0 && pci != sl->desc.pci)
    return -1; /* the log is another cell's */
  int scs = sl->desc.scs_khz > 0 ? sl->desc.scs_khz : 30;
  int mu = scs == 15 ? 0 : scs == 30 ? 1 : scs == 60 ? 2 : scs == 120 ? 3 : -1;
  if (mu < 0)
    return -1;
  double ssb_hz = sl->desc.ssb_hz;
  if (ssb_hz <= 0 && sl->desc.gscn > 0)
    ssb_hz = 3000000000.0 + (sl->desc.gscn - 7499) * 1440000.0; /* n77/n78 raster (TS 38.104 5.4.3.1), as autofront.gscn_hz */
  if (ssb_hz <= 0)
    return -1;
  int offa = -1;
  const char *op = find(log, len, "offsetToPointA=");
  if (op && (read_int_after(op + 15, end, &offa) || offa < 0))
    offa = -1;
  int ssb;
  if (offa >= 0)
    ssb = nr_mgr_ssb_start_sc(scs, offa, k);
  else {
    if (sl->desc.n_rb != 0)
      return 0; /* the front end already carries the grid and the log adds nothing */
    ssb = (m->cfg.pass1_ssb - (m->cfg.pass1_k_ssb >> mu)) + (k >> mu);
  }
  int nrb = sl->desc.n_rb ? sl->desc.n_rb : m->cfg.grid_n_rb;
  double centre = nr_mgr_grid_centre_hz(ssb_hz, scs, nrb, ssb);
  if (ssb_out)
    *ssb_out = ssb;
  if (centre_out)
    *centre_out = centre;
  if (centre == sl->desc.centre_hz && ssb == sl->ssb)
    return 0;
  double pc = sl->desc.centre_hz;
  int ps = sl->ssb;
  sl->desc.centre_hz = centre;
  sl->ssb = ssb;
  emit(m, NR_MGR_EV_RETUNE, s, sl->id, &sl->desc, pc, sl->desc.cfo_hz, ssb, ps, 0, "pass2", sl->state);
  return 1;
}

int nr_mgr_ssb_start_sc(int scs_khz, int offset_to_point_a, int k_ssb)
{
  int mu = scs_khz == 15 ? 0 : scs_khz == 30 ? 1 : scs_khz == 60 ? 2 : scs_khz == 120 ? 3 : 1;
  return 12 * (offset_to_point_a >> mu) + (k_ssb >> mu);
}
double nr_mgr_grid_centre_hz(double ssb_hz, int scs_khz, int n_rb, int ssb_start_sc)
{
  return ssb_hz + (6.0 * n_rb - 120.0 - ssb_start_sc) * scs_khz * 1000.0;
}
