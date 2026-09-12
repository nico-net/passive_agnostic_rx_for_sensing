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
  nr_pdcch_blind_ul_opts_t baseline;
  uint16_t target_rnti, target_length;
  uint64_t generation, touched;
  uint64_t samples[UL_DISCOVERY_SAMPLES];
  int nsamples, sample_cursor, tda_index;
  uint64_t feedbacks, grants, late_splits;
  /* Evidence belongs to one identity and one baseline, never to competing UEs. */
  bool logged_width, logged_interp;
} ul_context_t;
static ul_context_t contexts[UL_DISCOVERY_CONTEXTS];
static uint64_t generation_counter = 1, context_clock;
static uint64_t rejected_feedback;
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
  c->generation = ++generation_counter;
}
void nr_pdcch_ul_discovery_reset(void)
{
  pthread_mutex_lock(&lock);
  for (int i=0; i<UL_DISCOVERY_CONTEXTS; ++i) reset_locked(&contexts[i]);
  rejected_feedback=0;
  pthread_mutex_unlock(&lock);
}
typedef struct { nr_pdcch_blind_ul_opts_t opts; bool interpretation; ul_context_t *owner; } apply_ctx_t;
static bool extract(const nr_hyp_t *h, const uint64_t *p, const apply_ctx_t *ctx,
                    nr_pdcch_blind_ul_result_t *out)
{
  nr_pdcch_blind_ul_opts_t o=ctx->opts;
  if (ctx->interpretation) {
    if (!nr_pdcch_ul_interp_sweep_apply(h,ctx->owner->tda_index,&o)) return false;
  } else nr_pdcch_ul_field_sweep_apply(h,&o);
  return nr_pdcch_blind_extract_01(*p,ctx->owner->target_length,ctx->owner->target_rnti,&o,out);
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
    if(!nr_pdcch_ul_interp_sweep_apply(h,ctx->owner->tda_index,&opts)) return false;
  } else nr_pdcch_ul_field_sweep_apply(h,&opts);
  /* These modes are already refused by the delivery path. Refuse them
   * before selection so they cannot await CRC feedback that never arrives.
   * This is receiver scope, not evidence that the network mode is invalid. */
  return !opts.transform_precoding && opts.dmrs_config_type==0 && opts.dmrs_max_length<=1;
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
static bool init_search(search_t *s, apply_ctx_t *ctx)
{
  s->raw=calloc(NR_HYP_SWEEP_MAX_RAW,sizeof(*s->raw));
  if (!s->raw) { s->refused=true; LOG_E(PHY,"UL discovery: hypothesis allocation failed\n"); return false; }
  s->n_raw=ctx->interpretation
      ? nr_pdcch_ul_interp_sweep_generate(s->raw,NR_HYP_SWEEP_MAX_RAW)
      : nr_pdcch_ul_field_sweep_generate(&ctx->opts,ctx->owner->target_length,s->raw,NR_HYP_SWEEP_MAX_RAW);
  const void *observations[UL_DISCOVERY_SAMPLES];
  for (int i=0;i<ctx->owner->nsamples;++i) observations[i]=&ctx->owner->samples[i];
  int classes=s->n_raw>0 ? nr_hyp_sweep_init(&s->engine,s->raw,s->n_raw,supported,ctx,
                                           equivalent,observations,ctx->owner->nsamples,ctx) : s->n_raw;
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
static bool still_equivalent(search_t *s, const uint64_t *p, apply_ctx_t *ctx)
{
  for (int i=0;i<s->n_raw;++i) {
    int c=s->engine.class_of_raw[i];
    if (c<0 || s->engine.classes[c].members==1) continue;
    const nr_hyp_t *rep=&s->engine.classes[c].hyp;
    if (rep->len==s->raw[i].len && !memcmp(rep->bytes,s->raw[i].bytes,rep->len)) continue;
    if (distinguishes(rep,&s->raw[i],p,ctx)) return false;
  }
  return true;
}
bool nr_pdcch_ul_discovery_grant(const nr_pdcch_blind_ul_opts_t *fixed, uint16_t len,
                                 uint16_t rnti, uint64_t payload, nr_pdcch_blind_ul_result_t *out)
{
  if (!fixed || !out || !rnti || !len || len>63) return false;
  pthread_mutex_lock(&lock);
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
  for (int i=0;i<c->nsamples;++i) if(c->samples[i]==payload) novel=false;
  if (novel) {
  if(c->nsamples<UL_DISCOVERY_SAMPLES)
      LOG_I(PHY,"UL raw sample rnti=0x%x len=%u payload=0x%lx sample=%d/%d; interpretation unresolved\n",
            rnti,len,(unsigned long)payload,c->nsamples+1,UL_DISCOVERY_SAMPLES);
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
  if(c->nsamples<UL_DISCOVERY_SAMPLES || c->widths.refused || c->interp.refused) goto done;
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
  if(wi<0) goto done;
  nr_pdcch_ul_field_sweep_apply(&chosen,&ctx.opts);
  int ii=-1;
  double baseline_lo=0,baseline_hi=1;
  if(nr_hyp_sweep_winner(&c->widths.engine)>=0)
    nr_crc_interval(c->widths.engine.classes[wi].passes,c->widths.engine.classes[wi].trials,
                    NR_HYP_SWEEP_MAX_CLASSES,&baseline_lo,&baseline_hi);
  const bool baseline_validated=baseline_lo>=0.60;
  if(baseline_validated && !c->logged_width) {
    LOG_A(PHY,"UL width and baseline CRC-validated: class=%d rnti=0x%x lower=%.3f "
              "equivalent_layouts=%d; HARQ/NDI/DAI metadata unresolved\n",
          wi,rnti,baseline_lo,c->widths.engine.classes[wi].members);
    c->logged_width=true;
  }
  /* Uncertain is not failed. Keep measuring the selected baseline while
   * its interval includes the service target. Only a baseline whose UPPER
   * bound falls below the target warrants a different interpretation search.
   * Once started, keep that search separate from baseline CRC evidence. */
  if(nr_hyp_sweep_winner(&c->widths.engine)>=0 &&
     (c->interp.initialized || baseline_hi<0.60)) {
    if(!c->logged_width) { LOG_A(PHY,"UL width search converged: class=%d\n",wi); c->logged_width=true; }
    nr_pdcch_blind_ul_result_t probe;
    if(!nr_pdcch_blind_extract_01(payload,len,rnti,&ctx.opts,&probe)) goto done;
    if(c->tda_index<0) c->tda_index=probe.tda_index;
    if(probe.tda_index!=c->tda_index) {
      /* Per-index interpretation state is not yet implemented. Never apply entry zero
       * to traffic using another index or pool the CRC evidence of different entries. */
      if(!c->interp.refused) LOG_E(PHY,"UL interpretation unresolved: multiple observed TDA indices\n");
      c->interp.refused=true; goto done;
    }
    ctx.interpretation=true;
    if(!c->interp.initialized && !init_search(&c->interp,&ctx)) goto done;
    if(!still_equivalent(&c->interp,&payload,&ctx)) {
      LOG_W(PHY,"UL interpretation classes split; invalidating search evidence\n");
      clear_search(&c->interp); c->generation=++generation_counter; c->logged_interp=false; goto done;
    }
    ii=nr_hyp_sweep_next(&c->interp.engine,&payload,plausible,&ctx,&chosen);
    if(ii<0 || !nr_pdcch_ul_interp_sweep_apply(&chosen,c->tda_index,&ctx.opts)) goto done;
    if(nr_hyp_sweep_winner(&c->interp.engine)>=0 && !c->logged_interp) {
      LOG_A(PHY,"UL interpretation search converged: class=%d tda=%d\n",ii,c->tda_index);
      c->logged_interp=true;
    }
  }
  /* The existing receiver cannot resolve these antenna-port tables/DFT-s-OFDM.
   * This is missing receiver support, not evidence that the network hypothesis is false. */
  if(ctx.opts.transform_precoding || ctx.opts.dmrs_config_type!=0 || ctx.opts.dmrs_max_length>1)
    goto done;
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
  const bool width_owner=g->width_hyp_class>=0, interp_owner=g->interp_hyp_class>=0;
  if (width_owner==interp_owner || g->width_hyp_class < -1 || g->interp_hyp_class < -1) {
    ++rejected_feedback;
    LOG_W(PHY,"UL_FEEDBACK_REJECT reason=OWNER_NOT_UNIQUE generation=%lu rnti=0x%x\n",
          (unsigned long)g->hyp_generation,g->rnti);
    pthread_mutex_unlock(&lock);
    return;
  }
  for (int i=0; i<UL_DISCOVERY_CONTEXTS; ++i) {
    ul_context_t *c=&contexts[i];
    /* Generation, identity, stage and class all belong to the decoded grant. */
    if (g->hyp_generation != c->generation) continue;
    search_t *owner=width_owner?&c->widths:&c->interp;
    const int cls=width_owner?g->width_hyp_class:g->interp_hyp_class;
    if (g->rnti!=c->target_rnti || g->dci_length!=c->target_length
        || !owner->initialized || cls>=owner->engine.n_classes) {
      ++rejected_feedback;
      LOG_W(PHY,"UL_FEEDBACK_REJECT reason=IDENTITY_OR_CLASS_MISMATCH generation=%lu rnti=0x%x\n",
            (unsigned long)g->hyp_generation,g->rnti);
      break;
    }
    nr_hyp_sweep_feed(&owner->engine,cls,ok);
    if ((++c->feedbacks % 2000) == 0) {
      log_progress_locked(&c->widths,"width",c->target_rnti);
      log_progress_locked(&c->interp,"interp",c->target_rnti);
    }
    break;
  }
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
    s.interp_winners+=c->interp.initialized && nr_hyp_sweep_winner(&c->interp.engine)>=0;
    s.width_classes+=c->widths.engine.n_classes;
    s.interp_classes+=c->interp.engine.n_classes;
    for(int i=0;i<c->widths.engine.n_classes;++i) s.width_trials+=c->widths.engine.classes[i].trials;
    for(int i=0;i<c->interp.engine.n_classes;++i) s.interp_trials+=c->interp.engine.classes[i].trials;
  }
  pthread_mutex_unlock(&lock);
  return s;
}
