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

/** The two bits (c(2m) | c(2m+1) << 1) the sequence with state `cinit` puts on pilot m: lets a caller test which pilots of a window or
 *  carrier agree with a solved sequence (per-RB occupancy). */
int nr_dmrs_nid_predict_bits(uint32_t cinit, int m);

/** c_init for a given N_ID (the forward map), exposed for tests and callers that want to verify a candidate. */
uint32_t nr_dmrs_nid_cinit(int nid, int slot, int symbol, int symbols_per_slot, int nscid);

#ifdef __cplusplus
}
#endif
#endif
