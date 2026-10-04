/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_PRACH_PASSIVE_H
#define NR_PRACH_PASSIVE_H

#include <stdbool.h>
#include <stdint.h>
#include "PHY/defs_nr_UE.h"

typedef struct {
  int format;
  int start_symbol;
  int num_occasions;
  int duration;
} nr_prach_passive_occasion_t;

/* Broadcast PRACH table lookup for a TDD slot. */
bool nr_prach_passive_occasion(int config_index, int frame, int slot, int mu, frequency_range_t range,
                               nr_prach_passive_occasion_t *out);
/* -1: RAR is unrelated, 0: this occasion's RAR has another RAPID, 1: RAPID matches. */
int nr_prach_passive_rar_relation(int frame, int slot, int start_symbol, int fdm,
                                  int preamble, uint16_t ra_rnti, uint8_t rapid);
void nr_prach_passive_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc);
void nr_prach_passive_start(void); /* call before UE slot threads start */
void nr_prach_passive_rar(uint16_t ra_rnti, uint8_t rapid, uint16_t ta, uint64_t abs_slot);
typedef struct { uint8_t rapid; uint16_t ta; } nr_prach_rar_entry_t;
int nr_prach_passive_rar_entries(const uint8_t *tb, uint32_t len, nr_prach_rar_entry_t *out, int capacity);
/* Call only after nr_passive_mac_rar_ta has validated the whole transport block. */
void nr_prach_passive_rar_pdu(uint16_t ra_rnti, uint64_t abs_slot, const uint8_t *tb, uint32_t len);
void nr_prach_passive_flush(void);
void nr_prach_passive_metrics(uint64_t *prach_us, uint64_t *prach_drops, uint64_t *prach_skipped);
void nr_prach_passive_gain_changed(int gain_db);
#ifdef NR_PASSIVE_PRACH_TEST
int nr_prach_passive_rar_age_test(uint64_t detection_abs, uint64_t rar_abs, int slots_per_frame);
int nr_prach_passive_ra_slot_test(int slot, int mu, int format);
void nr_prach_passive_format_test(int relation, char *line, uint32_t capacity);
void nr_prach_passive_floor_reset_test(void);
int nr_prach_passive_floor_test(int fdm, int gain_db, int energy);
void nr_prach_passive_test_reset(void);
void nr_prach_passive_test_prior(int index, bool rach_valid, bool ignore_sib1);
void nr_prach_passive_test_drain(void);
void nr_prach_passive_test_threads(unsigned *slot_calls, unsigned *worker_calls, bool *same_thread);
void nr_prach_passive_test_heavy_calls(unsigned *ru, unsigned *correlator, unsigned *exported);
unsigned nr_prach_passive_test_start_calls(void);
#endif

#endif
