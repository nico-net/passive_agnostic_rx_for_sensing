#include <gtest/gtest.h>
#include "nr_sense_api.h"
#include "nr_traffic_record.h"
#include "nr_sense_records.h"
#include "nr_sense_unit.h"
#include "common/utils/nr_cell.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#ifdef NR_SENSE_HAVE_ZMQ
#include <zmq.h>
#endif

// libLOG/libCONFIG link-time deps pulled in via nr_sense_api (no config module in this test)
extern "C" {
void *uniqCfg = nullptr;
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
// strong test formatter overriding the weak reference in nr_sense_api.c
int nr_sense_dci_to_json(const nr_sense_dci_rec_t *r, char *buf, size_t n) { return snprintf(buf, n, "{\"type\":\"dci\",\"rnti\":%u}", r->o.rnti); }
}

namespace {
std::string temp_path() {
  char path[] = "/tmp/sense_api_XXXXXX";
  int fd = mkstemp(path);
  if (fd >= 0) close(fd);
  return path;
}
std::vector<uint8_t> bytes(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
uint16_t u16(const std::vector<uint8_t> &b, size_t p) { return b[p] | (uint16_t(b[p+1]) << 8); }
uint32_t u32(const std::vector<uint8_t> &b, size_t p) { return u16(b,p) | (uint32_t(u16(b,p+2)) << 16); }
uint64_t u64(const std::vector<uint8_t> &b, size_t p) { return u32(b,p) | (uint64_t(u32(b,p+4)) << 32); }
float f32(const std::vector<uint8_t> &b, size_t p) { uint32_t x=u32(b,p); float f; memcpy(&f,&x,4); return f; }
struct SenseApi : ::testing::Test {
  std::string rec=temp_path(), cfr=temp_path();
  nr_sense_cfg_t cfg{};
  void SetUp() override { cfg={rec.c_str(),cfr.c_str(),nullptr,16,64,16}; }
  void TearDown() override { nr_sense_test_pause_writer(false); nr_sense_test_fail_writes(false); nr_sense_test_write_chunk(0); nr_sense_test_writer_limit(0); nr_sense_close(); unlink(rec.c_str()); unlink(cfr.c_str()); }
};
}

TEST_F(SenseApi, RoundTripCfrFrame) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2*12*2]; uint16_t k[12]; uint8_t l[12];
  for(int i=0;i<48;i++) iq[i]=i+0.25f;
  for(int i=0;i<12;i++) { k[i]=100+i; l[i]=i%4; }
  bool truncated=true;
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_CSIRS,2,iq,12,12,k,l,1234,0.5f,2.25f,7,9,&truncated),1u);
  EXPECT_FALSE(truncated);
  nr_sense_close(); auto b=bytes(cfr);
  ASSERT_EQ(b.size(),48u+36u+192u);
  EXPECT_EQ(std::string(b.begin(),b.begin()+4),"NRCF");
  EXPECT_EQ(u16(b,4),1); EXPECT_EQ(u16(b,6),48); EXPECT_EQ(u64(b,8),1u);
  EXPECT_EQ(b[16],NR_SENSE_SRC_CSIRS); EXPECT_EQ(b[17],2); EXPECT_EQ(u16(b,18),0);
  EXPECT_EQ(u32(b,20),12u); EXPECT_EQ(u64(b,24),1234u);
  EXPECT_FLOAT_EQ(f32(b,32),0.5f); EXPECT_FLOAT_EQ(f32(b,36),2.25f);
  EXPECT_EQ(u32(b,40),7u); EXPECT_EQ(u32(b,44),9u);
  for(int i=0;i<12;i++) { EXPECT_EQ(u16(b,48+2*i),k[i]); EXPECT_EQ(b[72+i],l[i]); }
  for(int i=0;i<48;i++) EXPECT_FLOAT_EQ(f32(b,84+4*i),iq[i]);
}

TEST_F(SenseApi, RecordsAreJsonLines) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  for(int i=0;i<3;i++) EXPECT_TRUE(nr_sense_push_record("{\"schema\":2,\"type\":\"rs\"}"));
  nr_sense_close(); std::ifstream f(rec); std::string line; int count=0;
  while(std::getline(f,line)) { EXPECT_EQ(line,"{\"schema\":2,\"type\":\"rs\"}"); count++; }
  EXPECT_EQ(count,3);
}

