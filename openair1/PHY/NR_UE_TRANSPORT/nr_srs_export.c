/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_srs_export.h"
#include "nr_sense_records.h"
#include <inttypes.h>
#include <stdio.h>
#include <math.h>
static void emit_srs_record(const nr_srs_candidate_t *c,const nr_srs_capture_t *m,
                            int n_re,const uint8_t *l,uint64_t id,bool truncated)
{
  nr_sense_rs_meta_t rs={
    .rnti=c->rnti, .start_rb=c->start_rb, .nb_rb=c->nb_rb, .n_re=n_re, .n_ant=m->n_ant,
    .ports=1, .ssb_index=-1, .k_ssb=-1, .noise_var=c->noise_var, .slot_frac=c->start_symbol/14.0f,
    .abs_slot=m->abs_slot, .frame=m->frame, .slot=m->slot, .truncated=truncated, .pci=m->pci,
    .rx_ts=m->rx_ts_valid?(int64_t)m->rx_ts:-1, .t_mono_ns=m->t_mono_ns,
    .epoch=m->reconf?(int)m->epoch:-1, .identity_gen=m->reconf?(int)m->identity_gen:-1,
    .dedicated_epoch=m->reconf?m->dedicated_epoch:-1, .stamp_valid=true,
    .ta_est_samples=m->ta_est_samples, .transform_precoding=-1,
    .dmrs_type=-1, .dmrs_add_pos=-1, .dmrs_n_scid=-1,
    .srs_comb=c->comb, .srs_offset=(c->start_rb*12+c->comb_offset)%c->comb,
    .srs_cs=c->cs, .srs_group=c->group, .srs_period=c->period?c->period:-1,
    .srs_track_id=c->track_id, .srs_n_symbols=c->n_symbols,
    .srs_slot_offset=c->period?c->slot_offset:-1, .srs_confirmed=c->confirmed,
    .srs_timing_cs_ambiguous=!c->delay_resolved, .srs_delay_ambiguity_samples=c->delay_ambiguity_samples,
    .srs_attribution=c->rnti && c->attribution?c->attribution:"anonymous"};
  for(int i=0;i<n_re;i++) if(l[i]<14) rs.l_sym_mask|=1u<<l[i];
  nr_sense_emit_rs(NR_SENSE_SRC_SRS,&rs,id);
}
uint64_t nr_srs_export(const nr_srs_candidate_t *c,const nr_srs_capture_t *m,
                       int n_re,const float *iq,const uint16_t *k,const uint8_t *l)
{
  if(!nr_sense_enabled()) return 0;
  if(!c || !m || !iq || !k || !l || n_re<=0 || n_re>NR_SRS_MAX_RE) return 0;
  bool truncated=false;
  uint64_t id=nr_sense_push_cfr(NR_SENSE_SRC_SRS,m->n_ant,iq,n_re,n_re,k,l,m->abs_slot,
    c->start_symbol/14.0f,c->noise_var,m->reconf?m->epoch:0,m->reconf?m->identity_gen:0,&truncated);
  int stored_re=truncated && m->cfr_max_re>0 && m->cfr_max_re<n_re?m->cfr_max_re:n_re;
  emit_srs_record(c,m,stored_re,l,id,truncated);
  return id;
}
