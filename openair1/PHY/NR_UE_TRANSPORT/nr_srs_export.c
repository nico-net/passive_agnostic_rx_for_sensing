/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_srs_export.h"
#include "nr_sense_api.h"
#include <inttypes.h>
#include <stdio.h>
#include <math.h>
/* Kept behind one function so Task 3 can replace this with nr_sense_emit_rs at merge.
 * Metadata is a capture-time value, never sampled from current receiver state here. */
static void emit_srs_record(const nr_srs_candidate_t *c,const nr_srs_capture_t *m,
                            int n_re,const uint8_t *l,uint64_t id,bool truncated)
{
  char epoch[24]="null",identity[24]="null",rx[32]="null",rnti[16]="null",frame_id[32]="null";
  char period[16]="null",offset[16]="null",syms[64]="",line[2048];
  if(m->reconf) {snprintf(epoch,sizeof(epoch),"%u",m->epoch);snprintf(identity,sizeof(identity),"%u",m->identity_gen);}
  if(m->rx_ts_valid) snprintf(rx,sizeof(rx),"%"PRIu64,m->rx_ts);
  if(c->rnti) snprintf(rnti,sizeof(rnti),"%u",c->rnti);
  if(id) snprintf(frame_id,sizeof(frame_id),"%"PRIu64,id);
  if(c->period) {snprintf(period,sizeof(period),"%d",c->period);snprintf(offset,sizeof(offset),"%d",c->slot_offset);}
  unsigned mask=0;for(int i=0;i<n_re;i++) if(l[i]<14) mask|=1u<<l[i];
  size_t used=0;
  for(int i=0;i<14;i++) if(mask&(1u<<i)) used+=snprintf(syms+used,sizeof(syms)-used,"%s%d",used?",":"",i);
  const char *method=c->rnti && c->attribution?c->attribution:"anonymous";
  /* method is an internal enum-like literal, never receiver/user text. */
  int size=snprintf(line,sizeof(line),
    "{\"schema\":2,\"type\":\"rs\",\"source\":\"srs\",\"dir\":\"UL\","
    "\"epoch\":%s,\"identity_gen\":%s,\"dedicated_epoch\":null,\"abs_slot\":%"PRIu64","
    "\"frame\":%d,\"slot\":%d,\"t_mono_ns\":%"PRIu64",\"rx_ts\":%s,\"pci\":%d,"
    "\"rnti\":%s,\"start_rb\":%d,\"nb_rb\":%d,\"n_re\":%d,\"l_syms\":[%s],\"ports\":1,"
    "\"n_ant\":%d,\"noise_var\":%.9g,\"slot_frac\":%.9g,\"cfr_frame_id\":%s,\"truncated\":%s,"
    "\"comb\":%d,\"comb_offset\":%d,\"n_symbols\":%d,\"cs\":%d,\"group\":%d,"
    "\"period\":%s,\"slot_offset\":%s,\"track_id\":%"PRIu64",\"srs_track_id\":%"PRIu64",\"attribution\":\"%s\","
    "\"ta_est_samples\":%d,\"ul_timing_ref\":\"dl_frame\",\"timing_cs_ambiguous\":true,"
    "\"transform_precoding\":null,\"freq_hopping\":null,\"dmrs_type\":null,\"dmrs_add_pos\":null,\"dmrs_n_scid\":null,\"cfo_est_hz\":null}",
    epoch,identity,m->abs_slot,m->frame,m->slot,m->t_mono_ns,rx,m->pci,rnti,c->start_rb,c->nb_rb,n_re,syms,
    m->n_ant,isfinite(c->noise_var)?c->noise_var:0,c->start_symbol/14.0,frame_id,truncated?"true":"false",
    c->comb,c->comb_offset,c->n_symbols,c->cs,c->group,period,offset,c->track_id,c->track_id,method,m->ta_est_samples);
  if(size>0 && size<(int)sizeof(line)) nr_sense_push_record(line);
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