TEST_F(SenseApi, CostCountersSnapshotAndReset) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  nr_sense_cost_add(NR_SENSE_COST_PROD,NR_SENSE_SRC_CSIRS,2501);
  nr_sense_cost_add(NR_SENSE_COST_PROD,NR_SENSE_SRC_CSIRS,499);
  nr_sense_cost_add(NR_SENSE_COST_REC,NR_SENSE_SRC_SSB,900);
  nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_EQ(s.prod_ns[NR_SENSE_SRC_CSIRS],3000u);
  EXPECT_EQ(s.prod_count[NR_SENSE_SRC_CSIRS],2u);
  EXPECT_EQ(s.rec_ns,900u);
  EXPECT_EQ(s.rec_count,1u);
  nr_sense_close();
  ASSERT_EQ(nr_sense_open(&cfg),0);
  nr_sense_stats(&s);
  EXPECT_EQ(s.prod_ns[NR_SENSE_SRC_CSIRS],0u);
  EXPECT_EQ(s.rec_ns,0u);
}

TEST_F(SenseApi, BatchesAdjacentFramesWithoutChangingBytes) {
  nr_sense_test_pause_writer(true);
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={1,2}; uint16_t k=3; uint8_t l=4;
  for (int i=0;i<8;i++)
    ASSERT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,i,0,0,0,0,nullptr),uint64_t(i+1));
  nr_sense_test_pause_writer(false);
  nr_sense_close();
  auto b=bytes(cfr);
  ASSERT_EQ(b.size(),8u*60u);
  for (int i=0;i<8;i++) EXPECT_EQ(u64(b,60*i+8),uint64_t(i+1));
  nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_LT(s.write_count,8u);
}

TEST_F(SenseApi, GoldenReaderFixture) {
  const char *dir=std::getenv("NR_SENSE_GOLDEN_DIR");
  if (!dir) GTEST_SKIP() << "set NR_SENSE_GOLDEN_DIR to write the reader fixture";
  std::string records=std::string(dir)+"/records.jsonl", frames=std::string(dir)+"/frames.cfr";
  cfg.records_path=records.c_str(); cfg.cfr_path=frames.c_str();
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[]={1,2,3,4,5,6,7,8}; uint16_t k[]={120,121}; uint8_t l[]={2,2};
  ASSERT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_CSIRS,2,iq,2,2,k,l,42,0.25f,0.5f,3,4,nullptr),1u);
  ASSERT_TRUE(nr_sense_push_record("{\"schema\":2,\"type\":\"session\"}"));
  ASSERT_TRUE(nr_sense_push_record("{\"schema\":2,\"type\":\"rs\",\"source\":\"csirs\",\"cfr_frame_id\":1,\"k_abs_convention\":\"crb0_point_a\"}"));
  ASSERT_TRUE(nr_sense_push_record("{\"schema\":2,\"type\":\"rs\",\"source\":\"prach\",\"cfr_frame_id\":null}"));
  nr_sense_close();
}

TEST_F(SenseApi, SlowWriterNeverBlocksProducer) {
  nr_sense_test_pause_writer(true); ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={1,2}; uint16_t k=3; uint8_t l=4;
  for(int i=0;i<10000;i++) nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr);
  nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_EQ(s.cfr_pushed,16u); EXPECT_EQ(s.cfr_dropped[NR_SENSE_SRC_SSB],9984u);
}

TEST_F(SenseApi, FrameStreamSurvivesPartialWrites) {
  unlink(cfr.c_str()); ASSERT_EQ(mkfifo(cfr.c_str(),0600),0);
  int reader=open(cfr.c_str(),O_RDONLY|O_NONBLOCK); ASSERT_GE(reader,0);
  nr_sense_test_write_chunk(7);
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={1,2}; uint16_t k=3; uint8_t l=4;
  for(int i=0;i<2;i++) ASSERT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,i,0,0,0,0,nullptr),uint64_t(i+1));
  nr_sense_close();
  std::vector<uint8_t> b; uint8_t part[128]; ssize_t n;
  while((n=read(reader,part,sizeof(part)))>0) b.insert(b.end(),part,part+n);
  close(reader);
  ASSERT_EQ(b.size(),120u);
  EXPECT_EQ(u64(b,8),1u); EXPECT_EQ(u64(b,68),2u);
  nr_sense_stats_t s{}; nr_sense_stats(&s); EXPECT_EQ(s.io_errors,0u);
}

