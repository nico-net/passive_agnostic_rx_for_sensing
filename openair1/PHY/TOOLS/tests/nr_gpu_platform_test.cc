/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <gtest/gtest.h>
#include "nr_gpu_platform_internal.h"
#include "nr_gpu_mem.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
namespace {
int attrs[128], count, calls, device_allocs, host_allocs, frees, registrations;
unsigned flags;
int copy_kind, syncs;
void *copy_stream;
char memory[64][256];
int next_buffer, warnings, info_lines, failed_attribute, cleared_errors, probe_result, probe_calls, memsets, fail_device_alloc;
std::string last_platform_line;
void log_message(int warning,const char *m) {
  if (warning) ++warnings; else ++info_lines;
  if (strstr(m,"GPU_PLATFORM device=")) last_platform_line=m;
}
int probe_hook() { ++probe_calls; return probe_result; }
int memset_dev(void *p,int v,size_t n) { ++memsets; memset(p,v,n); return 0; }
int get_count(int *n) { ++calls; *n=count; return 0; }
int get_device(int *d) { *d=0; return 0; }
int attr(int *v,int a,int) { *v=attrs[a]; return a==failed_attribute ? 1 : 0; }
int mem_info(size_t *free,size_t *total) { *free=1024; *total=2048; return 0; }
int device_alloc(void **p,size_t) { if (fail_device_alloc) return 2; ++device_allocs; *p=memory[next_buffer++]; return 0; }
int host_alloc(void **p,size_t,unsigned f) { ++host_allocs; flags=f; *p=memory[next_buffer++]; return 0; }
int release(void *) { ++frees; return 0; }
int device_ptr(void **d,void *h,unsigned) { *d=h; return 0; }
int reg(void *,size_t,unsigned f) { ++registrations; flags=f; return 0; }
int copy(void *d,const void *s,size_t b,int kind,void *stream) { copy_kind=kind; copy_stream=stream; memcpy(d,s,b); return 0; }
int stream_sync(void *) { ++syncs; return 0; }
int device_sync() { ++syncs; return 0; }
int set_flags(unsigned) { return 0; }
int get_last_error() { ++cleared_errors; return 0; }
const nr_gpu_runtime_t runtime={get_count,get_device,attr,mem_info,device_alloc,host_alloc,
                              release,release,device_ptr,reg,release,copy,stream_sync,device_sync,set_flags,get_last_error,memset_dev};
class Platform : public testing::Test {
  void SetUp() override {
    memset(attrs,0,sizeof(attrs));
    attrs[18]=attrs[19]=attrs[41]=attrs[89]=1; attrs[75]=12; attrs[76]=1;
    failed_attribute=-1; cleared_errors=0;
    count=1; calls=device_allocs=host_allocs=frees=registrations=next_buffer=syncs=0;
    probe_result=NR_GPU_PROBE_OK; probe_calls=memsets=fail_device_alloc=0; last_platform_line.clear();
    unsetenv("NR_GPU_FORCE_MODEL");
    nr_gpu_test_reset(&runtime);
    nr_gpu_test_set_probe(probe_hook);
    nr_gpu_mem_test_force_discrete(0);
    nr_gpu_set_logger(log_message); warnings=info_lines=0;
  }
  void TearDown() override {
    unsetenv("NR_GPU_FORCE_MODEL");
    nr_gpu_mem_test_force_discrete(0);
    nr_gpu_test_reset(&runtime);
  }
};
TEST_F(Platform, CapabilityTable) {
  nr_gpu_caps_t c{};
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_NONE); EXPECT_STREQ(nr_gpu_choose_reason(&c),"no_device");
  c.available=1; c.can_map_host_memory=1;
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_NONE); EXPECT_STREQ(nr_gpu_choose_reason(&c),"no_uva");
  c.unified_addressing=1; c.can_map_host_memory=0;
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_NONE); EXPECT_STREQ(nr_gpu_choose_reason(&c),"no_mapped");
  c.can_map_host_memory=1; c.integrated=1; // integrated without pageable access cannot read caller memory
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_DISCRETE); EXPECT_STREQ(nr_gpu_choose_reason(&c),"no_pageable");
  c.pageable_memory_access=1; // GB10
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_SHARED); EXPECT_STREQ(nr_gpu_choose_reason(&c),"integrated");
  c.integrated=0; c.host_page_tables=1; // ATS / C2C coherent
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_SHARED); EXPECT_STREQ(nr_gpu_choose_reason(&c),"ats");
  c.host_page_tables=0; // HMM over PCIe is not hardware coherence.
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_DISCRETE); EXPECT_STREQ(nr_gpu_choose_reason(&c),"hmm_pcie");
  c.pageable_memory_access=0;
  EXPECT_EQ(nr_gpu_choose_model(&c),NR_GPU_DISCRETE); EXPECT_STREQ(nr_gpu_choose_reason(&c),"no_pageable");
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
TEST_F(Platform, NoDeviceFailsClosedAndDiscreteIsReady) {
  count=0;
  EXPECT_FALSE(nr_gpu_platform_ready()); EXPECT_EQ(nr_gpu_alloc(NR_GPU_DEVICE,128),nullptr);
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_NONE); EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_NE(last_platform_line.find("model=unavailable tier=cpu reason=no_device probe=skipped"),std::string::npos);
  EXPECT_FALSE(nr_gpu_module_check("mock","89-real,121-real,121-virtual"));
  EXPECT_NE(nr_gpu_register_host(memory[63],128),0);
  EXPECT_EQ(device_allocs+host_allocs+registrations,0);
  // A DISCRETE device (not integrated, no pageable access) is a working tier now.
  count=1; attrs[18]=0; nr_gpu_test_reset(&runtime); nr_gpu_test_set_probe(probe_hook);
  EXPECT_TRUE(nr_gpu_platform_ready()); EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_TRUE(nr_gpu_module_check("mock","89-real,121-real,121-virtual"));
  EXPECT_FALSE(nr_gpu_module_check("mock","89-real")); // the arch check still applies
  EXPECT_NE(nr_gpu_alloc(NR_GPU_STAGING,128),nullptr);
  EXPECT_EQ(nr_gpu_register_host(memory[63],128),0);
  EXPECT_EQ(probe_calls,0); // the proof runs for SHARED candidates only
}
TEST_F(Platform, DiscreteWarnsOnceAcrossModules) {
  attrs[18]=0; // not integrated, no pageable access: DISCRETE
  EXPECT_TRUE(nr_gpu_module_check("ldpc","121"));
  EXPECT_TRUE(nr_gpu_module_check("polar","121"));
  EXPECT_TRUE(nr_gpu_platform_ready());
  EXPECT_EQ(info_lines,2); EXPECT_EQ(warnings,0); // the platform line and the DISCRETE info line, once for all modules
  EXPECT_NE(last_platform_line.find("model=discrete tier=discrete reason=no_pageable probe=skipped"),std::string::npos);
}
TEST_F(Platform, HardwareCoherentDiscreteProbesAsShared) {
  attrs[18]=0; attrs[88]=attrs[100]=1;
  EXPECT_TRUE(nr_gpu_platform_ready());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_SHARED);
  EXPECT_TRUE(nr_gpu_platform_shared_direct());
  EXPECT_NE(last_platform_line.find("model=shared tier=shared reason=ats probe=ok"),std::string::npos);
}
TEST_F(Platform, IntegratedPageableIsSharedWithProbe) { // GB10
  attrs[88]=attrs[100]=attrs[86]=1;
  EXPECT_TRUE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(probe_calls,1);
  EXPECT_NE(last_platform_line.find("model=shared tier=shared reason=integrated probe=ok"),std::string::npos);
  EXPECT_EQ(warnings,0); EXPECT_EQ(info_lines,1);
}
TEST_F(Platform, HmmOnPcieIsDiscrete) { // sens6: pageable access but not through the host page tables
  attrs[18]=0; attrs[88]=1; attrs[100]=0;
  EXPECT_TRUE(nr_gpu_platform_ready()); EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_DISCRETE);
  EXPECT_EQ(probe_calls,0);
  EXPECT_NE(last_platform_line.find("model=discrete tier=discrete reason=hmm_pcie probe=skipped"),std::string::npos);
}
TEST_F(Platform, IntegratedWithoutPageableIsDiscrete) { // Jetson class
  attrs[18]=1; attrs[88]=0; attrs[100]=0;
  EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_DISCRETE);
  EXPECT_TRUE(nr_gpu_platform_ready());
  EXPECT_NE(last_platform_line.find("tier=discrete reason=no_pageable probe=skipped"),std::string::npos);
}
TEST_F(Platform, ProbeFailFallsBackToDiscrete) {
  attrs[88]=attrs[100]=1; probe_result=NR_GPU_PROBE_FAIL;
  EXPECT_TRUE(nr_gpu_platform_ready()); EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_DISCRETE);
  EXPECT_EQ(probe_calls,1);
  EXPECT_NE(last_platform_line.find("model=discrete tier=discrete reason=probe_fail probe=fail"),std::string::npos);
}
TEST_F(Platform, ProbeTimeoutKeepsShared) { // no verdict is not a wrong answer: the properties stand
  attrs[88]=attrs[100]=1; probe_result=NR_GPU_PROBE_TIMEOUT;
  EXPECT_TRUE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_SHARED);
  EXPECT_EQ(warnings,1);
  EXPECT_NE(last_platform_line.find("model=shared tier=shared reason=integrated probe=timeout"),std::string::npos);
}
TEST_F(Platform, ProbeFaultWithHealthyPrimaryIsDiscrete) {
  attrs[88]=attrs[100]=1; probe_result=NR_GPU_PROBE_FAULT;
  EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_DISCRETE);
  EXPECT_NE(last_platform_line.find("tier=discrete reason=probe_fault probe=fault"),std::string::npos);
}
TEST_F(Platform, ProbeFaultWithBrokenPrimaryIsCpu) {
  attrs[88]=attrs[100]=1; probe_result=NR_GPU_PROBE_FAULT; fail_device_alloc=1;
  EXPECT_FALSE(nr_gpu_platform_ready()); EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_NONE);
  EXPECT_NE(last_platform_line.find("tier=cpu reason=probe_fault probe=fault"),std::string::npos);
}
TEST_F(Platform, ProbeSkippedTrustsProperties) { // no probe tooling: GB10 must not regress
  attrs[88]=attrs[100]=1; probe_result=NR_GPU_PROBE_SKIPPED;
  EXPECT_TRUE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_SHARED);
  EXPECT_EQ(probe_calls,1);
  EXPECT_NE(last_platform_line.find("model=shared tier=shared reason=integrated probe=skipped"),std::string::npos);
}
TEST_F(Platform, ForceModelNoneIsCpuTier) {
  attrs[88]=attrs[100]=1;
  nr_gpu_platform_test_force_model(NR_GPU_NONE);
  EXPECT_FALSE(nr_gpu_platform_ready()); EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_platform_probe()->model,NR_GPU_NONE);
  EXPECT_FALSE(nr_gpu_module_check("mock","121"));
  EXPECT_EQ(nr_gpu_alloc(NR_GPU_STAGING,128),nullptr); EXPECT_NE(nr_gpu_copy_async(memory[0],memory[1],8,nullptr),0);
  EXPECT_EQ(nr_gpu_platform_runtime(),nullptr);
  nr_gpu_buf_t b; EXPECT_EQ(nr_gpu_buf_alloc(&b,64),-1); EXPECT_EQ(nr_gpu_buf_alloc_dev(&b,64),-1);
  EXPECT_EQ(device_allocs+host_allocs+registrations,0); EXPECT_EQ(probe_calls,0);
  EXPECT_NE(last_platform_line.find("model=unavailable tier=cpu reason=forced probe=skipped FORCED=cpu"),std::string::npos);
}
TEST_F(Platform, ForceModelDiscreteOnSharedDevice) { // the forced-DISCRETE regression on an integrated GPU
  attrs[88]=attrs[100]=1;
  nr_gpu_platform_test_force_model(NR_GPU_DISCRETE);
  EXPECT_TRUE(nr_gpu_platform_ready()); EXPECT_FALSE(nr_gpu_platform_shared_direct());
  EXPECT_EQ(nr_gpu_model(),NR_GPU_MODEL_DISCRETE);
  EXPECT_EQ(probe_calls,0);
  EXPECT_NE(last_platform_line.find("model=discrete tier=discrete reason=forced probe=skipped FORCED=discrete"),std::string::npos);
  nr_gpu_platform_test_force_model(NR_GPU_SHARED); // SHARED cannot be forced: back to automatic at the next probe
  nr_gpu_test_reset(&runtime); nr_gpu_test_set_probe(probe_hook);
  nr_gpu_platform_test_force_model(NR_GPU_DISCRETE); nr_gpu_platform_test_force_model(-1);
  EXPECT_TRUE(nr_gpu_platform_shared_direct());
}
TEST_F(Platform, ForceCannotExceedTheDevice) {
  count=0; nr_gpu_platform_test_force_model(NR_GPU_DISCRETE);
  EXPECT_FALSE(nr_gpu_platform_ready());
  EXPECT_NE(last_platform_line.find("model=unavailable tier=cpu reason=no_device probe=skipped FORCED=discrete"),std::string::npos);
}
TEST_F(Platform, ForceModelEnvParsing) {
  EXPECT_EQ(nr_gpu_platform_parse_force(nullptr),-1); EXPECT_EQ(nr_gpu_platform_parse_force(""),-1);
  EXPECT_EQ(nr_gpu_platform_parse_force("discrete"),NR_GPU_DISCRETE); EXPECT_EQ(nr_gpu_platform_parse_force("cpu"),NR_GPU_NONE);
  EXPECT_EQ(nr_gpu_platform_parse_force("shared"),-2); EXPECT_EQ(nr_gpu_platform_parse_force("DISCRETE"),-2);
  attrs[88]=attrs[100]=1;
  setenv("NR_GPU_FORCE_MODEL","discrete",1); nr_gpu_test_reset(&runtime); nr_gpu_test_set_probe(probe_hook);
  EXPECT_FALSE(nr_gpu_platform_shared_direct()); EXPECT_TRUE(nr_gpu_platform_ready());
  EXPECT_NE(last_platform_line.find("FORCED=discrete"),std::string::npos);
  setenv("NR_GPU_FORCE_MODEL","cpu",1); nr_gpu_test_reset(&runtime); nr_gpu_test_set_probe(probe_hook);
  EXPECT_FALSE(nr_gpu_platform_ready());
  EXPECT_NE(last_platform_line.find("FORCED=cpu"),std::string::npos);
  nr_gpu_platform_test_force_model(NR_GPU_DISCRETE); // the test hook overrides the environment
  nr_gpu_test_reset(&runtime); nr_gpu_test_set_probe(probe_hook); nr_gpu_platform_test_force_model(NR_GPU_DISCRETE);
  EXPECT_TRUE(nr_gpu_platform_ready());
  setenv("NR_GPU_FORCE_MODEL","bogus",1); nr_gpu_test_reset(&runtime); nr_gpu_test_set_probe(probe_hook);
  warnings=0;
  EXPECT_TRUE(nr_gpu_platform_shared_direct()); // unknown value: warned, automatic
  EXPECT_EQ(warnings,1); EXPECT_EQ(last_platform_line.find("FORCED"),std::string::npos);
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
  for (int kind=0;kind<3;++kind) {
    EXPECT_EQ(nr_gpu_storage(NR_GPU_DISCRETE,(nr_gpu_kind_t)kind),nr_gpu_storage(NR_GPU_SHARED,(nr_gpu_kind_t)kind));
    EXPECT_EQ(nr_gpu_storage(NR_GPU_NONE,(nr_gpu_kind_t)kind),NR_GPU_ALLOC_UNAVAILABLE);
  }
  EXPECT_EQ(nr_gpu_storage(NR_GPU_DISCRETE,NR_GPU_DEVICE),NR_GPU_ALLOC_DEVICE);
  EXPECT_EQ(nr_gpu_storage(NR_GPU_DISCRETE,NR_GPU_STAGING),NR_GPU_ALLOC_MAPPED);
  EXPECT_EQ(nr_gpu_storage(NR_GPU_SHARED,(nr_gpu_kind_t)99),NR_GPU_ALLOC_UNAVAILABLE);
  void *d=nr_gpu_alloc(NR_GPU_DEVICE,128), *s=nr_gpu_alloc(NR_GPU_STAGING,128), *r=nr_gpu_alloc(NR_GPU_SHARED_RW,128);
  ASSERT_NE(d,nullptr); ASSERT_NE(s,nullptr); ASSERT_NE(r,nullptr);
  EXPECT_EQ(device_allocs,1); EXPECT_EQ(host_allocs,2); EXPECT_EQ(flags,3u);
  EXPECT_EQ(nr_gpu_device_ptr(s),s);
  EXPECT_EQ(nr_gpu_free(d),0); EXPECT_EQ(nr_gpu_free(s),0); EXPECT_EQ(nr_gpu_free(r),0);
  EXPECT_EQ(frees,3); EXPECT_NE(nr_gpu_free(r),0);
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
  EXPECT_EQ(host_allocs,1); EXPECT_EQ(registrations,1); EXPECT_EQ(device_allocs+frees,0);
  uint64_t allocated,denied; nr_gpu_memory_stats(&allocated,&denied);
  EXPECT_EQ(allocated,2u); EXPECT_EQ(denied,6u);
  EXPECT_EQ(nr_gpu_copy_async(p,memory[63],128,nullptr),0);
}
TEST_F(Platform, RxFenceCannotLazilyInitializeCuda) {
  nr_gpu_rx_started();
  EXPECT_EQ(nr_gpu_alloc(NR_GPU_DEVICE,128),nullptr);
  EXPECT_EQ(calls,0);
}
TEST_F(Platform, DiscreteKindsAllocateAndFreeCorrectly) {
  attrs[18]=0; // DISCRETE
  void *d=nr_gpu_alloc(NR_GPU_DEVICE,128), *s=nr_gpu_alloc(NR_GPU_STAGING,128), *r=nr_gpu_alloc(NR_GPU_SHARED_RW,128);
  ASSERT_NE(d,nullptr); ASSERT_NE(s,nullptr); ASSERT_NE(r,nullptr);
  EXPECT_EQ(device_allocs,1); EXPECT_EQ(host_allocs,2); EXPECT_EQ(flags,3u);
  EXPECT_EQ(nr_gpu_free(d),0); EXPECT_EQ(nr_gpu_free(s),0); EXPECT_EQ(nr_gpu_free(r),0); EXPECT_EQ(frees,3);
}
TEST_F(Platform, FreeAtProbe) {
  EXPECT_EQ(nr_gpu_platform_free_at_probe(),1024u);
  count=0; nr_gpu_test_reset(&runtime);
  EXPECT_EQ(nr_gpu_platform_free_at_probe(),0u);
}
TEST_F(Platform, BufferModelsPerTier) {
  nr_gpu_buf_t b;
  attrs[88]=attrs[100]=1; // SHARED: DIRECT, mapped host == dev, device-only variant identical
  ASSERT_EQ(nr_gpu_buf_alloc(&b,64),0); EXPECT_EQ(b.model,NR_GPU_MODEL_DIRECT); EXPECT_EQ(b.dev,b.host); nr_gpu_buf_free(&b);
  ASSERT_EQ(nr_gpu_buf_alloc_dev(&b,64),0); EXPECT_EQ(b.model,NR_GPU_MODEL_DIRECT); EXPECT_NE(b.host,nullptr); nr_gpu_buf_free(&b);
  // DISCRETE: pinned mirror + device copy; the device-only variant has no mirror
  int hb=host_allocs, db=device_allocs;
  nr_gpu_mem_test_force_discrete(1);
  ASSERT_EQ(nr_gpu_buf_alloc(&b,64),0); EXPECT_EQ(b.model,NR_GPU_MODEL_DISCRETE); EXPECT_NE(b.dev,b.host);
  EXPECT_EQ(host_allocs-hb,1); EXPECT_EQ(device_allocs-db,1);
  EXPECT_EQ(nr_gpu_buf_h2d_async(&b,0,64,nullptr),0); EXPECT_EQ(copy_kind,1);
  EXPECT_EQ(nr_gpu_buf_d2h_async(&b,0,64,nullptr),0); EXPECT_EQ(copy_kind,2);
  nr_gpu_buf_free(&b);
  hb=host_allocs; db=device_allocs; memsets=0;
  ASSERT_EQ(nr_gpu_buf_alloc_dev(&b,64),0); EXPECT_EQ(b.model,NR_GPU_MODEL_DISCRETE);
  EXPECT_EQ(b.host,nullptr); EXPECT_NE(b.dev,nullptr);
  EXPECT_EQ(host_allocs-hb,0); EXPECT_EQ(device_allocs-db,1); EXPECT_EQ(memsets,1);
  EXPECT_EQ(nr_gpu_buf_h2d_async(&b,0,64,nullptr),-1); EXPECT_EQ(nr_gpu_buf_d2h_async(&b,0,64,nullptr),-1);
  nr_gpu_buf_free(&b); EXPECT_EQ(b.dev,nullptr);
  nr_gpu_rx_started(); // allocation fence
  EXPECT_EQ(nr_gpu_buf_alloc_dev(&b,64),-1);
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
