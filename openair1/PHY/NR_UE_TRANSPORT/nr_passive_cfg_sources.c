#include "nr_passive_cfg_sources.h"
#include <stdio.h>

uint32_t nr_cfg_semantic_start(void) { return UINT32_C(2166136261); }

uint32_t nr_cfg_semantic_add(uint32_t hash, uint64_t value)
{
  for (unsigned i = 0; i < 8; ++i) {
    hash ^= (uint8_t)(value >> (i * 8));
    hash *= UINT32_C(16777619);
  }
  return hash;
}

uint32_t nr_cfg_semantic_bits(uint32_t hash, const uint8_t *bytes, size_t nbits)
{
  hash = nr_cfg_semantic_add(hash, nbits);
  if (!bytes) return hash;
  for (size_t i = 0; i < nbits; ++i)
    hash = nr_cfg_semantic_add(hash, (bytes[i / 8] >> (7 - i % 8)) & 1u);
  return hash;
}

uint32_t nr_cfg_mib_hash(unsigned scs, unsigned k_ssb, unsigned dmrs_type_a,
                         unsigned coreset0, unsigned search_space0,
                         unsigned cell_barred, unsigned intra_freq_reselection)
{
  uint32_t h = nr_cfg_semantic_start();
  h = nr_cfg_semantic_add(h, scs);
  h = nr_cfg_semantic_add(h, k_ssb);
  h = nr_cfg_semantic_add(h, dmrs_type_a);
  h = nr_cfg_semantic_add(h, coreset0);
  h = nr_cfg_semantic_add(h, search_space0);
  h = nr_cfg_semantic_add(h, cell_barred);
  return nr_cfg_semantic_add(h, intra_freq_reselection);
}

bool nr_cfg_prnti_si_modified(unsigned indicator, unsigned message)
{
  /* TS 38.212 7.3.1.2.1: SMI 10/11 carries the 8-bit short message.
   * TS 38.331 6.5.1: its first (MSB) bit is systemInfoModification. */
  return (indicator == 2 || indicator == 3) && (message & 0x80u) != 0;
}

uint32_t nr_cfg_si_period_slots(unsigned coefficient, unsigned paging_cycle_frames, unsigned slots_per_frame)
{
  if (!coefficient || !paging_cycle_frames || !slots_per_frame) return 0;
  const uint64_t slots = (uint64_t)coefficient * paging_cycle_frames * slots_per_frame;
  return slots <= UINT32_MAX ? (uint32_t)slots : 0;
}

bool nr_cfg_sib1_redecode_due(uint64_t abs_slot, uint64_t last_request_slot, unsigned slots_per_second)
{
  return slots_per_second && abs_slot >= last_request_slot
      && abs_slot - last_request_slot >= 5u * (uint64_t)slots_per_second;
}

void nr_cfg_sib1_cache_name(char *out, size_t capacity, const char *directory,
                            uint16_t pci, uint32_t semantic_hash, bool reconf)
{
  const char *dir = directory && directory[0] ? directory : "/tmp/passive_rx";
  if (reconf)
    snprintf(out, capacity, "%s/sib1_common_pci%u_hash%08x.bin", dir, pci, semantic_hash);
  else
    snprintf(out, capacity, "%s/sib1_common_pci%u.bin", dir, pci);
}