TEST_F(SenseApi, InvalidPushCounted) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={}; uint16_t k=0; uint8_t l=0;
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,0,iq,1,1,&k,&l,0,0,0,0,0,nullptr),0u);
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,9,iq,1,1,&k,&l,0,0,0,0,0,nullptr),0u);
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,nullptr,1,1,&k,&l,0,0,0,0,0,nullptr),0u);
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,nullptr,&l,0,0,0,0,0,nullptr),0u);
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,nullptr,0,0,0,0,0,nullptr),0u);
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,0,1,&k,&l,0,0,0,0,0,nullptr),0u);
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_COUNT,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr),0u);
  nr_sense_stats_t s{}; nr_sense_stats(&s); EXPECT_EQ(s.cfr_rejected,7u);
}

TEST_F(SenseApi, RecordsNotStarvedUnderCfrLoad) {
  nr_sense_test_pause_writer(true); nr_sense_test_writer_limit(2);
  ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={}; uint16_t k=0; uint8_t l=0;
  for(int i=0;i<4;i++) ASSERT_NE(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr),0u);
  ASSERT_TRUE(nr_sense_push_record("{\"schema\":2}"));
  nr_sense_test_pause_writer(false);
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(nr_sense_test_written_items()<2 && std::chrono::steady_clock::now()<deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(nr_sense_test_written_items(),2u);
  EXPECT_FALSE(bytes(rec).empty());
}

TEST_F(SenseApi, DroppedFramesLeaveIdGaps) {
  cfg.cfr_ring_frames=1; nr_sense_test_pause_writer(true); ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[2]={}; uint16_t k=0; uint8_t l=0;
  auto push=[&] { return nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr); };
  EXPECT_EQ(push(),1u); EXPECT_EQ(push(),0u);
  nr_sense_test_pause_writer(false);
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(nr_sense_test_written_items()<1 && std::chrono::steady_clock::now()<deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(nr_sense_test_written_items(),1u);
  EXPECT_EQ(push(),3u);
  nr_sense_close(); auto b=bytes(cfr); ASSERT_EQ(b.size(),120u);
  EXPECT_EQ(u64(b,8),1u); EXPECT_EQ(u64(b,68),3u);
}

TEST_F(SenseApi, EmbeddedNewlineRecordRejected) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  EXPECT_FALSE(nr_sense_push_record("{}\n{}"));
  nr_sense_stats_t s{}; nr_sense_stats(&s); EXPECT_EQ(s.rec_dropped,1u);
}

TEST_F(SenseApi, IoErrorCounted) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  nr_sense_test_fail_writes(true);
  EXPECT_TRUE(nr_sense_push_record("{\"schema\":2}"));
  nr_sense_close(); nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_GT(s.io_errors,0u);
}

TEST_F(SenseApi, OversizeFrameTruncatedAndFlagged) {
  cfg.cfr_max_re=4; ASSERT_EQ(nr_sense_open(&cfg),0);
  float iq[18]; uint16_t k[9]; uint8_t l[9];
  for(int i=0;i<18;i++) iq[i]=i;
  for(int i=0;i<9;i++) { k[i]=i; l[i]=i; }
  bool truncated=false;
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,9,9,k,l,0,0,0,0,0,&truncated),1u);
  nr_sense_close(); auto b=bytes(cfr); nr_sense_stats_t s{}; nr_sense_stats(&s);
  ASSERT_GE(b.size(),48u); EXPECT_EQ(u32(b,20),4u); EXPECT_TRUE(truncated); EXPECT_EQ(s.truncated,1u);
}

