/* ISAC_UL_LLR_DUMP=<dir> (default off): offline dump of wide real UL grants' LLRs for the U4 n_ID brute force
 * (docs/superpowers/plans/2026-10-17-u4-ul-dataid-search.md, Task 0.1). Tier 1 only: the int16 LLRs exactly as the
 * decoder saw them (descrambled with tried_id), plus a text sidecar. Offline, the scrambled-neutral LLR is
 * llr * sign(c(tried_id, rnti)), then * sign(c(candidate, rnti)).
 * The hook never blocks: it copies into a free slot of a small ring or drops; a background thread (SCHED_OTHER) writes. */
#ifndef NR_UL_LLR_DUMP_H
#define NR_UL_LLR_DUMP_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool probe;      /* solver probe, layout probe or layout CRC trial: never dumped */
  bool crc_ok;     /* TB CRC passed with tried_id (control grants, separate small cap) */
  float snr_db;    /* DM-RS SNR; NaN never passes */
  uint16_t pci, rnti, tried_id;
  uint8_t dci_format;       /* 0 = DCI 0_1, 1 = DCI 0_0 (nr_blind_ul_dci_format_t) */
  int cce, al;              /* -1 = unknown (the grant carries neither) */
  const char *search_space; /* "css" | "uss" | "unknown" */
  uint32_t frame;
  uint8_t slot;
  uint64_t abs_slot;
  uint16_t start_prb, num_rb;
  uint8_t start_sym, num_sym, mcs, mcs_table, qm, rv, ndi, harq_pid, nl, tp, dmrs_type, cdm_groups, nscid, bg;
  uint16_t dmrs_symb_pos, dmrs_ports, dmrs_id, R;
  uint32_t tbs_bits, G, unav_res;
  int delay, ta;
} nr_ul_llr_dump_rec_t;

/* getenv once; false when ISAC_UL_LLR_DUMP is unset/empty */
bool nr_ul_llr_dump_enabled(void);
/* 1 = queued, 0 = gate/cap/busy (nothing touched) */
int nr_ul_llr_dump_maybe(const nr_ul_llr_dump_rec_t *rec, const int16_t *llr);
/* wait until the queue is drained (tests, shutdown) */
void nr_ul_llr_dump_flush(void);
/* tests: re-read the environment and zero the counters (flushes first) */
void nr_ul_llr_dump_reset(void);
#endif
