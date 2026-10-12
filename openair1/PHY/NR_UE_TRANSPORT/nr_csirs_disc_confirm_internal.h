/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Included by the RT owner. MPSC owned slots: actors never acquire the state
 * mutex. A separate reserve protects pending confirmations from maintenance
 * backlog. Saturation is counted; contention never discards a pending slot.
 * Lifecycle: prepare before RX, drain on csirsDisc only, reset under state lock. */
#define DISC_CONFIRM_QUEUE 128
#define DISC_CONFIRM_RESERVE 128
static struct {
  unsigned state; /* 0 free, 1 producer, 2 ready/owned by the sole consumer */
  uint64_t ticket, epoch;
  uint32_t absolute_slot;
  int slot;
  uint16_t symbols;
  bool raw;
  nr_csirs_disc_fep_slot_t fep;
  c16_t *iq;
} discovery_queue[DISC_CONFIRM_QUEUE + DISC_CONFIRM_RESERVE];
static uint16_t discovery_due[640], discovery_pending[640];
static uint64_t discovery_ticket, discovery_epoch;
static uint64_t discovery_birth[NR_CSIRS_BLIND_MAX_CAND];
static __thread uint64_t discovery_consumed_ticket;
static NR_DL_FRAME_PARMS discovery_frame;
static bool discovery_queue_ready,discovery_disabled;
/* nr_csirs_blind_rt_discovery_disable: disables the discovery path for the process (discovery_disabled) and marks the queue ready so that producers stop enqueuing.
 *   Called by nr_csirs_disc_pipeline_init() through a weak symbol when the GPU pipeline could not be prepared.
 */
void nr_csirs_blind_rt_discovery_disable(void) {discovery_disabled=true;discovery_queue_ready=true;}
static __thread bool discovery_consuming;
static __thread uint64_t discovery_consumed_epoch;
static __thread uint16_t discovery_copied_symbols;
static c16_t *discovery_storage;
extern const nr_csirs_disc_gpu_ops_t *nr_csirs_disc_gpu_load(void) __attribute__((weak));
extern uint32_t get_samples_slot_timestamp(const NR_DL_FRAME_PARMS *,unsigned) __attribute__((weak));
#pragma weak nr_slot_fep_fo_override_hz
#pragma weak nr_csirs_disc_alloc_unified
static const nr_csirs_disc_gpu_ops_t *discovery_gpu;
static size_t discovery_iq_stride;
static nr_csirs_disc_fep_cfg_t discovery_fep_cfg;
static nr_csirs_disc_ring_t discovery_fep_ring;
static int16_t discovery_compact_fallback[NR_CSIRS_DISC_MAX_ANT*14*24*NR_CSIRS_DISC_MAX_RB] __attribute__((aligned(64)));
static int16_t *discovery_compact=discovery_compact_fallback;
static bool discovery_device_scoring;
/* discovery_symbols: returns the 14-bit mask of OFDM symbols that a CSI-RS candidate's mapping occupies (its symbol set from get_csi_mapping_parms).
 *   Pure function. Callers: discovery_publish(), discovery_enqueue(), discovery_gpu_score() and nr_csirs_blind_rt.c (score_candidate).
 */
static uint16_t discovery_symbols(const nr_csirs_candidate_t *c)
{
  csi_mapping_parms_t m=get_csi_mapping_parms(c->row,c->freq_domain,c->symb_l0,c->symb_l1);
  uint16_t bits=0;
  for(int j=0;j<m.size;++j) for(int lp=0;lp<=m.lprime;++lp) bits |= 1u<<(m.loverline[j]+lp);
  return bits;
}
/* Immutable export snapshots. Readers reserve a buffer before touching it;
 * writer never waits for a DL reader and can defer publication to next drain. */
static struct {
  unsigned users;
  int n;
  struct { fapi_nr_dl_config_csirs_pdu_rel15_t p; uint16_t period, phase[2]; int nphase; } resource[2*NR_CSIRS_BLIND_DISC_MAX_CONF];
} discovery_exports[3];
static unsigned discovery_active;
/* discovery_export: copies the exported CSI-RS resources that occur on `slot` (up to max) from the active immutable export snapshot.
 *   Readers reserve the snapshot with a user count and never block the publisher; the writer switches the active buffer. Returns the count.
 *   Callers: nr_csirs_blind_rt_observed_nzp() and nr_csirs_blind_rt_rate_match_all() when discovery is queued.
 */
