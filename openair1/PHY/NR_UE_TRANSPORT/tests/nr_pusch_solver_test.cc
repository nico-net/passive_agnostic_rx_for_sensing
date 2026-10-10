#include <gtest/gtest.h>
#include <cstdlib>
extern "C" {
#include "nr_pusch_solver.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "executables/softmodem-common.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { std::abort(); }
softmodem_params_t *get_softmodem_params(void) { static softmodem_params_t p; return &p; }
}

static nr_pusch_solver_reading_t reading(unsigned rb, uint64_t layouts, unsigned payload)
{
  nr_pusch_solver_reading_t r{};
  auto &g=r.grant;
  g.plausible=true;
  g.rnti=0x8f5a;
  g.dci_length=44;
  g.raw_payload.w[0]=payload;
  g.bwp_start=0; g.bwp_size=217;
  g.start_rb=rb; g.num_rb=6;
  g.start_symbol=1; g.num_symbols=13; g.k2=4; g.tda_index=0;
  g.ul_dmrs_symb_pos=(1u<<2)|(1u<<11);
  g.n_dmrs_cdm_groups=1;
  g.mcs=2; g.mcs_table=0; g.rv=0; g.nrOfLayers=1;
  g.data_scrambling_id=415; g.ul_dmrs_scrambling_id=415;
  r.layouts=layouts;
  return r;
}

TEST(PuschSolver, HiddenReadingNeedsTwoDistinctDcis)
{
  setenv("ISAC_UL_PUSCH_SOLVER","1",1);
  nr_pusch_solver_reset();
  const nr_ue_ul_ticket_t ticket={1,2,3,0x8f5a};
  const uint16_t ids[]={415};
  nr_pusch_solver_reading_t rd[]={reading(40,1,0x123),reading(60,2,0x123)};
  nr_pusch_solver_var_t vars[2];
  int n=nr_pusch_solver_plan(&ticket,rd,2,100,104,0,ids,1,vars,2);
  ASSERT_EQ(n,2);
  EXPECT_EQ(vars[0].grant.start_rb,40);
  EXPECT_EQ(vars[1].grant.start_rb,60);
  nr_pusch_solver_feedback(&ticket,&vars[0],false,true,1000);
  nr_pusch_solver_feedback(&ticket,&vars[1],true,true,2000);
  EXPECT_FALSE(nr_pusch_solver_is_solved(&ticket));
  rd[0]=reading(41,1,0x456); rd[1]=reading(61,2,0x456);
  n=nr_pusch_solver_plan(&ticket,rd,2,140,144,0,ids,1,vars,2);
  ASSERT_GE(n,1);
  EXPECT_EQ(vars[0].grant.start_rb,61);
  if (n>1) {
    nr_pusch_solver_feedback(&ticket,&vars[1],false,true,1000);
    EXPECT_FALSE(nr_pusch_solver_is_solved(&ticket));
  }
  nr_pusch_solver_feedback(&ticket,&vars[0],true,true,1500);
  EXPECT_TRUE(nr_pusch_solver_is_solved(&ticket));
  rd[0]=reading(42,1,0x789); rd[1]=reading(62,2,0x789);
  n=nr_pusch_solver_plan(&ticket,rd,2,180,184,0,ids,1,vars,2);
  ASSERT_EQ(n,1);
  EXPECT_FALSE(vars[0].probe);
  EXPECT_EQ(vars[0].grant.start_rb,62);
  nr_pusch_solver_feedback(&ticket,&vars[0],true,true,3000);
  nr_pusch_solver_stats_t s{};
  nr_pusch_solver_stats(&s);
  EXPECT_GE(s.probe_ok,2);
  EXPECT_EQ(s.candidates,1);
  EXPECT_EQ(s.solved,1);
  EXPECT_EQ(s.full_ok,1);
}

TEST(PuschSolver, HiddenDmrsLayoutIsEnumeratedAndConfirmed)
{
  nr_pusch_solver_reset();
  const nr_ue_ul_ticket_t ticket={1,2,3,0x8f5a};
  const uint16_t ids[]={415};
  auto rd=reading(20,1,0x123);
  const int hidden_mask=nr_pdcch_blind_ul_dmrs_mask(13,1,0,2,1,0);
  ASSERT_GT(hidden_mask,0);
  rd.grant.ul_dmrs_symb_pos=nr_pdcch_blind_ul_dmrs_mask(13,1,0,1,1,0);
  ASSERT_NE(rd.grant.ul_dmrs_symb_pos,hidden_mask);
  nr_pusch_solver_var_t v[1];
  bool found=false;
  for(int i=0;i<64 && !found;++i) {
    const uint64_t slot=100+40*i;
    if(nr_pusch_solver_plan(&ticket,&rd,1,slot,slot+4,0,ids,1,v,1)!=1) continue;
    found=v[0].grant.ul_dmrs_symb_pos==hidden_mask;
    nr_pusch_solver_feedback(&ticket,&v[0],found,true,1000);
  }
  ASSERT_TRUE(found);
  EXPECT_FALSE(nr_pusch_solver_is_solved(&ticket));
  rd.grant.raw_payload.w[0]=0x456;
  ASSERT_EQ(nr_pusch_solver_plan(&ticket,&rd,1,4000,4004,0,ids,1,v,1),1);
  EXPECT_TRUE(v[0].confirmation);
  EXPECT_EQ(v[0].grant.ul_dmrs_symb_pos,hidden_mask);
  nr_pusch_solver_feedback(&ticket,&v[0],true,true,1000);
  EXPECT_TRUE(nr_pusch_solver_is_solved(&ticket));
}

