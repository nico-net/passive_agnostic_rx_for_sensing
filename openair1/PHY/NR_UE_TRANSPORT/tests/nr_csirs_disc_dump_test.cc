/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_synth.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <array>
#include <set>
#include <tuple>
extern "C" {
#include "common/utils/LOG/log.h"
#include "nr_csirs_monitor.h"
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *s, int) {fprintf(stderr,"%s\n",s?s:"");abort();}
int nr_csirs_blind_rt_test_confirm_ring(const nr_csirs_disc_ring_t *, fapi_nr_dl_config_csirs_pdu_rel15_t *,
                                       uint16_t *, uint16_t *, int16_t *, int, uint32_t *);
int nr_csirs_blind_rt_test_phases(int, uint16_t *);
uint64_t nr_csirs_blind_rt_test_confirm_us(void);
uint64_t nr_csirs_blind_rt_test_confirm_cpu_us(void);
void nr_csirs_blind_rt_test_dump_bank(void);
int nr_csirs_blind_rt_test_confirmed_entry(int, fapi_nr_dl_config_csirs_pdu_rel15_t *, uint16_t *, uint16_t *);
extern uint64_t csirs_disc_readmit_skipped;
int nr_csirs_blind_rt_test_discovered(int, fapi_nr_dl_config_csirs_pdu_rel15_t *,uint16_t *,uint16_t *);
}
TEST(CsirsDiscDump, ProductionStream)
{
  const char *path = getenv("ISAC_CSIRS_DISC_DUMP_IN");
  if (!path) GTEST_SKIP() << "Set ISAC_CSIRS_DISC_DUMP_IN to the OTA ring dump";
  setenv("ISAC_CSIRS_DISC", "1", 1);
  nr_csirs_disc_synth_init(); set_glog(OAILOG_INFO);
  FILE *file=fopen(path,"rb"); ASSERT_NE(file,nullptr);
  int32_t header[5]; ASSERT_EQ(fread(header,sizeof(header),1,file),1u);
  ASSERT_EQ(header[0],0x43534431); ASSERT_EQ(header[1],4); ASSERT_EQ(header[2],217);
  ASSERT_EQ(header[3],20); ASSERT_EQ(header[4],14);
  const bool require_gpu=getenv("ISAC_CSIRS_DISC_BACKEND") && !strcmp(getenv("ISAC_CSIRS_DISC_BACKEND"),"gpu");
  if(require_gpu) {
    const auto *ops=nr_csirs_disc_gpu_load();
    ASSERT_TRUE(ops && ops->accumulate && ops->lattice && ops->sweep) << "GPU pipeline required";
  }
  uint64_t cpu_detect=csirs_disc_cpu_detect_batches,cpu_lattice=csirs_disc_cpu_lattice_calls;
  nr_csirs_disc_ring_t ring={}, snapshot={};
  ASSERT_EQ(nr_csirs_disc_ring_init(&ring,4,217,20,NR_CSIRS_DISC_DEFAULT_CAPACITY,require_gpu),0);
  ASSERT_EQ(nr_csirs_disc_ring_init(&snapshot,4,217,20,NR_CSIRS_DISC_DEFAULT_CAPACITY,require_gpu),0);
  std::vector<int16_t> iq(ring.slot_stride);
  nr_csirs_disc_slot_meta_t meta;
  unsigned fed=0; int previous=0; double confirm_us=0, snapshot_us=0;
  std::array<unsigned,4> confirmed_at={}, winner_phases={};
  std::vector<uint64_t> batches, detection, hypotheses, sweep, confirm;
  const uint64_t drops_before=csirs_disc_feed_drops;
  setvbuf(stdout,nullptr,_IONBF,0);
  while (fread(&meta,sizeof(meta),1,file)==1) {
    ASSERT_EQ(fread(iq.data(),sizeof(int16_t),iq.size(),file),iq.size());
    const int16_t *ants[4];
    for(int a=0;a<4;++a) ants[a]=iq.data()+a*14*24*217;
    ASSERT_GE(nr_csirs_disc_ring_push(&ring,ants,12*217,0,meta.abs_slot,meta.frame,meta.slot),0);
    ++fed;
    if(fed%NR_CSIRS_DISC_DEFAULT_BATCH==0) {
      nr_csirs_disc_metrics_t before={},after={}; nr_csirs_disc_get_metrics(&before);
      auto copy_begin=std::chrono::steady_clock::now();
      ASSERT_EQ(nr_csirs_disc_ring_snapshot(&ring,&snapshot),0);
      snapshot_us+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-copy_begin).count();
      nr_csirs_disc_process(&snapshot); nr_csirs_disc_get_metrics(&after);
      batches.push_back(after.batch_us-before.batch_us);detection.push_back(after.detect_us-before.detect_us);
      hypotheses.push_back(after.hyp_us-before.hyp_us);sweep.push_back(after.sweep_us-before.sweep_us);
      for(int k=0;;++k) {
        fapi_nr_dl_config_csirs_pdu_rel15_t p={};uint16_t period=0,phases[2]={};
        int count=nr_csirs_blind_rt_test_discovered(k,&p,&period,phases);
        if(count<0) break;
        // The adaptive gate can admit additional weak/coherent provisional
        // hypotheses. U5 confirmation remains the authority for export; its
        // geometry, phase, duplicate and monitor assertions below stay strict.
        // Count only the eight exact target winners here, without using this
        // validation catalogue as an input to discovery.
        if(p.scramb_id==415 && p.row==1 && p.freq_density==3 && p.cdm_type==0
            && p.start_rb==0 && p.nr_of_rbs==52 && p.symb_l1==0 && period==160) {
          int at=p.symb_l0==4?0:p.symb_l0==8?1:p.symb_l0==5?2:p.symb_l0==9?3:-1;
          if(at>=0 && p.freq_domain==(at<2?1:2))
            for(int o=0;o<count;++o)
              if(phases[o]==15 || phases[o]==16) winner_phases[at]|=1u<<(phases[o]-15);
        }
      }
      printf("BATCH fed=%u detect=%llu hyp=%llu sweep=%llu total=%llu us\n",fed,
        (unsigned long long)(after.detect_us-before.detect_us),(unsigned long long)(after.hyp_us-before.hyp_us),
        (unsigned long long)(after.sweep_us-before.sweep_us),(unsigned long long)(after.batch_us-before.batch_us));
    }
    auto one=ring; one.capacity=1; one.grid_base=iq.data();one.meta=&meta;
    fapi_nr_dl_config_csirs_pdu_rel15_t pdus[32]={}; uint16_t periods[32]={},offsets[32]={};int16_t deltas[32]={};
    auto begin=std::chrono::steady_clock::now();
    uint64_t core_before=nr_csirs_blind_rt_test_confirm_us();
    int n=nr_csirs_blind_rt_test_confirm_ring(&one,pdus,periods,offsets,deltas,32,nullptr);
    confirm.push_back(nr_csirs_blind_rt_test_confirm_us()-core_before);
    confirm_us+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
    ASSERT_GE(n,0);
    for(int i=previous;i<n;++i) {
      uint16_t phases[2]={};int nph=nr_csirs_blind_rt_test_phases(i,phases);
      printf("DUMP CONFIRM fed=%u row=%u fd=%u l0=%u l1=%u dens=%u rb=%u:%u id=%u P=%u phases=%u,%u nph=%d delta=%d\n",
        fed,pdus[i].row,pdus[i].freq_domain,pdus[i].symb_l0,pdus[i].symb_l1,pdus[i].freq_density,
        pdus[i].start_rb,pdus[i].nr_of_rbs,pdus[i].scramb_id,periods[i],phases[0],phases[1],nph,deltas[i]);
      EXPECT_EQ(pdus[i].row,1);EXPECT_EQ(pdus[i].freq_density,3);EXPECT_EQ(pdus[i].nr_of_rbs,52);
      EXPECT_EQ(pdus[i].cdm_type,0);EXPECT_EQ(pdus[i].symb_l1,0);
      EXPECT_EQ(periods[i],160);EXPECT_EQ(deltas[i],0);
      if(pdus[i].scramb_id==415) {
        int at=pdus[i].symb_l0==4?0:pdus[i].symb_l0==8?1:pdus[i].symb_l0==5?2:pdus[i].symb_l0==9?3:-1;
        ASSERT_GE(at,0);EXPECT_EQ(pdus[i].freq_domain,at<2?1:2);EXPECT_EQ(pdus[i].start_rb,0);
        EXPECT_EQ(nph,2);EXPECT_EQ(std::min(phases[0],phases[1]),15);EXPECT_EQ(std::max(phases[0],phases[1]),16);
        EXPECT_EQ(confirmed_at[at],0u);confirmed_at[at]=fed;
      }
    }
    previous=n;
  }
  nr_csirs_blind_rt_test_dump_bank();
  printf("DUMP slots=%u confirmed=%d confirm_us=%.0f\n",fed,previous,confirm_us);
  auto stats=[](const char *name,std::vector<uint64_t> values) {
    ASSERT_FALSE(values.empty());std::sort(values.begin(),values.end());uint64_t total=0;
    for(auto v:values) total+=v;
    printf("TIMING %s mean=%.1f p50=%llu p95=%llu max=%llu us\n",name,double(total)/values.size(),
      (unsigned long long)values[values.size()/2],(unsigned long long)values[(values.size()-1)*95/100],(unsigned long long)values.back());
  };
  stats("batch",batches);stats("detect",detection);stats("hyp",hypotheses);stats("sweep",sweep);stats("confirm_slot",confirm);
  nr_csirs_disc_metrics_t cpu={};nr_csirs_disc_get_metrics(&cpu);
  double air=fed/(100.0*ring.slots_per_frame);
  printf("CPU air_s=%.6f detect_us=%llu hyp_us=%llu sweep_us=%llu confirm_us=%llu cpu_s_per_air_s=%.6f\n",
      air,(unsigned long long)cpu.detect_cpu_us,(unsigned long long)cpu.hyp_cpu_us,
      (unsigned long long)cpu.sweep_cpu_us,(unsigned long long)nr_csirs_blind_rt_test_confirm_cpu_us(),
      (cpu.cpu_us+nr_csirs_blind_rt_test_confirm_cpu_us())/1e6/air);
  printf("TIMING snapshot_total=%.0f confirm_total=%.0f us drops=%llu\n",snapshot_us,confirm_us,
    (unsigned long long)(csirs_disc_feed_drops-drops_before));
  for(int i=0;i<4;++i) {EXPECT_EQ(winner_phases[i],3u)<<i;EXPECT_GT(confirmed_at[i],0u)<<i;EXPECT_LE(confirmed_at[i],4000u)<<i;}
  // T14 checks the entire final physical bank, including weak neighbors. The
  // four-score actor budget now serves 20 resources; the dump deadline is 2 s
  // (4000 slots), versus T12's 2000-slot deadline for just the strong subset.
  using Key=std::tuple<unsigned,unsigned,unsigned,unsigned>;
  std::set<Key> expected;
  auto pair=[&](unsigned id,unsigned fd,unsigned rb,unsigned l) {
    expected.emplace(id,fd,rb,l); expected.emplace(id,fd,rb,l+4);
  };
  pair(415,1,0,4); pair(415,2,0,5);
  pair(748,4,0,4); pair(748,8,0,5);
  pair(713,4,128,4);
  pair(380,1,128,4); pair(380,2,128,5);
  pair(582,4,64,4); pair(582,8,64,5);
  pair(249,2,64,5);
  std::set<Key> actual;
  for(int k=0;;++k) {
    fapi_nr_dl_config_csirs_pdu_rel15_t p={}; uint16_t period=0, phases[2]={};
    int count=nr_csirs_blind_rt_test_confirmed_entry(k,&p,&period,phases);
    if(count<0) break;
    printf("DUMP FINAL row=%u fd=%u l0=%u rb=%u:%u id=%u P=%u phases=%u,%u nph=%d\n",
           p.row,p.freq_domain,p.symb_l0,p.start_rb,p.nr_of_rbs,p.scramb_id,period,phases[0],phases[1],count);
    EXPECT_TRUE(actual.emplace(p.scramb_id,p.freq_domain,p.start_rb,p.symb_l0).second);
    EXPECT_EQ(p.row,1); EXPECT_EQ(p.nr_of_rbs,52); EXPECT_EQ(p.freq_density,3);
    EXPECT_EQ(p.cdm_type,0); EXPECT_EQ(p.symb_l1,0); EXPECT_EQ(period,160); EXPECT_EQ(count,2);
    unsigned phase=p.start_rb==128 && p.symb_l0%4==0 ? 20 : 15;
    EXPECT_EQ(phases[0],phase); EXPECT_EQ(phases[1],phase+1);
  }
  EXPECT_EQ(actual,expected);
  printf("DUMP readmit_skipped=%llu\n",(unsigned long long)csirs_disc_readmit_skipped);
  EXPECT_GT(csirs_disc_readmit_skipped,0u);
  for(unsigned phase : {15u,16u,20u,21u,95u,96u,100u,101u}) {
    unsigned slot=(meta.abs_slot/640+1)*640+phase;
    const fapi_nr_dl_config_csirs_pdu_rel15_t *due[64]={};
    int count=nr_csirs_monitor_due_ex(slot/20,slot%20,20,due,nullptr,64);
    std::set<Key> exported;
    for(int k=0;k<count;++k)
      EXPECT_TRUE(exported.emplace(due[k]->scramb_id,due[k]->freq_domain,due[k]->start_rb,due[k]->symb_l0).second);
    EXPECT_EQ(count,phase<17?16:phase<22?4:0);
    for(const auto &key:exported) EXPECT_EQ(expected.count(key),1u);
  }
  // Confirm actual monitor registration for both phases, including bank pressure from neighbors.
  for(unsigned phase:{15u,16u}) {
    uint32_t future=(meta.abs_slot/160+1)*160+phase;
    const fapi_nr_dl_config_csirs_pdu_rel15_t *due[64]={};int16_t delta[64]={};
    int count=nr_csirs_monitor_due_ex(future/20,future%20,20,due,delta,64);
    unsigned found=0;
    for(int i=0;i<count;++i) if(due[i]->scramb_id==415) {
      EXPECT_EQ(due[i]->start_rb,0);EXPECT_EQ(due[i]->nr_of_rbs,52);EXPECT_EQ(delta[i],0);
      EXPECT_EQ(due[i]->row,1);EXPECT_EQ(due[i]->freq_density,3);
      EXPECT_EQ(due[i]->cdm_type,0);EXPECT_EQ(due[i]->symb_l1,0);
      int at=due[i]->symb_l0==4?0:due[i]->symb_l0==8?1:due[i]->symb_l0==5?2:due[i]->symb_l0==9?3:-1;
      ASSERT_GE(at,0);EXPECT_EQ(due[i]->freq_domain,at<2?1:2);found|=1u<<at;
    }
    EXPECT_EQ(found,15u)<<"monitor phase="<<phase;
  }
  if(require_gpu) {
    EXPECT_EQ(csirs_disc_cpu_detect_batches,cpu_detect);
    EXPECT_EQ(csirs_disc_cpu_lattice_calls,cpu_lattice);
    EXPECT_GT(csirs_disc_gpu_lattice_calls,0u);
  }
  EXPECT_EQ(fed,5468u);EXPECT_EQ(csirs_disc_feed_drops,drops_before);
  nr_csirs_disc_ring_free(&snapshot);nr_csirs_disc_ring_free(&ring);nr_csirs_disc_detect_reset();fclose(file);
}
