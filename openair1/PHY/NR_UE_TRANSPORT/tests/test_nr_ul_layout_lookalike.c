#include <assert.h>
#include <stdio.h>
#include "../nr_ul_layout_lookalike.h"
#include "../nr_ul_trial_variant.h"

static void test_lookalike_without_edge_evidence_prefers_prior(void)
{
  assert(nr_ul_layout_lookalike(216, 1, 217, 0));
  assert(nr_ul_layout_lookalike(217, 0, 216, 1));
  assert(!nr_ul_layout_lookalike(216, 1, 106, 0));
  assert(!nr_ul_layout_lookalike(216, 1, 216, 0));
  assert(!nr_ul_layout_discriminates(216, 1, 217, 0, 100, 3));
  assert(nr_ul_layout_discriminates(216, 1, 217, 0, 0, 3));
  assert(nr_ul_layout_discriminates(217, 0, 216, 1, 0, 1));
  const uint32_t d1[2] = {3, 2}, d2[2] = {9, 1}, d3[2] = {2, 2};
  const uint8_t p1[2] = {0, 1}, p0[2] = {0, 0};
  assert(nr_ul_layout_resolve_lookalike(d1, p1) == 1);
  assert(nr_ul_layout_resolve_lookalike(d2, p1) == 0);
  assert(nr_ul_layout_resolve_lookalike(d3, p0) == -1);
}

static void test_trial_variants(void)
{
  int t, k;
  assert(nr_ul_variant_count(false, false, false) == 1 && nr_ul_variant_count(true, false, false) == 3);
  assert(nr_ul_variant_count(true, true, false) == 6 && nr_ul_variant_count(true, true, true) == 4);
  nr_ul_variant_get(0, true, true, false, &t, &k); assert(t == -1 && k == 0);
  nr_ul_variant_get(1, true, true, false, &t, &k); assert(t == 1 && k == 0);
  nr_ul_variant_get(2, true, true, false, &t, &k); assert(t == 2 && k == 0);
  nr_ul_variant_get(3, true, true, false, &t, &k); assert(t == -1 && k == 1);
  nr_ul_variant_get(5, true, true, false, &t, &k); assert(t == 2 && k == 1);
  nr_ul_variant_get(1, true, false, true, &t, &k); assert(t == 4 && k == 0);
  nr_ul_variant_get(1, false, true, false, &t, &k); assert(t == -1 && k == 1); /* k2 pair only */
  assert(!nr_ul_variant_latch_ok(1, 1) && !nr_ul_variant_latch_ok(2, 1) && nr_ul_variant_latch_ok(2, 2));
}

static void test_lookalike_partners(void)
{
  uint16_t p[4][2];
  int c = nr_ul_lookalike_partners(105, 0, 106, p); /* A3b: voted 105@0, truth 106@0 */
  assert(c == 3 && p[0][0] == 106 && p[0][1] == 0 && p[1][0] == 104 && p[1][1] == 0 && p[2][0] == 104 && p[2][1] == 1);
  c = nr_ul_lookalike_partners(106, 0, 106, p); /* 107@x does not fit the carrier */
  assert(c == 2 && p[0][0] == 105 && p[0][1] == 0 && p[1][0] == 105 && p[1][1] == 1);
}

int main(void)
{
  test_trial_variants();
  test_lookalike_partners();
  test_lookalike_without_edge_evidence_prefers_prior();
  puts("ok");
  return 0;
}
