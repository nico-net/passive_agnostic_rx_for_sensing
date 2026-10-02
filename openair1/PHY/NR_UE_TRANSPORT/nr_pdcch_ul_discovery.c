/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "nr_pdcch_ul_discovery.h"
#include "nr_crc_evidence.h"
#include "nr_pdcch_ul_field_sweep.h"
#include "nr_pdcch_ul_interp_sweep.h"
#include "common/utils/LOG/log.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UL_DISCOVERY_SAMPLES 8
typedef struct {
  nr_hyp_sweep_state_t engine;
  nr_hyp_t *raw;
  int n_raw;
  bool initialized, refused;
} search_t;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define UL_DISCOVERY_CONTEXTS NR_PDCCH_BLIND_MAX_UE
typedef struct {
  search_t widths, interp;
  int joint_winner[16], joint_cursor[16], joint_tda_cursor;
  nr_pdcch_blind_ul_opts_t baseline;
  uint16_t target_rnti, target_length;
  uint64_t generation, touched;
  nr_dci_bits_t samples[UL_DISCOVERY_SAMPLES];
  int nsamples, sample_cursor, tda_index;
  uint64_t feedbacks, grants, late_splits;
  /* Evidence belongs to one identity and one baseline, never to competing UEs. */
  bool logged_width, logged_interp;
} ul_context_t;
static ul_context_t contexts[UL_DISCOVERY_CONTEXTS];
static uint64_t generation_counter = 1, context_clock;
static uint64_t rejected_feedback;
/* MEASURED 2026-09-21 (Swisscom macro, cons6_css0_auto): nr_pdcch_ul_discovery_reset() was 1370 us
 * of a 1399 us occasion, on EVERY occasion, in a conf whose UL sweep never ran. The RT caller
 * resets whenever full_auto or scan_01 is off, and each ul_context_t holds two
 * nr_hyp_sweep_state_t of 1,163,280 bytes -- 16 contexts is a ~37 MB memset per slot. State that
 * nobody has written does not need clearing: reset() is a no-op until a mutator has run. */
static bool s_discovery_dirty = true;   /* true at start so the FIRST reset initialises winners */
/* Struct padding is not a network configuration value. Compare only declared
 * members, retaining the existing conservative identity of every option. */
static bool same_options(const nr_pdcch_blind_ul_opts_t *a, const nr_pdcch_blind_ul_opts_t *b)
{
#define SAME(field) (a->field == b->field)
  return SAME(bwp_start) && SAME(bwp_size) && SAME(numerology) && SAME(dmrs_typeA_position)
      && SAME(tda_count)
      && !memcmp(a->tda_start,b->tda_start,sizeof(a->tda_start))
      && !memcmp(a->tda_length,b->tda_length,sizeof(a->tda_length))
      && !memcmp(a->tda_mapping,b->tda_mapping,sizeof(a->tda_mapping))
      && !memcmp(a->tda_k2,b->tda_k2,sizeof(a->tda_k2))
      && SAME(dmrs_config_type) && SAME(dmrs_add_pos) && SAME(dmrs_max_length)
      && SAME(transform_precoding) && SAME(mcs_table) && SAME(data_scrambling_id)
      && SAME(ul_dmrs_scrambling_id) && SAME(phy_cell_id)
      && SAME(carrier_indicator_bits) && SAME(ul_sul_bits) && SAME(bwp_indicator_bits)
      && SAME(freq_hopping_bits) && SAME(harq_pid_bits) && SAME(dai1_bits) && SAME(dai2_bits)
      && SAME(sri_bits) && SAME(precoding_info_bits) && SAME(antenna_ports_bits)
      && SAME(srs_request_bits) && SAME(csi_request_bits) && SAME(cbg_bits)
      && SAME(ptrs_dmrs_bits) && SAME(beta_offset_bits) && SAME(dmrs_seq_init_bits);
#undef SAME
}


static void clear_search(search_t *s)
{
  free(s->raw);
  memset(s,0,sizeof(*s));
  s->engine.winner=-1;
}
static void reset_locked(ul_context_t *c)
{
  clear_search(&c->widths); clear_search(&c->interp);
  memset(c, 0, sizeof(*c));
  c->widths.engine.winner = c->interp.engine.winner = -1;
  c->tda_index = -1;
  for (int i=0; i<16; ++i) c->joint_winner[i]=-1;
  c->generation = ++generation_counter;
}
void nr_pdcch_ul_discovery_reset(void)
{
  pthread_mutex_lock(&lock);
  if (s_discovery_dirty) {
    for (int i=0; i<UL_DISCOVERY_CONTEXTS; ++i) reset_locked(&contexts[i]);
    rejected_feedback=0;
    s_discovery_dirty=false;
  }
  pthread_mutex_unlock(&lock);
}
typedef struct { nr_pdcch_blind_ul_opts_t opts; bool interpretation; ul_context_t *owner; } apply_ctx_t;
/* Keep RAW width identity: equivalence under a CP baseline says nothing about
 * equivalence under another DM-RS/TP interpretation. Each TDA index also owns
 * distinct CRC evidence, even when two rows currently describe the same wave. */
