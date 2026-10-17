#include <assert.h>
#include <stdio.h>
#include "../nr_ul_layout_lookalike.h"

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

int main(void)
{
  test_lookalike_without_edge_evidence_prefers_prior();
  puts("ok");
  return 0;
}
