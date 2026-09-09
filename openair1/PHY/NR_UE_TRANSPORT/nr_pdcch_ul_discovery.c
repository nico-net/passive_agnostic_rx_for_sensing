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
  bool logged_width, logged_interp;
} ul_context_t;
static ul_context_t contexts[UL_DISCOVERY_CONTEXTS];
static uint64_t generation_counter = 1, context_clock;

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
static bool equivalent(const nr_hyp_t *a, const nr_hyp_t *b, const void *sample, void *ctx)
{
  nr_pdcch_blind_ul_result_t ga,gb;
  /* Two failures are NOT equal grants. In particular, unsupported modes cannot be
   * collapsed away as if they had been proved observationally equivalent. */
  if (!extract(a,sample,ctx,&ga) || !extract(b,sample,ctx,&gb)) return false;
  /* extract zero-initializes the entire struct; reject_reason is NULL on success.
   * Compare all decoded fields (including UCI), not just PRB shape or MCS. */
  return memcmp(&ga,&gb,sizeof(ga))==0;
}
static bool plausible(const nr_hyp_t *h, const void *candidate, void *ctx)
{
  nr_pdcch_blind_ul_result_t out;
  return extract(h,candidate,ctx,&out);
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
  int classes=s->n_raw>0 ? nr_hyp_sweep_init(&s->engine,s->raw,s->n_raw,NULL,NULL,
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
    if (!equivalent(rep,&s->raw[i],p,ctx)) return false;
  }
  return true;
}
bool nr_pdcch_ul_discovery_grant(const nr_pdcch_blind_ul_opts_t *fixed, uint16_t len,
                                 uint16_t rnti, uint64_t payload, nr_pdcch_blind_ul_result_t *out)
{
  if (!fixed || !out || !rnti || !len || len>63) return false;
  pthread_mutex_lock(&lock);
  ul_context_t *c = NULL, *oldest = &contexts[0];
  for (int i=0; i<UL_DISCOVERY_CONTEXTS; ++i) {
    if (contexts[i].target_rnti == rnti) { c=&contexts[i]; break; }
    if (contexts[i].touched < oldest->touched) oldest=&contexts[i];
  }
  if (!c) { c=oldest; reset_locked(c); }
  bool ok=false;
  if (c->target_rnti!=rnti || c->target_length!=len || memcmp(&c->baseline,fixed,sizeof(c->baseline))) {
    reset_locked(c); c->baseline=*fixed; c->target_rnti=rnti; c->target_length=len;
  }
  c->touched = ++context_clock;
  bool novel=true;
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
  /* Finite-sample equivalence is provisional. New distinguishing traffic invalidates
   * old class scores and queued feedback; never keep a convenient representative silently. */
  if (!still_equivalent(&c->widths,&payload,&ctx)) {
    LOG_W(PHY,"UL width classes split on new payload; invalidating search evidence\n");
    clear_search(&c->widths); clear_search(&c->interp); c->generation=++generation_counter;
    c->logged_width=c->logged_interp=false;
    goto done;
  }
  nr_hyp_t chosen;
  int wi=nr_hyp_sweep_next(&c->widths.engine,&payload,plausible,&ctx,&chosen);
  if(wi<0) goto done;
  nr_pdcch_ul_field_sweep_apply(&chosen,&ctx.opts);
  int ii=-1;
  if(nr_hyp_sweep_winner(&c->widths.engine)>=0) {
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
    out->width_hyp_class=nr_hyp_sweep_winner(&c->widths.engine)<0?wi:-1;
    out->interp_hyp_class=ii>=0 && nr_hyp_sweep_winner(&c->interp.engine)<0?ii:-1;
    out->hyp_generation=c->generation;
  }
done:
  pthread_mutex_unlock(&lock);
  return ok;
}
void nr_pdcch_ul_discovery_feedback(const nr_pdcch_blind_ul_result_t *g, bool ok)
{
  if (!g || !g->hyp_generation) return;
  pthread_mutex_lock(&lock);
  for (int i=0; i<UL_DISCOVERY_CONTEXTS; ++i) {
    ul_context_t *c=&contexts[i];
    if (g->hyp_generation != c->generation || g->rnti != c->target_rnti) continue;
    if (c->widths.initialized && g->width_hyp_class>=0)
      nr_hyp_sweep_feed(&c->widths.engine,g->width_hyp_class,ok);
    if (c->interp.initialized && g->interp_hyp_class>=0)
      nr_hyp_sweep_feed(&c->interp.engine,g->interp_hyp_class,ok);
    break;
  }
  pthread_mutex_unlock(&lock);
}

nr_pdcch_ul_discovery_snapshot_t nr_pdcch_ul_discovery_snapshot(void)
{
  pthread_mutex_lock(&lock);
  nr_pdcch_ul_discovery_snapshot_t s={.generation=generation_counter};
  for (int k=0; k<UL_DISCOVERY_CONTEXTS; ++k) {
    const ul_context_t *c=&contexts[k];
    s.raw_samples+=c->nsamples;
    s.width_classes+=c->widths.engine.n_classes;
    s.interp_classes+=c->interp.engine.n_classes;
    for(int i=0;i<c->widths.engine.n_classes;++i) s.width_trials+=c->widths.engine.classes[i].trials;
    for(int i=0;i<c->interp.engine.n_classes;++i) s.interp_trials+=c->interp.engine.classes[i].trials;
  }
  pthread_mutex_unlock(&lock);
  return s;
}