typedef struct {
  uint16_t width, interpretation;
  uint8_t tda;
  nr_pdcch_ul_interp_hyp_t config;
} joint_hyp_t;
_Static_assert(sizeof(joint_hyp_t)<=NR_HYP_BYTES,"joint hypothesis storage");
static bool apply_joint(const nr_hyp_t *h, const apply_ctx_t *ctx,
                        nr_pdcch_blind_ul_opts_t *o, joint_hyp_t *j)
{
  if(h->len!=sizeof(*j)) return false;
  memcpy(j,h->bytes,sizeof(*j));
  if(j->width>=ctx->owner->widths.n_raw || j->tda>=16) return false;
  nr_pdcch_ul_field_sweep_apply(&ctx->owner->widths.raw[j->width],o);
  /* count=0 is the complete default 16-row table with a four-bit TDA field.
   * Materialize that size before replacing the one hypothesized row. */
  if(o->tda_count==0) o->tda_count=16;
  nr_hyp_t ih={.len=sizeof(j->config)};
  memcpy(ih.bytes,&j->config,sizeof(j->config));
  return nr_pdcch_ul_interp_sweep_apply(&ih,j->tda,o);
}
static bool extract(const nr_hyp_t *h, const nr_dci_bits_t *p, const apply_ctx_t *ctx,
                    nr_pdcch_blind_ul_result_t *out)
{
  nr_pdcch_blind_ul_opts_t o=ctx->opts;
  joint_hyp_t j={0};
  if (ctx->interpretation) {
    if (!apply_joint(h,ctx,&o,&j)) return false;
  } else nr_pdcch_ul_field_sweep_apply(h,&o);
  return nr_pdcch_blind_extract_01(*p,ctx->owner->target_length,ctx->owner->target_rnti,&o,out)
      && (!ctx->interpretation || out->tda_index==j.tda);
}
/* Compare the waveform and sample interpretation that the standalone CRC
 * oracle can distinguish, not every decoded DCI metadata field.
 *
 * This path rejects RV!=0 and decodes fresh transport blocks without HARQ
 * combining. nr_ulsch_decoding identifies its storage by ULSCH_id, not the
 * DCI HARQ PID; NDI does not affect a fresh decode. Online UCI recovery
 * infers the footprint from CRC evidence and does not consume DAI.
 * Consequently HARQ/NDI/DAI alternatives remain members of one decoder-
 * equivalent class. A winning class does NOT resolve those metadata bits.
 * If HARQ combining or DAI-based UCI is introduced, revise this contract.
 * Later waveform differences still split classes without inventing evidence.
 */
static bool decode_equivalent(const nr_pdcch_blind_ul_result_t *x, const nr_pdcch_blind_ul_result_t *y)
{
  return x->start_rb == y->start_rb && x->num_rb == y->num_rb
      && x->bwp_start == y->bwp_start && x->bwp_size == y->bwp_size
      && x->tda_index == y->tda_index && x->start_symbol == y->start_symbol
      && x->num_symbols == y->num_symbols && x->mapping_type == y->mapping_type
      && x->k2 == y->k2 && x->mcs == y->mcs && x->mcs_table == y->mcs_table
      && x->nrOfLayers == y->nrOfLayers
      && x->ul_dmrs_symb_pos == y->ul_dmrs_symb_pos
      && x->dmrs_config_type == y->dmrs_config_type
      && x->n_dmrs_cdm_groups == y->n_dmrs_cdm_groups
      && x->dmrs_ports == y->dmrs_ports && x->nscid == y->nscid
      && x->transform_precoding == y->transform_precoding
      && x->frequency_hopping == y->frequency_hopping
      && x->data_scrambling_id == y->data_scrambling_id
      && x->ul_dmrs_scrambling_id == y->ul_dmrs_scrambling_id
      && x->rv == y->rv
      && x->ulsch_indicator == y->ulsch_indicator
      && x->carrier_indicator == y->carrier_indicator
      && x->ul_sul_indicator == y->ul_sul_indicator;
}
static bool equivalent(const nr_hyp_t *a, const nr_hyp_t *b, const void *sample, void *ctx)
{
  nr_pdcch_blind_ul_result_t ga,gb;
  /* Two failures are NOT equal grants. In particular, unsupported modes cannot be
   * collapsed away as if they had been proved observationally equivalent. */
  if (!extract(a,sample,ctx,&ga) || !extract(b,sample,ctx,&gb)) return false;
  return decode_equivalent(&ga,&gb);
}
/* Initial merging requires equal decoded grants. Refinement instead needs
 * a contradictory observation: two extraction failures do not distinguish
 * layouts, and a representative can never split away from itself. */
