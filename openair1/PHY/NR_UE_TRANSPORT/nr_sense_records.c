/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_sense_records.h"
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_ue_ctx.h"
#include "PHY/defs_nr_common.h"
#include "common/oai_version.h"
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { char *p; size_t n, w; bool bad; } json_t;
static void put(json_t *j, const char *fmt, ...) __attribute__((format(printf,2,3)));
static void put(json_t *j, const char *fmt, ...)
{
  if (j->bad) return;
  va_list ap; va_start(ap, fmt);
  int k = vsnprintf(j->p+j->w, j->n-j->w, fmt, ap);
  va_end(ap);
  if (k < 0 || (size_t)k >= j->n-j->w) { j->bad = true; return; }
  j->w += k;
}
static void integer(json_t *j, const char *key, int64_t v)
{
  if (v < 0) put(j, ",\"%s\":null", key);
  else put(j, ",\"%s\":%" PRId64, key, v);
}
static void signed_optional(json_t *j, const char *key, int64_t v, bool valid)
{
  if (valid) put(j, ",\"%s\":%" PRId64, key, v);
  else put(j, ",\"%s\":null", key);
}
static void real(json_t *j, const char *key, double v)
{
  if (!isfinite(v)) put(j, ",\"%s\":null", key);
  else put(j, ",\"%s\":%.9g", key, v);
}
static void quoted(json_t *j, const char *key, const char *v)
{
  if (!v) { put(j, ",\"%s\":null", key); return; }
  put(j, ",\"%s\":\"", key);
  for (; *v; v++) {
    const unsigned char c = (unsigned char)*v;
    if (c == '"' || c == '\\') put(j, "\\%c", c);
    else if (c < 0x20) put(j, "\\u%04x", c);
    else put(j, "%c", c);
  }
  put(j, "\"");
}
static uint64_t mono_ns(void)
{
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec*1000000000ull + t.tv_nsec;
}
static void common(json_t *j, int epoch, int identity, int dedicated, int64_t abs_slot,
                   int frame, int slot, uint64_t t_ns, int64_t rx_ts, int pci)
{
  integer(j, "epoch", epoch); integer(j, "identity_gen", identity);
  integer(j, "dedicated_epoch", dedicated); integer(j, "abs_slot", abs_slot);
  integer(j, "frame", frame); integer(j, "slot", slot);
  put(j, ",\"t_mono_ns\":%" PRIu64, t_ns);
  integer(j, "rx_ts", rx_ts); integer(j, "pci", pci);
}