TEST_F(SenseApi, CloseTwiceAndPushAfterCloseSafe) {
  ASSERT_EQ(nr_sense_open(&cfg),0); nr_sense_close(); nr_sense_close();
  EXPECT_FALSE(nr_sense_enabled()); EXPECT_FALSE(nr_sense_push_record("{\"schema\":2}"));
  float iq[2]={}; uint16_t k=0; uint8_t l=0;
  EXPECT_EQ(nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr),0u);
  nr_sense_stats_t s{}; nr_sense_stats(&s); EXPECT_EQ(s.after_close,2u);
}

TEST_F(SenseApi, ZmqMirrorOptional) {
#ifndef NR_SENSE_HAVE_ZMQ
  GTEST_SKIP() << "libzmq not configured";
#else
  std::string ep="inproc://sense_api_test";
  cfg.endpoint=ep.c_str(); ASSERT_EQ(nr_sense_open(&cfg),0);
  void *sub=zmq_socket(nr_sense_test_zmq_context(),ZMQ_SUB); ASSERT_NE(sub,nullptr);
  int timeout=2000; zmq_setsockopt(sub,ZMQ_RCVTIMEO,&timeout,sizeof(timeout));
  ASSERT_EQ(zmq_setsockopt(sub,ZMQ_SUBSCRIBE,"cfr",3),0);
  ASSERT_EQ(zmq_connect(sub,ep.c_str()),0);
  usleep(150000); // PUB/SUB subscription propagation
  float iq[2]={1,2}; uint16_t k=3; uint8_t l=4;
  for(int i=0;i<4;i++) { nr_sense_push_cfr(NR_SENSE_SRC_SSB,1,iq,1,1,&k,&l,0,0,0,0,0,nullptr); usleep(20000); }
  char topic[16]; ASSERT_EQ(zmq_recv(sub,topic,sizeof(topic),0),3); EXPECT_EQ(std::string(topic,3),"cfr");
  std::vector<uint8_t> payload(128); int n=zmq_recv(sub,payload.data(),payload.size(),0); ASSERT_GT(n,0);
  zmq_close(sub);
  nr_sense_close(); auto file=bytes(cfr); ASSERT_GE(file.size(),size_t(n));
  EXPECT_TRUE(std::equal(payload.begin(),payload.begin()+n,file.begin()));
#endif
}

static void push_dci(int n, int *queued) {
  nr_sense_dci_rec_t r; memset(&r,0,sizeof(r));
  for (int i=0;i<n;i++) { r.o.rnti=100+i; if (nr_sense_push_dci(&r)==1) (*queued)++; }
}
static int lines(const std::string &p) { auto b=bytes(p); return (int)std::count(b.begin(),b.end(),'\n'); }

TEST_F(SenseApi, DciQueuedAndPersisted) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  int q=0; push_dci(10,&q); EXPECT_EQ(q,10);
  nr_sense_close(); nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_EQ(lines(rec),10); EXPECT_EQ(s.dci_generated,10u); EXPECT_EQ(s.dci_persisted,10u); EXPECT_EQ(s.dci_lost,0u);
}
TEST_F(SenseApi, DciRingFullCountsLost) {
  cfg.rec_ring_slots=4; nr_sense_test_pause_writer(true); ASSERT_EQ(nr_sense_open(&cfg),0);
  int q=0; push_dci(10,&q); EXPECT_EQ(q,4);
  nr_sense_stats_t s{}; nr_sense_stats(&s); EXPECT_EQ(s.dci_generated,10u); EXPECT_EQ(s.dci_lost,6u);
  nr_sense_test_pause_writer(false); nr_sense_close(); nr_sense_stats(&s);
  EXPECT_EQ(s.dci_persisted,4u); EXPECT_EQ(s.dci_lost,6u);
}
TEST_F(SenseApi, DciWithoutRecordFileCountsLost) {
  cfg.records_path=nullptr; ASSERT_EQ(nr_sense_open(&cfg),0);   // cfr-only sink: no record file, no ZMQ
  int q=0; push_dci(3,&q);
  nr_sense_close(); nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_EQ(s.dci_persisted,0u); EXPECT_EQ(s.dci_lost,3u);
}
TEST_F(SenseApi, DciWriteErrorCountsLost) {
  ASSERT_EQ(nr_sense_open(&cfg),0);
  nr_sense_test_fail_writes(true);
  int q=0; push_dci(3,&q);
  nr_sense_close(); nr_sense_stats_t s{}; nr_sense_stats(&s);
  EXPECT_EQ(s.dci_lost,3u); EXPECT_EQ(s.dci_persisted,0u);
}

