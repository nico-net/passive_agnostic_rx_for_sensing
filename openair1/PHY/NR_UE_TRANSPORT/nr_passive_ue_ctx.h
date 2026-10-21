/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* UeContext, schema uectx/1. This comment is the schema's single source of truth.
 * JSON Lines, append mode, three records in key order:
 * ue_snapshot: schema,type,t_mono_ns,identity_gen,rnti,incarnation,state,cfg,stats,n_changes,n_reconfigs.
 * ue_change: schema,type,t_mono_ns,abs_slot,epoch,identity_gen,rnti,incarnation,param,old,new,cause,evidence.
 * ue_reconfig: schema,type,t_mono_ns,abs_slot,epoch,identity_gen,rnti,incarnation,params,class.
 * cfg has every nr_ue_param_t name in enum order. An unknown value is JSON null; a known value is
 * {value,epoch_learned,verif,first_abs_slot,last_confirmed_abs_slot,source}. States and verification
 * values are uppercase strings. source is nr_ue_source_t. Stats keys follow nr_ue_stats_t
 * in declaration order; an unknown
 * EMA/range/slot is null. Packed values: TD_WINNER = S[0:3],L[4:7],k0[8:13],mapping[14],
 * dmrs_add_pos[15:16],dmrs_max_len[17],dmrs_mask[18:31]; BWP = start[0:15],size[16:31],scs_khz[32:47];
 * CORESET = first_rb[0:15],n_rb[16:31],duration[32:39]. Other scalar values retain their units.
 * APERIODIC_CSI changes carry the DCI 0_1 CSI-request value in new and the observed NZP CSI-RS
 * resource coincident with the triggering DCI slot (or null) in evidence; this does not identify
 * the resource selected by the CSI-request field. abs_slot is the triggering DCI slot. Resource keys are
 * row,freq_domain,start_rb,nr_of_rbs,symb_l0,scramb_id. It never emits ue_reconfig.
 * SIB1_HASH, SIB1_BWP and SIB1_TDRA_HASH are present only when an actual SIB1 has been decoded
 * (SA and NSA alike). SIB1_BWP uses the BWP packing above; TDRA_HASH is FNV-1a over common rows.
 * UE_ID (Msg4 Contention Resolution Identity, TS 38.321 5.1.5): kind<<48 | value, kind 1 random, 2 S-TMSI part 1, 3 resume identity (nr_reconf_id_kind_t).
 * REATTACH: (rnti<<16|incarnation) of the earlier incarnation of the same RNTI this one replaces (a different UE); the change cause is the evidence
 * (REATTACH_RAR / _MSG4 / _HARQ / _DCI_SIZE). RESUME_OF: (rnti<<16|incarnation) of an earlier context whose linkable UE_ID is equal (same UE, cause RESUME_LINK).
 * ue_reconfig class RRC_WINDOW_CLOSED: the 38.331 12 processing delay of an inferred RRC message elapsed. An incarnation is never reused for another UE.
 * The writer owns contexts; producers only copy bounded events into a lock-free MPSC ring. */
#ifndef NR_PASSIVE_UE_CTX_H
#define NR_PASSIVE_UE_CTX_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_passive_obs.h"
#include "nr_passive_cfg_epoch.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NR_UECTX_SCHEMA "uectx/1"
#define NR_UECTX_MAX_UE 1024
/* IDLE: silent for NR_RECONF_INACTIVE_NS (RRC_IDLE, RRC_INACTIVE or DRX cannot be told apart passively). INACTIVE: the same for a UE whose identity is a resume
 * identity (it has resumed from RRC_INACTIVE before). GONE after NR_RECONF_GONE_NS. Appended last: existing numeric values keep their meaning. */
typedef enum { NR_UE_FIRST_SEEN, NR_UE_ACTIVE, NR_UE_IDLE, NR_UE_GONE, NR_UE_INACTIVE } nr_ue_state_t;
typedef enum { NR_UEV_TRUSTED, NR_UEV_HINT, NR_UEV_SUSPECT } nr_ue_verif_t;
typedef enum { NR_UES_UNKNOWN, NR_UES_OBSERVATION, NR_UES_PDCCH, NR_UES_SWEEP,
               NR_UES_BWP, NR_UES_SIB1, NR_UES_RAR } nr_ue_source_t;