int nr_sense_rs_to_json(nr_sense_src_t src, const nr_sense_rs_meta_t *m, uint64_t frame_id, char *buf, size_t n)
{
  static const char *names[] = {"ssb","csirs","pdsch_dmrs","pdsch_data","pusch_dmrs","pusch_data","prach","srs"};
  if (!m || !buf || !n || src < 0 || src >= NR_SENSE_SRC_COUNT) return -1;
  const bool ul = src == NR_SENSE_SRC_PUSCH_DMRS || src == NR_SENSE_SRC_PUSCH_DATA ||
                  src == NR_SENSE_SRC_PRACH || src == NR_SENSE_SRC_SRS;
  json_t j = {buf,n,0,false};
  put(&j,"{\"schema\":2,\"type\":\"rs\"");
  common(&j,m->epoch,m->identity_gen,m->dedicated_epoch,(int64_t)m->abs_slot,m->frame,m->slot,
         m->t_mono_ns ? m->t_mono_ns : mono_ns(),m->rx_ts,m->pci);
  quoted(&j,"source",names[src]); quoted(&j,"dir",ul ? "UL" : "DL");
  integer(&j,"rnti",src==NR_SENSE_SRC_SSB || src==NR_SENSE_SRC_CSIRS || src==NR_SENSE_SRC_PRACH || !m->rnti ? -1 : m->rnti);
  integer(&j,"start_rb",m->start_rb); integer(&j,"nb_rb",m->nb_rb);
  integer(&j,"n_re",m->n_re);
  put(&j,",\"l_syms\":[");
  bool first=true;
  for (int l=0;l<32;l++) if (m->l_sym_mask & (1u<<l)) { put(&j,"%s%d",first?"":",",l); first=false; }
  put(&j,"]");
  integer(&j,"ports",m->ports); integer(&j,"n_ant",m->n_ant);
  integer(&j,"port_mask",m->port_mask > 0 ? m->port_mask : -1);
  real(&j,"noise_var",m->noise_var); real(&j,"slot_frac",m->slot_frac);
  if (frame_id) put(&j,",\"cfr_frame_id\":%" PRIu64,frame_id);
  else put(&j,",\"cfr_frame_id\":null");
  put(&j,",\"truncated\":%s",m->truncated?"true":"false");
  integer(&j,"ssb_index",src==NR_SENSE_SRC_SSB?m->ssb_index:-1);
  integer(&j,"k_ssb",src==NR_SENSE_SRC_SSB?m->k_ssb:-1);
  quoted(&j,"csirs_resource",src==NR_SENSE_SRC_CSIRS?m->csirs_resource:NULL);
  /* SRS has a measured, signed representative; PUSCH retains S34's unknown sentinel. */
  signed_optional(&j,"ta_est_samples",m->ta_est_samples,ul && (src==NR_SENSE_SRC_SRS || m->ta_est_samples>=0));
  quoted(&j,"k_abs_convention","crb0_point_a");
  integer(&j,"freq_hopping",-1); real(&j,"cfo_est_hz",NAN);
  signed_optional(&j,"ta_applied_samples",m->ta_applied_samples,ul && m->ta_applied_valid);
  integer(&j,"transform_precoding",ul?m->transform_precoding:-1);
  integer(&j,"dmrs_type",ul?m->dmrs_type:-1);
  integer(&j,"dmrs_add_pos",ul?m->dmrs_add_pos:-1);
  integer(&j,"dmrs_n_scid",ul?m->dmrs_n_scid:-1);
  quoted(&j,"ul_timing_ref",ul?"dl_frame":NULL);
  integer(&j,"preamble",src==NR_SENSE_SRC_PRACH?m->prach_preamble:-1);
  real(&j,"energy",src==NR_SENSE_SRC_PRACH?m->prach_energy:NAN);
  real(&j,"delay",src==NR_SENSE_SRC_PRACH?m->prach_delay:NAN);
  integer(&j,"comb",src==NR_SENSE_SRC_SRS?m->srs_comb:-1);
  integer(&j,"offset",src==NR_SENSE_SRC_SRS?m->srs_offset:-1);
  integer(&j,"comb_offset",src==NR_SENSE_SRC_SRS?m->srs_offset:-1);
  integer(&j,"cs",src==NR_SENSE_SRC_SRS?m->srs_cs:-1);
  integer(&j,"group",src==NR_SENSE_SRC_SRS?m->srs_group:-1);
  integer(&j,"period",src==NR_SENSE_SRC_SRS?m->srs_period:-1);
  if (src==NR_SENSE_SRC_SRS) {
    put(&j,",\"srs_track_id\":%" PRIu64 ",\"track_id\":%" PRIu64,m->srs_track_id,m->srs_track_id);
    integer(&j,"n_symbols",m->srs_n_symbols);
    integer(&j,"slot_offset",m->srs_slot_offset);
    put(&j,",\"confirmed\":%s,\"timing_cs_ambiguous\":%s",
        m->srs_confirmed?"true":"false",m->srs_timing_cs_ambiguous?"true":"false");
    real(&j,"delay_ambiguity_samples",m->srs_delay_ambiguity_samples);
  } else integer(&j,"srs_track_id",-1);
  if (src==NR_SENSE_SRC_PRACH) {
    real(&j,"delay_samples",m->prach_delay);
    put(&j,",\"occasion\":{\"frame\":%d,\"slot\":%d,\"fdm_index\":%d,\"symbol\":%d}",
        m->frame,m->slot,m->prach_fdm,m->prach_symbol);
    put(&j,",\"rar_match\":%s",m->prach_rar_match<0?"null":m->prach_rar_match?"true":"false");
    integer(&j,"rar_rapid",m->prach_rar_rapid); integer(&j,"rar_ta",m->prach_rar_ta);
  }
  quoted(&j,"attribution",src==NR_SENSE_SRC_SRS?m->srs_attribution:NULL);
  put(&j,"}");
  return j.bad ? -1 : (int)j.w;
}

static const char *receiver_id, *receiver_array;
void nr_sense_set_session_identity(const char *id, const char *array)
{ receiver_id=id; receiver_array=array; }

