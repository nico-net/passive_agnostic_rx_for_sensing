/* Single cell configuration epoch authority. Trigger sources and consumers are wired in R8–R11.
 * All inputs are receiver-observed evidence. Initial observations establish a baseline without a bump. */
#ifndef NR_PASSIVE_CFG_EPOCH_H
#define NR_PASSIVE_CFG_EPOCH_H

#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NR_EPOCH_SOFT = 0, NR_EPOCH_HARD_REVERIFY = 1, NR_EPOCH_HARD_RESET = 2 } nr_epoch_class_t;
typedef enum {
  NR_CAUSE_CELL_IDENTITY_CHANGE,
  NR_CAUSE_MIB_CHANGE,
  NR_CAUSE_SIB1_CHANGE,
  NR_CAUSE_SI_MODIFICATION_ANNOUNCED,
  NR_CAUSE_CONTINUITY_LOSS,
  NR_CAUSE_BWP_CHANGE,
  NR_CAUSE_DEDICATED_CHANGE_SUSPECTED,
  NR_CAUSE_CSIRS_MAP_CHANGE
} nr_epoch_cause_t;
typedef struct {
  uint32_t epoch;
  uint32_t identity_gen;
  nr_epoch_class_t last_class;
  nr_epoch_cause_t last_cause;
} nr_cfg_epoch_snapshot_t;

/* Thread-local provenance for synchronous feedback inside a queued/inline job.
 * Nested decoders inherit the original stamp; every stale job is counted once. */
typedef struct nr_cfg_epoch_work {
  struct nr_cfg_epoch_work *parent, *owner;
  uint32_t epoch;
  void *dropped_counter; /* uint64_t, atomically incremented */
  bool counted;
} nr_cfg_epoch_work_t;
void nr_cfg_epoch_work_begin(nr_cfg_epoch_work_t *work, uint32_t stamp, void *dropped_counter);
void nr_cfg_epoch_work_end(nr_cfg_epoch_work_t *work);
uint32_t nr_cfg_epoch_work_stamp(void);
bool nr_cfg_epoch_work_current(void);
#define NR_CFG_EPOCH_WORK(stamp, counter) \
  nr_cfg_epoch_work_t epoch_work __attribute__((cleanup(nr_cfg_epoch_work_end))); \
  nr_cfg_epoch_work_begin(&epoch_work, (stamp), (counter))

uint32_t nr_cfg_epoch_current(void); /* lock-free atomic read */
bool nr_cfg_reconf_enabled(void); /* ISAC_RECONF=1, read once */
bool nr_cfg_ignore_sib1(void); /* independent ISAC_TD_IGNORE_SIB1=1 test arm */
void nr_cfg_ignore_sib1_reset_for_test(void); /* test-only; call after changing the environment */
uint32_t nr_cfg_epoch_identity_gen(void); /* increments only on HARD_RESET */
nr_cfg_epoch_snapshot_t nr_cfg_epoch_snapshot(void);
void nr_cfg_epoch_note_identity(uint16_t pci, uint64_t ssb_arfcn, uint64_t point_a);
void nr_cfg_epoch_refine_point_a(uint64_t point_a); /* 0 = unknown; first SIB1 observation fills it */
void nr_cfg_epoch_note_mib(uint32_t mib_hash_without_sfn);
bool nr_cfg_epoch_note_sib1(uint32_t semantic_hash, uint64_t abs_slot); /* called whenever SIB1 is decoded (SA or NSA), never required */
void nr_cfg_epoch_note_si_modification(uint64_t abs_slot, uint32_t modification_period_slots);
void nr_cfg_epoch_note_continuity_loss(void);
/* Signed RF timestamp delta and samples in one millisecond; nonpositive deltas are hard. */
void nr_cfg_epoch_note_continuity_loss_samples(int64_t gap_samples, uint64_t samples_per_ms);
void nr_cfg_epoch_note_bwp_change(void);
void nr_cfg_epoch_note_csirs_map_change(void); /* source must confirm a changed map first */
void nr_cfg_epoch_note_rnti_reopened(uint16_t rnti, bool was_converged, uint64_t abs_slot);
void nr_cfg_epoch_tick(uint64_t abs_slot);
void nr_cfg_epoch_drain(void); /* ordered callbacks; caller MUST hold no receiver locks */
typedef void (*nr_cfg_epoch_listener_t)(const nr_cfg_epoch_snapshot_t *);
void nr_cfg_epoch_subscribe(nr_cfg_epoch_listener_t fn); /* duplicate subscriptions ignored */

/* R8 sets the cell numerology (1000, 2000, 4000, ... slots/s); default 1000.
 * This only controls the two-second dedicated-change window. */
void nr_cfg_epoch_set_slots_per_second(uint32_t slots_per_second);
void nr_cfg_epoch_set_si_period(uint32_t period_slots);
uint32_t nr_cfg_epoch_si_period(void);
uint64_t nr_cfg_epoch_si_boundary(void);
bool nr_cfg_epoch_sib1_request_allowed(uint64_t abs_slot);
bool nr_cfg_epoch_si_redecode_pending(uint64_t abs_slot);
uint64_t nr_cfg_epoch_observe_slot(uint16_t sfn, uint8_t slot, uint8_t slots_per_frame);
/* Clears all state and listeners; intended for a new receiver lifetime and unit-test isolation. */
void nr_cfg_epoch_reset(void);

#ifdef __cplusplus
}
#endif
#endif
