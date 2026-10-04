#ifndef NR_PASSIVE_CFG_SOURCES_H
#define NR_PASSIVE_CFG_SOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* FNV-1a over ordered, decoded integer fields. Never hash ASN.1 allocation or wire padding. */
uint64_t nr_cfg_identity_frequency(uint64_t hz, unsigned scs_hz);
uint32_t nr_cfg_semantic_start(void);
uint32_t nr_cfg_semantic_add(uint32_t hash, uint64_t value);
uint32_t nr_cfg_semantic_bits(uint32_t hash, const uint8_t *bytes, size_t nbits);
uint32_t nr_cfg_mib_hash(unsigned scs, unsigned k_ssb, unsigned dmrs_type_a,
                         unsigned coreset0, unsigned search_space0,
                         unsigned cell_barred, unsigned intra_freq_reselection);
bool nr_cfg_prnti_si_credible(unsigned indicator, unsigned message, unsigned mismatches, unsigned threshold);
bool nr_cfg_prnti_si_modified(unsigned short_messages_indicator, unsigned short_messages);
uint32_t nr_cfg_si_period_slots(unsigned modification_coefficient, unsigned paging_cycle_frames,
                                unsigned slots_per_frame);
bool nr_cfg_sib1_window_expired(uint64_t abs_slot, uint64_t start, unsigned occasions, unsigned slots_per_second);
bool nr_cfg_sib1_redecode_due(uint64_t abs_slot, uint64_t last_request_slot, unsigned slots_per_second);
void nr_cfg_sib1_cache_name(char *out, size_t capacity, const char *directory,
                            uint16_t pci, uint32_t semantic_hash, bool reconf);

#ifdef __cplusplus
}
#endif

#endif