TEST(PuschSolver, DedicatedDmrsIdentityIsTriedBeforeGeometrySweep)
{
  nr_pusch_solver_reset();
  const nr_ue_ul_ticket_t ticket={1,2,3,0x8f5a};
  const uint16_t ids[]={415,62922};
  auto rd=reading(20,1,0x123);
  nr_pusch_solver_var_t v[1];
  bool found=false;
  for(int i=0;i<8 && !found;++i) {
    const uint64_t slot=100+40*i;
    if(nr_pusch_solver_plan(&ticket,&rd,1,slot,slot+4,0,ids,2,v,1)!=1) continue;
    found=v[0].grant.ul_dmrs_scrambling_id==62922 &&
          v[0].grant.data_scrambling_id==415;
    nr_pusch_solver_feedback(&ticket,&v[0],found,true,1000);
  }
  ASSERT_TRUE(found);
  rd.grant.raw_payload.w[0]=0x456;
  ASSERT_EQ(nr_pusch_solver_plan(&ticket,&rd,1,2000,2004,0,ids,2,v,1),1);
  EXPECT_TRUE(v[0].confirmation);
  EXPECT_EQ(v[0].grant.ul_dmrs_scrambling_id,62922);
  nr_pusch_solver_feedback(&ticket,&v[0],true,true,1000);
  EXPECT_TRUE(nr_pusch_solver_is_solved(&ticket));
}

TEST(PuschSolver, SameDciCannotConfirmAndBucketDrops)
{
  nr_pusch_solver_reset();
  const nr_ue_ul_ticket_t ticket={1,2,3,0x8f5a};
  const uint16_t ids[]={415};
  auto rd=reading(20,1,0x123);
  nr_pusch_solver_var_t vars[4];
  int n=nr_pusch_solver_plan(&ticket,&rd,1,100,104,0,ids,1,vars,1);
  ASSERT_EQ(n,1);
  nr_pusch_solver_feedback(&ticket,&vars[0],true,true,1000);
  auto duplicate=vars[0];
  duplicate.confirmation=true;
  nr_pusch_solver_feedback(&ticket,&duplicate,true,true,1000);
  EXPECT_FALSE(nr_pusch_solver_is_solved(&ticket));
  for(int i=0;i<4;++i) (void)nr_pusch_solver_plan(&ticket,&rd,1,100,104,0,ids,1,vars,1);
  nr_pusch_solver_stats_t s{};
  nr_pusch_solver_stats(&s);
  EXPECT_GT(s.drops,0);
}

TEST(PuschSolver, FalseCandidateCanBeReplacedByAnotherLayout)
{
  nr_pusch_solver_reset();
  const nr_ue_ul_ticket_t ticket={1,2,3,0x8f5a};
  const uint16_t ids[]={415};
  auto rd=reading(20,1,0x123);
  nr_pusch_solver_var_t v[1];
  ASSERT_EQ(nr_pusch_solver_plan(&ticket,&rd,1,100,104,0,ids,1,v,1),1);
  nr_pusch_solver_feedback(&ticket,&v[0],true,true,1000); // CRC16 false pass
  auto replacement=v[0];
  replacement.layouts=2;
  replacement.dci_hash++;
  replacement.dci_slot=140;
  replacement.confirmation=false;
  nr_pusch_solver_feedback(&ticket,&replacement,true,true,1000);
  EXPECT_FALSE(nr_pusch_solver_is_solved(&ticket));
  auto confirmation=replacement;
  confirmation.confirmation=true;
  confirmation.dci_hash++;
  confirmation.dci_slot=180;
  nr_pusch_solver_feedback(&ticket,&confirmation,true,true,1000);
  EXPECT_TRUE(nr_pusch_solver_is_solved(&ticket));
}

TEST(PuschSolver, ProbeAdmissionRejectsWideAndHighMcsGrants)
{
  nr_pusch_solver_reset();
  const nr_ue_ul_ticket_t ticket={1,2,3,0x8f5a};
  const uint16_t ids[]={415};
  nr_pusch_solver_var_t v[2];
  auto wide=reading(20,1,0x123);
  wide.grant.num_rb=80;
  EXPECT_EQ(nr_pusch_solver_plan(&ticket,&wide,1,100,104,0,ids,1,v,2),0);
  auto high=reading(20,1,0x456);
  high.grant.mcs=22;
  EXPECT_EQ(nr_pusch_solver_plan(&ticket,&high,1,140,144,0,ids,1,v,2),0);
  EXPECT_FALSE(nr_pusch_solver_is_solved(&ticket));
}

int main(int argc, char **argv)
{
  logInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
