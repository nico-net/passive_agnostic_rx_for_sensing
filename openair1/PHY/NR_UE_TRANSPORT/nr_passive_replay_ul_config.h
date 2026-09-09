/* Recorded-IQ diagnostic only. Configuration is an explicit hypothesis, never
 * published to the online discovery controller. Called only after live DL TB
 * bytes have been reproduced by the production decoder on this recording. */
#include "nr_pdcch_blind_monitor_rt.h"
#include <stdatomic.h>

extern _Atomic long nr_ue_diag_producer_absolute_slot;

static void replay_ul_config(PHY_VARS_NR_UE *ue, const replay_header_t *h,
                             const unsigned char *samples)
{
  const nr_pdcch_blind_monitor_cfg_t *cfg=nr_pdcch_blind_monitor_get_cfg();
  nr_pdcch_blind_monitor_init(); /* Live initialization normally occurs after this replay exit. */
  nr_pdcch_blind_ul_opts_t opts=cfg->ul;
  opts.phy_cell_id=h->fp.Nid_cell;
  opts.numerology=h->fp.numerology_index;
  opts.dmrs_typeA_position=cfg->dmrs_typeA_position;
  if (!opts.bwp_size || opts.tda_count<=0 || !cfg->dci01_length_override) {
    printf("UL-CONFIG VOID: explicit recorded-receiver BWP/TDA/length hypothesis required\n");
    return;
  }
  const int ta=(int)((uint64_t)25600*h->fp.samples_per_subframe/(4096ull*480ull));
  const long saved=atomic_load(&nr_ue_diag_producer_absolute_slot);
  unsigned attempts=0,passes=0,rejects=0,repeat_bad=0;
  for (unsigned i=0;i<h->n_ul;++i) {
    const replay_ul_t *r=&h->ul[i];
    if (r->length!=cfg->dci01_length_override) continue;
    nr_pdcch_blind_ul_result_t g;
    if (!nr_pdcch_blind_extract_01(r->payload,r->length,r->rnti,&opts,&g)) {
      ++rejects; continue;
    }
    const long target=r->source+g.k2,index=target-h->start;
    if (index<h->fp.slots_per_frame || index>=h->slots) { ++rejects; continue; }
    replay_load_slot(ue,h,samples,index);
    atomic_store(&nr_ue_diag_producer_absolute_slot,target);
    nr_pusch_passive_out_t out,repeat;
    nr_pusch_passive_decode(ue,0,(target/h->fp.slots_per_frame)%1024,
        target%h->fp.slots_per_frame,&g,ta,target,false,h->slot[index].fo,&out);
    uint64_t hash=UINT64_C(14695981039346656037);
    if (out.status==NR_PUSCH_PASSIVE_OK)
      for(unsigned j=0;j<out.tbs_bytes;++j) hash=(hash^out.tb[j])*UINT64_C(1099511628211);
    nr_pusch_passive_decode(ue,0,(target/h->fp.slots_per_frame)%1024,
        target%h->fp.slots_per_frame,&g,ta,target,false,h->slot[index].fo,&repeat);
    uint64_t rhash=UINT64_C(14695981039346656037);
    if (repeat.status==NR_PUSCH_PASSIVE_OK)
      for(unsigned j=0;j<repeat.tbs_bytes;++j) rhash=(rhash^repeat.tb[j])*UINT64_C(1099511628211);
    const bool identical=out.status==repeat.status && out.tbs_bytes==repeat.tbs_bytes && hash==rhash;
    repeat_bad+=!identical;
    const bool tried=out.status==NR_PUSCH_PASSIVE_OK || out.status==NR_PUSCH_PASSIVE_CRC_FAIL
                   || out.status==NR_PUSCH_PASSIVE_ZERO_TB;
    attempts+=tried; passes+=out.status==NR_PUSCH_PASSIVE_OK; rejects+=!tried;
    printf("UL-CONFIG source=%ld target=%ld rnti=%04x bits=%u mcs=%u table=%u rb=%u+%u "
           "sym=%u+%u dmrs=%x ports=%x cdm=%u scid=%u rv=%u tbs=%u G=%u "
           "delay=%d pre=%d segments=%d/%d status=%u identical=%d\n",
           r->source,target,r->rnti,r->length,g.mcs,g.mcs_table,g.start_rb,g.num_rb,
           g.start_symbol,g.num_symbols,g.ul_dmrs_symb_pos,g.dmrs_ports,g.n_dmrs_cdm_groups,
           g.nscid,g.rv,out.tbs_bytes,out.G,out.est_delay,out.est_delay_pre,
           out.segments_ok,out.n_segments,out.status,identical);
  }
  atomic_store(&nr_ue_diag_producer_absolute_slot,saved);
  printf("UL-CONFIG %s: crc=%u/%u rejected=%u repeat_mismatches=%u; hypothesis only, not convergence\n",
         repeat_bad?"VOID":"REPEATABLE",passes,attempts,rejects,repeat_bad);
}
