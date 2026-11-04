#include <assert.h>
#include <string.h>
#include "../nr_lockhold.h"

static void ident(char n[16], int *t) { strcpy(n, "pdcchDisc"); *t = 42; }
#define MS 1000000ull

int main(void)
{
  static nr_lockhold_t lh;
  nr_lh_evt_t e;
  uint64_t t = 1000 * MS;
  /* fast hold: nothing */
  nr_lh_acquired(&lh, t, 0, "f", 1);
  nr_lh_release(&lh, t + 4 * MS, ident, &e);
  assert(!e.line && !e.summary && lh.ring_n == 0);
  /* slow hold: first line emitted, split carried */
  nr_lh_acquired(&lh, t, 2 * MS, "a", 10);
  nr_lh_split(&lh, 1, 7 * MS);
  nr_lh_release(&lh, t + 8 * MS, ident, &e);
  assert(e.line && !e.summary && e.rec.hold_us == 8000 && e.rec.wait_us == 2000 && e.rec.tid == 42 && e.rec.split_us[1] == 7000);
  char b[512];
  nr_lh_fmt_line(b, sizeof b, 3, "phase2", &e);
  assert(strstr(b, "SENSING: LOCKHOLD cell=3 lock=phase2 holder=pdcchDisc tid=42 site=a:10 hold_ms=8.00 wait_ms=2.00 suppressed=0 step_ex_ms=7.00"));
  /* within 1 s: suppressed, counted; ring grows */
  for (int i = 0; i < 3; i++) {
    t += 100 * MS;
    nr_lh_acquired(&lh, t, 0, "b", 20);
    nr_lh_release(&lh, t + 6 * MS, ident, &e);
    assert(!e.line);
  }
  assert(lh.suppressed == 3 && lh.ring_n == 4 && lh.by_n == 2);
  /* after 1 s the next slow hold emits with suppressed=3 */
  t += 1000 * MS;
  nr_lh_acquired(&lh, t, 0, "b", 20);
  nr_lh_release(&lh, t + 9 * MS, ident, &e);
  assert(e.line && e.suppressed == 3 && lh.suppressed == 0);
  /* ring wraps without overflow */
  for (int i = 0; i < 40; i++) { nr_lh_acquired(&lh, t, 0, "c", 1); nr_lh_release(&lh, t + 6 * MS, ident, &e); }
  assert(lh.ring_n == 45);
  /* summary after 10 s window (window began at the first acquire = 1000 ms) */
  t = 1000 * MS + 11000 * MS;
  nr_lh_acquired(&lh, t, 0, "f", 1);
  nr_lh_release(&lh, t + 1 * MS, ident, &e);
  assert(e.summary && e.cnt == 45 && e.max_hold_ns == 9 * MS && e.max_wait_ns == 2 * MS && e.by_n == 3);
  nr_lh_fmt_summary(b, sizeof b, 0, "ullayout", &e);
  assert(strstr(b, "SENSING: LOCKHOLD_SUMMARY cell=0 lock=ullayout window_s=10 holds_gt5ms=45") && strstr(b, "pdcchDisc@a:10=1"));
  /* window reset: a quiet window emits no summary */
  t += 11000 * MS;
  nr_lh_acquired(&lh, t, 0, "f", 1);
  nr_lh_release(&lh, t + 1 * MS, ident, &e);
  assert(!e.summary && lh.win_cnt == 0);
  return 0;
}