static int discovery_export(uint32_t slot,fapi_nr_dl_config_csirs_pdu_rel15_t *out,int max)
{
  if(discovery_disabled || !out || max<=0) return 0;
  unsigned b;
  for(;;) {
    b=__atomic_load_n(&discovery_active,__ATOMIC_ACQUIRE);
    unsigned users=__atomic_load_n(&discovery_exports[b].users,__ATOMIC_RELAXED);
    if(users==UINT32_MAX) continue;
    if(!__atomic_compare_exchange_n(&discovery_exports[b].users,&users,users+1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) continue;
    if(b==__atomic_load_n(&discovery_active,__ATOMIC_ACQUIRE)) break;
    __atomic_fetch_sub(&discovery_exports[b].users,1,__ATOMIC_RELEASE);
  }
  int n=0;
  for(int i=0;i<discovery_exports[b].n && n<max;++i) {
    const __typeof__(discovery_exports[b].resource[0]) *r=&discovery_exports[b].resource[i];
    if(r->period && (slot%r->period==r->phase[0] || (r->nphase==2 && slot%r->period==r->phase[1]))) {
      out[n++]=r->p;
    }
  }
  __atomic_fetch_sub(&discovery_exports[b].users,1,__ATOMIC_RELEASE);
  return n;
}
/* discovery_cooccur: BLINDFIX F1. Slots (of 640) in which the exported resources a and c both occur, 0 when either is not in
 * the active snapshot. Resources match on geometry and scrambling ID. Caller: nr_csirs_blind_rt.c full_add_block(). */
static int discovery_cooccur(const fapi_nr_dl_config_csirs_pdu_rel15_t *a, const fapi_nr_dl_config_csirs_pdu_rel15_t *c)
{
  if(discovery_disabled || !discovery_queue_ready) return 0;
  unsigned b;
  for(;;) {
    b=__atomic_load_n(&discovery_active,__ATOMIC_ACQUIRE);
    unsigned users=__atomic_load_n(&discovery_exports[b].users,__ATOMIC_RELAXED);
    if(users==UINT32_MAX) continue;
    if(!__atomic_compare_exchange_n(&discovery_exports[b].users,&users,users+1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) continue;
    if(b==__atomic_load_n(&discovery_active,__ATOMIC_ACQUIRE)) break;
    __atomic_fetch_sub(&discovery_exports[b].users,1,__ATOMIC_RELEASE);
  }
  const __typeof__(discovery_exports[0].resource[0]) *ra=NULL,*rc=NULL;
  for(int i=0;i<discovery_exports[b].n;++i) {
    const __typeof__(discovery_exports[0].resource[0]) *r=&discovery_exports[b].resource[i];
    const fapi_nr_dl_config_csirs_pdu_rel15_t *p=&r->p;
    #define SAME(x) (p->row==(x)->row && p->freq_domain==(x)->freq_domain && p->symb_l0==(x)->symb_l0 && p->symb_l1==(x)->symb_l1 && p->cdm_type==(x)->cdm_type \
                     && p->freq_density==(x)->freq_density && p->start_rb==(x)->start_rb && p->nr_of_rbs==(x)->nr_of_rbs && p->scramb_id==(x)->scramb_id)
    if(!ra && SAME(a)) ra=r;
    if(!rc && SAME(c)) rc=r;
    #undef SAME
  }
  const int j=(ra && rc) ? nbs_joint_slots(ra->period,ra->phase,ra->nphase,rc->period,rc->phase,rc->nphase) : 0;
  __atomic_fetch_sub(&discovery_exports[b].users,1,__ATOMIC_RELEASE);
  return j;
}
/* discovery_publish: recomputes the due and pending slot tables (per absolute slot mod 640) from the search state and publishes a new export snapshot into an idle buffer.
 *   Due = occasions that must be scored (seeds not yet settled, confirmed resources when reconfiguration detection is on, ZP probation and divisor probes); pending = unsettled ones.
 *   Caller: the search code under g_csirs_state_lock (nr_csirs_blind_rt_discovery_*, csirs_unlock).
 */
static void discovery_publish(void)
{
  if(discovery_disabled || !discovery_queue_ready) return;
  uint16_t due[640]={0},pending[640]={0};
  for(int i=0;i<g_st.n;++i) {
    unsigned p=g_st.seed_period[i];
    if(!p || 640%p) continue;
    uint16_t mask=discovery_symbols(&g_st.cand[i]);
    bool unsettled=true;
    for(int k=0;k<g_st.n_conf;++k) if(g_st.conf_idx[k]==i) {
      unsettled=g_st.conf_n_off[k]<g_st.seed_n_off[i] || g_st.conf_period[k]>p;break;
    }
    for(unsigned phase=0;phase<g_st.seed_n_off[i];++phase) {
      if(!unsettled && !g_disc_probe[i][phase] && g_reconf==0) continue;
      unsigned off=phase?g_st.seed_offset2[i]:g_st.seed_offset[i];
      for(unsigned t=off;t<640;t+=p) {
        due[t]|=mask;
        if(unsettled) pending[t]|=mask;
      }
    }
  }
  if(g_reconf!=0) for(int k=0;k<g_st.n_conf;++k) {
    unsigned p=g_st.conf_period[k];if(!p || 640%p) continue;
    uint16_t mask=discovery_symbols(&g_st.cand[g_st.conf_idx[k]]);
    for(int phase=0;phase<g_st.conf_n_off[k];++phase)
      for(unsigned t=g_st.conf_off[k][phase]%p;t<640;t+=p) due[t]|=mask;
  }
  /* ZP probation includes proper-divisor probes, not just exported occasions. */
  for(int k=0;k<NR_CSIRS_BLIND_MAX_CONF;++k) {
    const nr_csirs_zp_probation_t *b=&g_zp.zp_bank[k];
    if(!b->owner || g_zp.zp_failed_run[b->owner-1]==UINT64_MAX) continue;
    uint16_t mask=discovery_symbols(&g_zp.cand[b->owner-1]);
    if(!b->period) {for(int t=0;t<640;++t) due[t]|=mask;continue;}
    bool confirmed=nr_csirs_blind_is_confirmed(&g_zp,b->owner-1);
    for(int pi=-1;pi<NR_CSIRS_BLIND_N_PERIODS;++pi) {
      unsigned p=pi<0?b->period:nr_csirs_blind_periods[pi];
      if(!p || 640%p || (pi>=0 && (confirmed || p>=b->period || b->period%p))) continue;
      for(int phase=0;phase<b->n_off;++phase) for(unsigned t=b->off[phase]%p;t<640;t+=p) due[t]|=mask;
    }
  }
  for(int t=0;t<640;++t) {
    __atomic_store_n(&discovery_pending[t],pending[t],__ATOMIC_RELEASE);
    __atomic_store_n(&discovery_due[t],due[t],__ATOMIC_RELEASE);
  }
  unsigned active=__atomic_load_n(&discovery_active,__ATOMIC_RELAXED);
  for(unsigned b=0;b<3;++b) if(b!=active) {
    unsigned zero=0;
    if(!__atomic_compare_exchange_n(&discovery_exports[b].users,&zero,UINT32_MAX,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) continue;
    discovery_exports[b].n=0;
    if(g_on>0 && g_armed && g_rank<=0) for(int bank=0;bank<2;++bank) {
      nr_csirs_blind_state_t *st=bank?&g_zp:&g_st;
      for(int k=0;k<st->n_conf;++k) {
        __typeof__(discovery_exports[b].resource[0]) *r=&discovery_exports[b].resource[discovery_exports[b].n++];
        fill_pdu(&st->cand[st->conf_idx[k]],bank?2:1,&r->p);
        r->period=st->conf_period[k];r->nphase=st->conf_n_off[k];
        r->phase[0]=st->conf_off[k][0];r->phase[1]=st->conf_off[k][1];
      }
    }
    __atomic_store_n(&discovery_exports[b].users,0,__ATOMIC_RELEASE);
    __atomic_store_n(&discovery_active,b,__ATOMIC_RELEASE);
    break;
  }
}
/* discovery_mark_admitted: stamps a newly admitted candidate with the current ticket, so that only slots enqueued after its admission are scored for it. */
static void discovery_mark_admitted(int i) {discovery_birth[i]=__atomic_load_n(&discovery_ticket,__ATOMIC_RELAXED);}
/* discovery_queue_reset: advances the discovery epoch so that all queued slots of the old epoch are discarded by the consumer.
 *   Caller: nr_csirs_blind_rt_discovery_reset().
 */
static void discovery_queue_reset(void)
{
  __atomic_add_fetch(&discovery_epoch,1,__ATOMIC_ACQ_REL);
}
/* nr_csirs_blind_rt_discovery_prepare: sizes and allocates the discovery queue for a frame configuration (IQ storage per slot, compact symbol buffer and, with a GPU, the device FEP ring) and publishes the initial due tables.
 *   Returns 0, or -1 for unsupported frame parameters or allocation failure. Idempotent for the same frame parameters (-1 when they differ).
 *   Caller: nr_csirs_disc_pipeline_init() through a weak symbol (nr_csirs_disc.c).
 */
int nr_csirs_blind_rt_discovery_prepare(const NR_DL_FRAME_PARMS *fp)
{
  if(!fp || !fp->ofdm_symbol_size || fp->nb_antennas_rx<1 || fp->nb_antennas_rx>4) return -1;
  if(discovery_queue_ready) return memcmp(&discovery_frame,fp,sizeof(*fp))? -1:0;
  discovery_gpu=nr_csirs_disc_gpu_load?nr_csirs_disc_gpu_load():NULL;
  size_t td_stride=14*(fp->ofdm_symbol_size+fp->nb_prefix_samples0+fp->nb_prefix_samples);
  discovery_iq_stride=td_stride>fp->samples_per_slot_wCP?td_stride:fp->samples_per_slot_wCP;
  size_t stride=(size_t)fp->nb_antennas_rx*discovery_iq_stride;
  size_t bytes=stride*(DISC_CONFIRM_QUEUE+DISC_CONFIRM_RESERVE)*sizeof(c16_t);
  discovery_storage=discovery_gpu && &nr_csirs_disc_alloc_unified && nr_csirs_disc_alloc_unified?nr_csirs_disc_alloc_unified(bytes):calloc(1,bytes);
  if(!discovery_storage) return -1;
  if(discovery_gpu && &nr_csirs_disc_alloc_unified && nr_csirs_disc_alloc_unified) {
    discovery_compact=nr_csirs_disc_alloc_unified((size_t)fp->nb_antennas_rx*14*24*fp->N_RB_DL*sizeof(int16_t));
    if(!discovery_compact) return -1;
  }
  discovery_frame=*fp;
  if(discovery_gpu && discovery_gpu->device_alloc) {
    discovery_fep_ring=(nr_csirs_disc_ring_t){.n_ant=fp->nb_antennas_rx,.n_rb=fp->N_RB_DL,
      .slots_per_frame=fp->slots_per_frame,.capacity=1,.grid_base=discovery_compact,
      .slot_stride=(size_t)fp->nb_antennas_rx*14*24*fp->N_RB_DL};
    discovery_fep_ring.device_grid=discovery_gpu->device_alloc(discovery_fep_ring.slot_stride*2);
    if(!discovery_fep_ring.device_grid) return -1;
    discovery_fep_cfg=(nr_csirs_disc_fep_cfg_t){.fft=fp->ofdm_symbol_size,.n_ant=fp->nb_antennas_rx,
      .n_rb=fp->N_RB_DL,.first_carrier=fp->first_carrier_offset,.samples_per_ms=fp->samples_per_subframe,
      .stride=discovery_iq_stride,.timeshift=(int16_t *)fp->timeshift_symbol_rotation};
#if defined(__aarch64__) && defined(__ARM_FEATURE_QRDMX)
    discovery_fep_cfg.rotation_mode=2;
#elif defined(__aarch64__)
    discovery_fep_cfg.rotation_mode=1;
#endif
  }
  for(int i=0;i<DISC_CONFIRM_QUEUE+DISC_CONFIRM_RESERVE;++i) discovery_queue[i].iq=discovery_storage+i*stride;
  discovery_queue_ready=true;
  discovery_publish();
  return 0;
}
/* discovery_enqueue: producer side of the discovery queue: for a slot with due symbols, takes a free queue entry and copies or FFTs the due symbols into it (raw IQ on the GPU path, FEP per symbol otherwise).
 *   Non-blocking: a full queue counts a confirm drop. Returns true when the slot was handled by the queue (or dropped), false when the caller must run the search directly.
 *   Caller: nr_csirs_blind_rt_slot_fep() and nr_csirs_blind_rt_slot() (when not consuming).
 */
static bool discovery_enqueue(PHY_VARS_NR_UE *ue,int slot,uint32_t absolute_slot,
                              c16_t rx[][ue->frame_parms.samples_per_slot_wCP],bool map[14])
{
  if(discovery_disabled) return true;
  if(!discovery_queue_ready) return false; /* standalone legacy fixture */
  const NR_DL_FRAME_PARMS *fp=&ue->frame_parms;
  if(fp->ofdm_symbol_size!=discovery_frame.ofdm_symbol_size || fp->nb_antennas_rx!=discovery_frame.nb_antennas_rx
      || fp->samples_per_slot_wCP!=discovery_frame.samples_per_slot_wCP) return true;
  uint64_t epoch=__atomic_load_n(&discovery_epoch,__ATOMIC_ACQUIRE);
  uint16_t mask=__atomic_load_n(&discovery_due[absolute_slot%640],__ATOMIC_ACQUIRE);
  if(!mask) return true;
  uint64_t begin=now_us();
  bool pending=__atomic_load_n(&discovery_pending[absolute_slot%640],__ATOMIC_ACQUIRE)!=0;
  int limit=pending?DISC_CONFIRM_QUEUE+DISC_CONFIRM_RESERVE:DISC_CONFIRM_QUEUE,at=-1;
  uint64_t ticket=__atomic_fetch_add(&discovery_ticket,1,__ATOMIC_RELAXED);
  for(int k=0;k<limit;++k) {
    int i=(ticket+k)%limit;unsigned zero=0;
    if(__atomic_compare_exchange_n(&discovery_queue[i].state,&zero,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {at=i;break;}
  }
  uint64_t wait=now_us()-begin,peak=__atomic_load_n(&csirs_disc_confirm_wait_max_us,__ATOMIC_RELAXED);
  while(wait>peak && !__atomic_compare_exchange_n(&csirs_disc_confirm_wait_max_us,&peak,wait,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
  if(at<0) {__atomic_fetch_add(&csirs_disc_confirm_drops,1,__ATOMIC_RELAXED);return true;}
  bool missing=false;for(int l=0;l<14;++l) if((mask&(1u<<l)) && !map[l]) missing=true;
  bool raw=missing && discovery_gpu && discovery_gpu->fep && get_samples_slot_timestamp && fp->samples_per_frame && fp->ofdm_offset_divisor;
  discovery_queue[at].raw=raw;
  if(raw) {
    nr_csirs_disc_fep_slot_t *d=&discovery_queue[at].fep;
    d->samples=(int16_t *)discovery_queue[at].iq;d->compensate=ue->cont_fo_comp;
    d->fo=&nr_slot_fep_fo_override_hz && !isnan(nr_slot_fep_fo_override_hz)?nr_slot_fep_fo_override_hz:ue->dl_Doppler_shift+ue->freq_offset;
    unsigned offset=get_samples_slot_timestamp(fp,slot);
    unsigned cp0=ue->is_synchronized?fp->nb_prefix_samples0:fp->nb_prefix_samples;
    for(int l=0;l<14;++l) {
      offset+=((slot*14+l)%(7<<fp->numerology_index))?fp->nb_prefix_samples:cp0;
      d->offset[l]=offset-fp->nb_prefix_samples/fp->ofdm_offset_divisor;offset+=fp->ofdm_symbol_size;
      memcpy(d->rotation+2*l,&fp->symbol_rotation[link_type_dl][(slot%fp->slots_per_subframe)*14+l],sizeof(c16_t));
    }
    unsigned span=d->offset[13]-d->offset[0]+fp->ofdm_symbol_size,total=2*fp->samples_per_frame;
    unsigned start=d->offset[0]%total,first=span<total-start?span:total-start;
    if(span>discovery_iq_stride) {__atomic_store_n(&discovery_queue[at].state,0,__ATOMIC_RELEASE);return true;}
    for(int a=0;a<fp->nb_antennas_rx;++a) {
      c16_t *dst=discovery_queue[at].iq+a*discovery_iq_stride;
      memcpy(dst,ue->common_vars.rxdata[a]+start,first*sizeof(c16_t));
      if(first<span) memcpy(dst+first,ue->common_vars.rxdata[a],(span-first)*sizeof(c16_t));
    }
  } else {
  for(int l=0;l<14;++l) if(mask&(1u<<l)) {
    if(!map[l]) {nr_slot_fep(ue,fp,slot,l,rx,link_type_dl,0,ue->common_vars.rxdata);map[l]=true;}
    for(int a=0;a<fp->nb_antennas_rx;++a)
      memcpy(discovery_queue[at].iq+(size_t)a*fp->samples_per_slot_wCP+l*fp->ofdm_symbol_size,
             rx[a]+l*fp->ofdm_symbol_size,fp->ofdm_symbol_size*sizeof(c16_t));
  }
  }
  discovery_queue[at].ticket=ticket;discovery_queue[at].epoch=epoch;
  discovery_queue[at].absolute_slot=absolute_slot;discovery_queue[at].slot=slot;discovery_queue[at].symbols=mask;
  __atomic_store_n(&discovery_queue[at].state,2,__ATOMIC_RELEASE);
  return true;
}
/* discovery_pack: packs the due OFDM symbols of one slot (all antennas) from the FEP output into the compact staging buffer, removing the carrier offset wrap.
 *   Used before a GPU upload or GPU scoring. Callers: nr_csirs_blind_rt_discovery_drain() and discovery_gpu_score().
 */
static void discovery_pack(const NR_DL_FRAME_PARMS *fp,const c16_t *rx,uint16_t symbols)
{
  int nk=12*fp->N_RB_DL,first=nk<fp->ofdm_symbol_size-fp->first_carrier_offset?nk:fp->ofdm_symbol_size-fp->first_carrier_offset;
  for(int a=0;a<fp->nb_antennas_rx;++a) for(int l=0;l<14;++l) if(symbols&(1u<<l)) {
    int16_t *dst=discovery_compact+((size_t)a*14+l)*2*nk;
    const c16_t *src=rx+(size_t)a*fp->samples_per_slot_wCP+l*fp->ofdm_symbol_size;
    memcpy(dst,src+fp->first_carrier_offset,first*sizeof(c16_t));
    if(first<nk) memcpy(dst+2*first,src,(nk-first)*sizeof(c16_t));
  }
}
/* nr_csirs_blind_rt_discovery_drain: consumer side of the discovery queue (the csirsDisc worker): processes up to 32 queued slots in ticket order, running the search for each one under the discovery flags.
 *   Slots of an old epoch are dropped. GPU raw entries run their FEP first; otherwise the due symbols are packed and uploaded. Each slot is scored by nr_csirs_blind_rt_slot().
 *   Returns the number of entries completed. Caller: disc_worker_main() in nr_csirs_disc_feed.c (weak symbol) and the test bridge.
 */
int nr_csirs_blind_rt_discovery_drain(void)
{
  if(discovery_disabled || !discovery_queue_ready) return 0;
  static PHY_VARS_NR_UE worker_ue;
  worker_ue.frame_parms=discovery_frame;
  int done=0;
  /* A finite quantum lets ingest and discovery make progress under load. */
  for(int q=0;q<32;++q) {
    int at=-1;uint64_t ticket=UINT64_MAX;
    for(int i=0;i<DISC_CONFIRM_QUEUE+DISC_CONFIRM_RESERVE;++i)
      if(__atomic_load_n(&discovery_queue[i].state,__ATOMIC_ACQUIRE)==2 && discovery_queue[i].ticket<ticket)
        {at=i;ticket=discovery_queue[i].ticket;}
    if(at<0) break;
    if(discovery_queue[at].epoch==__atomic_load_n(&discovery_epoch,__ATOMIC_ACQUIRE)) {
      discovery_device_scoring=false;
      int rc=0;
      if(discovery_queue[at].raw) {
        rc=discovery_gpu->fep(&discovery_fep_cfg,&discovery_queue[at].fep,1,&discovery_fep_ring,0);
        discovery_device_scoring=rc==0;
      } else if(discovery_gpu && discovery_gpu->upload_symbols) {
        // One H2D copy of the due symbols, reused by every resource this slot.
        discovery_pack(&discovery_frame,discovery_queue[at].iq,discovery_queue[at].symbols);
        rc=discovery_gpu->upload_symbols(&discovery_fep_ring,discovery_queue[at].symbols);
        discovery_device_scoring=rc==0;
      }
      if(rc<0) {
        __atomic_fetch_add(&csirs_disc_confirm_drops,1,__ATOMIC_RELAXED);
        __atomic_store_n(&discovery_queue[at].state,0,__ATOMIC_RELEASE);++done;continue;
      }
      discovery_consumed_ticket=discovery_queue[at].ticket;
      discovery_consumed_epoch=discovery_queue[at].epoch;
      discovery_consuming=true;discovery_copied_symbols=discovery_queue[at].symbols;
      nr_csirs_blind_rt_slot(&worker_ue,discovery_queue[at].slot,discovery_queue[at].absolute_slot,
          (c16_t (*)[discovery_frame.samples_per_slot_wCP])discovery_queue[at].iq);
      discovery_consuming=false;
    }
    __atomic_store_n(&discovery_queue[at].state,0,__ATOMIC_RELEASE);++done;
  }
  return done;
}

/* discovery_gpu_score: scores one CSI-RS candidate on the GPU (the confirm entry of the sweep module) on the compact symbol buffer and writes five scores.
 *   Returns the GPU rc, or -1 when no confirm entry exists. Packs the symbols unless the device copy of this slot is already valid.
 *   Caller: score_candidate() in nr_csirs_blind_rt.c.
 */
static int discovery_gpu_score(const NR_DL_FRAME_PARMS *fp,const nr_csirs_candidate_t *c,int slot,
                              const c16_t *rx,double scores[5])
{
  const nr_csirs_disc_gpu_ops_t *ops=nr_csirs_disc_gpu_load?nr_csirs_disc_gpu_load():NULL;
  if(!ops || !ops->confirm) return -1;
  int16_t *compact=discovery_compact;
  uint16_t symbols=discovery_symbols(c);
  int nk=12*fp->N_RB_DL;
  if(!(discovery_consuming && discovery_device_scoring)) discovery_pack(fp,rx,symbols);
  nr_csirs_disc_slot_meta_t meta={.slot=(slot+c->slot_delta+fp->slots_per_frame)%fp->slots_per_frame,.valid=1};
  nr_csirs_disc_ring_t ring={.n_ant=fp->nb_antennas_rx,.n_rb=fp->N_RB_DL,.slots_per_frame=fp->slots_per_frame,
      .capacity=1,.grid_base=compact,.slot_stride=(size_t)fp->nb_antennas_rx*14*2*nk,.meta=&meta};
  nr_csirs_disc_hyp_t h={.row=c->row,.freq_domain=c->freq_domain,.symb_l0=c->symb_l0,.symb_l1=c->symb_l1,
      .cdm_type=c->cdm_type,.density=c->freq_density,.rb_start=c->start_rb,.n_rb=c->nr_of_rbs,.period=1};
  if(discovery_consuming && discovery_device_scoring) {ring.device_grid=discovery_fep_ring.device_grid;ring.device_valid=true;}
  return ops->confirm(&ring,&h,c->scramb_id,scores);
}
