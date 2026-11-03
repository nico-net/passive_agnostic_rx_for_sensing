/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic-mode DCI record: one struct per accepted DCI, copied under the sense-ring mutex and formatted on the writer thread. */
#ifndef NR_TRAFFIC_RECORD_H
#define NR_TRAFFIC_RECORD_H
#include "nr_passive_obs.h"
#include "nr_sense_records.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NR_LAYOUT_FIXED = 0, NR_LAYOUT_SOLVED, NR_LAYOUT_CANDIDATE, NR_LAYOUT_UNSOLVED } nr_layout_state_t;
typedef enum { NR_DCI_DECODE_NONE = 0, NR_DCI_DECODE_QUEUED = 1 } nr_dci_decode_t;
typedef struct {
  nr_passive_obs_t o;            /* existing grant keys, filled as the post-decode callers do (crc = -1 here) */
  nr_sense_grant_meta_t m;       /* existing meta keys */
  uint8_t dci_format;            /* nr_blind_dci_format_t / UL 0_0, 0_1 */
  int8_t ss_type, al; int16_t cce, coreset_id; uint16_t dci_len;
  uint8_t dci_bits[18];          /* raw payload, MSB-first, dci_len bits (<= 140) */
  bool rnti_confirmed;           /* nr_pdcch_blind_monitor_rnti_confirmed() at accept */
  nr_layout_state_t layout_state; uint32_t layout_checks_ok, layout_checks_fail;
  uint32_t cfg_ref_ver;          /* with m.identity_gen + m.dedicated_epoch = cfg_ref */
  int16_t bwp_start, bwp_size, start_rb_bwp;  /* -1 unknown */
  int8_t vrb_prb, tda_index, k0, k1, dai, tpc, pri, srs_req, csi_req, ulsch_ind, cbgti, retx; /* -1 unknown */
  int8_t si_ind, short_msg_ind; int16_t short_msg; int8_t tb_scaling;   /* P/SI/RA only, -1 otherwise */
  int64_t pdsch_abs_slot, harq_fb_abs_slot;  /* -1 unknown */
  nr_dci_decode_t decode;
} nr_sense_dci_rec_t;
int nr_sense_dci_to_json(const nr_sense_dci_rec_t *r, char *buf, size_t n);   /* writer-thread formatter, pure */
void nr_sense_emit_dci(nr_sense_dci_rec_t *r);   /* stamps epoch/identity/t_mono like nr_sense_emit_grant_meta, then nr_sense_push_dci */
/* outcome of a probe/check decode: {"type":"decode","abs_slot","rnti","dir","probe","crc","snr_db","learned":...} */
void nr_sense_emit_decode(uint64_t abs_slot, uint16_t rnti, int dir, bool probe, int crc, float snr_db, const char *learned);
/* 1 queued, 0 dropped (ring full / no sink: counted in dci_lost), -1 API not open. Never blocks beyond the per-cell mu. */
int nr_sense_push_dci(const nr_sense_dci_rec_t *r);
#ifdef __cplusplus
}
#endif
#endif
