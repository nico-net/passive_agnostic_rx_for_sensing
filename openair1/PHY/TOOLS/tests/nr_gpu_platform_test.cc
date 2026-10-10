/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include "nr_gpu_platform_internal.h"
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
namespace {
int attrs[128], count, calls, device_allocs, host_allocs, managed_allocs, frees, registrations;
unsigned flags;
int copy_kind, syncs;
void *copy_stream;
char memory[64][256];
int next_buffer, warnings, info_lines, failed_attribute, cleared_errors;
void log_message(int warning,const char *) { if (warning) ++warnings; else ++info_lines; }
int get_count(int *n) { ++calls; *n=count; return 0; }
int get_device(int *d) { *d=0; return 0; }
int attr(int *v,int a,int) { *v=attrs[a]; return a==failed_attribute ? 1 : 0; }
int mem_info(size_t *free,size_t *total) { *free=1024; *total=2048; return 0; }
int device_alloc(void **p,size_t) { ++device_allocs; *p=memory[next_buffer++]; return 0; }
int host_alloc(void **p,size_t,unsigned f) { ++host_allocs; flags=f; *p=memory[next_buffer++]; return 0; }
int managed_alloc(void **p,size_t,unsigned f) { ++managed_allocs; flags=f; *p=memory[next_buffer++]; return 0; }
int release(void *) { ++frees; return 0; }
int device_ptr(void **d,void *h,unsigned) { *d=h; return 0; }
int reg(void *,size_t,unsigned f) { ++registrations; flags=f; return 0; }
int copy(void *d,const void *s,size_t b,int kind,void *stream) { copy_kind=kind; copy_stream=stream; memcpy(d,s,b); return 0; }
int stream_sync(void *) { ++syncs; return 0; }
int device_sync() { ++syncs; return 0; }
int set_flags(unsigned) { return 0; }
int get_last_error() { ++cleared_errors; return 0; }
const nr_gpu_runtime_t runtime={get_count,get_device,attr,mem_info,device_alloc,host_alloc,managed_alloc,
                              release,release,device_ptr,reg,release,copy,stream_sync,device_sync,set_flags,get_last_error};
class Platform : public testing::Test {
  std::string old_model,old_managed;
  bool had_model,had_managed;
  void SetUp() override {
    const char *m=getenv("ISAC_GPU_MEMORY_MODEL"), *o=getenv("ISAC_GPU_SHARED_MANAGED");
    had_model=m; had_managed=o; old_model=m?m:""; old_managed=o?o:"";
    unsetenv("ISAC_GPU_MEMORY_MODEL"); unsetenv("ISAC_GPU_SHARED_MANAGED");
    memset(attrs,0,sizeof(attrs));
    attrs[18]=attrs[19]=attrs[41]=attrs[89]=1; attrs[75]=12; attrs[76]=1;
    failed_attribute=-1; cleared_errors=0;
    count=1; calls=device_allocs=host_allocs=managed_allocs=frees=registrations=next_buffer=syncs=0;
    nr_gpu_test_reset(&runtime);
    nr_gpu_set_logger(log_message); warnings=info_lines=0;
  }
  void TearDown() override {
    if (had_model) setenv("ISAC_GPU_MEMORY_MODEL",old_model.c_str(),1); else unsetenv("ISAC_GPU_MEMORY_MODEL");
    if (had_managed) setenv("ISAC_GPU_SHARED_MANAGED",old_managed.c_str(),1); else unsetenv("ISAC_GPU_SHARED_MANAGED");
    nr_gpu_test_reset(&runtime);
  }
};
TEST_F(Platform, CapabilityTableAndOverrides) {
  nr_gpu_caps_t c{};
  EXPECT_EQ(nr_gpu_choose_model(&c,"shared"),NR_GPU_NONE);
  c.available=1; c.integrated=1;
  EXPECT_EQ(nr_gpu_choose_model(&c,"auto"),NR_GPU_SHARED);
  c.integrated=0; c.pageable_memory_access=c.host_page_tables=1;
  EXPECT_EQ(nr_gpu_choose_model(&c,nullptr),NR_GPU_SHARED);
  c.host_page_tables=0; // HMM alone is not hardware coherence.
  EXPECT_EQ(nr_gpu_choose_model(&c,"auto"),NR_GPU_DISCRETE);
  c.pageable_memory_access=0;
  EXPECT_EQ(nr_gpu_choose_model(&c,"auto"),NR_GPU_DISCRETE);
  EXPECT_EQ(nr_gpu_choose_model(&c,"shared"),NR_GPU_SHARED);
  c.integrated=1;
  EXPECT_EQ(nr_gpu_choose_model(&c,"discrete"),NR_GPU_DISCRETE);
  EXPECT_EQ(nr_gpu_choose_model(&c,"typo"),NR_GPU_NONE);
}
TEST_F(Platform, ProbeIsOnceAndCapturesCapabilities) {
  attrs[88]=attrs[100]=attrs[86]=1;
  std::vector<std::thread> threads;
  for (int i=0;i<8;++i) threads.emplace_back([] { EXPECT_TRUE(nr_gpu_platform_ready()); });
  for (auto &t:threads) t.join();
  auto c=nr_gpu_platform_probe();
  EXPECT_EQ(info_lines,1); EXPECT_EQ(warnings,0);
  EXPECT_EQ(calls,1); EXPECT_EQ(c->major,12); EXPECT_EQ(c->minor,1);
  EXPECT_TRUE(c->pageable_memory_access); EXPECT_TRUE(c->host_page_tables);
  EXPECT_TRUE(c->concurrent_managed_access); EXPECT_TRUE(c->host_native_atomic_supported);
  EXPECT_EQ(c->total_memory,2048u); EXPECT_EQ(c->free_memory,1024u);
}
TEST_F(Platform, NoDeviceAndDiscreteFailClosed) {
  count=0;
  EXPECT_FALSE(nr_gpu_platform_ready()); EXPECT_EQ(nr_gpu_alloc(NR_GPU_DEVICE,128),nullptr);
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_NONE);
  count=1; attrs[18]=0; nr_gpu_test_reset(&runtime);
  EXPECT_FALSE(nr_gpu_module_check("mock","89-real,121-real,121-virtual"));
  EXPECT_EQ(nr_gpu_alloc(NR_GPU_STAGING,128),nullptr);
  EXPECT_NE(nr_gpu_register_host(memory[63],128),0);
  EXPECT_EQ(device_allocs+host_allocs+managed_allocs+registrations,0);
}
TEST_F(Platform, DiscreteWarnsOnceAcrossModules) {
  setenv("ISAC_GPU_MEMORY_MODEL","discrete",1);
  EXPECT_FALSE(nr_gpu_module_check("ldpc","121"));
  EXPECT_FALSE(nr_gpu_module_check("polar","121"));
  EXPECT_FALSE(nr_gpu_platform_ready());
  EXPECT_EQ(info_lines,1); EXPECT_EQ(warnings,1);
}
TEST_F(Platform, HardwareCoherentDiscreteProbesAsShared) {
  attrs[18]=0; attrs[88]=attrs[100]=1;
  EXPECT_TRUE(nr_gpu_platform_ready());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_SHARED);
}
TEST_F(Platform, MissingMappedCapabilityRejectsShared) {
  attrs[19]=0;
  EXPECT_FALSE(nr_gpu_platform_ready());
  EXPECT_EQ(nr_gpu_alloc(NR_GPU_SHARED_RW,128),nullptr);
}
TEST_F(Platform, UnsupportedOptionalAttributeDoesNotPoisonWarmup) {
  failed_attribute=86;
  EXPECT_TRUE(nr_gpu_platform_ready());
  EXPECT_EQ(nr_gpu_platform_probe()->host_native_atomic_supported,0);
  EXPECT_EQ(cleared_errors,1);
}
TEST_F(Platform, KindMappingAndCorrectFree) {
  for (int kind=0;kind<3;++kind) EXPECT_EQ(nr_gpu_storage(NR_GPU_DISCRETE,(nr_gpu_kind_t)kind,0),NR_GPU_ALLOC_UNAVAILABLE);
  EXPECT_EQ(nr_gpu_storage(NR_GPU_SHARED,(nr_gpu_kind_t)99,0),NR_GPU_ALLOC_UNAVAILABLE);
  void *d=nr_gpu_alloc(NR_GPU_DEVICE,128), *s=nr_gpu_alloc(NR_GPU_STAGING,128), *r=nr_gpu_alloc(NR_GPU_SHARED_RW,128);
  ASSERT_NE(d,nullptr); ASSERT_NE(s,nullptr); ASSERT_NE(r,nullptr);
  EXPECT_EQ(device_allocs,1); EXPECT_EQ(host_allocs,2); EXPECT_EQ(managed_allocs,0); EXPECT_EQ(flags,3u);
  EXPECT_EQ(nr_gpu_device_ptr(s),s);
  EXPECT_EQ(nr_gpu_free(d),0); EXPECT_EQ(nr_gpu_free(s),0); EXPECT_EQ(nr_gpu_free(r),0);
  EXPECT_EQ(frees,3); EXPECT_NE(nr_gpu_free(r),0);
}
TEST_F(Platform, ManagedRequiresExplicitOptIn) {
  setenv("ISAC_GPU_SHARED_MANAGED","1",1);
  auto r=nr_gpu_alloc(NR_GPU_SHARED_RW,128), s=nr_gpu_alloc(NR_GPU_STAGING,128);
  EXPECT_EQ(managed_allocs,1); EXPECT_EQ(host_allocs,1);
  EXPECT_EQ(nr_gpu_free(r),0); EXPECT_EQ(nr_gpu_free(s),0);
}
TEST_F(Platform, RegistrationAndTransferContract) {
  char source[128]="GPU", target[128]{};
  EXPECT_EQ(nr_gpu_register_host(source,128),0);
  EXPECT_EQ(nr_gpu_register_host(source,128),0); EXPECT_EQ(registrations,1);
  EXPECT_NE(nr_gpu_register_host(source,64),0);
  EXPECT_EQ(nr_gpu_device_ptr(source),source);
  void *stream=reinterpret_cast<void *>(16);
  EXPECT_EQ(nr_gpu_copy_async(target,source,128,stream),0);
  EXPECT_EQ(copy_kind,4); EXPECT_EQ(copy_stream,stream);
  EXPECT_EQ(nr_gpu_sync(stream),0); EXPECT_EQ(nr_gpu_sync(nullptr),0); EXPECT_EQ(syncs,2);
  EXPECT_STREQ(target,"GPU"); EXPECT_EQ(nr_gpu_unregister_host(source),0);
}
TEST_F(Platform, RxFencePreventsEveryMemoryMutationButAllowsTransfers) {
  auto p=nr_gpu_alloc(NR_GPU_STAGING,128);
  ASSERT_EQ(nr_gpu_register_host(memory[63],128),0);
  nr_gpu_rx_started(); nr_gpu_rx_started();
  EXPECT_FALSE(nr_gpu_allocations_allowed());
  for (int kind=0;kind<3;++kind) EXPECT_EQ(nr_gpu_alloc((nr_gpu_kind_t)kind,128),nullptr);
  EXPECT_NE(nr_gpu_register_host(memory[62],128),0);
  EXPECT_NE(nr_gpu_unregister_host(memory[63]),0);
  EXPECT_NE(nr_gpu_free(p),0);
  EXPECT_EQ(host_allocs,1); EXPECT_EQ(registrations,1); EXPECT_EQ(device_allocs+managed_allocs+frees,0);
  uint64_t allocated,denied; nr_gpu_memory_stats(&allocated,&denied);
  EXPECT_EQ(allocated,2u); EXPECT_EQ(denied,6u);
  EXPECT_EQ(nr_gpu_copy_async(p,memory[63],128,nullptr),0);
}
TEST_F(Platform, RxFenceCannotLazilyInitializeCuda) {
  nr_gpu_rx_started();
  EXPECT_EQ(nr_gpu_alloc(NR_GPU_DEVICE,128),nullptr);
  EXPECT_EQ(calls,0);
}
TEST_F(Platform, CubinAndPtxCompatibility) {
  const char *fat="89-real,121-real,121-virtual";
  EXPECT_TRUE(nr_gpu_arch_compatible(8,9,fat)); EXPECT_TRUE(nr_gpu_arch_compatible(12,1,fat));
  EXPECT_TRUE(nr_gpu_arch_compatible(13,0,fat)); EXPECT_FALSE(nr_gpu_arch_compatible(9,0,fat));
  EXPECT_FALSE(nr_gpu_arch_compatible(8,6,fat)); EXPECT_FALSE(nr_gpu_arch_compatible(12,0,fat));
  EXPECT_TRUE(nr_gpu_arch_compatible(8,9,"86-real"));
  EXPECT_FALSE(nr_gpu_arch_compatible(13,0,"121-real"));
  EXPECT_FALSE(nr_gpu_arch_compatible(12,1,"121a"));
  EXPECT_FALSE(nr_gpu_module_check("mock","89-real"));
}
}
