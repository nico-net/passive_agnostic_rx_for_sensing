/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BLINDPIPE 2026-10-13: DL traffic probe -- tells "no UE-specific traffic on this cell" from "traffic present but the
 * dedicated CORESET is not found". Pure energy statistics per (OFDM symbol, RB) of one antenna, no DM-RS sequence, no cell
 * assumption: a cell is OCCUPIED when its energy exceeds the noise floor by the chi-square(24 dof: 12 REs x I/Q) 1e-4
 * tail; the floor is the 10th percentile of the slot's cells (noise p10 = 0.6524 x mean, so floor_mean = p10 / 0.6524).
 * Masked out: RBs outside the SIB1 carrier, the SSB RBs, CORESET#0 (control symbols) and every PDSCH grant the receiver
 * decoded (registered by nr_dltp_note_grant from the PDSCH queue). What is left and lit is traffic the receiver does
 * not explain: control-region occupancy outside CSS0 (symbols 0-2) and PDSCH-like RBs (>= 3 consecutive lit data
 * symbols, which a 1-symbol CSI-RS or a 2-symbol TRS pair cannot produce). Confounders (co-channel cells on the same
 * carrier, UL slots when the TDD pattern is unknown) are reported, not hidden: see the TRAFFIC log line. */
#ifndef NR_DL_TRAFFIC_PROBE_H
#define NR_DL_TRAFFIC_PROBE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_DLTP_NSYM 14
#define NR_DLTP_MAX_RB 275
#define NR_DLTP_P10_RATIO 0.6524f      /* chi2_24 p10 / mean */
#define NR_DLTP_OCC_RATIO 2.4422f      /* chi2_24 q0.9999 / mean: per-cell false alarm 1e-4 */
#define NR_DLTP_PDSCH_RUN 3            /* lit consecutive data symbols that make an RB PDSCH-like */
#define NR_DLTP_GRANTS 4096

typedef struct {
  int n_rb;                       /* receiver grid */
  int carrier_rb0, carrier_rb1;   /* [rb0, rb1): SIB1 carrier in grid RBs (grid when unknown) */
  int css0_rb0, css0_rb1, css0_nsym; /* CORESET#0 RBs and duration (0 = unknown: none masked) */
  int ssb_rb0, ssb_rb1;           /* SSB band, masked in every slot (timing not needed) */
} nr_dltp_geom_t;

typedef struct { uint16_t frame; uint8_t slot; uint16_t rb0, n_rb; uint8_t s, l; } nr_dltp_grant_t;

typedef struct {
  uint32_t slots, slots_tdd_unknown;
  uint32_t ctrl_slots[3];          /* slots with >= 1 ungranted lit carrier RB in symbol s outside CSS0 */
  uint64_t ctrl_rbs[3];
  uint64_t cells, cells_lit, cells_granted_lit, cells_ungranted_lit; /* data symbols 3..13 of carrier RBs minus SSB */
  uint64_t pdsch_like, pdsch_like_ungranted;  /* RB-slots */
  uint64_t pdsch_like_ungranted_nocss0;       /* ACQMISS: the same outside the CORESET#0 RBs (SIB1/SI/paging PDSCH live there and are not registered as grants) */
  uint32_t slots_ungranted_pdsch;
  uint32_t grants_used;
  float floor_mean;                /* last slot's floor (energy units) */
} nr_dltp_stats_t;

/* |IQ|^2 summed over the 12 REs of every (symbol, RB): rxF is the FEP'd slot, int16 I/Q interleaved, 14 symbols of fft
 * bins, subcarrier k of RB r at bin (fco + 12 r + k) % fft. e has NR_DLTP_NSYM * n_rb floats, symbol-major. */
void nr_dltp_energy(const int16_t *rxF, int fft, int fco, int n_rb, float *e);
/* Noise floor mean of one slot from the 10th percentile of its cells (scratch: n floats). */
float nr_dltp_floor(const float *e, int n, float *scratch);
/* Accumulates one slot. grants: the registered grants of THIS slot (nr_dltp_grants_for). */
void nr_dltp_slot(const float *e, const nr_dltp_geom_t *g, float floor_mean, bool tdd_known,
                  const nr_dltp_grant_t *grants, int n_grants, nr_dltp_stats_t *st);
/* Verdict: 1 traffic present (ungranted PDSCH-like RB-slots far above the false-alarm bound), -1 absent (enough
 * slots, none), 0 undecided. Writes the TRAFFIC log text. */
int nr_dltp_verdict(const nr_dltp_stats_t *st, char *buf, size_t n);

/* Grant registry (lock-free single writer per call under its own mutex): PDSCH allocations the receiver decoded. */
void nr_dltp_note_grant(uint16_t frame, uint8_t slot, int rb0, int n_rb, int s, int l);
int nr_dltp_grants_for(uint16_t frame, uint8_t slot, nr_dltp_grant_t *out, int cap);

