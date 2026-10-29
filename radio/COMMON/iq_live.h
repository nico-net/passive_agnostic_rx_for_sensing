/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef IQ_LIVE_H
#define IQ_LIVE_H
/* MPCI-LIVE (plan task A12): live cell changes applied at run time, on the multi-cell wideband path (radio -> iq_feed -> iq_chan -> cell rings).
 * The receiver calls this module from the inventory control thread when the manager reports a removal, an addition into a prepared slot or a retune of a cell;
 * the module owns the DECISIONS and their ORDER, the receiver supplies the actions as hooks (so the unit test drives the same code with fakes):
 *
 *   slot states: EMPTY    prepared before the RX fence (UE instance, RU on its ring, channelizer entry, threads), holds no cell; its ring is paused, its slot thread parked
 *                SERVED   holds a cell, its channelizer entry runs, its UE is tuned to the cell
 *                UNSERVED holds a cell that the running capture cannot carry (admitted, ring paused, logged); it is retried on every change
 *                PARKED   the cell left (retire after the hysteresis): entry paused, UE parked, per-cell state kept for a re-activation
 *   Nothing is allocated here or by any hook of the receiver at run time: slots, rings, entries and threads exist since start-up (A8 fence).
 *
 *   remove:  chan_pause -> state PARKED (the slot thread parks in its read, outside the radio gate: ru_park.h; the CHAN demand follows the running entries: iq_chan.c).
 *   add:     the cell must have the slot's prepared geometry (N_RB, SCS, RX count <= the slot's): else UNSERVED reason=geometry.
 *            Fits the running capture (iq_span_fit) -> chan_retune (entry, paused) -> ue_retune (ring RU centre + UE mailbox: SSB, CFO, resync of THIS cell's SFO) ->
 *            chan_resume -> wake: SERVED. Does not fit -> re-plan the span (iq_span_plan over the served cells + this one): accepted only when it needs no change of
 *            the capture RATE and every served cell stays served; then radio_recentre(new centre) once (bounded radio retune: the LO moves, the wideband ring starts a new
 *            run, every cell resyncs once), else the cell stays UNSERVED with the reason and the radio is untouched.
 *   retune:  a SERVED cell whose centre / SSB / CFO changed: centre inside the capture -> pause, chan_retune, ue_retune, resume (a CFO or SSB-only change skips the
 *            channelizer); outside -> the re-plan rule of add; not accepted -> PARKED/UNSERVED (the old tuning no longer describes the cell).
 * Every transition is logged "MPCI_LIFE cell=<k> id=<id> event=<..> ..." through ops.log. Not thread safe: the control thread owns it. No environment variable, no option. */
#include "iq_span.h"
#ifdef __cplusplus
extern "C" {
#endif
#define IQ_LIVE_MAX_SLOTS IQ_SPAN_MAX_CELLS
#define IQ_LIVE_ID_MAX 40

typedef enum { IQ_LIVE_EMPTY = 0, IQ_LIVE_SERVED, IQ_LIVE_UNSERVED, IQ_LIVE_PARKED } iq_live_state_t;

typedef struct {
  double centre_hz;  /* the cell's grid centre (UE -C) */
  double cfo_hz;     /* initial carrier offset seed */
  int ssb;           /* SSB first subcarrier in the cell's grid (UE --ssb) */
  int n_rb;          /* grid N_RB (0 = the slot's) */
  double scs_hz;     /* 0 = the slot's */
  unsigned n_rx;     /* RX channels the cell uses, 0 = the slot's */
} iq_live_tune_t;

typedef struct {
  /* hooks return 0 on success; they run on the calling (control) thread */
  int (*chan_pause)(void *ctx, int slot);
  int (*chan_resume)(void *ctx, int slot);
  int (*chan_retune)(void *ctx, int slot, double centre_hz);               /* entry of `slot` to a new cell centre inside the running capture (entry paused) */
  int (*ue_retune)(void *ctx, int slot, const iq_live_tune_t *t);          /* ring RU centre + UE mailbox (SSB, CFO, resync of this cell only) */
  void (*wake)(void *ctx);                                                 /* wake the parked slot threads */
  int (*radio_recentre)(void *ctx, double centre_hz);                      /* same rate: move the LO, start a new wideband run; != 0: failed, the radio unchanged */
  void (*span_changed)(void *ctx, const iq_span_plan_t *p);                /* the running capture changed (after a successful recentre) */
  void (*log)(void *ctx, const char *line);
  void *ctx;
} iq_live_ops_t;

typedef struct {
  int state;                  /* iq_live_state_t */
  char id[IQ_LIVE_ID_MAX];
  iq_live_tune_t geom;        /* prepared geometry (n_rb, scs, n_rx) */
  iq_live_tune_t cur;         /* the tuning IN FORCE of the slot's channelizer entry and UE (what the hooks last applied) */
  iq_live_tune_t want;        /* the cell's tuning as the inventory describes it (UNSERVED: what is waiting for a capture that carries it) */
  double rate;                /* the cell's own sample rate (fixed by the prepared frame parameters) */
  int reason;                 /* UNSERVED: iq_span_reason_t, or an IQ_LIVE_REASON_* */
} iq_live_slot_t;
#define IQ_LIVE_REASON_GEOMETRY 100 /* the cell's grid is not the slot's prepared one */
#define IQ_LIVE_REASON_RATE 101     /* serving it needs another capture rate: no radio restart at run time */
#define IQ_LIVE_REASON_RADIO 102    /* the radio retune failed, the capture is unchanged */
#define IQ_LIVE_REASON_FAIL 103     /* a channelizer / UE hook failed */

typedef struct {
  iq_span_dev_t dev;
  iq_span_plan_t plan;        /* the capture in force */
  iq_live_ops_t ops;
  iq_live_slot_t slot[IQ_LIVE_MAX_SLOTS];
  int n;
  int recentres;              /* radio retunes done */
} iq_live_t;

const char *iq_live_reason_name(int reason);
/* `plan` = the capture in force at start (iq_span plan; cell[k].served says which slots carry data), `dev` = what the radio reported. */
void iq_live_init(iq_live_t *L, const iq_span_dev_t *dev, const iq_span_plan_t *plan, const iq_live_ops_t *ops);
/* A slot prepared before the fence. `cur` = the tuning its entry and UE are built with; holds_cell = 1: it holds a cell now (SERVED or UNSERVED by
 * plan.cell[slot].served); 0: EMPTY (a spare; its entry is paused). `rate` = the slot's sample rate. */
void iq_live_slot_prepare(iq_live_t *L, int slot, const char *id, const iq_live_tune_t *geom, const iq_live_tune_t *cur, double rate, int holds_cell);
/* Retry the UNSERVED cells against the capture in force (after a removal the re-plan may accept what it refused). Returns the number now served. */
int iq_live_retry(iq_live_t *L);
/* The three transitions. Return the new state, or -1 on a bad slot. */
int iq_live_remove(iq_live_t *L, int slot, const char *why);
int iq_live_add(iq_live_t *L, int slot, const char *id, const iq_live_tune_t *t);
int iq_live_retune(iq_live_t *L, int slot, const iq_live_tune_t *t);
int iq_live_state(const iq_live_t *L, int slot);
int iq_live_served(const iq_live_t *L);
/* "MPCI_LIVE served=s/n slot0=<id>:served,... span=<centre MHz>/<rate MS/s> recentres=r" */
int iq_live_format(const iq_live_t *L, char *buf, unsigned n);
#ifdef __cplusplus
}
#endif
#endif