typedef enum {
  NR_UEP_RNTI_CLASS, NR_UEP_ANCHOR, NR_UEP_CORESET, NR_UEP_DCI_LEN_DL, NR_UEP_DCI_LEN_UL,
  NR_UEP_DCI_LEN_STATE, NR_UEP_PDCCH_SCR_ID, NR_UEP_PDSCH_SCR_ID, NR_UEP_TD_WINNER,
  NR_UEP_TD_STATE, NR_UEP_MCS_TABLE, NR_UEP_DMRS_CFG, NR_UEP_MAX_LAYERS, NR_UEP_LBRM,
  NR_UEP_BWP, NR_UEP_PUSCH_LAYOUT, NR_UEP_SIB1_HASH, NR_UEP_SIB1_BWP,
  NR_UEP_SIB1_TDRA_HASH, NR_UEP_APERIODIC_CSI,
  NR_UEP_UE_ID, NR_UEP_REATTACH, NR_UEP_RESUME_OF, NR_UEP_COUNT
} nr_ue_param_t;
typedef enum {
  NR_UEC_FIRST_LEARNED, NR_UEC_CONVERGED, NR_UEC_RELOCK, NR_UEC_REOPENED_NEW_WINNER,
  NR_UEC_BWP_CHANGE, NR_UEC_CORESET_CHANGE, NR_UEC_EPOCH_REVERIFIED,
  NR_UEC_EPOCH_DISCARDED, NR_UEC_HARD_RESET,
  NR_UEC_REATTACH_RAR, NR_UEC_REATTACH_MSG4, NR_UEC_REATTACH_HARQ, NR_UEC_REATTACH_DCI_SIZE,
  NR_UEC_RESUME_LINK, NR_UEC_RRC_WINDOW
} nr_ue_cause_t;
typedef struct {
  int64_t value;
  uint32_t epoch_learned;
  nr_ue_verif_t verif;
  int64_t first_abs_slot, last_confirmed_abs_slot;
  uint8_t source;
} nr_ue_cfg_value_t;
typedef struct {
  uint64_t grants_dl, grants_ul, crc_ok_dl, crc_ok_ul, bytes_dl, bytes_ul, retx_dl;
  uint32_t mcs_hist[32], layers_hist[5];
  float snr_ema_db, nvar_ema, fo_ema_hz, ta_ema_samples;
  int16_t prb_start_min, prb_start_max, prb_size_min, prb_size_max;
  double prb_size_mean;
  int64_t last_abs_slot;
} nr_ue_stats_t;
typedef enum { NR_UL_EVID_UNKNOWN, NR_UL_EVID_DL_PRIOR, NR_UL_EVID_PROVISIONAL,
               NR_UL_EVID_TB_CONFIRMED } nr_ue_ul_evidence_t;
typedef enum { NR_UL_F_DCI_LEN, NR_UL_F_LAYOUT, NR_UL_F_BWP, NR_UL_F_TDA,
               NR_UL_F_K2, NR_UL_F_DMRS_ID0, NR_UL_F_DMRS_ID1, NR_UL_F_DATA_ID,
               NR_UL_F_TA, NR_UL_F_CFO, NR_UL_F_COUNT } nr_ue_ul_field_t;
typedef struct { int64_t value; uint8_t evidence; uint32_t epoch; } nr_ue_ul_value_t;
typedef struct {
  nr_ue_ul_value_t field[NR_UL_F_COUNT];
  int64_t dl_bwp_prior; /* packed DL BWP, never a hard UL constraint; -1 unknown */
  int32_t dl_cfo_prior_hz;
  bool dl_cfo_prior_valid;
  int32_t ta_command_units; /* RAR absolute plus CE deltas, in 16*64/2^mu Tc units */
  uint8_t ta_mu;
  bool ta_command_valid;
  uint16_t id_priors[3]; /* PCI, DL PDSCH ID, PDCCH nID; ranking hints */
  uint8_t n_id_priors;
} nr_ue_ul_state_t;
typedef struct { uint32_t identity_gen, epoch; uint16_t incarnation, rnti; } nr_ue_ul_ticket_t;
typedef struct {
  uint16_t dci_len, layout, bwp_start, bwp_size, dmrs_id, data_id;
  uint8_t nscid, tda, k2;
  int32_t ta_samples, cfo_hz;
  uint16_t pci;
} nr_ue_ul_update_t;
typedef struct {
  uint32_t identity_gen;
  uint16_t rnti, incarnation;
  nr_ue_state_t state;
  nr_ue_cfg_value_t cfg[NR_UEP_COUNT];
  nr_ue_stats_t st;
  nr_ue_ul_state_t ul;
  uint32_t n_changes, n_reconfigs;
} nr_ue_ctx_t;
typedef struct {
  uint8_t row, symb_l0;
  uint16_t freq_domain, start_rb, nr_of_rbs, scramb_id;
} nr_ue_csi_resource_t;

