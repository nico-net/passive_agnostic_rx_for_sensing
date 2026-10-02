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

uint32_t nr_cfg_epoch_current(void); /* lock-free atomic read */
uint32_t nr_cfg_epoch_identity_gen(void); /* increments only on HARD_RESET */
nr_cfg_epoch_snapshot_t nr_cfg_epoch_snapshot(void);
void nr_cfg_epoch_note_identity(uint16_t pci, uint64_t ssb_arfcn, uint64_t point_a);
void nr_cfg_epoch_note_mib(uint32_t mib_hash_without_sfn);
void nr_cfg_epoch_note_sib1(uint32_t semantic_hash); /* called whenever SIB1 is decoded (SA or NSA), never required */
void nr_cfg_epoch_note_si_modification(uint64_t abs_slot, uint32_t modification_period_slots);
void nr_cfg_epoch_note_continuity_loss(void);
void nr_cfg_epoch_note_bwp_change(void);
void nr_cfg_epoch_note_csirs_map_change(void); /* source must confirm a changed map first */
void nr_cfg_epoch_note_rnti_reopened(uint16_t rnti, bool was_converged, uint64_t abs_slot);
void nr_cfg_epoch_tick(uint64_t abs_slot);
typedef void (*nr_cfg_epoch_listener_t)(const nr_cfg_epoch_snapshot_t *);
void nr_cfg_epoch_subscribe(nr_cfg_epoch_listener_t fn); /* duplicate subscriptions ignored */

/* R8 sets the cell numerology (1000, 2000, 4000, ... slots/s); default 1000.
 * This only controls the two-second dedicated-change window. */
void nr_cfg_epoch_set_slots_per_second(uint32_t slots_per_second);
/* Clears all state and listeners; intended for a new receiver lifetime and unit-test isolation. */
void nr_cfg_epoch_reset(void);

#ifdef __cplusplus
}
#endif
#endif
