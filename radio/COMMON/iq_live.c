/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI-LIVE: live cell changes (decisions and order), see iq_live.h. Pure: hooks only, no environment, no file, no clock, no allocation. */
#include "iq_live.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

const char *iq_live_reason_name(int r)
{
  switch (r) {
    case IQ_LIVE_REASON_GEOMETRY: return "geometry";
    case IQ_LIVE_REASON_RATE: return "capture_rate_change";
    case IQ_LIVE_REASON_RADIO: return "radio_retune_failed";
    case IQ_LIVE_REASON_FAIL: return "hook_failed";
    default: return iq_span_reason_name(r);
  }
}

static const char *state_name(int s)
{
  return s == IQ_LIVE_SERVED ? "served" : s == IQ_LIVE_UNSERVED ? "unserved" : s == IQ_LIVE_PARKED ? "parked" : "empty";
}

static void llog(iq_live_t *L, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void llog(iq_live_t *L, const char *fmt, ...)
{
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (L->ops.log) L->ops.log(L->ops.ctx, line);
  else printf("%s\n", line);
}

void iq_live_init(iq_live_t *L, const iq_span_dev_t *dev, const iq_span_plan_t *plan, const iq_live_ops_t *ops)
{
  memset(L, 0, sizeof *L);
  if (dev) L->dev = *dev;
  if (plan) L->plan = *plan;
  if (ops) L->ops = *ops;
}

void iq_live_slot_prepare(iq_live_t *L, int slot, const char *id, const iq_live_tune_t *geom, const iq_live_tune_t *cur, double rate, int holds_cell)
{
  if (slot < 0 || slot >= IQ_LIVE_MAX_SLOTS) return;
  iq_live_slot_t *s = &L->slot[slot];
  memset(s, 0, sizeof *s);
  snprintf(s->id, sizeof s->id, "%s", id ? id : "");
  if (geom) s->geom = *geom;
  if (cur) s->cur = s->want = *cur;
  s->rate = rate;
  if (holds_cell) {
    s->state = L->plan.ok && slot < L->plan.n_cells && L->plan.cell[slot].served ? IQ_LIVE_SERVED : IQ_LIVE_UNSERVED;
    if (s->state == IQ_LIVE_UNSERVED) s->reason = slot < L->plan.n_cells ? L->plan.cell[slot].reason : IQ_SPAN_INVALID;
  } else {
    s->state = IQ_LIVE_EMPTY;
  }
  if (slot >= L->n) L->n = slot + 1;
}

int iq_live_state(const iq_live_t *L, int slot) { return slot >= 0 && slot < L->n ? L->slot[slot].state : -1; }
int iq_live_served(const iq_live_t *L)
{
  int n = 0;
  for (int i = 0; i < L->n; ++i) n += L->slot[i].state == IQ_LIVE_SERVED;
  return n;
}

/* the cell of a slot as the planner sees it */
static iq_span_cell_t span_cell(const iq_live_slot_t *s, const iq_live_tune_t *t)
{
  return (iq_span_cell_t){.id = s->id, .centre_hz = t->centre_hz, .n_rb = s->geom.n_rb, .scs_hz = s->geom.scs_hz, .rate = s->rate, .n_rx = s->geom.n_rx};
}

/* a tuning given by the inventory completed with the slot's prepared geometry; 0 = the cell's grid is not the slot's */
static int complete(const iq_live_slot_t *s, const iq_live_tune_t *t, iq_live_tune_t *o)
{
  *o = *t;
  if (o->n_rb <= 0) o->n_rb = s->geom.n_rb;
  if (!(o->scs_hz > 0)) o->scs_hz = s->geom.scs_hz;
  if (o->n_rx == 0) o->n_rx = s->geom.n_rx;
  if (o->n_rb != s->geom.n_rb || fabs(o->scs_hz - s->geom.scs_hz) > 1.0 || o->n_rx > s->geom.n_rx) return 0;
  o->n_rb = s->geom.n_rb;
  o->scs_hz = s->geom.scs_hz;
  o->n_rx = s->geom.n_rx;
  return 1;
}

static void set_unserved(iq_live_t *L, int slot, int reason, const char *ev)
{
  iq_live_slot_t *s = &L->slot[slot];
  const int quiet = !strcmp(ev, "retry") && s->state == IQ_LIVE_UNSERVED && s->reason == reason; /* a retry that fails the same way says nothing new */
  s->state = IQ_LIVE_UNSERVED;
  s->reason = reason;
  if (!quiet) llog(L, "MPCI_LIFE cell=%d id=%s event=%s unserved reason=%s centre_hz=%.0f capture_centre_hz=%.0f rate=%.0f (radio untouched)", slot, s->id, ev, iq_live_reason_name(reason),
       s->want.centre_hz, L->plan.centre_hz, L->plan.rate);
}

/* Re-plan the capture with the served cells plus slot `slot` at tuning t, at the RATE in force (the radio is not restarted): the LO moves to the middle of the new span,
 * which must lie inside the flat band at that rate for every cell. 1 = accepted (*p = the capture in force with the new centre), 0 = refused (*reason: IQ_LIVE_REASON_RATE when a
 * capture at another rate would carry them all, else the planner's reason for the new cell). */
static int replan(iq_live_t *L, int slot, const iq_live_tune_t *t, iq_span_plan_t *p, int *reason)
{
  iq_span_cell_t cells[IQ_SPAN_MAX_CELLS];
  int m = 0, who = -1;
  double lo = INFINITY, hi = -INFINITY;
  for (int i = 0; i < L->n && m < IQ_SPAN_MAX_CELLS; ++i) {
    if (i == slot) {
      cells[m] = span_cell(&L->slot[i], t);
      who = m++;
    } else if (L->slot[i].state == IQ_LIVE_SERVED) {
      cells[m++] = span_cell(&L->slot[i], &L->slot[i].cur);
    } else
      continue;
    const double h = iq_span_cell_half_bw(&cells[m - 1]);
    if (cells[m - 1].centre_hz - h < lo) lo = cells[m - 1].centre_hz - h;
    if (cells[m - 1].centre_hz + h > hi) hi = cells[m - 1].centre_hz + h;
  }
  if (m < 1 || who < 0) { *reason = IQ_SPAN_INVALID; return 0; }
  *p = L->plan;
  p->centre_hz = 0.5 * (lo + hi);
  p->lo_hz = lo;
  p->hi_hz = hi;
  iq_span_cell_plan_t o;
  for (int j = 0; j < m; ++j)
    if (!iq_span_fit(&L->dev, p, &cells[j], &o)) {
      iq_span_plan_t q;
      *reason = o.reason;
      if (j == who || o.reason == IQ_SPAN_OUT_OF_SPAN) {
        if (iq_span_plan(&L->dev, cells, m, &q) == m && q.ok) *reason = IQ_LIVE_REASON_RATE; /* another capture rate would carry them all */
        else if (j != who) *reason = IQ_SPAN_OUT_OF_SPAN;
      }
      return 0;
    }
  return 1;
}

/* bring slot into service at tuning t. State EMPTY / PARKED / UNSERVED: the entry is paused. SERVED (a retune): it runs and is paused around a centre move. */
static int serve(iq_live_t *L, int slot, const iq_live_tune_t *t, int was_running, const char *ev)
{
  iq_live_slot_t *s = &L->slot[slot];
  const int moved = fabs(t->centre_hz - s->cur.centre_hz) > 0.5;
  const int tune_changed = moved || t->ssb != s->cur.ssb || fabs(t->cfo_hz - s->cur.cfo_hz) > 0.5;
  s->want = *t;
  /* any change of what the UE is tuned to pauses the entry: the slot thread parks, the retune is applied while nothing reads, and the resume is the forward gap that
   * makes the cell's timing loop re-acquire (only this cell; the other entries keep running) */
  if (was_running && tune_changed && L->ops.chan_pause) L->ops.chan_pause(L->ops.ctx, slot);
  if (moved && L->ops.chan_retune && L->ops.chan_retune(L->ops.ctx, slot, t->centre_hz)) {
    set_unserved(L, slot, IQ_LIVE_REASON_FAIL, ev);
    return IQ_LIVE_UNSERVED;
  }
  if (tune_changed && L->ops.ue_retune && L->ops.ue_retune(L->ops.ctx, slot, t)) {
    set_unserved(L, slot, IQ_LIVE_REASON_FAIL, ev);
    return IQ_LIVE_UNSERVED;
  }
  const double prev = s->cur.centre_hz;
  s->cur = *t;
  s->state = IQ_LIVE_SERVED;
  s->reason = 0;
  if ((!was_running || tune_changed) && L->ops.chan_resume) L->ops.chan_resume(L->ops.ctx, slot);
  if (L->ops.wake) L->ops.wake(L->ops.ctx);
  llog(L, "MPCI_LIFE cell=%d id=%s event=%s served centre_hz=%.0f prev_centre_hz=%.0f cfo_hz=%.0f ssb=%d shift_hz=%+.0f (radio not stopped)", slot, s->id, ev, t->centre_hz, prev,
       t->cfo_hz, t->ssb, t->centre_hz - L->plan.centre_hz);
  return IQ_LIVE_SERVED;
}

/* common entry of add / retune of a cell at tuning t into slot `slot` (state before: EMPTY / PARKED / UNSERVED / SERVED) */
static int bring(iq_live_t *L, int slot, const iq_live_tune_t *tin, const char *ev)
{
  iq_live_slot_t *s = &L->slot[slot];
  iq_live_tune_t t;
  const int was_running = s->state == IQ_LIVE_SERVED;
  if (!complete(s, tin, &t)) {
    s->want = *tin;
    if (was_running && L->ops.chan_pause) L->ops.chan_pause(L->ops.ctx, slot);
    set_unserved(L, slot, IQ_LIVE_REASON_GEOMETRY, ev);
    return IQ_LIVE_UNSERVED;
  }
  iq_span_cell_t c = span_cell(s, &t);
  iq_span_cell_plan_t o;
  if (iq_span_fit(&L->dev, &L->plan, &c, &o)) return serve(L, slot, &t, was_running, ev);
  /* outside the running capture: re-plan; the radio moves only when every served cell (and this one) fits the new plan at the same rate */
  iq_span_plan_t p;
  int reason = o.reason;
  if (replan(L, slot, &t, &p, &reason)) {
    llog(L, "MPCI_LIFE cell=%d id=%s event=%s span_replan centre_hz %.0f -> %.0f rate=%.0f span_mhz=[%.3f..%.3f]: radio recentre", slot, s->id, ev, L->plan.centre_hz, p.centre_hz,
         p.rate, p.lo_hz / 1e6, p.hi_hz / 1e6);
    if (fabs(p.centre_hz - L->plan.centre_hz) > 0.5 && L->ops.radio_recentre && L->ops.radio_recentre(L->ops.ctx, p.centre_hz)) {
      if (was_running && L->ops.chan_pause) L->ops.chan_pause(L->ops.ctx, slot);
      s->want = t;
      set_unserved(L, slot, IQ_LIVE_REASON_RADIO, ev);
      return IQ_LIVE_UNSERVED;
    }
    /* cells keep their slot indices: the planner's cell order was the served slots then this one, the running capture needs only the new centre / usable width */
    L->plan.centre_hz = p.centre_hz;
    L->plan.usable_hz = p.usable_hz;
    L->plan.lo_hz = p.lo_hz;
    L->plan.hi_hz = p.hi_hz;
    L->recentres++;
    if (L->ops.span_changed) L->ops.span_changed(L->ops.ctx, &L->plan);
    return serve(L, slot, &t, was_running, ev);
  }
  /* not servable: the old tuning of a retuned cell is wrong now, so its entry stops */
  if (was_running && L->ops.chan_pause) L->ops.chan_pause(L->ops.ctx, slot);
  s->want = t;
  set_unserved(L, slot, reason, ev);
  return IQ_LIVE_UNSERVED;
}

int iq_live_remove(iq_live_t *L, int slot, const char *why)
{
  if (!L || slot < 0 || slot >= L->n) return -1;
  iq_live_slot_t *s = &L->slot[slot];
  if (s->state == IQ_LIVE_EMPTY || s->state == IQ_LIVE_PARKED) return s->state;
  if (s->state == IQ_LIVE_SERVED && L->ops.chan_pause) L->ops.chan_pause(L->ops.ctx, slot);
  llog(L, "MPCI_LIFE cell=%d id=%s event=park reason=%s was=%s centre_hz=%.0f entry paused, slot thread parked outside the radio gate (radio not stopped)", slot, s->id,
       why ? why : "removed", state_name(s->state), s->cur.centre_hz);
  s->state = IQ_LIVE_PARKED;
  return s->state;
}

int iq_live_add(iq_live_t *L, int slot, const char *id, const iq_live_tune_t *t)
{
  if (!L || !t || slot < 0 || slot >= L->n) return -1;
  iq_live_slot_t *s = &L->slot[slot];
  if (s->state == IQ_LIVE_SERVED) return s->state;
  if (id) snprintf(s->id, sizeof s->id, "%s", id);
  return bring(L, slot, t, "add");
}

int iq_live_retry(iq_live_t *L)
{
  int n = 0;
  for (int i = 0; L && i < L->n; ++i)
    if (L->slot[i].state == IQ_LIVE_UNSERVED && L->slot[i].reason != IQ_LIVE_REASON_GEOMETRY) {
      const iq_live_tune_t w = L->slot[i].want;
      n += bring(L, i, &w, "retry") == IQ_LIVE_SERVED;
    }
  return n;
}

int iq_live_retune(iq_live_t *L, int slot, const iq_live_tune_t *t)
{
  if (!L || !t || slot < 0 || slot >= L->n) return -1;
  iq_live_slot_t *s = &L->slot[slot];
  if (s->state == IQ_LIVE_EMPTY || s->state == IQ_LIVE_PARKED) return s->state; /* no cell there: the next add carries the new tuning */
  iq_live_tune_t c;
  if (s->state == IQ_LIVE_SERVED && complete(s, t, &c) && fabs(c.centre_hz - s->cur.centre_hz) < 0.5 && c.ssb == s->cur.ssb && fabs(c.cfo_hz - s->cur.cfo_hz) < 0.5)
    return s->state; /* nothing the receiver tunes changed */
  return bring(L, slot, t, "retune");
}

int iq_live_format(const iq_live_t *L, char *buf, unsigned n)
{
  if (!buf || !n) return 0;
  unsigned o = (unsigned)snprintf(buf, n, "MPCI_LIVE served=%d/%d span=%.3f/%.2f recentres=%d slots=", iq_live_served(L), L->n, L->plan.centre_hz / 1e6, L->plan.rate / 1e6, L->recentres);
  for (int i = 0; i < L->n && o < n; ++i)
    o += (unsigned)snprintf(buf + o, n - o, "%s%d:%s:%s", i ? "," : "", i, L->slot[i].id[0] ? L->slot[i].id : "-", state_name(L->slot[i].state));
  return (int)(o < n ? o : n - 1);
}
