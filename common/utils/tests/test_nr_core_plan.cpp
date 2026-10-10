/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_core_plan.h"
#include <gtest/gtest.h>
#include <sched.h>
#include <cstring>
#include <set>
#include <climits>
#include <unistd.h>

static nr_core_machine_t host(int count, int dpdk_first = -1, bool big_little = false)
{
  nr_core_machine_t m = {};
  strcpy(m.host,big_little ? "mock-DGX" : "mock-x86");
  for(int c=0;c<count;c++) {
    m.online[c]=m.allowed[c]=1; m.irq[c]=1; m.sibling[c]=c; m.cluster[c]=c/10;
    m.capacity[c]=big_little && c%10>=5 ? 1024 : 512;
    if(dpdk_first>=0 && c>=dpdk_first) { m.dpdk[c]=1; if(big_little) { m.isolated[c]=1; m.irq[c]=0; } }
  }
  return m;
}
static nr_core_plan_t request(bool ring, int pdsch=3, bool uhd_without_ring=false)
{
  nr_core_plan_t p = {};
  nr_core_plan_add(&p,NR_CP_UE,1,-1);
  if(ring) nr_core_plan_add(&p,NR_CP_RING,1,-1);
  if(ring || uhd_without_ring) nr_core_plan_add(&p,NR_CP_UHD,1,-1);
  nr_core_plan_add(&p,NR_CP_DL,1,-1); nr_core_plan_add(&p,NR_CP_UL,1,-1); nr_core_plan_add(&p,NR_CP_SYNC,1,-1);
  nr_core_plan_add(&p,NR_CP_SCAN,1,-1); nr_core_plan_add(&p,NR_CP_USS,1,-1);
  nr_core_plan_add(&p,NR_CP_PDSCH,pdsch,-1); nr_core_plan_add(&p,NR_CP_POOL,4,-1);
  nr_core_plan_add(&p,NR_CP_GPU,1,-1);nr_core_plan_add(&p,NR_CP_CSIRS,1,-1);nr_core_plan_add(&p,NR_CP_NID,1,-1);
  nr_core_plan_add(&p,NR_CP_STATS,1,-1);return p;
}
static int cpu(const nr_core_plan_t &p, nr_core_role_t role, int idx=0)
{
  for(int i=0;i<p.count;i++) if(p.a[i].role==role && p.a[i].index==idx) return p.a[i].core;
  return -1;
}
static void pin(nr_core_plan_t &p, nr_core_role_t role, int index, int core)
{
  for (int i=0;i<p.count;i++) if (p.a[i].role==role && p.a[i].index==index) {
    p.a[i].core=core; p.a[i].explicit_core=true; return;
  }
  FAIL() << "missing role " << nr_core_role_name(role) << '[' << index << ']';
}
static void plan_line(const nr_core_plan_t &p)
{
  printf("SENSING: CORE_PLAN fixture ->");
  for (int i=0;i<p.count;i++) printf(" %s[%d]:%d%s%s",nr_core_role_name(p.a[i].role),p.a[i].index,
                                      p.a[i].core,p.a[i].explicit_core ? "(user)" : "",p.a[i].shared ? "*" : "");
  puts("");
}
static void invariant(const nr_core_machine_t &m,const nr_core_plan_t &p)
{
  for(int i=0;i<p.count;i++) {
    int c=p.a[i].core; ASSERT_GE(c,0); ASSERT_LT(c,NR_CP_MAX_CPUS);
    EXPECT_TRUE(m.allowed[c]); EXPECT_FALSE(m.dpdk[c]);
    if(p.a[i].role<=NR_CP_SYNC) {
      EXPECT_FALSE(p.background[c]);
      for(int j=0;j<p.count;j++) if(i!=j) { EXPECT_NE(c,p.a[j].core); }
    } else EXPECT_TRUE(p.background[c]);
  }
}
TEST(CorePlan,Dgx)
{
  auto m=host(20,17,true);auto p=request(true);char error[512];
  ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;invariant(m,p);
  EXPECT_EQ(5,cpu(p,NR_CP_UE));EXPECT_EQ(15,cpu(p,NR_CP_RING));EXPECT_EQ(16,cpu(p,NR_CP_UHD));
  EXPECT_EQ(6,cpu(p,NR_CP_DL));EXPECT_EQ(7,cpu(p,NR_CP_UL));EXPECT_EQ(8,cpu(p,NR_CP_SYNC));EXPECT_EQ(9,cpu(p,NR_CP_SCAN));
  for(int i=0;i<p.count;i++) printf("%s[%d]:%d%s ",nr_core_role_name(p.a[i].role),p.a[i].index,p.a[i].core,p.a[i].shared ? "*" : "");
  puts("");
}
TEST(CorePlan,DgxCapacityVariation)
{
  auto m=host(20,17,true); m.capacity[5]=997; m.capacity[15]=1017;
  auto p=request(true);char error[512];
  ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;
  EXPECT_EQ(5,cpu(p,NR_CP_UE));EXPECT_EQ(15,cpu(p,NR_CP_RING));
}
TEST(CorePlan,DgxDedicatedExplicitLayout)
{
  // spark-74c3: CPUs 0..19, isolated/nohz 17..19, IRQ 0..16,
  // UHD DPDK lcores 17..19. The dldedicated cfg has one scan on 6 and
  // three PDSCH consumers starting at 2. Four explicit pool CPUs: 8,9,0,1.
  auto m=host(20,17,true);char error[512];
  for (int c=17;c<20;c++) m.nohz[c]=1;
  for (bool ring : {false,true}) {
    SCOPED_TRACE(ring ? "ring enabled" : "exact listed env: ring disabled");
    auto p=request(ring,3,true);
    pin(p,NR_CP_UE,0,5);if(ring) pin(p,NR_CP_RING,0,16);pin(p,NR_CP_UHD,0,15);
    pin(p,NR_CP_SCAN,0,6);pin(p,NR_CP_USS,0,7);
    for (int i=0;i<3;i++) pin(p,NR_CP_PDSCH,i,2+i);
    const int pool[]={8,9,0,1};
    for (int i=0;i<4;i++) pin(p,NR_CP_POOL,i,pool[i]);
    pin(p,NR_CP_GPU,0,10);pin(p,NR_CP_CSIRS,0,11);pin(p,NR_CP_NID,0,13);
    // ISAC_CSIRS_DISC_DETECT_CORES=12,13,14 is unused by the T16 single GPU worker.
    const auto requested=p;
    ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;
    for (int i=0;i<p.count;i++) {
      EXPECT_FALSE(m.dpdk[p.a[i].core]);
      if(p.a[i].explicit_core) { EXPECT_EQ(requested.a[i].core,p.a[i].core); }
    }
    EXPECT_NE(cpu(p,NR_CP_DL),cpu(p,NR_CP_UL));
    EXPECT_EQ(5,cpu(p,NR_CP_UE));EXPECT_EQ(15,cpu(p,NR_CP_UHD));
    EXPECT_EQ(ring ? 16 : -1,cpu(p,NR_CP_RING));EXPECT_EQ(13,cpu(p,NR_CP_NID));
    EXPECT_GE(cpu(p,NR_CP_SYNC),0);
    if (ring) {
      bool shared_rt=false;
      for (int i=0;i<p.count;i++) if (p.a[i].role<=NR_CP_SYNC && p.a[i].shared) shared_rt=true;
      EXPECT_TRUE(shared_rt);
    }
    plan_line(p);
  }
}
TEST(CorePlan,DgxDedicatedAutomaticLayout)
{
  auto m=host(20,17,true);auto p=request(true);char error[512];
  for (int c=17;c<20;c++) m.nohz[c]=1;
  ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;
  invariant(m,p);plan_line(p);
}
TEST(CorePlan,Sens6)
{
  auto m=host(12,9);auto p=request(true);char error[512];
  ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;invariant(m,p);
  EXPECT_EQ(0,cpu(p,NR_CP_UE));EXPECT_EQ(1,cpu(p,NR_CP_RING));EXPECT_EQ(2,cpu(p,NR_CP_UHD));
  EXPECT_EQ(6,cpu(p,NR_CP_SCAN));EXPECT_TRUE(p.a[p.count-1].shared);
  for(int i=0;i<p.count;i++) printf("%s[%d]:%d%s ",nr_core_role_name(p.a[i].role),p.a[i].index,p.a[i].core,p.a[i].shared ? "*" : "");
  puts("");
}
TEST(CorePlan,EightNoDpdk)
{
  auto m=host(8);auto p=request(false);char error[512];
  ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;invariant(m,p);
  EXPECT_EQ(0,cpu(p,NR_CP_UE));EXPECT_EQ(4,cpu(p,NR_CP_SCAN));
}
TEST(CorePlan,OverridesReservedBeforeAutomatic)
{
  auto m=host(20,17,true);auto p=request(true);char error[512];
  for(int i=0;i<p.count;i++) if(p.a[i].role==NR_CP_PDSCH) { p.a[i].core=5+p.a[i].index;p.a[i].explicit_core=true; }
  ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;invariant(m,p);
  EXPECT_EQ(8,cpu(p,NR_CP_UE));EXPECT_EQ(5,cpu(p,NR_CP_PDSCH));
}
TEST(CorePlan,RejectUnsafeOverridesAndImpossibleHosts)
{
  char error[512];auto m=host(12,9); auto p=request(true);
  p.a[0].core=10;p.a[0].explicit_core=true;EXPECT_LT(nr_core_plan_build(&m,&p,error,sizeof(error)),0);
  p=request(true);p.a[0].core=1;p.a[0].explicit_core=true;p.a[1].core=1;p.a[1].explicit_core=true;
  EXPECT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;
  EXPECT_TRUE(p.a[0].shared);EXPECT_TRUE(p.a[1].shared);
  p=request(true);m=host(4);EXPECT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;
}
TEST(CorePlan,SparseCpusetAndSmt)
{
  auto m=host(32,30);for(int i=0;i<16;i++) {m.sibling[i+16]=i;m.cluster[i+16]=m.cluster[i];}
  for(int i=0;i<4;i++) m.allowed[i]=m.allowed[i+16]=0;
  auto p=request(true);char error[512];ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;
  for(int i=0;i<p.count;i++) {
    EXPECT_GE(p.a[i].core%16,4);EXPECT_LT(p.a[i].core%16,14);
    if(p.a[i].role<=NR_CP_SYNC) for(int j=0;j<p.count;j++) if(i!=j) { EXPECT_NE(p.a[i].core%16,p.a[j].core%16); }
  }
}
TEST(CorePlan,IrqAndIsolationPreference)
{
  auto m=host(12);m.irq[7]=0;m.isolated[7]=1;m.nohz[7]=1;auto p=request(false);char error[512];
  ASSERT_EQ(0,nr_core_plan_build(&m,&p,error,sizeof(error)))<<error;EXPECT_EQ(7,cpu(p,NR_CP_UE));
}
TEST(CorePlan,ParsersAndBounds)
{
  unsigned char mask[NR_CP_MAX_CPUS]={};
  ASSERT_EQ(0,nr_core_parse_list(" 0-3,8,31-32\n",mask));EXPECT_TRUE(mask[32]);EXPECT_FALSE(mask[4]);
  for(auto s:{"-1","3-1","1024","0,","0, ","1x","99999999999999999999"}) EXPECT_LT(nr_core_parse_list(s,mask),0)<<s;
  ASSERT_EQ(0,nr_core_parse_hex("00000001,80000003\n",mask));EXPECT_TRUE(mask[0]);EXPECT_TRUE(mask[1]);EXPECT_TRUE(mask[31]);EXPECT_TRUE(mask[32]);EXPECT_FALSE(mask[2]);
  memset(mask,0,sizeof(mask));EXPECT_EQ(1,nr_core_parse_uhd("[other]\ndpdk_corelist=2\n[use_dpdk = 1]\ndpdk_corelist = 17,18,19 # data\n",mask));
  EXPECT_FALSE(mask[2]);EXPECT_TRUE(mask[17]);EXPECT_TRUE(mask[19]);
  nr_core_plan_t p={};EXPECT_LT(nr_core_plan_add(&p,NR_CP_POOL,INT_MAX,-1),0);EXPECT_LT(nr_core_plan_add(&p,NR_CP_POOL,2,1023),0);
}
TEST(CorePlan,DisabledHooksAreInert)
{
  EXPECT_FALSE(nr_core_plan_active());EXPECT_EQ(-1,nr_core_plan_cpu(NR_CP_UE,0,-1));
  pthread_attr_t attr;pthread_attr_init(&attr);int priority=97;EXPECT_EQ(0,nr_core_plan_thread_attr(&attr,100000,&priority));EXPECT_EQ(97,priority);pthread_attr_destroy(&attr);
}