static bool distinguishes(const nr_hyp_t *a, const nr_hyp_t *b, const void *sample, void *ctx)
{
  if(a->len==b->len && !memcmp(a->bytes,b->bytes,a->len)) return false;
  nr_pdcch_blind_ul_result_t ga,gb;
  const bool va=extract(a,sample,ctx,&ga), vb=extract(b,sample,ctx,&gb);
  return va!=vb || (va && !decode_equivalent(&ga,&gb));
}
static bool supported(const nr_hyp_t *h, void *context)
{
  const apply_ctx_t *ctx=context;
  nr_pdcch_blind_ul_opts_t opts=ctx->opts;
  if(ctx->interpretation) {
    joint_hyp_t j;
    if(!apply_joint(h,ctx,&opts,&j)) return false;
  } else nr_pdcch_ul_field_sweep_apply(h,&opts);
  /* No mode is excluded here any more. blind_ul_finish() decodes antenna-ports via
   * decode_dci_antenna_ports_val() for DM-RS type 1/2 AND transform-precoding-enabled alike (one
   * reverse table per mode, front_load/cdm_groups are outputs, not separate inputs);
   * nr_pdcch_blind_ul_dmrs_mask() already has both TS 38.211 6.4.1.1.3-3 (maxLength 1) and -4
   * (maxLength 2) tables (a maxLength-2 hypothesis paired with an add_pos the -4 table reserves --
   * 2 or 3, mapping type A -- simply fails nr_pdcch_blind_ul_dmrs_mask()'s own -1 return, not
   * special-cased); and nr_pusch_passive_decode.c now drives OAI's own shared low-PAPR-DM-RS/IDFT
   * PUSCH RX chain for transform precoding instead of rejecting it outright. */
  return true;
}
static bool plausible(const nr_hyp_t *h, const void *candidate, void *context)
{
  if(!supported(h,context)) return false;
  nr_pdcch_blind_ul_result_t out;
  /* The standalone TB oracle has no HARQ history. RV!=0 is unsupported,
   * not a CRC trial; skip it before queueing rather than waiting for feedback
   * that the decoder deliberately never emits. */
  return extract(h,candidate,context,&out) && !out.carrier_indicator && !out.ul_sul_indicator
      && out.rv==0;
}
static int generate_joint(nr_hyp_t *out, const apply_ctx_t *ctx)
{
  nr_hyp_t *catalog=calloc(NR_HYP_SWEEP_MAX_RAW,sizeof(*catalog));
  if(!catalog) return NR_HYP_SWEEP_INVALID;
  /* Gap item 2 (PUSCH TDRA type B): once the DM-RS energy oracle has pinned (S,L), cross the FULL
   * legal set at that point instead of the curated catalogue; curated list until a pin exists. */
  int pin_s=0,pin_l=0,pin_map=0;
  const int ni=nr_pusch_ul_dmrs_pin_get(&pin_s,&pin_l,&pin_map)
      ? nr_pdcch_ul_interp_sweep_generate_pinned(catalog,NR_HYP_SWEEP_MAX_RAW,pin_s,pin_l,pin_map)
      : nr_pdcch_ul_interp_sweep_generate(catalog,NR_HYP_SWEEP_MAX_RAW);
  const int nt=ctx->opts.tda_count?ctx->opts.tda_count:16;
  int n=0;
  for(int w=0;w<ctx->owner->widths.n_raw && ni>0;++w) {
    nr_pdcch_ul_field_widths_t width;
    memcpy(&width,ctx->owner->widths.raw[w].bytes,sizeof(width));
    for(int t=0;t<nt;++t) {
      nr_pdcch_ul_interp_hyp_t baseline_row={.tda_start=ctx->opts.tda_start[t],
          .tda_length=ctx->opts.tda_length[t],.tda_mapping=ctx->opts.tda_mapping[t],
          .tda_k2=ctx->opts.tda_k2[t]};
      bool add_baseline=ctx->opts.tda_count>0 && nr_pusch_tda_legal(baseline_row.tda_mapping,
                                                                 baseline_row.tda_start,
                                                                 baseline_row.tda_length);
      /* All generic rows cross the same configuration axes. If this timing
       * row already occurs, its full cross-product is already present. */
      for(int k=0;k<ni && add_baseline;++k)
        if(!memcmp(catalog[k].bytes,&baseline_row,4)) add_baseline=false;
      for(int i=0;i<ni*(add_baseline?2:1);++i) {
      joint_hyp_t j={.width=w,.interpretation=i,.tda=t};
      memcpy(&j.config,catalog[i%ni].bytes,sizeof(j.config));
      if(i>=ni) {
        /* Clone each blind configuration axis once, from the first generic
         * timing row. This retains receiver-held timing as an unvalidated
         * hypothesis and never changes or removes generic timing candidates. */
        if(memcmp(catalog[i-ni].bytes,catalog[0].bytes,4)) continue;
        memcpy(&j.config,&baseline_row,4);
      }
      /* TS 38.212: sequence-initialization is absent iff TP is enabled.
       * This removes illegal pairs only, never a low-scoring candidate. */
      if(width.dmrs_seq_init_bits!=(j.config.transform_precoding?0:1)) continue;
      if(n==NR_HYP_SWEEP_MAX_RAW) {
        free(catalog);
        return NR_HYP_SWEEP_RAW_OVERFLOW;
      }
      out[n]=(nr_hyp_t){.len=sizeof(j)};
      memcpy(out[n++].bytes,&j,sizeof(j));
      }
    }
  }
  free(catalog);
  return ni>0?n:ni;
}
static bool init_search(search_t *s, apply_ctx_t *ctx)
{
  s->raw=calloc(NR_HYP_SWEEP_MAX_RAW,sizeof(*s->raw));
  if (!s->raw) { s->refused=true; LOG_E(PHY,"UL discovery: hypothesis allocation failed\n"); return false; }
  s->n_raw=ctx->interpretation
      ? generate_joint(s->raw,ctx)
      : nr_pdcch_ul_field_sweep_generate(&ctx->opts,ctx->owner->target_length,s->raw,NR_HYP_SWEEP_MAX_RAW);
  const void *observations[UL_DISCOVERY_SAMPLES];
  for (int i=0;i<ctx->owner->nsamples;++i) observations[i]=&ctx->owner->samples[i];
  int classes=s->n_raw>0 ? nr_hyp_sweep_init(&s->engine,s->raw,s->n_raw,supported,ctx,
                                           ctx->interpretation?NULL:equivalent,observations,
                                           ctx->owner->nsamples,ctx) : s->n_raw;
  if (classes<=0) {
    LOG_E(PHY,"UL discovery %s refused: raw=%d classes/error=%d; configuration unresolved\n",
          ctx->interpretation?"interpretation":"width",s->n_raw,classes);
    free(s->raw); s->raw=NULL; s->refused=true; return false;
  }
  s->initialized=true;
  LOG_A(PHY,"UL discovery %s armed: raw=%d classes=%d rnti=0x%x\n",
        ctx->interpretation?"interpretation":"width",s->n_raw,classes,ctx->owner->target_rnti);
  return true;
}
/* The baseline may be wrong for EVERY raw width. Requiring a positive-CRC
 * width winner before exploring TP creates a permanent dependency cycle.
 * Confidence only starts exploration; it never elects a width or a config. */
