/* Deterministic mid-stream changes shared by the hidden-waveform transmitter and
 * the offline receiver replay. Slots are absolute at mu=1 (2000 slots/s). */
#ifndef HIDDEN_RECONFIG_SCHEDULE_H
#define HIDDEN_RECONFIG_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
  HIDDEN_RECONF_NONE = 0,
  HIDDEN_RECONF_DCI_LENGTH,
  HIDDEN_RECONF_CORESET_MOVE,
  HIDDEN_RECONF_SIB1_SEMANTIC,
  HIDDEN_RECONF_PCI
} hidden_reconfig_kind_t;

typedef struct {
  hidden_reconfig_kind_t kind;
  uint32_t slot;
  uint8_t dci_length_add;
  uint8_t old_coreset_group, new_coreset_group;
  uint16_t old_tac, new_tac;
  uint16_t pci_add;
} hidden_reconfig_schedule_t;

static inline hidden_reconfig_schedule_t hidden_reconfig_schedule(hidden_reconfig_kind_t kind)
{
  hidden_reconfig_schedule_t s = {kind, 0, 4, 0, 8, 0x1579, 0x157a, 1};
  switch (kind) {
    case HIDDEN_RECONF_DCI_LENGTH: s.slot = 2000; break;
    case HIDDEN_RECONF_CORESET_MOVE: s.slot = 2400; break;
    case HIDDEN_RECONF_SIB1_SEMANTIC: s.slot = 2800; break;
    case HIDDEN_RECONF_PCI: s.slot = 3200; break;
    default: break;
  }
  return s;
}

static inline bool hidden_reconfig_hard(hidden_reconfig_kind_t kind)
{
  return kind == HIDDEN_RECONF_SIB1_SEMANTIC || kind == HIDDEN_RECONF_PCI;
}

static inline hidden_reconfig_kind_t hidden_reconfig_parse(const char *value)
{
  if (!value) return HIDDEN_RECONF_NONE;
  if (!strcmp(value, "dci_length")) return HIDDEN_RECONF_DCI_LENGTH;
  if (!strcmp(value, "coreset_move")) return HIDDEN_RECONF_CORESET_MOVE;
  if (!strcmp(value, "sib1_semantic")) return HIDDEN_RECONF_SIB1_SEMANTIC;
  if (!strcmp(value, "pci")) return HIDDEN_RECONF_PCI;
  return HIDDEN_RECONF_NONE;
}

static inline const char *hidden_reconfig_name(hidden_reconfig_kind_t kind)
{
  switch (kind) {
    case HIDDEN_RECONF_DCI_LENGTH: return "dci_length";
    case HIDDEN_RECONF_CORESET_MOVE: return "coreset_move";
    case HIDDEN_RECONF_SIB1_SEMANTIC: return "sib1_semantic";
    case HIDDEN_RECONF_PCI: return "pci";
    default: return "none";
  }
}

#endif