/* Actual Linux affinity, in a child so neither the test runner nor other tests inherit it. */
TEST(CorePlan,ApplyBeforeThreadEntry)
{
  nr_core_machine_t probe_machine; nr_core_plan_t probe_plan={}; char probe_error[512];
  nr_core_plan_add(&probe_plan,NR_CP_UE,1,-1); nr_core_plan_add(&probe_plan,NR_CP_STATS,1,-1);
  if (nr_core_machine_read(&probe_machine,nullptr,probe_error,sizeof(probe_error)) ||
      nr_core_plan_build(&probe_machine,&probe_plan,probe_error,sizeof(probe_error)))
    GTEST_SKIP() << "Need two allowed physical CPUs: " << probe_error;
  ASSERT_EXIT({
    nr_core_machine_t m; nr_core_plan_t p={}; char error[512];
    if(nr_core_machine_read(&m,nullptr,error,sizeof(error))) _exit(1);
    int allowed=0; for(int c=0;c<NR_CP_MAX_CPUS;c++) allowed+=m.online[c]&&m.allowed[c];
    if(allowed<2) _exit(0); // solver corner cases are covered by mocks
    if(nr_core_plan_add(&p,NR_CP_UE,1,-1) || nr_core_plan_add(&p,NR_CP_STATS,1,-1)
       || nr_core_plan_build(&m,&p,error,sizeof(error))) _exit(2);
    // Model a logging thread that existed before planning.
    struct Early { const nr_core_plan_t *p; pthread_barrier_t barrier; } early;
    early.p=&p; pthread_barrier_init(&early.barrier,nullptr,2);
    auto early_worker=[](void *arg)->void * {
      auto *e=static_cast<Early *>(arg); pthread_barrier_wait(&e->barrier);
      cpu_set_t mask; if(sched_getaffinity(0,sizeof(mask),&mask)) return reinterpret_cast<void *>(1);
      for(int c=0;c<NR_CP_MAX_CPUS;c++) if(CPU_ISSET(c,&mask)&&!e->p->background[c]) return reinterpret_cast<void *>(2);
      return nullptr;
    };
    pthread_t early_thread; void *early_result;
    if(pthread_create(&early_thread,nullptr,early_worker,&early)) _exit(13);
    unsetenv("ISAC_UE_RT_CORE");
    if(nr_core_plan_activate(&m,&p)) _exit(3);
    pthread_barrier_wait(&early.barrier);
    if(pthread_join(early_thread,&early_result)||early_result) _exit(14);
    pthread_barrier_destroy(&early.barrier);
    if(atoi(getenv("ISAC_UE_RT_CORE"))!=cpu(p,NR_CP_UE)) _exit(4);
    cpu_set_t before; if(sched_getaffinity(0,sizeof(before),&before)) _exit(5);
    if(CPU_ISSET(cpu(p,NR_CP_UE),&before)) _exit(6);
    pthread_attr_t attr;pthread_attr_init(&attr);int priority=97;
    if(nr_core_plan_thread_attr(&attr,cpu(p,NR_CP_UE),&priority)||priority!=97) _exit(7);
    cpu_set_t actual;pthread_attr_getaffinity_np(&attr,sizeof(actual),&actual);
    if(CPU_COUNT(&actual)!=1||!CPU_ISSET(cpu(p,NR_CP_UE),&actual)) _exit(8);
    priority=97;
    if(nr_core_plan_thread_attr(&attr,cpu(p,NR_CP_STATS),&priority)||priority!=0) _exit(9);
    if(!nr_core_plan_thread_attr(&attr,NR_CP_MAX_CPUS,&priority)) _exit(10);
    pthread_attr_destroy(&attr);
    // Simulate a raw helper spawned by a pinned RT actor.
    CPU_ZERO(&actual);CPU_SET(cpu(p,NR_CP_UE),&actual);
    if(sched_setaffinity(0,sizeof(actual),&actual)) _exit(11);
    auto worker=[](void *arg)->void * {
      const auto *plan=static_cast<nr_core_plan_t *>(arg);
      cpu_set_t mask;if(sched_getaffinity(0,sizeof(mask),&mask)) return reinterpret_cast<void *>(1);
      for(int c=0;c<NR_CP_MAX_CPUS;c++) if(CPU_ISSET(c,&mask)&&!plan->background[c]) return reinterpret_cast<void *>(2);
      int policy;struct sched_param sp;
      pthread_getschedparam(pthread_self(),&policy,&sp);
      return policy==SCHED_OTHER ? nullptr : reinterpret_cast<void *>(3);
    };
    pthread_t t;void *result;
    if(nr_core_plan_create(&t,nullptr,worker,&p,-1)||pthread_join(t,&result)||result) _exit(12);
    _exit(0);
  },::testing::ExitedWithCode(0),"CORE_PLAN");
}

TEST(EnvList,Nth){
  unsetenv("DTQ_L");
  EXPECT_EQ(nr_env_list_nth("DTQ_L",0,-1),-1);
  setenv("DTQ_L","17,18",1);
  EXPECT_EQ(nr_env_list_nth("DTQ_L",0,-1),17);
  EXPECT_EQ(nr_env_list_nth("DTQ_L",1,-1),18);
  EXPECT_EQ(nr_env_list_nth("DTQ_L",2,-1),-1);
  setenv("DTQ_L","x",1);
  EXPECT_EQ(nr_env_list_nth("DTQ_L",0,5),5);
}

int main(int argc,char **argv){::testing::InitGoogleTest(&argc,argv);return RUN_ALL_TESTS();}