namespace {
struct QRing : ::testing::Test {
  std::string rec=temp_path(), cfr=temp_path();
  std::vector<float> iq; std::vector<uint16_t> k; std::vector<uint8_t> l;
  void open(uint32_t ring) {
    nr_sense_cfg_t cfg{rec.c_str(),cfr.c_str(),nullptr,ring,4096,16};
    ASSERT_EQ(nr_sense_open(&cfg),0);
    for (int s=2;s<=3;s++) for (int p=0;p<4;p++) for (int i=0;i<6;i++) { k.push_back(12*p+2*i); l.push_back(s); }
    iq.assign(2*k.size(),0.f);
    for (size_t i=0;i<k.size();i++) { iq[2*i]=4.f; iq[2*i+1]=0.5f*float(k[i]%5); }
  }
  uint64_t push(uint64_t slot) { return nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DMRS,1,iq.data(),(int)k.size(),(int)k.size(),k.data(),l.data(),slot,0,1.f,0,0,nullptr); }
  /* the writer computes the quality asynchronously: wait for it */
  static bool wait_q(uint64_t id, nr_sense_quality_t *q) {
    for (int i=0;i<400;i++) { if (nr_sense_quality_for_frame(id,q)) return true; usleep(5000); }
    return false;
  }
  void TearDown() override { nr_sense_test_pause_writer(false); nr_sense_close(); nr_isac_mode_set(NR_ISAC_MODE_LEGACY); unlink(rec.c_str()); unlink(cfr.c_str()); }
};
}
TEST_F(QRing, FrameIdRingMiss)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  open(2);                                   /* quality ring = frames + 64 slots = 66 */
  nr_sense_quality_t q;
  std::vector<uint64_t> id;
  for (int i=0;i<70;i++) { id.push_back(push(10+i)); ASSERT_NE(id.back(),0u) << i; ASSERT_TRUE(wait_q(id.back(),&q)) << i; }
  EXPECT_FALSE(nr_sense_quality_for_frame(id[0],&q)) << "overwritten frame id must not return a newer frame's quality";
  EXPECT_FALSE(nr_sense_quality_for_frame(id[3],&q));
  ASSERT_TRUE(nr_sense_quality_for_frame(id[5],&q));
  ASSERT_TRUE(nr_sense_quality_for_frame(id[69],&q));
  EXPECT_EQ(q.allocated_prbs,4);
  EXPECT_FALSE(nr_sense_quality_for_frame(0,&q));
  EXPECT_FALSE(nr_sense_quality_for_frame(9999,&q));
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  EXPECT_FALSE(nr_sense_quality_for_frame(id[69],&q)) << "legacy: no lookup";
}
TEST_F(QRing, DroppedPushLeavesNoQuality)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  open(1);
  nr_sense_test_pause_writer(true);
  const uint64_t a=push(1), b=push(2);              /* ring of 1, writer paused: the second is dropped (returns 0) */
  ASSERT_EQ(a,1u); ASSERT_EQ(b,0u);
  nr_sense_test_pause_writer(false);
  nr_sense_quality_t q;
  ASSERT_TRUE(wait_q(a,&q));
  EXPECT_FALSE(nr_sense_quality_for_frame(2,&q)) << "the dropped frame consumed id 2: nothing stored for it";
}
TEST_F(QRing, PushingThreadDoesNotCompute)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  open(4);
  nr_sense_test_pause_writer(true);
  const uint64_t a=push(1);
  nr_sense_quality_t q;
  EXPECT_FALSE(nr_sense_quality_for_frame(a,&q)) << "no quality exists until the writer has packed the frame";
  nr_sense_test_pause_writer(false);
  EXPECT_TRUE(wait_q(a,&q));
}
TEST_F(QRing, QualityMatchesComputeAndWideU32Path)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  open(4);
  nr_sense_quality_t want, got;
  nr_sense_quality_compute(1,iq.data(),(int)k.size(),(int)k.size(),k.data(),l.data(),1.f,&want);
  const uint64_t a=push(1);
  ASSERT_TRUE(wait_q(a,&got));
  EXPECT_EQ(memcmp(got.valid_mask,want.valid_mask,sizeof want.valid_mask),0);
  EXPECT_EQ(got.valid_cfr_prbs,want.valid_cfr_prbs);
  EXPECT_EQ(got.allocated_prbs,want.allocated_prbs);
  EXPECT_EQ(got.snr_db_p50,want.snr_db_p50);
  std::vector<uint32_t> k32(k.begin(),k.end()), l32(l.begin(),l.end());   /* the u32 entry point (data-aided / nr_isac) */
  const uint64_t b=nr_sense_push_cfr_u32(NR_SENSE_SRC_PDSCH_DATA,1,iq.data(),(int)k.size(),(int)k.size(),k32.data(),l32.data(),2,0,1.f,0,0,nullptr);
  ASSERT_TRUE(wait_q(b,&got));
  EXPECT_EQ(memcmp(got.valid_mask,want.valid_mask,sizeof want.valid_mask),0);
  EXPECT_EQ(got.valid_cfr_prbs,want.valid_cfr_prbs);
  EXPECT_EQ(got.snr_db_p50,want.snr_db_p50);
}
/* S5: the writer reports each formatted RS record of a sensing-mode frame to its unit; legacy and traffic leave the unit table untouched */
TEST_F(QRing, UnitCfrFromWriter)
{
  nr_cell_bind(0);
  nr_sense_unit_reset(0);
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  open(8);
  /* a wideband frame (130 PRBs): rows are opened by wideband-capable records only */
  k.clear(); l.clear();
  for (int sy = 2; sy <= 3; sy++) for (int p = 0; p < 130; p++) for (int i = 0; i < 6; i++) { k.push_back(12*p+2*i); l.push_back(sy); }
  iq.assign(2*k.size(),0.f);
  for (size_t i=0;i<k.size();i++) { iq[2*i]=4.f; iq[2*i+1]=0.5f*float(k[i]%5); }
  nr_sense_rs_meta_t m{};
  m.rnti = 0x4601; m.ssb_index = -1; m.k_ssb = -1; m.ta_est_samples = -1; m.transform_precoding = -1; m.dmrs_type = 1; m.dmrs_add_pos = -1; m.dmrs_n_scid = -1;
  m.nb_rb = 130; m.n_re = (int)k.size(); m.n_ant = 1; m.ports = 1;
  for (nr_isac_mode_t md : {NR_ISAC_MODE_LEGACY, NR_ISAC_MODE_TRAFFIC}) {
    nr_isac_mode_set(md);
    nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS, &m, push(1));
    usleep(300000);
    nr_sense_unit_view_t v[4];
    EXPECT_EQ(nr_sense_unit_snapshot(0, v, 4), 0) << "mode " << md << " must not touch the unit table";
  }
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  nr_sense_emit_rs(NR_SENSE_SRC_PDSCH_DMRS, &m, push(2));
  nr_sense_unit_view_t v[4];
  int n = 0;
  for (int i = 0; i < 400 && !(n = nr_sense_unit_snapshot(0, v, 4)); i++) usleep(5000);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(v[0].rnti, 0x4601);
  EXPECT_EQ(v[0].src, NR_UNIT_PDSCH_DMRS);
  EXPECT_GT(v[0].valid_ratio, 0.f);
  nr_sense_unit_reset(0);
}
TEST_F(QRing, LegacyPushComputesNothing)
{
  nr_isac_mode_set(NR_ISAC_MODE_LEGACY);
  open(4);
  const uint64_t a = push(1);
  ASSERT_EQ(a, 1u);
  usleep(300000);                           /* the writer has packed it (legacy: no quality) */
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);   /* the lookup is open now: a quality computed during the legacy pass would show */
  nr_sense_quality_t q;
  EXPECT_FALSE(nr_sense_quality_for_frame(a, &q)) << "legacy must not spend the quality computation";
  nr_isac_mode_set(NR_ISAC_MODE_TRAFFIC);
  const uint64_t b = push(2);
  usleep(300000);
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  EXPECT_FALSE(nr_sense_quality_for_frame(b, &q)) << "traffic must not spend it either";
  nr_sense_stats_t st; nr_sense_stats(&st);
  EXPECT_EQ(st.q_computed, 0u);
}