static bool baseline_rejected(const search_t *s)
{
  for(int i=0;i<s->engine.n_classes;++i) {
    const nr_hyp_class_t *h=&s->engine.classes[i];
    if(!h->trials && h->skipped>=NR_HYP_SWEEP_MIN_TRIALS) continue;
    double lo,hi;
    nr_crc_interval(h->passes,h->trials,NR_HYP_SWEEP_MAX_CLASSES,&lo,&hi);
    if(hi>=0.60) return false;
  }
  return true;
}
/* One bounded round-robin pass. No global winner: two TDA entries can both
 * be correct. Missing a row on this payload is not a failed CRC or evidence
 * eliminating that row. Classes retain exact raw width/TDA/config identity. */
static int joint_next(ul_context_t *c, const nr_dci_bits_t *payload, apply_ctx_t *ctx, nr_hyp_t *out)
{
  nr_hyp_sweep_state_t *e=&c->interp.engine;
  const int nt=c->baseline.tda_count?c->baseline.tda_count:16;
  for(int step=0;step<nt;++step) {
    const int t=(c->joint_tda_cursor+step)%nt;
    /* A settled row shares the same rotation as unresolved rows. A width
     * alternative can parse several payloads as this TDA, so consulting all
     * winners first would permanently starve another row's CRC evidence. */
    const int win=c->joint_winner[t];
    if(win>=0 && plausible(&e->classes[win].hyp,payload,ctx)) {
      *out=e->classes[win].hyp;
      c->joint_tda_cursor=(t+1)%nt;
      return win;
    }
    for(int n=0;n<e->n_classes;++n) {
      const int i=c->joint_cursor[t];
      c->joint_cursor[t]=(i+1)%e->n_classes;
      joint_hyp_t j;
      memcpy(&j,e->classes[i].hyp.bytes,sizeof(j));
      if(j.tda!=t) continue;
      if(plausible(&e->classes[i].hyp,payload,ctx)) {
        *out=e->classes[i].hyp;
        c->joint_tda_cursor=(t+1)%nt;
        return i;
      }
    }
  }
  return -1;
}
static void joint_feed(ul_context_t *c, int cls, bool ok)
{
  nr_hyp_class_t *h=&c->interp.engine.classes[cls];
  if(h->trials==UINT64_MAX) return;
  ++h->trials; h->passes+=ok;
  joint_hyp_t j;
  memcpy(&j,h->hyp.bytes,sizeof(j));
  /* Evidence belongs to this complete joint interpretation. A high lower
   * bound validates its waveform; it does not resolve equivalent metadata.
   * Other TDA rows have independent winners and continue receiving probes. */
  if(h->trials>=64 && h->trials%16==0) {
    double lo,hi;
    nr_crc_interval(h->passes,h->trials,NR_HYP_SWEEP_MAX_CLASSES,&lo,&hi);
    if(lo>=0.60 && c->joint_winner[j.tda]<0) {
      c->joint_winner[j.tda]=cls;
      LOG_A(PHY,"UL joint interpretation CRC-validated: raw_width=%u tda=%u config=%u class=%d lower=%.3f\n",
            j.width,j.tda,j.interpretation,cls,lo);
    }
  }
}
bool nr_pdcch_ul_discovery_grant(const nr_pdcch_blind_ul_opts_t *fixed, uint16_t len,
                                 uint16_t rnti, nr_dci_bits_t payload, nr_pdcch_blind_ul_result_t *out)
{
  if (!fixed || !out || !rnti || !len || len>NR_DCI_MAX_PAYLOAD) return false;
  pthread_mutex_lock(&lock);
  s_discovery_dirty=true;
  /* Equal DCI lengths do not prove equal dedicated configurations. Keep
   * each identity's CRC evidence independent; no pooled-probe winner veto. */
  ul_context_t *c = NULL, *oldest = &contexts[0];
  for (int i=0; i<UL_DISCOVERY_CONTEXTS; ++i) {
    if (contexts[i].target_rnti == rnti && contexts[i].target_length == len && contexts[i].nsamples > 0
        && same_options(&contexts[i].baseline, fixed)) {
      c=&contexts[i];
      break;
    }
    if (contexts[i].touched < oldest->touched) oldest=&contexts[i];
  }
  bool ok=false;
  if (!c) {
    c=oldest;
    reset_locked(c);
    c->baseline=*fixed;
    c->target_length=len;
  }
  c->target_rnti=rnti;
  c->touched = ++context_clock;
  /* FREEZE the sample set once the width search is armed. Measured 2026-09-09 on live traffic:
   * the ring kept admitting novel payloads, every payload that distinguished two previously-merged
   * hypotheses invalidated ALL accumulated evidence, and the search was torn down and rebuilt every
   * ~30 s -- 26 arms, 25 invalidations, 0 convergences in a 30 min capture, with the class count
   * drifting 108 -> 222 as the sample set rotated underneath it. Real traffic is an endless supply
   * of distinct payloads, so that condition fires forever; the original design assumed a finite
   * sample set. Equivalence classes were always documented as finite-sample EVIDENCE, not proof --
   * freezing makes the class definition stable enough for the CRC oracle to finish scoring it. */
  bool novel = !c->widths.initialized;
  for (int i=0;i<c->nsamples;++i) if(nr_dci_bits_eq(&c->samples[i], &payload)) novel=false;
  if (novel) {
  if(c->nsamples<UL_DISCOVERY_SAMPLES)
      LOG_I(PHY,"UL raw sample rnti=0x%x len=%u payload=0x%lx sample=%d/%d; interpretation unresolved\n",
            rnti,len,(unsigned long)payload.w[0],c->nsamples+1,UL_DISCOVERY_SAMPLES);
    c->samples[c->sample_cursor]=payload;
    c->sample_cursor=(c->sample_cursor+1)%UL_DISCOVERY_SAMPLES;
    if(c->nsamples<UL_DISCOVERY_SAMPLES) ++c->nsamples;
  }
  /* No geometry or TDA width inferred from DCI total length alone. The current opts
   * contract still supplies these facts; a default/unknown dedicated TDA list is unresolved. */
  /* tda_count == 0 means the TS 38.214 Table 6.1.2.1.1-2 default 16-entry table -- a COMPLETE
   * interpretation with a 4-bit field, which nr_pdcch_blind_dci01_size() already derives. Refusing
   * it treated a resolved case as unresolved. The UL BWP genuinely is required: it is the RIV
   * reference and sets the frequency-domain field's width. It is seeded from SIB1's common initial
   * UL BWP in full_auto (see nr_pdcch_blind_monitor_rt.c) as a hypothesis the TB CRC then judges. */
  if (fixed->bwp_size==0 || fixed->tda_count<0 || fixed->tda_count>16) {
    if (!c->widths.refused) LOG_E(PHY,"UL discovery unresolved: no UL BWP (SIB1 not decoded, and none configured)\n");
    c->widths.refused=true;
    goto done;
  }
  if(c->nsamples<UL_DISCOVERY_SAMPLES || c->widths.refused) goto done;
  apply_ctx_t ctx={.opts=c->baseline,.owner=c};
  if (!c->widths.initialized && !init_search(&c->widths,&ctx)) goto done;
  /* Refine provisional classes without discarding measurements of their
   * actual representatives. A new member starts with zero CRC evidence. */
  ++c->grants;
  if ((c->grants % 64) == 0) {
    search_t *s=&c->widths;
    for(int raw=0;raw<s->n_raw;raw++) {
      int cls=s->engine.class_of_raw[raw];
      if(cls<0 || s->engine.classes[cls].members<=1 ||
          !distinguishes(&s->engine.classes[cls].hyp,&s->raw[raw],&payload,&ctx)) continue;
      if(s->engine.n_classes>=NR_HYP_SWEEP_MAX_CLASSES) {
        s->refused=true;
        LOG_E(PHY,"UL width refinement exceeds class capacity; unresolved\n");
        goto done;
      }
      int fresh=s->engine.n_classes++;
      s->engine.classes[cls].members--;
      memset(&s->engine.classes[fresh],0,sizeof(s->engine.classes[fresh]));
      s->engine.classes[fresh].hyp=s->raw[raw];
      s->engine.classes[fresh].members=1;
      s->engine.class_of_raw[raw]=fresh;
      s->engine.order[fresh]=fresh;
      s->engine.cursor=0; s->engine.winner=-1;
      c->logged_width=false;
      ++c->late_splits;
    }
  }
  nr_hyp_t chosen;
  int wi=nr_hyp_sweep_next(&c->widths.engine,&payload,plausible,&ctx,&chosen);
  int ii=-1;
  double baseline_lo=0,baseline_hi=1;
  if(wi>=0 && nr_hyp_sweep_winner(&c->widths.engine)>=0)
    nr_crc_interval(c->widths.engine.classes[wi].passes,c->widths.engine.classes[wi].trials,
                    NR_HYP_SWEEP_MAX_CLASSES,&baseline_lo,&baseline_hi);
  const bool baseline_validated=baseline_lo>=0.60;
  if(baseline_validated && !c->logged_width) {
    LOG_A(PHY,"UL width and baseline CRC-validated: class=%d rnti=0x%x lower=%.3f "
              "equivalent_layouts=%d; HARQ/NDI/DAI metadata unresolved\n",
          wi,rnti,baseline_lo,c->widths.engine.classes[wi].members);
    c->logged_width=true;
  }
  /* Alternate independent joint probes with baseline measurements, so a
   * temporarily poor CP baseline can still recover. Overflow refuses only
   * joint exploration; it never disables the established baseline search. */
  if(!c->interp.initialized && !c->interp.refused &&
     (baseline_hi<0.60 || baseline_rejected(&c->widths))) {
    apply_ctx_t joint_ctx={.opts=c->baseline,.interpretation=true,.owner=c};
    init_search(&c->interp,&joint_ctx);
  }
  joint_hyp_t joint={0};
  if(c->interp.initialized && (wi<0 || (c->grants%2)==0)) {
    apply_ctx_t joint_ctx={.opts=c->baseline,.interpretation=true,.owner=c};
    nr_hyp_t joint_choice;
    ii=joint_next(c,&payload,&joint_ctx,&joint_choice);
    if(ii>=0) {
      ctx=joint_ctx;
      chosen=joint_choice;
      if(!apply_joint(&chosen,&ctx,&ctx.opts,&joint)) goto done;
    }
  }
  if(ii<0) {
    if(wi<0) goto done;
    nr_pdcch_ul_field_sweep_apply(&chosen,&ctx.opts);
  }
  /* No mode is gated here any more -- blind_ul_finish()/nr_pdcch_blind_extract_01() decode DM-RS
   * type, maxLength and transform precoding all via decode_dci_antenna_ports_val() and
   * nr_pdcch_blind_ul_dmrs_mask() (see supported()'s comment above). */
  ok=nr_pdcch_blind_extract_01(payload,len,rnti,&ctx.opts,out);
  if(ok && (out->carrier_indicator || out->ul_sul_indicator)) {
    /* No receiver/geometry for another carrier or SUL: not a failed TB trial. */
    ok=false;
  }
  if(ok) {
    /* Preserve the exact producer-time owner even after convergence. A future
     * winner or interpretation stage must never inherit this queued result. */
    out->width_hyp_class=ii<0?wi:-1;
    out->interp_hyp_class=ii;

    out->hyp_generation=c->generation;
    if(ii>=0) { out->hyp_width_raw=joint.width+1; out->hyp_interp_raw=joint.interpretation+1; }
  }
done:
  pthread_mutex_unlock(&lock);
  return ok;
}
/* Convergence needs EVERY class at NR_HYP_SWEEP_MIN_TRIALS before a winner can be declared, so
 * the aggregate CRC rate says nothing useful -- at most one of N classes is correct and the rest
 * MUST fail. Report the laggard (what sets the remaining time) and the leader (what the oracle
 * actually scores), so a run can be judged while it is still going. */