static void stamp(int *epoch, int *identity, int *dedicated)
{
  if (!nr_cfg_reconf_enabled()) { *epoch=*identity=*dedicated=-1; return; }
  uint64_t pair=nr_cfg_epoch_cfr_stamp();
  *epoch=(int)(pair>>32); *identity=(int)(uint32_t)pair;
  *dedicated=(int)nr_cfg_epoch_dedicated_current();
}
void nr_sense_capture_grant_stamp(nr_sense_grant_meta_t *m)
{
  if (!m) return;
  stamp(&m->epoch,&m->identity_gen,&m->dedicated_epoch);
  m->stamp_valid=true;
}
void nr_sense_emit_session(const NR_DL_FRAME_PARMS *fp, double gain)
{
  if (!nr_sense_enabled()) return;
  char line[2048]; json_t j={line,sizeof line,0,false};
  int e,i,d; stamp(&e,&i,&d);
  put(&j,"{\"schema\":2,\"type\":\"session\",\"reconf\":%s",e<0?"false":"true");
  common(&j,e,i,d,-1,-1,-1,mono_ns(),-1,fp?fp->Nid_cell:-1);
  integer(&j,"carrier_dl_hz",fp && fp->dl_CarrierFreq?(int64_t)fp->dl_CarrierFreq:-1);
  integer(&j,"carrier_ul_hz",fp && fp->ul_CarrierFreq?(int64_t)fp->ul_CarrierFreq:-1);
  integer(&j,"scs_khz",fp?(int64_t)fp->subcarrier_spacing/1000:-1);
  integer(&j,"fs_hz",fp?(int64_t)fp->samples_per_subframe*1000:-1);
  integer(&j,"n_rb_dl",fp?fp->N_RB_DL:-1);
  integer(&j,"n_rx",fp?fp->nb_antennas_rx:-1);
  real(&j,"rx_gain_db",gain);
  quoted(&j,"rx_id",receiver_id); quoted(&j,"rx_array",receiver_array);
#ifdef OAI_PACKAGE_VERSION
  quoted(&j,"build",OAI_PACKAGE_VERSION);
#else
  quoted(&j,"build",NULL);
#endif
  put(&j,"}");
  if (!j.bad) nr_sense_push_record(line);
}

static nr_sense_grant_meta_t unknown_grant(void)
{
  nr_sense_grant_meta_t m={.epoch=-1,.identity_gen=-1,.dedicated_epoch=-1,.rx_ts=-1,
    .k2=-1,.dci_slot=-1,.transform_precoding=-1,.freq_hopping=-1,
    .dmrs_type=-1,.dmrs_add_pos=-1,.dmrs_n_scid=-1,.ta_est_samples=-1,.cfo_est_hz=NAN};
  return m;
}
void nr_sense_emit_grant_meta(const nr_passive_obs_t *o, const nr_sense_grant_meta_t *in)
{
  if (!nr_sense_enabled() || !o) return;
  nr_sense_grant_meta_t m=in?*in:unknown_grant();
  if (!m.stamp_valid) stamp(&m.epoch,&m.identity_gen,&m.dedicated_epoch);
  if (nr_ue_ctx_enabled()) {
    nr_ue_ctx_t ctx={0};
    if (nr_ue_ctx_try_get(o->rnti,&ctx)) {
      const nr_ue_cfg_value_t *v=&ctx.cfg[o->dir==NR_OBS_DIR_UL?NR_UEP_DCI_LEN_UL:NR_UEP_DCI_LEN_DL];
      if (v->value >= 0) m.verif=v->verif==NR_UEV_TRUSTED?"TRUSTED":v->verif==NR_UEV_HINT?"HINT":"SUSPECT";
    }
  }
  char line[2048];
  if (nr_sense_grant_to_json(o,&m,line,sizeof line)>0) nr_sense_push_record(line);
}
void nr_sense_emit_grant(const nr_passive_obs_t *o, int dl_inline)
{
  nr_sense_grant_meta_t m=unknown_grant(); m.dl_inline=dl_inline;
  nr_sense_emit_grant_meta(o,&m);
}
void nr_sense_emit_rs(nr_sense_src_t src, const nr_sense_rs_meta_t *in, uint64_t frame_id)
{
  if (!nr_sense_enabled() || !in) return;
  nr_sense_rs_meta_t m=*in;
  if (!m.stamp_valid) stamp(&m.epoch,&m.identity_gen,&m.dedicated_epoch);
  char line[2048];
  if (nr_sense_rs_to_json(src,&m,frame_id,line,sizeof line)>0) nr_sense_push_record(line);
}
