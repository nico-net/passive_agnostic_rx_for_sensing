/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* No-chance gate shared by DL and UL (TAIL-FIX-20261019). A grant whose DM-RS SNR is more than NR_SNR_GATE_MARGIN_DB below the SNR a decode needs is not
 * demodulated or LDPC-decoded and is not retried: its verdict is CRC fail with skip=1. required_snr_db is the exact formula of
 * tests/passive_rx/goal_metrics.py: 10 log10(2^(Qm R/1024) - 1) + 1.5 dB, R from TS 38.214 tables 5.1.3.1-1/2/3 (tbl 0 = 64QAM, 1 = 256QAM, 2 = 64QAM low SE;
 * an unknown tbl reads the 64QAM table, as the script does). Header-only: the DL decoder, the UL decoder and the unit test include this one copy. */
#ifndef NR_SNR_GATE_H
#define NR_SNR_GATE_H
#include <math.h>
#include <stdbool.h>

#define NR_SNR_GATE_MARGIN_DB 5.0
#define NR_SNR_GATE_IMPL_LOSS_DB 1.5

typedef struct { unsigned char qm; float r1024; } nr_snr_gate_mcs_t;
static const nr_snr_gate_mcs_t nr_snr_gate_t64[] = {
    {2, 120}, {2, 157}, {2, 193}, {2, 251}, {2, 308}, {2, 379}, {2, 449}, {2, 526}, {2, 602}, {2, 679}, {4, 340}, {4, 378}, {4, 434}, {4, 490}, {4, 553},
    {4, 616}, {4, 658}, {6, 438}, {6, 466}, {6, 517}, {6, 567}, {6, 616}, {6, 666}, {6, 719}, {6, 772}, {6, 822}, {6, 873}, {6, 910}, {6, 948}};
static const nr_snr_gate_mcs_t nr_snr_gate_t256[] = {
    {2, 120}, {2, 193}, {2, 308}, {2, 449}, {2, 602}, {4, 378}, {4, 434}, {4, 490}, {4, 553}, {4, 616}, {4, 658}, {6, 466}, {6, 517}, {6, 567},
    {6, 616}, {6, 666}, {6, 719}, {6, 772}, {6, 822}, {6, 873}, {8, 682.5f}, {8, 711}, {8, 754}, {8, 797}, {8, 841}, {8, 885}, {8, 916.5f}, {8, 948}};
static const nr_snr_gate_mcs_t nr_snr_gate_tlow[] = {
    {2, 30}, {2, 40}, {2, 50}, {2, 64}, {2, 78}, {2, 99}, {2, 120}, {2, 157}, {2, 193}, {2, 251}, {2, 308}, {2, 379}, {2, 449}, {2, 526}, {2, 602},
    {4, 340}, {4, 378}, {4, 434}, {4, 490}, {4, 553}, {4, 616}, {6, 438}, {6, 466}, {6, 517}, {6, 567}, {6, 616}, {6, 666}, {6, 719}, {6, 772}};

/* NAN for a reserved / out-of-range MCS (retransmission-only indices): such a grant is never gated */
static inline double nr_required_snr_db(int mcs, int tbl)
{
  const nr_snr_gate_mcs_t *t = tbl == 1 ? nr_snr_gate_t256 : tbl == 2 ? nr_snr_gate_tlow : nr_snr_gate_t64;
  const int n = tbl == 1 ? (int)(sizeof nr_snr_gate_t256 / sizeof *t) : tbl == 2 ? (int)(sizeof nr_snr_gate_tlow / sizeof *t)
                                                                             : (int)(sizeof nr_snr_gate_t64 / sizeof *t);
  if (mcs < 0 || mcs >= n) return NAN;
  return 10.0 * log10(pow(2.0, t[mcs].qm * (double)t[mcs].r1024 / 1024.0) - 1.0) + NR_SNR_GATE_IMPL_LOSS_DB;
}

/* snr_db: DM-RS SNR (dB) of the grant, DL comb or UL snr_db; <= -90 or non-finite = no estimate, never gated */
static inline bool nr_snr_no_chance(double snr_db, int mcs, int tbl)
{
  const double req = nr_required_snr_db(mcs, tbl);
  return isfinite(snr_db) && snr_db > -90.0 && isfinite(req) && snr_db < req - NR_SNR_GATE_MARGIN_DB;
}

/* whether a grant is exempt from the gate: probes (discovery), SI/P/RA grants, and, with HARQ combining on, retransmissions (their soft bits are needed) */
static inline bool nr_snr_gate_applies(bool probe, bool dedicated, int rv, bool harq_combine)
{
  return !probe && dedicated && !(rv != 0 && harq_combine);
}
#endif