/* cost split (printed, not asserted: host-dependent): pushing thread vs writer */
TEST_F(QRing, CostPerRecord)
{
  struct Shape { const char *name; int nprb, nsym, step; } shapes[] = {
    {"dmrs 128prb x2sym comb2", 128, 2, 2}, {"data 217prb x13sym full-grid", 217, 13, 1}, {"data 128prb x13sym full-grid", 128, 13, 1}};
  for (const auto &sh : shapes) {
    std::vector<uint16_t> kk; std::vector<uint8_t> ll;
    for (int s = 0; s < sh.nsym; s++) for (int p = 0; p < sh.nprb; p++) for (int i = 0; i < 12; i += sh.step) { kk.push_back(12*p+i); ll.push_back(s); }
    const int n = (int)kk.size(), nant = 4;
    std::vector<float> v(2 * (size_t)nant * n);
    for (size_t i = 0; i < v.size(); i++) v[i] = 3.f * std::cos(0.01f * float(i % 977)) + 0.1f * float(i % 7);
    double push_us[2] = {0, 0}, writer_us[2] = {0, 0};
    for (int mode = 0; mode < 2; mode++) {
      nr_isac_mode_set(mode ? NR_ISAC_MODE_SENSING : NR_ISAC_MODE_LEGACY);
      nr_sense_cfg_t cfg{nullptr, nullptr, nullptr, 8, 70000, 16};
      cfg.cfr_path = cfr.c_str();
      ASSERT_EQ(nr_sense_open(&cfg), 0);
      const int reps = 20; uint64_t tot = 0;
      for (int r = 0; r < reps; r++) {
        auto t0 = std::chrono::steady_clock::now();
        nr_sense_push_cfr(NR_SENSE_SRC_PDSCH_DATA, nant, v.data(), n, n, kk.data(), ll.data(), r, 0, 1.f, 0, 0, nullptr);
        tot += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        usleep(20000);
      }
      nr_sense_stats_t st; nr_sense_stats(&st);   /* quality time is charged to the REC counter (no RS records are emitted here) */
      push_us[mode] = double(tot) / reps / 1e3;
      writer_us[mode] = st.q_computed ? double(st.rec_ns) / double(st.q_computed) / 1e3 : 0;
      nr_sense_close();
    }
    printf("S3_COST %-30s n_re=%6d push legacy=%7.1f us sensing=%7.1f us (delta %+6.1f us) | writer quality %7.1f us/frame\n", sh.name, n, push_us[0], push_us[1], push_us[1]-push_us[0], writer_us[1]);
  }
}