static void log_progress_locked(const search_t *s, const char *what, uint16_t rnti)
{
  const nr_hyp_sweep_state_t *e = &s->engine;
  if (!s->initialized || e->n_classes <= 0) return;
  uint64_t min_trials = UINT64_MAX, total = 0;
  int best = 0;
  for (int c = 0; c < e->n_classes; ++c) {
    if (e->classes[c].trials < min_trials) min_trials = e->classes[c].trials;
    total += e->classes[c].trials;
    const double r  = e->classes[c].trials ? (double)e->classes[c].passes / e->classes[c].trials : 0.0;
    const double rb = e->classes[best].trials ? (double)e->classes[best].passes / e->classes[best].trials : 0.0;
    if (r > rb) best = c;
  }
  LOG_I(PHY, "UL %s progress rnti=0x%x classes=%d trials=%lu min_per_class=%lu/%d "
             "best=class%d %lu/%lu winner=%d\n",
        what, rnti, e->n_classes, (unsigned long)total, (unsigned long)min_trials,
        NR_HYP_SWEEP_MIN_TRIALS, best, (unsigned long)e->classes[best].passes,
        (unsigned long)e->classes[best].trials, e->winner);
  /* Print the leading WIDTH hypothesis in pdcch_blind_monitor_ul_dci_bits order, so the search's
   * current best answer can be pinned as a manual configuration without reverse-engineering a class
   * index. That is the only way to get a CLEAN uplink CRC number: every UL decode in a full_auto run
   * comes from a stream that is ~99 % deliberately-wrong hypotheses, so it cannot say whether the
   * decode chain works. Leading, NOT converged -- quote it as a candidate, never as a result. */
  if (what[0] == 'w' && e->classes[best].hyp.len == (int)sizeof(nr_pdcch_ul_field_widths_t)) {
    nr_pdcch_ul_field_widths_t w;
    memcpy(&w, e->classes[best].hyp.bytes, sizeof(w));
    LOG_I(PHY, "UL leading widths (class%d, %lu/%lu) ul_dci_bits=\"%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d\"\n",
          best, (unsigned long)e->classes[best].passes, (unsigned long)e->classes[best].trials,
          w.carrier_indicator_bits, w.ul_sul_bits, w.bwp_indicator_bits, w.freq_hopping_bits,
          w.harq_pid_bits, w.dai1_bits, w.dai2_bits, w.sri_bits, w.precoding_info_bits,
          w.antenna_ports_bits, w.srs_request_bits, w.csi_request_bits, w.cbg_bits,
          w.ptrs_dmrs_bits, w.beta_offset_bits, w.dmrs_seq_init_bits);
  }
}

