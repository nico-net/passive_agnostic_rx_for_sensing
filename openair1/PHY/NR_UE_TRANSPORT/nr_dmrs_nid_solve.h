/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * Blind recovery of a DM-RS scrambling identity N_ID (0..65535) from the received pilots of ONE window, by solving for the Gold
 * sequence's initial state over GF(2) instead of correlating against every hypothesis.
 *
 * Why it works. TS 38.211 5.2.1: c(n) = x1(n + 1600) xor x2(n + 1600), x1 fixed, x2 a LFSR whose 31-bit initial state is c_init. A LFSR
 * is linear, so every output bit of x2 is a XOR of a fixed subset of the c_init bits: c(n) = a(n) xor <M(n), c_init>. A QPSK pilot
 * r(m) = ((1 - 2c(2m)) + j(1 - 2c(2m+1))) / sqrt(2) therefore gives two linear equations in the 31 unknown bits. 16+ pilots are enough to
 * solve for c_init, and c_init = (2^17 A (2 N_ID + 1) + 2 N_ID [+ n_SCID]) mod 2^31 (A = symbols_per_slot * slot + symbol + 1) inverts in
 * closed form because 1 + 2^17 A is odd: N_ID = ((c_init - 2^17 A) / 2) * (1 + 2^17 A)^-1 mod 2^30, valid iff it is < 65536. A wrong
 * solution passes that test with probability 2^-15, so a valid result is itself ~15 bits of confirmation.
 *
 * Cost: a few microseconds per window and rotation class, against 65,536 Gold sequences and correlations for the sweep.
 *
 * Channel and errors. The window is assumed flat (one complex gain h for all its pilots). The QPSK points are rotated by arg(h), found
 * mod 90 degrees from the 4th power (r^4 = -1 for every constellation point); the four 90-degree classes are tried. Bits are decided
 * on the derotated samples and the system is solved on the MOST RELIABLE independent equations (information-set decoding, a few
 * randomised trials), the remaining equations are parity checks. Low SNR therefore costs fewer valid solves per window, not a cliff;
 * the caller accumulates votes for the same N_ID over windows and slots.
 *
 * Pure: no PHY headers, no allocation after the first call.
 */
#ifndef NR_DMRS_NID_SOLVE_H
#define NR_DMRS_NID_SOLVE_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_NID_SOLVE_MAX_PILOT 900 /* pilot index m up to 3 * 275 RB, plus margin */

typedef struct {
  int nid;         /* solved N_ID in [0, 65535] */
  int rot;         /* 90-degree rotation class that produced it (0..3) */
  int n_checks;    /* equations beyond the 31 that were used to solve (parity checks) */
  int mismatches;  /* checks violated by the solution */
  double score;    /* reliability-weighted agreement of the checks, in [-1, 1]; 1 = all agree */
  int top_checks;      /* checks among the equations at least half as reliable as the 31st-ranked one (beyond the 31 used to solve) */
  int top_mismatches;  /* of those, how many are violated: a wrong solution fails ~half, so a few extra reliable agreements confirm it
                        * even when most of the input is unoccupied or noise (a partially occupied CORESET, a whole-carrier solve) */
  uint32_t cinit;      /* the solved Gold-sequence state */
  double phi;          /* channel rotation (radians) the pilots were derotated by to decide the bits */
} nr_nid_solution_t;

/** Solve for N_ID.
 *  yr/yi : received pilot samples (any consistent scale), m[i] : pilot index of sample i in the sequence r(m) (c(2m), c(2m+1)),
 *  n : number of samples (>= 16), slot : slot number in the frame, symbol : OFDM symbol in the slot,
 *  symbols_per_slot : 14 (normal CP), nscid : 0/1 for a PDSCH-style c_init (+ n_SCID), -1 for PDCCH (no n_SCID term).
 *  Writes up to max_out DISTINCT valid solutions (best score first); returns how many (0 if none). */
int nr_dmrs_nid_solve(const float *yr, const float *yi, const int *m, int n, int slot, int symbol, int symbols_per_slot, int nscid,
                      nr_nid_solution_t *out, int max_out);

/** Same, with fast=1: rotation classes share one cos/sin and the search stops at the first class that gives a confirmed solution (>= 4 top
 *  checks, none violated, <= 1 violated overall). The best solution is the same whenever the sure class is the best-scoring one. */
int nr_dmrs_nid_solve_ex(const float *yr, const float *yi, const int *m, int n, int slot, int symbol, int symbols_per_slot, int nscid,
                         nr_nid_solution_t *out, int max_out, int fast);

/** The two bits (c(2m) | c(2m+1) << 1) the sequence with state `cinit` puts on pilot m: lets a caller test which pilots of a window or
 *  carrier agree with a solved sequence (per-RB occupancy). */
int nr_dmrs_nid_predict_bits(uint32_t cinit, int m);

/** c_init for a given N_ID (the forward map), exposed for tests and callers that want to verify a candidate. */
uint32_t nr_dmrs_nid_cinit(int nid, int slot, int symbol, int symbols_per_slot, int nscid);

/** Live observer (nr_dmrs_nid_solve_live.c), default OFF: ISAC_DMRS_NID_SOLVE=1 solves N_ID from FFT'd DL symbols on its own thread and logs
 *  "SENSING: DMRS_NID_SOLVE leader/summary" lines; it changes nothing else in the receiver. Called from the NID capture path with the
 *  symbol the GPU sweep also gets: rx = first FFT bin array of the symbol (fft bins), fco = first carrier bin, nsc = carrier subcarriers,
 *  pa = Point A subcarrier on that grid. Cheap no-op when off. */
struct nr_pdcch_nid_record_s;
void nr_dmrs_nid_live_offer(const void *rx_c16, int fft, int fco, int nsc, int pa, int slot, int sym);

/** ISAC_DMRS_NID_SOLVE=3 (solver primary). Pure helpers, unit tested.
 *  top2: the (at most 2) ids with >= minv votes, most votes first. A DCI-less id with many votes takes one slot, never both. */
static inline int nr_dmrs_nid_top2(const uint32_t *votes, int n, uint32_t minv, int out[2])
{
  int c = 0;
  for (int r = 0; r < 2; r++) {
    int best = -1;
    for (int i = 0; i < n; i++)
      if (votes[i] >= minv && (c == 0 || i != out[0]) && (best < 0 || votes[i] > votes[best])) best = i;
    if (best < 0) break;
    out[c++] = best;
  }
  return c;
}
/** On-demand sweep trigger: 1 when the GPU nID sweep must run now. published = a solver hint or a sweep record exists; verified = a CRC-valid
 *  DCI confirmed it; wake = unexplained PDCCH occupancy edge (always runs). No result: held for grace_us, then every call (fallback).
 *  Unverified result: every maintain_us. Verified: every recheck_us (slow re-verify). Times in us; last_run 0 = never ran. */
static inline int nr_dmrs_nid_sweep_demand(int published, int verified, int wake, uint64_t now, uint64_t first, uint64_t last_run, uint64_t grace,
                                           uint64_t maintain, uint64_t recheck)
{
  if (wake) return 1;
  if (!published) return now - first >= grace;
  if (last_run == 0) return !verified;
  return now - last_run >= (verified ? recheck : maintain);
}

#ifdef __cplusplus
}
#endif
#endif