/* ACQMISS 2026-10-13 (ISAC_CORESET_DISC_WATCH=1, default off): "missed it" vs "no traffic" for the dedicated CORESET.
 * Pure state machine fed by the probe worker every TRAFFIC line. Inputs are receiver-side only (bank count, probe verdict,
 * carrier verdict, time): no gNB information, no cell constant. MISSED = the probe sees PDSCH-like occupancy that no decoded
 * grant explains (UE traffic exists) while no dedicated CORESET is banked miss_s seconds after the nID sweep could start.
 * NO_TRAFFIC = enough probed slots and none lit. Recovery is requested at most max_recover times with linear back-off. */
typedef enum { NR_CDISC_SEARCHING = 0, NR_CDISC_LOCKED, NR_CDISC_NO_TRAFFIC, NR_CDISC_MISSED, NR_CDISC_SYNC_POOR } nr_cdisc_state_t;
typedef struct {
  nr_cdisc_state_t state;
  int armed;                 /* carrier verdict seen: the sweep can run */
  double armed_s, last_recover_s, poor_since_s, last_reacquire_s;
  int recoveries, was_poor, reacquires;
} nr_cdisc_t;
typedef struct {
  double now_s;
  int carrier_ok;            /* nr_passive_acq carrier_verified == 1 */
  int bank_count;            /* dedicated CORESETs banked */
  uint64_t window_probed, window_rbslots; /* probed slots / ungranted PDSCH-like RB-slots outside CORESET#0 since the sweep was armed */
  int sync_poor;             /* ISAC_CORESET_DISC_SYNCQ: nr_syncq_status() == POOR */
  double miss_s;             /* grace after arming before declaring MISSED */
  int max_recover;
} nr_cdisc_in_t;
typedef struct { int changed, recover, traffic, reacquire, rearm; const char *reason; } nr_cdisc_out_t;
#define NR_CDISC_RBSLOTS_PRESENT 64   /* 8 x the probe's own leakage bound (8) */
#define NR_CDISC_RATE_PRESENT 0.5     /* RB-slots per probed slot: sustained, not a lone burst (engineering floor, not fitted to a cell) */
#define NR_CDISC_PROBED_ABSENT 100    /* sampling depth, as nr_dltp_verdict */
void nr_cdisc_step(nr_cdisc_t *c, const nr_cdisc_in_t *in, nr_cdisc_out_t *out);

/* ACQMISS-2 sync-quality metric (ISAC_CORESET_DISC_SYNCQ=1, default off). Input: the magnitude in samples of every PBCH-based timing step
 * (nr-ue.c, one per SSB period). Window = last NR_SYNCQ_WIN steps; needs NR_SYNCQ_MIN to decide. Three levels:
 *   GOOD (0), DEGRADED (1: logged only), POOR (2: SYNC_POOR state, nID capture paused, sync re-acquired).
 * Thresholds, from the OTA run population (636 runs with >= 40 PBCH steps, first ~90 steps of each, all cells) and the replay injection:
 * healthy steady state is median 1 / p90 2 samples; the worst 32-step window of a run has median p50 1, p90 1, p99 21.6 and
 * p90-of-window p95 8, p99 54. DEGRADED above median 8 (measured: an injected +-20..60 sample per-frame wander gives median 11 and the
 * receiver still banks its CORESETs, so no action there); POOR above median 20 (~p99 of the population; an injected +-60..150 wander gives
 * median 25-29 and the carrier verdict never arrives) or p90 64. Release hysteresis: median <= 10 and p90 <= 32. No cell constant. */
#define NR_SYNCQ_WIN 32
#define NR_SYNCQ_MIN 16
#define NR_SYNCQ_MED_DEGRADED 8
#define NR_SYNCQ_MED_POOR 20
#define NR_SYNCQ_P90_POOR 64
#define NR_SYNCQ_MED_GOOD 10
#define NR_SYNCQ_P90_GOOD 32
#define NR_SYNCQ_CLAMP 16   /* dynamic PBCH step clamp while recovering: above the 6 samples/frame true drift, below the +-50 jumps */
/* Pure: classify a window. prev_poor gives the POOR-release hysteresis. Returns 2 POOR, 1 DEGRADED, 0 GOOD, -1 unknown (n < NR_SYNCQ_MIN). */
int nr_syncq_classify(const unsigned *mag, int n, int prev_poor, int *med, int *p90);
/* Runtime (single writer = the UE thread, readers lock-free). All no-ops unless ISAC_CORESET_DISC_SYNCQ=1. */
int nr_syncq_enabled(void);
void nr_syncq_note(int step);
int nr_syncq_status(int *med, int *p90);       /* 2 POOR, 1 DEGRADED, 0 GOOD, -1 unknown */
int nr_syncq_gate_closed(void);                /* 1 while POOR: the nID capture must not spend symbols */
void nr_syncq_request_reacquire(void);         /* from the CORESET_DISC machine */
int nr_syncq_take_reacquire(void);             /* nr-ue.c: consume once; resets max_pos_acc and engages the dynamic clamp */
int nr_syncq_clamp(void);                      /* 0 or NR_SYNCQ_CLAMP */
void nr_syncq_clamp_release(void);
#ifdef __cplusplus
}
#endif
#endif