void nr_pdcch_ul_discovery_feedback(const nr_pdcch_blind_ul_result_t *g, bool ok)
{
  if (!g || !g->hyp_generation) return;
  pthread_mutex_lock(&lock);
  s_discovery_dirty=true;
  const bool width_owner=g->width_hyp_class>=0, interp_owner=g->interp_hyp_class>=0;
  if (width_owner==interp_owner || g->width_hyp_class < -1 || g->interp_hyp_class < -1) {
    ++rejected_feedback;
    LOG_W(PHY,"UL_FEEDBACK_REJECT reason=OWNER_NOT_UNIQUE generation=%lu rnti=0x%x\n",
          (unsigned long)g->hyp_generation,g->rnti);
    pthread_mutex_unlock(&lock);
    return;
  }
  bool matched_context=false;
  for (int i=0; i<UL_DISCOVERY_CONTEXTS; ++i) {
    ul_context_t *c=&contexts[i];
    /* Generation, identity, stage and class all belong to the decoded grant. */
    if (g->hyp_generation != c->generation) continue;
    matched_context=true;
    search_t *owner=width_owner?&c->widths:&c->interp;
    const int cls=width_owner?g->width_hyp_class:g->interp_hyp_class;
    if (g->rnti!=c->target_rnti || g->dci_length!=c->target_length
        || !owner->initialized || cls>=owner->engine.n_classes) {
      ++rejected_feedback;
      LOG_W(PHY,"UL_FEEDBACK_REJECT reason=IDENTITY_OR_CLASS_MISMATCH generation=%lu rnti=0x%x\n",
            (unsigned long)g->hyp_generation,g->rnti);
      break;
    }
    apply_ctx_t ctx={.opts=c->baseline,.interpretation=interp_owner,.owner=c};
    nr_pdcch_blind_ul_result_t expected;
    bool identity_ok=extract(&owner->engine.classes[cls].hyp,&g->raw_payload,&ctx,&expected)
                     && decode_equivalent(g,&expected);
    if(interp_owner) {
      joint_hyp_t j;
      memcpy(&j,owner->engine.classes[cls].hyp.bytes,sizeof(j));
      identity_ok=identity_ok && g->hyp_width_raw==j.width+1 && g->hyp_interp_raw==j.interpretation+1
                             && g->tda_index==j.tda;
    } else identity_ok=identity_ok && !g->hyp_width_raw && !g->hyp_interp_raw;
    if(!identity_ok) {
      ++rejected_feedback;
      LOG_W(PHY,"UL_FEEDBACK_REJECT reason=JOINT_IDENTITY_MISMATCH generation=%lu rnti=0x%x\n",
            (unsigned long)g->hyp_generation,g->rnti);
      break;
    }
    if(interp_owner) joint_feed(c,cls,ok);
    else nr_hyp_sweep_feed(&owner->engine,cls,ok);
    if ((++c->feedbacks % 2000) == 0) {
      log_progress_locked(&c->widths,"width",c->target_rnti);
      log_progress_locked(&c->interp,"interp",c->target_rnti);
    }
    break;
  }
  if(!matched_context) ++rejected_feedback;
  pthread_mutex_unlock(&lock);
}

nr_pdcch_ul_discovery_snapshot_t nr_pdcch_ul_discovery_snapshot(void)
{
  pthread_mutex_lock(&lock);
  nr_pdcch_ul_discovery_snapshot_t s={.generation=generation_counter,.rejected_feedback=rejected_feedback};
  for (int k=0; k<UL_DISCOVERY_CONTEXTS; ++k) {
    const ul_context_t *c=&contexts[k];
    s.raw_samples+=c->nsamples;
    s.width_winners+=c->widths.initialized && nr_hyp_sweep_winner(&c->widths.engine)>=0;
    for(int t=0;t<16;++t) s.interp_winners+=c->interp.initialized && c->joint_winner[t]>=0;
    s.width_classes+=c->widths.engine.n_classes;
    s.interp_classes+=c->interp.engine.n_classes;
    s.interp_refusals+=c->interp.refused;
    for(int i=0;i<c->widths.engine.n_classes;++i) s.width_trials+=c->widths.engine.classes[i].trials;
    for(int i=0;i<c->interp.engine.n_classes;++i) s.interp_trials+=c->interp.engine.classes[i].trials;
  }
  pthread_mutex_unlock(&lock);
  return s;
}