/* Defect 2026-11-05 (OTA 923, 40 SRS records, all valid_cfr_prbs 0): an UL CFR in the DL frame carries the transmitter's timing advance as a phase ramp (TA 1606 samples at
 * N = 4096 is 2.46 rad per subcarrier); the per-PRB constant+slope fit could not follow it (residual ~1 at ANY SNR). Synthetic SRS CFR through the real writer quality path: pilots every
 * comb-th RE (noise variance nv, the producer's LS scale), the other REs interpolated from the two neighbouring pilots, a 2-tap channel, a common timing ramp, partial bandwidth at first_prb > 0. */
TEST_F(QRing, SrsQualityWithTimingRamp)
{
  nr_isac_mode_set(NR_ISAC_MODE_SENSING);
  open(8);
  const float nv = 1e-3f;
  uint64_t rng = 88172645463325252ull;
  auto gauss = [&] { double u = 0; for (int i = 0; i < 12; i++) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; u += (rng >> 11) * (1.0 / 9007199254740992.0); } return float(u - 6); };
  for (int comb : {2, 4}) for (double snr : {6.0, 12.0, 20.0}) for (double ta : {0.0, 1606.0, -900.0}) {
    const int first = 20, nprb = 40, nk = nprb * 12, nant = 4;
    std::vector<uint16_t> kk; std::vector<uint8_t> ll; std::vector<float> v(2 * (size_t)nant * nk);
    for (int i = 0; i < nk; i++) { kk.push_back(12 * first + i); ll.push_back(12); }
    for (int a = 0; a < nant; a++) {
      std::vector<std::complex<float>> h(nk);
      for (int i = 0; i < nk; i++) { /* pilot REs only; unit-power channel scaled to the SNR */
        const double th = 2 * M_PI * 3 * i / 4096.0 + 0.9 * a;
        std::complex<float> h0 = std::complex<float>(1.f, 0.f) + 0.4f * std::complex<float>(cosf(th), sinf(th));
        h[i] = h0 * float(std::sqrt(nv * std::pow(10.0, snr / 10.0) / std::norm(h0)));
      }
      std::vector<std::complex<float>> e(nk);
      for (int i = 0; i < nk; i++) e[i] = ((i - 1) % comb == 0) ? h[i] + std::sqrt(nv / 2) * std::complex<float>(gauss(), gauss()) : 0.f;
      for (int i = 0; i < nk; i++) {
        std::complex<float> x;
        if ((i - 1) % comb == 0) x = e[i];
        else { int a0 = i - ((i - 1) % comb + comb) % comb, a1 = a0 + comb; if (a1 >= nk) a1 = a0; x = 0.5f * (e[a0] + e[a1]); }
        const double ph = 2 * M_PI * ta * (12 * first + i) / 4096.0;
        x *= std::complex<float>(cosf(ph), sinf(ph));
        v[2 * ((size_t)a * nk + i)] = x.real(); v[2 * ((size_t)a * nk + i) + 1] = x.imag();
      }
    }
    nr_sense_quality_t q;
    const uint64_t id = nr_sense_push_cfr(NR_SENSE_SRC_SRS, nant, v.data(), nk, nk, kk.data(), ll.data(), 7, 0.857f, nv, 0, 0, nullptr);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(wait_q(id, &q));
    SCOPED_TRACE(testing::Message() << "comb " << comb << " snr " << snr << " ta " << ta);
    EXPECT_EQ(q.first_prb, first);
    EXPECT_EQ(q.allocated_prbs, nprb);
    if (snr >= 12) EXPECT_EQ(q.valid_cfr_prbs, nprb);
    else EXPECT_GE(q.valid_cfr_prbs, nprb * 9 / 10) << "6 dB is 3 dB above the per-PRB threshold: a few PRBs fluctuate under it";
    EXPECT_NEAR(q.snr_db_p50, snr, 2.5) << "measured SNR follows the injected one (interpolated REs average two pilots' noise: a small positive bias)";
  }
  /* no evidence, no valid PRB: a noise-only record (same noise, no channel) and a 0 dB one are not valid */
  for (double snr : {-100.0, 0.0}) {
    const int nk = 480, nant = 4; std::vector<uint16_t> kk; std::vector<uint8_t> ll; std::vector<float> v(2 * (size_t)nant * nk);
    for (int i = 0; i < nk; i++) { kk.push_back(240 + i); ll.push_back(12); }
    for (int a = 0; a < nant; a++) for (int i = 0; i < nk; i++) {
      const float s = float(std::sqrt(nv * std::pow(10.0, snr / 10.0)));
      const double ph = 2 * M_PI * 1606.0 * (240 + i) / 4096.0;
      std::complex<float> x = std::complex<float>(s, 0.f) + std::sqrt(nv / 2) * std::complex<float>(gauss(), gauss());
      x *= std::complex<float>(cosf(ph), sinf(ph));
      v[2 * ((size_t)a * nk + i)] = x.real(); v[2 * ((size_t)a * nk + i) + 1] = x.imag();
    }
    nr_sense_quality_t q;
    const uint64_t id = nr_sense_push_cfr(NR_SENSE_SRC_SRS, nant, v.data(), nk, nk, kk.data(), ll.data(), 8, 0.857f, nv, 0, 0, nullptr);
    ASSERT_TRUE(wait_q(id, &q));
    EXPECT_EQ(q.valid_cfr_prbs, 0) << "snr " << snr;
  }
}
