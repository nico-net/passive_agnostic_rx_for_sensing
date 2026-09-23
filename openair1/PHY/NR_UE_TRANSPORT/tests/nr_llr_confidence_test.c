#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../nr_llr_confidence.h"

static double gauss(void) { double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
  return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }
/* bit b -> LLR mean +A for b=0, -A for b=1 (OAI: positive LLR = bit 0) */
static void make(uint32_t G, double A, double sigma, uint8_t *truth, int16_t *llr) {
  for (uint32_t i = 0; i < G; i++) { truth[i] = rand() & 1; double v = (truth[i] ? -A : A) + sigma * gauss();
    if (v > 32767) { v = 32767; } if (v < -32767) { v = -32767; } llr[i] = (int16_t)v; } }

int main(void) {
  enum { G = 40000 }; static uint8_t truth[G], bits[G], keep[G / 2]; static int16_t llr[G];
  srand(1); nr_llrconf_reset();
  float tau;
  assert(nr_llrconf_threshold(2, &tau) == 0);                 /* uncalibrated */
  for (int k = 0; k < 6; k++) { make(G, 100.0, 60.0, truth, llr); nr_llrconf_observe(2, llr, truth, G); }
  assert(nr_llrconf_threshold(2, &tau) == 1 && tau > 0.0f);   /* 120000 symbols observed */
  assert(nr_llrconf_threshold(4, &tau) == 0);                 /* other Qm stays uncalibrated */
  nr_llrconf_threshold(2, &tau);
  make(G, 100.0, 60.0, truth, llr);
  uint32_t kept = nr_llrconf_hard(2, llr, G, tau, bits, keep), wrong = 0;
  for (uint32_t m = 0; m < G / 2; m++)
    if (keep[m] && (bits[2 * m] != truth[2 * m] || bits[2 * m + 1] != truth[2 * m + 1])) wrong++;
  assert(kept > G / 8);                                       /* the gate keeps a real fraction */
  assert((double)wrong / kept < 0.015);                       /* and meets the 1 % target (+slack) */
  assert(nr_llrconf_hard(2, llr, G, 0.0f, bits, keep) == G / 2); /* tau 0 keeps everything */
  /* sign-convention guard: inverted LLRs must disable the masked path */
  for (uint32_t i = 0; i < G; i++) llr[i] = (int16_t)-llr[i];
  for (int k = 0; k < 2; k++) nr_llrconf_agreement(2, llr, truth, G);
  assert(nr_llrconf_disabled() == 1);
  puts("nr_llr_confidence_test: PASS");
  return 0;
}