bool nr_ue_ctx_open(const char *path, uint32_t ring_capacity, double snapshot_period_s);
void nr_ue_ctx_close(void);
void nr_ue_ctx_on_obs(const nr_passive_obs_t *o);
void nr_ue_ctx_on_param(uint16_t rnti, nr_ue_param_t p, int64_t value, nr_ue_verif_t v, int cause, int64_t abs_slot);
/* dedicated_format is 1_1 or 0_1; dedicated_uss excludes common/CSS fallback geometry. */
void nr_ue_ctx_on_dci_accept(uint16_t rnti, bool dedicated_format, bool is_ul, bool dedicated_uss,
                             int dci_length, int64_t coreset, int pdcch_scr_id, int64_t abs_slot);
void nr_ue_ctx_on_anchor(uint16_t rnti, int anchor_kind, int64_t abs_slot);
void nr_ue_ctx_on_sib1(uint32_t semantic_hash, int64_t abs_slot);
void nr_ue_ctx_on_sib1_param(nr_ue_param_t p, int64_t value, int64_t abs_slot);
void nr_ue_ctx_on_epoch(const nr_cfg_epoch_snapshot_t *s);
void nr_ue_ctx_on_aperiodic_csi(uint16_t rnti, uint8_t request, int64_t abs_slot, int64_t resource);
void nr_ue_ctx_on_dci01_csi(uint16_t rnti, uint8_t request, int64_t trigger_slot,
                            const nr_ue_csi_resource_t *observed);
void nr_ue_ctx_tick(int64_t abs_slot, uint64_t t_mono_ns);
bool nr_ue_ctx_get(uint16_t rnti, nr_ue_ctx_t *out);
/* Msg3 (UL CCCH SDU of a TC-RNTI, 48 bits packed) / Msg4 (UE Contention Resolution Identity CE, 48 bits packed): TS 38.321 5.1.5. */
void nr_ue_ctx_on_msg3(uint16_t rnti, uint64_t ccch48, int64_t abs_slot);
void nr_ue_ctx_on_msg4(uint16_t rnti, uint64_t cr48, int64_t abs_slot);
/* An RRC message was received by rnti at rx_slot (kind: nr_reconf_rrc_kind_t); the new dedicated configuration applies after TS 38.331 12. */
void nr_ue_ctx_on_rrc(uint16_t rnti, int kind, int64_t rx_slot, int mu);
bool nr_ue_ctx_try_get(uint16_t rnti, nr_ue_ctx_t *out); /* producer: miss on lock contention */
bool nr_ue_ctx_ul_ticket(uint16_t rnti, nr_ue_ul_ticket_t *out);
void nr_ue_ctx_ul_grant(const nr_ue_ul_ticket_t *ticket, const nr_ue_ul_update_t *update);
void nr_ue_ctx_ul_feedback(const nr_ue_ul_ticket_t *ticket, const nr_ue_ul_update_t *update, bool tb_crc_ok);
void nr_ue_ctx_ul_ta_command(uint16_t rnti, int units, int mu, bool absolute);
void nr_ue_ctx_stats(uint64_t *events, uint64_t *written, uint64_t *dropped);
extern bool nr_ue_ctx_fast_open;
static inline bool nr_ue_ctx_enabled(void)
{
  return __atomic_load_n(&nr_ue_ctx_fast_open, __ATOMIC_SEQ_CST);
}
#ifdef __cplusplus
}
#endif
#endif
